#pragma once

#include "metal/column_defaults.hpp"
#include "metal/execution.hpp"
#include "metal/reflection.hpp"
#include "metal/runtime_pagination.hpp"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace metal {

enum class DtoMode {
    response,
    create,
    update
};

struct DtoField {
    std::string name;
    std::string column_name;
    bool required{false};
    bool nullable{false};
    bool generated{false};
    bool primary_key{false};
    bool has_default{false};
};

struct DtoDescriptor {
    std::string table;
    DtoMode mode{DtoMode::response};
    std::vector<DtoField> fields;
};

namespace detail {

template <std::meta::info Member>
std::string dto_member_name() {
    return std::string(std::meta::identifier_of(Member));
}

template <std::meta::info Member, std::meta::info... Excluded>
consteval bool dto_member_excluded() {
    return ((Member == Excluded) || ...);
}

template <reflect::Entity T, std::meta::info... Members>
consteval bool validate_dto_members() {
    if constexpr (sizeof...(Members) > 0) {
        static_assert((std::meta::is_nonstatic_data_member(Members) && ...),
                      "MetalORM: DTO member selection requires reflected data members");
        static_assert((std::same_as<reflect::owner_type_t<Members>, T> && ...),
                      "MetalORM: DTO members must belong to the declared entity");
        static_assert((reflect::is_persistent_member<Members>() && ...),
                      "MetalORM: DTO members must be persistent scalar columns");
    }
    return true;
}

template <DtoMode Mode, reflect::Entity T, std::meta::info... Excluded>
DtoDescriptor describe_dto_impl() {
    static_assert(validate_dto_members<T, Excluded...>());
    static_assert(reflect::validate_column_defaults<T>());

    DtoDescriptor out{reflect::table_name<T>(), Mode, {}};
    template for (constexpr auto member : reflect::data_members<T>()) {
        if constexpr (reflect::is_persistent_member<member>() &&
                      !dto_member_excluded<member, Excluded...>()) {
            constexpr bool generated = reflect::has<mapping::generated_t>(member);
            constexpr bool primary = reflect::has<mapping::primary_key_t>(member);
            constexpr bool has_default = reflect::has_column_default<member>();
            using M = reflect::member_type_t<member>;
            constexpr bool nullable = is_optional_v<M>;

            if constexpr (!((Mode == DtoMode::create || Mode == DtoMode::update) && generated)) {
                bool required = false;
                if constexpr (Mode == DtoMode::response) {
                    required = !nullable || primary;
                } else if constexpr (Mode == DtoMode::create) {
                    required = !nullable && !has_default;
                }

                out.fields.push_back(DtoField{
                    dto_member_name<member>(),
                    reflect::column_name<member>(),
                    required,
                    nullable,
                    generated,
                    primary,
                    has_default
                });
            }
        }
    }
    return out;
}

template <DtoMode Mode, reflect::Entity T, std::meta::info... Excluded>
Row entity_to_dto_impl(const T& entity) {
    static_assert(validate_dto_members<T, Excluded...>());
    static_assert(reflect::validate_column_defaults<T>());

    Row out;
    const auto* ptr = std::addressof(entity);
    template for (constexpr auto member : reflect::data_members<T>()) {
        if constexpr (reflect::is_persistent_member<member>() &&
                      !dto_member_excluded<member, Excluded...>()) {
            constexpr bool generated = reflect::has<mapping::generated_t>(member);
            if constexpr (!((Mode == DtoMode::create || Mode == DtoMode::update) && generated)) {
                out.emplace(dto_member_name<member>(), to_value(ptr->[:member:]));
            }
        }
    }
    return out;
}

} // namespace detail

template <reflect::Entity T, std::meta::info... Excluded>
DtoDescriptor describe_response_dto() {
    return detail::describe_dto_impl<DtoMode::response, T, Excluded...>();
}

template <reflect::Entity T, std::meta::info... Excluded>
DtoDescriptor describe_create_dto() {
    return detail::describe_dto_impl<DtoMode::create, T, Excluded...>();
}

template <reflect::Entity T, std::meta::info... Excluded>
DtoDescriptor describe_update_dto() {
    return detail::describe_dto_impl<DtoMode::update, T, Excluded...>();
}

