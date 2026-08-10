#pragma once

#include "metal/mapping.hpp"

#include <meta>
#include <string_view>
#include <type_traits>

namespace metal::mapping {

enum class referential_action {
    unspecified,
    no_action,
    restrict,
    cascade,
    set_null,
    set_default
};

constexpr std::string_view referential_action_sql(referential_action action) noexcept {
    switch (action) {
        case referential_action::no_action: return "NO ACTION";
        case referential_action::restrict: return "RESTRICT";
        case referential_action::cascade: return "CASCADE";
        case referential_action::set_null: return "SET NULL";
        case referential_action::set_default: return "SET DEFAULT";
        case referential_action::unspecified: return {};
    }
    return {};
}

/**
 * Explicit physical foreign-key metadata for a persistent scalar member.
 *
 * ORM relation annotations such as belongs_to remain independent from this
 * declaration. A relation does not imply a database constraint.
 */
template <
    std::meta::info TargetColumn,
    referential_action OnDelete = referential_action::unspecified,
    referential_action OnUpdate = referential_action::unspecified>
struct reference {};

/**
 * Code-generation-safe physical reference.
 *
 * TargetType is reflected directly while TargetColumn is the target's physical
 * database column name. This avoids requiring a target data-member expression
 * while the generated target type may still only be forward-declared. The
 * column name is resolved back to a persistent reflected member during mapping
 * validation after all generated types are complete.
 */
template <
    std::meta::info TargetType,
    fixed_text TargetColumn,
    referential_action OnDelete = referential_action::unspecified,
    referential_action OnUpdate = referential_action::unspecified>
struct reference_to {};

template <typename T>
struct reference_annotation_traits {
    static constexpr bool value = false;
};

template <
    std::meta::info TargetColumn,
    referential_action OnDelete,
    referential_action OnUpdate>
struct reference_annotation_traits<reference<TargetColumn, OnDelete, OnUpdate>> {
    static constexpr bool value = true;
    static constexpr bool by_member = true;
    static constexpr referential_action on_delete = OnDelete;
    static constexpr referential_action on_update = OnUpdate;
    static consteval std::meta::info target_column() { return TargetColumn; }
};

template <
    std::meta::info TargetType,
    fixed_text TargetColumn,
    referential_action OnDelete,
    referential_action OnUpdate>
struct reference_annotation_traits<reference_to<TargetType, TargetColumn, OnDelete, OnUpdate>> {
    static constexpr bool value = true;
    static constexpr bool by_member = false;
    static constexpr referential_action on_delete = OnDelete;
    static constexpr referential_action on_update = OnUpdate;
    static constexpr auto target_column_name = TargetColumn;
    static consteval std::meta::info target_type() { return TargetType; }
};

template <typename T>
inline constexpr bool is_reference_annotation_v =
    reference_annotation_traits<std::remove_cv_t<T>>::value;

/** Inline physical CHECK constraint for one persistent scalar column. */
template <fixed_text Expression>
struct check {};

template <typename T>
struct check_annotation_traits {
    static constexpr bool value = false;
};

template <fixed_text Expression>
struct check_annotation_traits<check<Expression>> {
    static constexpr bool value = true;
    static constexpr auto expression = Expression;
};

template <typename T>
inline constexpr bool is_check_annotation_v =
    check_annotation_traits<std::remove_cv_t<T>>::value;

/** Unnamed table-level CHECK constraint. */
template <fixed_text Expression>
struct table_check {};

/** Named table-level CHECK constraint. */
template <fixed_text Name, fixed_text Expression>
struct named_table_check {};

template <typename T>
struct table_check_annotation_traits {
    static constexpr bool value = false;
};

template <fixed_text Expression>
struct table_check_annotation_traits<table_check<Expression>> {
    static constexpr bool value = true;
    static constexpr bool named = false;
    static constexpr auto expression = Expression;
};

template <fixed_text Name, fixed_text Expression>
struct table_check_annotation_traits<named_table_check<Name, Expression>> {
    static constexpr bool value = true;
    static constexpr bool named = true;
    static constexpr auto name = Name;
    static constexpr auto expression = Expression;
};

template <typename T>
inline constexpr bool is_table_check_annotation_v =
    table_check_annotation_traits<std::remove_cv_t<T>>::value;

} // namespace metal::mapping
