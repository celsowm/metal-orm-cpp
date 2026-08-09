#pragma once

#include "metal/dto.hpp"
#include "metal/query.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace metal {

struct SortInput {
    std::optional<std::string> field;
    bool ascending{true};
};

template <std::meta::info... Members>
struct DtoMemberPolicy {};

namespace detail {

template <std::meta::info Member, std::meta::info... Allowed>
consteval bool sort_member_allowed() {
    if constexpr (sizeof...(Allowed) == 0) return true;
    return ((Member == Allowed) || ...);
}

} // namespace detail

template <std::meta::info... Allowed, reflect::Entity T>
SelectQuery<T> apply_sort(SelectQuery<T> query, const SortInput& input = {}) {
    static_assert(detail::validate_dto_members<T, Allowed...>());
    constexpr auto Pk = reflect::primary_key_member<T>();
    const auto pk_name = detail::dto_member_name<Pk>();

    if (!input.field || input.field->empty()) {
        query.order_by(field<Pk>, true);
        return query;
    }

    bool known = false;
    bool allowed = false;
    reflect::for_each_column<T>([&]<std::meta::info Member>() {
        if (known) return;
        if (detail::dto_member_name<Member>() != *input.field) return;
        known = true;
        if constexpr (detail::sort_member_allowed<Member, Allowed...>()) {
            allowed = true;
            query.order_by(field<Member>, input.ascending);
        }
    });

    if (!known) {
        throw std::invalid_argument(
            "MetalORM: unknown REST sort field '" + *input.field + "'");
    }
    if (!allowed) {
        throw std::invalid_argument(
            "MetalORM: REST sort field '" + *input.field + "' is not allowed by the reflected sort policy");
    }

    if (*input.field != pk_name) {
        query.order_by(field<Pk>, true);
    }
    return query;
}

} // namespace metal