template <reflect::Entity T, std::meta::info... Excluded>
Row to_response_dto(const T& entity) {
    return detail::entity_to_dto_impl<DtoMode::response, T, Excluded...>(entity);
}

template <reflect::Entity T, std::meta::info... Excluded>
Row to_create_dto(const T& entity) {
    return detail::entity_to_dto_impl<DtoMode::create, T, Excluded...>(entity);
}

template <reflect::Entity T, std::meta::info... Excluded>
Row to_update_dto(const T& entity) {
    return detail::entity_to_dto_impl<DtoMode::update, T, Excluded...>(entity);
}

template <std::meta::info... Members, reflect::Entity T>
Row pick_dto(const T& entity) {
    static_assert(sizeof...(Members) > 0,
                  "MetalORM: pick_dto requires at least one reflected member");
    static_assert(detail::validate_dto_members<T, Members...>());

    Row out;
    const auto* ptr = std::addressof(entity);
    ((out.emplace(detail::dto_member_name<Members>(), to_value(ptr->[:Members:]))), ...);
    return out;
}

inline Row to_response(Row input, const Row& auto_fields) {
    for (const auto& [key, value] : auto_fields) input[key] = value;
    return input;
}

inline Row with_defaults(Row dto, const Row& defaults) {
    Row out = defaults;
    for (auto& [key, value] : dto) out[key] = std::move(value);
    return out;
}

inline Row exclude_fields(Row value, std::initializer_list<std::string_view> keys) {
    for (const auto key : keys) value.erase(std::string(key));
    return value;
}

inline Row pick_fields(const Row& value, std::initializer_list<std::string_view> keys) {
    Row out;
    for (const auto key : keys) {
        auto found = value.find(std::string(key));
        if (found != value.end()) out.emplace(found->first, found->second);
    }
    return out;
}

inline Row map_fields(
    const Row& value,
    std::initializer_list<std::pair<std::string_view, std::string_view>> field_map) {
    Row out;
    std::vector<std::string> mapped_sources;
    mapped_sources.reserve(field_map.size());

    for (const auto& [source, target] : field_map) {
        mapped_sources.emplace_back(source);
        auto found = value.find(std::string(source));
        if (found != value.end()) out[std::string(target)] = found->second;
    }

    for (const auto& [key, item] : value) {
        if (std::find(mapped_sources.begin(), mapped_sources.end(), key) == mapped_sources.end()) {
            out.emplace(key, item);
        }
    }
    return out;
}

template <typename T>
struct PagedResponse {
    std::vector<T> items;
    std::size_t total_items{};
    std::size_t page{};
    std::size_t page_size{};
    std::size_t total_pages{};
    bool has_next_page{false};
    bool has_prev_page{false};
};

inline std::size_t calculate_total_pages(std::size_t total_items, std::size_t page_size) {
    if (page_size == 0) {
        throw std::invalid_argument("MetalORM: page_size must be greater than 0");
    }
    return std::max<std::size_t>(1, (total_items + page_size - 1) / page_size);
}

inline bool has_next_page(std::size_t current_page, std::size_t total_pages) noexcept {
    return current_page < total_pages;
}

inline bool has_prev_page(std::size_t current_page) noexcept {
    return current_page > 1;
}

struct PaginationMetadata {
    std::size_t total_pages{};
    bool has_next_page{false};
    bool has_prev_page{false};
};

inline PaginationMetadata compute_pagination_metadata(
    std::size_t total_items,
    std::size_t page,
    std::size_t page_size) {
    const auto total_pages = calculate_total_pages(total_items, page_size);
    return PaginationMetadata{
        total_pages,
        has_next_page(page, total_pages),
        has_prev_page(page)
    };
}

template <typename Page>
auto to_paged_response(Page page) {
    using Items = std::remove_cvref_t<decltype(page.items)>;
    using Item = typename Items::value_type;
    const auto metadata = compute_pagination_metadata(
        page.total_items,
        page.page,
        page.page_size);

    return PagedResponse<Item>{
        std::move(page.items),
        page.total_items,
        page.page,
        page.page_size,
        metadata.total_pages,
        metadata.has_next_page,
        metadata.has_prev_page
    };
}

} // namespace metal
