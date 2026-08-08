#pragma once

#include <cstddef>
#include <meta>
#include <string_view>

namespace metal::mapping {

template <std::size_t Capacity>
struct fixed_text {
    char data[Capacity]{};

    template <std::size_t N>
    consteval fixed_text(const char (&text)[N]) {
        static_assert(N <= Capacity, "MetalORM mapping string is too long");
        for (std::size_t i = 0; i < N; ++i) data[i] = text[i];
    }

    constexpr std::string_view view() const noexcept {
        std::size_t n = 0;
        while (n < Capacity && data[n] != '\0') ++n;
        return {data, n};
    }
};

struct table {
    fixed_text<96> name;

    template <std::size_t N>
    consteval table(const char (&value)[N]) : name(value) {}
};

struct column {
    fixed_text<96> name;

    template <std::size_t N>
    consteval column(const char (&value)[N]) : name(value) {}
};

struct primary_key_t {};
struct generated_t {};
struct ignore_t {};

inline constexpr primary_key_t primary_key{};
inline constexpr generated_t generated{};
inline constexpr ignore_t ignore{};

enum class relation_kind {
    belongs_to,
    has_one,
    has_many,
    many_to_many
};

// Relationship metadata carries reflections, never column/table names.
// A null key means "use the primary key of that side".
template <std::meta::info ForeignKey, std::meta::info TargetKey = std::meta::info{}>
struct belongs_to {};

template <std::meta::info TargetForeignKey, std::meta::info LocalKey = std::meta::info{}>
struct has_one {};

template <std::meta::info TargetForeignKey, std::meta::info LocalKey = std::meta::info{}>
struct has_many {};

template <
    std::meta::info Pivot,
    std::meta::info PivotRootForeignKey,
    std::meta::info PivotTargetForeignKey,
    std::meta::info LocalKey = std::meta::info{},
    std::meta::info TargetKey = std::meta::info{}>
struct many_to_many {};

template <typename T>
struct relation_annotation_traits {
    static constexpr bool value = false;
};

template <std::meta::info ForeignKey, std::meta::info TargetKey>
struct relation_annotation_traits<belongs_to<ForeignKey, TargetKey>> {
    static constexpr bool value = true;
    static constexpr relation_kind kind = relation_kind::belongs_to;
    static consteval std::meta::info foreign_key() { return ForeignKey; }
    static consteval std::meta::info target_key() { return TargetKey; }
};

template <std::meta::info TargetForeignKey, std::meta::info LocalKey>
struct relation_annotation_traits<has_one<TargetForeignKey, LocalKey>> {
    static constexpr bool value = true;
    static constexpr relation_kind kind = relation_kind::has_one;
    static consteval std::meta::info target_foreign_key() { return TargetForeignKey; }
    static consteval std::meta::info local_key() { return LocalKey; }
};

template <std::meta::info TargetForeignKey, std::meta::info LocalKey>
struct relation_annotation_traits<has_many<TargetForeignKey, LocalKey>> {
    static constexpr bool value = true;
    static constexpr relation_kind kind = relation_kind::has_many;
    static consteval std::meta::info target_foreign_key() { return TargetForeignKey; }
    static consteval std::meta::info local_key() { return LocalKey; }
};

template <
    std::meta::info Pivot,
    std::meta::info PivotRootForeignKey,
    std::meta::info PivotTargetForeignKey,
    std::meta::info LocalKey,
    std::meta::info TargetKey>
struct relation_annotation_traits<many_to_many<
    Pivot,
    PivotRootForeignKey,
    PivotTargetForeignKey,
    LocalKey,
    TargetKey>> {
    static constexpr bool value = true;
    static constexpr relation_kind kind = relation_kind::many_to_many;
    static consteval std::meta::info pivot() { return Pivot; }
    static consteval std::meta::info pivot_root_foreign_key() { return PivotRootForeignKey; }
    static consteval std::meta::info pivot_target_foreign_key() { return PivotTargetForeignKey; }
    static consteval std::meta::info local_key() { return LocalKey; }
    static consteval std::meta::info target_key() { return TargetKey; }
};

template <typename T>
inline constexpr bool is_relation_annotation_v = relation_annotation_traits<T>::value;

} // namespace metal::mapping
