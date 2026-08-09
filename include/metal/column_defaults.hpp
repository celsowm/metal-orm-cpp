#pragma once

#include "metal/mapping_defaults.hpp"
#include "metal/reflection.hpp"
#include "metal/value.hpp"

#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace metal::reflect {

template <info Member>
consteval std::size_t column_default_count() {
    return std::meta::annotations_of_with_type(Member, ^^mapping::default_value).size() +
           std::meta::annotations_of_with_type(Member, ^^mapping::default_text).size() +
           std::meta::annotations_of_with_type(Member, ^^mapping::default_sql).size() +
           std::meta::annotations_of_with_type(Member, ^^mapping::default_null_t).size();
}

template <info Member>
consteval bool has_column_default() {
    return column_default_count<Member>() != 0;
}

template <info Member>
consteval bool validate_column_default() {
    static_assert(std::meta::is_nonstatic_data_member(Member),
                  "MetalORM: column default requires a reflected data member");
    static_assert(column_default_count<Member>() <= 1,
                  "MetalORM: a column can declare at most one default annotation");

    if constexpr (has_column_default<Member>()) {
        static_assert(is_persistent_member<Member>(),
                      "MetalORM: column defaults are valid only on persistent scalar members");
        using M = optional_value_t<member_type_t<Member>>;

        if constexpr (has<mapping::default_text>(Member)) {
            static_assert(std::same_as<M, std::string>,
                          "MetalORM: default_text requires a string column");
        }

        if constexpr (has<mapping::default_null_t>(Member)) {
            static_assert(is_optional_v<member_type_t<Member>>,
                          "MetalORM: default_null requires an optional/nullable column");
        }

        if constexpr (has<mapping::default_value>(Member)) {
            constexpr auto value = annotation<mapping::default_value>(Member);
            if constexpr (value.kind == mapping::default_value_kind::boolean) {
                static_assert(std::same_as<M, bool>,
                              "MetalORM: boolean default_value requires a bool column");
            } else if constexpr (value.kind == mapping::default_value_kind::integer) {
                static_assert(std::is_integral_v<M> && !std::same_as<M, bool>,
                              "MetalORM: integral default_value requires an integral column");
            } else if constexpr (value.kind == mapping::default_value_kind::real) {
                static_assert(std::is_floating_point_v<M>,
                              "MetalORM: floating default_value requires a floating-point column");
            }
        }
    }
    return true;
}

inline std::string quote_sql_text(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('\'');
    for (const char ch : value) {
        if (ch == '\'') out.push_back('\'');
        out.push_back(ch);
    }
    out.push_back('\'');
    return out;
}

template <info Member>
std::string column_default_sql() {
    static_assert(validate_column_default<Member>());
    static_assert(has_column_default<Member>(),
                  "MetalORM: column_default_sql requires a declared default");

    if constexpr (has<mapping::default_text>(Member)) {
        constexpr auto value = annotation<mapping::default_text>(Member);
        return quote_sql_text(value.value.view());
    } else if constexpr (has<mapping::default_sql>(Member)) {
        constexpr auto value = annotation<mapping::default_sql>(Member);
        return std::string(value.expression.view());
    } else if constexpr (has<mapping::default_null_t>(Member)) {
        return "NULL";
    } else {
        constexpr auto value = annotation<mapping::default_value>(Member);
        if constexpr (value.kind == mapping::default_value_kind::boolean) {
            return value.boolean ? "1" : "0";
        } else if constexpr (value.kind == mapping::default_value_kind::integer) {
            return std::to_string(value.integer);
        } else {
            std::ostringstream out;
            out << std::setprecision(17) << value.real;
            return out.str();
        }
    }
}

template <info Member>
std::optional<Value> column_default_literal() {
    static_assert(validate_column_default<Member>());
    if constexpr (!has_column_default<Member>() || has<mapping::default_sql>(Member)) {
        return std::nullopt;
    } else if constexpr (has<mapping::default_text>(Member)) {
        constexpr auto value = annotation<mapping::default_text>(Member);
        return Value{std::string(value.value.view())};
    } else if constexpr (has<mapping::default_null_t>(Member)) {
        return Value{nullptr};
    } else {
        constexpr auto value = annotation<mapping::default_value>(Member);
        if constexpr (value.kind == mapping::default_value_kind::boolean) {
            return Value{value.boolean};
        } else if constexpr (value.kind == mapping::default_value_kind::integer) {
            return Value{value.integer};
        } else {
            return Value{value.real};
        }
    }
}

template <Mapped T>
consteval bool validate_column_defaults() {
    template for (constexpr auto member : data_members<T>()) {
        static_assert(validate_column_default<member>());
    }
    return true;
}

} // namespace metal::reflect
