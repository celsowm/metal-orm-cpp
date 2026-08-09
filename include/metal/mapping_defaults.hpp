#pragma once

#include "metal/mapping.hpp"

#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace metal::mapping {

enum class default_value_kind {
    integer,
    real,
    boolean
};

struct default_value {
    default_value_kind kind{default_value_kind::integer};
    std::int64_t integer{};
    double real{};
    bool boolean{};

    consteval default_value(bool value)
        : kind(default_value_kind::boolean), boolean(value) {}

    template <std::integral T>
    requires (!std::same_as<std::remove_cv_t<T>, bool>)
    consteval default_value(T value)
        : kind(default_value_kind::integer) {
        if constexpr (std::is_unsigned_v<T>) {
            if (value > static_cast<T>(std::numeric_limits<std::int64_t>::max())) {
                throw "MetalORM: integral column default does not fit in int64";
            }
        }
        integer = static_cast<std::int64_t>(value);
    }

    template <std::floating_point T>
    consteval default_value(T value)
        : kind(default_value_kind::real), real(static_cast<double>(value)) {}
};

struct default_text {
    fixed_text<256> value;

    template <std::size_t N>
    consteval default_text(const char (&text)[N]) : value(text) {}
};

struct default_sql {
    fixed_text<256> expression;

    template <std::size_t N>
    consteval default_sql(const char (&text)[N]) : expression(text) {
        if (expression.view().empty()) {
            throw "MetalORM: raw SQL column default cannot be empty";
        }
    }
};

struct default_null_t {};
inline constexpr default_null_t default_null{};

} // namespace metal::mapping
