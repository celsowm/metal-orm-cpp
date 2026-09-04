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

template <reflect::Entity T, std::meta::info... Excluded>
class DtoView {
public:
    explicit DtoView(T value) : value_(std::move(value)) {}

    [[nodiscard]] const T& value() const noexcept { return value_; }

    [[nodiscard]] Row response() const {
        return to_response_dto<T, Excluded...>(value_);
    }

    [[nodiscard]] Row create() const {
        return to_create_dto<T, Excluded...>(value_);
    }

    [[nodiscard]] Row update() const {
        return to_update_dto<T, Excluded...>(value_);
    }

private:
    T value_;
};

template <reflect::Entity T, std::meta::info... Excluded>
DtoView<T, Excluded...> dto(T value) {
    return DtoView<T, Excluded...>{std::move(value)};
}

} // namespace metal
