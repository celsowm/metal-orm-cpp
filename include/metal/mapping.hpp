#pragma once

#include <concepts>
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

template <std::size_t N>
fixed_text(const char (&)[N]) -> fixed_text<N>;

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
    many_to_many,
    morph_to,
    morph_one,
    morph_many
};

enum class cascade_mode { none, all, persist, remove, link };

constexpr bool cascades_persist(cascade_mode mode) noexcept {
    return mode == cascade_mode::persist || mode == cascade_mode::all;
}

constexpr bool cascades_remove(cascade_mode mode) noexcept {
    return mode == cascade_mode::remove || mode == cascade_mode::all;
}

constexpr bool links_only(cascade_mode mode) noexcept {
    return mode == cascade_mode::link;
}

// Reflections are the relation metadata. Optional keys default to the PK.
template <
    std::meta::info ForeignKey,
    cascade_mode Cascade = cascade_mode::none,
    std::meta::info TargetKey = std::meta::info{}>
struct belongs_to {};

template <
    std::meta::info TargetForeignKey,
    cascade_mode Cascade = cascade_mode::none,
    std::meta::info LocalKey = std::meta::info{}>
struct has_one {};

template <
    std::meta::info TargetForeignKey,
    cascade_mode Cascade = cascade_mode::none,
    std::meta::info LocalKey = std::meta::info{}>
struct has_many {};

template <
    std::meta::info Pivot,
    std::meta::info PivotRootForeignKey,
    std::meta::info PivotTargetForeignKey,
    cascade_mode Cascade = cascade_mode::none,
    std::meta::info LocalKey = std::meta::info{},
    std::meta::info TargetKey = std::meta::info{}>
struct many_to_many {};

// A morph target is a compile-time discriminator -> mapped type mapping.
// TargetKey defaults to the target primary key.
template <
    fixed_text TypeValue,
    std::meta::info Target,
    std::meta::info TargetKey = std::meta::info{}>
struct morph_target {};

// Child-side polymorphic inverse. TypeField and IdField live on the root.
template <
    std::meta::info TypeField,
    std::meta::info IdField,
    cascade_mode Cascade = cascade_mode::none,
    typename... Targets>
struct morph_to {};

// Parent-side polymorphic one/one and one/many. The reflected fields live on
// the target entity; LocalKey defaults to the root primary key.
template <
    std::meta::info TypeField,
    std::meta::info IdField,
    fixed_text TypeValue,
    cascade_mode Cascade = cascade_mode::none,
    std::meta::info LocalKey = std::meta::info{}>
struct morph_one {};

template <
    std::meta::info TypeField,
    std::meta::info IdField,
    fixed_text TypeValue,
    cascade_mode Cascade = cascade_mode::none,
    std::meta::info LocalKey = std::meta::info{}>
struct morph_many {};

template <typename T>
struct morph_target_traits;

template <fixed_text TypeValue, std::meta::info Target, std::meta::info TargetKey>
struct morph_target_traits<morph_target<TypeValue, Target, TargetKey>> {
    static constexpr auto type_value = TypeValue;
    static consteval std::meta::info target() { return Target; }
    static consteval std::meta::info target_key() { return TargetKey; }
};

template <typename... Ts>
struct type_list {};

template <typename T>
struct relation_annotation_traits {
    static constexpr bool value = false;
};

template <std::meta::info ForeignKey, cascade_mode Cascade, std::meta::info TargetKey>
struct relation_annotation_traits<belongs_to<ForeignKey, Cascade, TargetKey>> {
    static constexpr bool value = true;
    static constexpr relation_kind kind = relation_kind::belongs_to;
    static constexpr cascade_mode cascade = Cascade;
    static consteval std::meta::info foreign_key() { return ForeignKey; }
    static consteval std::meta::info target_key() { return TargetKey; }
};

