#pragma once

#include "metal/mapping.hpp"
#include "metal/value.hpp"

#include <meta>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#ifndef __cpp_impl_reflection
#error "MetalORM requires C++26 static reflection (GCC 16+ with -std=c++26 -freflection)"
#endif

namespace metal::reflect {

using std::meta::info;

template <typename Annotation>
consteval bool has(info item) {
    return !std::meta::annotations_of_with_type(item, ^^Annotation).empty();
}

template <typename Annotation>
consteval Annotation annotation(info item) {
    auto values = std::meta::annotations_of_with_type(item, ^^Annotation);
    if (values.size() != 1) {
        throw "MetalORM: expected exactly one annotation of the requested type";
    }
    return std::meta::extract<Annotation>(values.front());
}

template <typename T>
concept Entity = std::is_class_v<T> && has<mapping::table>(^^T);

template <Entity T>
inline constexpr auto table_mapping = annotation<mapping::table>(^^T);

template <Entity T>
std::string table_name() {
    return std::string(table_mapping<T>.name.view());
}

template <typename T>
consteval auto data_members() {
    const auto ctx = std::meta::access_context::current();
    return std::define_static_array(
        std::meta::nonstatic_data_members_of(^^T, ctx));
}

template <info Member>
using member_type_t = [: std::meta::type_of(Member) :];

template <info Member>
using owner_type_t = [: std::meta::parent_of(Member) :];

template <info Member>
consteval bool is_persistent_member() {
    using M = member_type_t<Member>;
    return PersistableValue<M> &&
           !has<mapping::ignore_t>(Member) &&
           !has<mapping::many_to_many>(Member);
}

template <info Member>
std::string column_name() {
    static_assert(std::meta::is_nonstatic_data_member(Member));
    if constexpr (has<mapping::column>(Member)) {
        constexpr auto mapped = annotation<mapping::column>(Member);
        return std::string(mapped.name.view());
    } else {
        return std::string(std::meta::identifier_of(Member));
    }
}

template <Entity T, typename F>
constexpr void for_each_column(F&& fn) {
    template for (constexpr auto member : data_members<T>()) {
        if constexpr (is_persistent_member<member>()) {
            fn.template operator()<member>();
        }
    }
}

template <Entity T>
consteval info primary_key_member() {
    info result{};
    std::size_t count = 0;
    template for (constexpr auto member : data_members<T>()) {
        if constexpr (is_persistent_member<member>() && has<mapping::primary_key_t>(member)) {
            result = member;
            ++count;
        }
    }
    if (count != 1) {
        throw "MetalORM: an entity must declare exactly one [[=metal::mapping::primary_key]] member";
    }
    return result;
}

template <Entity T>
Value primary_key_value(const T& entity) {
    constexpr auto pk = primary_key_member<T>();
    return to_value(entity.[:pk:]);
}

template <Entity T>
void set_primary_key_value(T& entity, const Value& value) {
    constexpr auto pk = primary_key_member<T>();
    using Key = member_type_t<pk>;
    entity.[:pk:] = from_value<Key>(value);
}

template <Entity T>
std::string primary_key_name() {
    constexpr auto pk = primary_key_member<T>();
    return column_name<pk>();
}

template <Entity T>
consteval bool primary_key_is_generated() {
    constexpr auto pk = primary_key_member<T>();
    return has<mapping::generated_t>(pk);
}

template <typename T>
struct many_collection_traits {
    static constexpr bool value = false;
};

template <typename Target, typename Alloc>
struct many_collection_traits<std::vector<std::shared_ptr<Target>, Alloc>> {
    static constexpr bool value = true;
    using target_type = Target;
};

template <typename T>
inline constexpr bool is_many_collection_v = many_collection_traits<std::remove_cvref_t<T>>::value;

template <typename T>
using many_target_t = typename many_collection_traits<std::remove_cvref_t<T>>::target_type;

template <Entity T>
Value value_for_column(const T& entity, std::string_view name) {
    Value result{nullptr};
    bool found = false;
    for_each_column<T>([&]<info Member>() {
        if (column_name<Member>() == name) {
            result = to_value(entity.[:Member:]);
            found = true;
        }
    });
    if (!found) {
        throw std::runtime_error("MetalORM: relation references an unmapped column");
    }
    return result;
}

} // namespace metal::reflect
