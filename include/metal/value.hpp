#pragma once

#include <cstdint>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace metal {

using Value = std::variant<std::nullptr_t, std::int64_t, double, std::string, bool>;

template <typename T>
struct is_optional : std::false_type {};

template <typename T>
struct is_optional<std::optional<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_optional_v = is_optional<std::remove_cvref_t<T>>::value;

template <typename T>
struct optional_value { using type = T; };

template <typename T>
struct optional_value<std::optional<T>> { using type = T; };

template <typename T>
using optional_value_t = typename optional_value<std::remove_cvref_t<T>>::type;

template <typename T>
inline constexpr bool PersistableValue = [] {
    using U = optional_value_t<T>;
    return std::is_integral_v<U> || std::is_floating_point_v<U> ||
           std::is_same_v<U, std::string> || std::is_same_v<U, bool>;
}();

template <typename T>
Value to_value(const T& value) {
    using U = std::remove_cvref_t<T>;
    if constexpr (is_optional_v<U>) {
        return value ? to_value(*value) : Value{nullptr};
    } else if constexpr (std::is_same_v<U, std::string>) {
        return value;
    } else if constexpr (std::is_same_v<U, std::string_view>) {
        return std::string(value);
    } else if constexpr (std::is_convertible_v<const T&, std::string_view>) {
        return std::string(std::string_view(value));
    } else if constexpr (std::is_same_v<U, const char*> || std::is_same_v<U, char*>) {
        return std::string(value);
    } else if constexpr (std::is_same_v<U, bool>) {
        return value;
    } else if constexpr (std::is_integral_v<U>) {
        return static_cast<std::int64_t>(value);
    } else if constexpr (std::is_floating_point_v<U>) {
        return static_cast<double>(value);
    } else {
        static_assert(!sizeof(U), "Unsupported MetalORM value type");
    }
}

template <typename T>
T from_value(const Value& value) {
    using U = std::remove_cvref_t<T>;
    if constexpr (is_optional_v<U>) {
        using Inner = typename U::value_type;
        if (std::holds_alternative<std::nullptr_t>(value)) return std::nullopt;
        return U{from_value<Inner>(value)};
    } else if constexpr (std::is_same_v<U, std::string>) {
        if (const auto* p = std::get_if<std::string>(&value)) return *p;
    } else if constexpr (std::is_same_v<U, bool>) {
        if (const auto* p = std::get_if<bool>(&value)) return *p;
        if (const auto* p = std::get_if<std::int64_t>(&value)) return *p != 0;
    } else if constexpr (std::is_integral_v<U>) {
        if (const auto* p = std::get_if<std::int64_t>(&value)) return static_cast<U>(*p);
        if (const auto* p = std::get_if<bool>(&value)) return static_cast<U>(*p);
        if (const auto* p = std::get_if<double>(&value)) return static_cast<U>(*p);
    } else if constexpr (std::is_floating_point_v<U>) {
        if (const auto* p = std::get_if<double>(&value)) return static_cast<U>(*p);
        if (const auto* p = std::get_if<std::int64_t>(&value)) return static_cast<U>(*p);
    }
    throw std::runtime_error("MetalORM: incompatible database value conversion");
}

inline std::string value_key(const Value& value) {
    return std::visit([](const auto& v) -> std::string {
        using U = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<U, std::nullptr_t>) return "null";
        else if constexpr (std::is_same_v<U, std::string>) return "s:" + v;
        else if constexpr (std::is_same_v<U, bool>) return v ? "b:1" : "b:0";
        else {
            std::ostringstream out;
            out << v;
            return out.str();
        }
    }, value);
}

inline bool is_empty_generated_value(const Value& value) {
    if (std::holds_alternative<std::nullptr_t>(value)) return true;
    if (const auto* p = std::get_if<std::int64_t>(&value)) return *p == 0;
    return false;
}

} // namespace metal