template <std::meta::info TargetForeignKey, cascade_mode Cascade, std::meta::info LocalKey>
struct relation_annotation_traits<has_one<TargetForeignKey, Cascade, LocalKey>> {
    static constexpr bool value = true;
    static constexpr relation_kind kind = relation_kind::has_one;
    static constexpr cascade_mode cascade = Cascade;
    static consteval std::meta::info target_foreign_key() { return TargetForeignKey; }
    static consteval std::meta::info local_key() { return LocalKey; }
};

template <std::meta::info TargetForeignKey, cascade_mode Cascade, std::meta::info LocalKey>
struct relation_annotation_traits<has_many<TargetForeignKey, Cascade, LocalKey>> {
    static constexpr bool value = true;
    static constexpr relation_kind kind = relation_kind::has_many;
    static constexpr cascade_mode cascade = Cascade;
    static consteval std::meta::info target_foreign_key() { return TargetForeignKey; }
    static consteval std::meta::info local_key() { return LocalKey; }
};

template <
    std::meta::info Pivot,
    std::meta::info PivotRootForeignKey,
    std::meta::info PivotTargetForeignKey,
    cascade_mode Cascade,
    std::meta::info LocalKey,
    std::meta::info TargetKey>
struct relation_annotation_traits<many_to_many<
    Pivot,
    PivotRootForeignKey,
    PivotTargetForeignKey,
    Cascade,
    LocalKey,
    TargetKey>> {
    static constexpr bool value = true;
    static constexpr relation_kind kind = relation_kind::many_to_many;
    static constexpr cascade_mode cascade = Cascade;
    static consteval std::meta::info pivot() { return Pivot; }
    static consteval std::meta::info pivot_root_foreign_key() { return PivotRootForeignKey; }
    static consteval std::meta::info pivot_target_foreign_key() { return PivotTargetForeignKey; }
    static consteval std::meta::info local_key() { return LocalKey; }
    static consteval std::meta::info target_key() { return TargetKey; }
};

template <std::meta::info TypeField, std::meta::info IdField, cascade_mode Cascade, typename... Targets>
struct relation_annotation_traits<morph_to<TypeField, IdField, Cascade, Targets...>> {
    static constexpr bool value = true;
    static constexpr relation_kind kind = relation_kind::morph_to;
    static constexpr cascade_mode cascade = Cascade;
    using targets = type_list<Targets...>;
    static consteval std::meta::info type_field() { return TypeField; }
    static consteval std::meta::info id_field() { return IdField; }
};

template <
    std::meta::info TypeField,
    std::meta::info IdField,
    fixed_text TypeValue,
    cascade_mode Cascade,
    std::meta::info LocalKey>
struct relation_annotation_traits<morph_one<TypeField, IdField, TypeValue, Cascade, LocalKey>> {
    static constexpr bool value = true;
    static constexpr relation_kind kind = relation_kind::morph_one;
    static constexpr cascade_mode cascade = Cascade;
    static constexpr auto type_value = TypeValue;
    static consteval std::meta::info type_field() { return TypeField; }
    static consteval std::meta::info id_field() { return IdField; }
    static consteval std::meta::info local_key() { return LocalKey; }
};

template <
    std::meta::info TypeField,
    std::meta::info IdField,
    fixed_text TypeValue,
    cascade_mode Cascade,
    std::meta::info LocalKey>
struct relation_annotation_traits<morph_many<TypeField, IdField, TypeValue, Cascade, LocalKey>> {
    static constexpr bool value = true;
    static constexpr relation_kind kind = relation_kind::morph_many;
    static constexpr cascade_mode cascade = Cascade;
    static constexpr auto type_value = TypeValue;
    static consteval std::meta::info type_field() { return TypeField; }
    static consteval std::meta::info id_field() { return IdField; }
    static consteval std::meta::info local_key() { return LocalKey; }
};

template <typename T>
inline constexpr bool is_relation_annotation_v = relation_annotation_traits<T>::value;

} // namespace metal::mapping
