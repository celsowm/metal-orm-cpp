#pragma once

#include "metal/mapping.hpp"

#include <meta>
#include <string_view>

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
struct reference {
    std::meta::info target_column{};
    referential_action on_delete{referential_action::unspecified};
    referential_action on_update{referential_action::unspecified};

    consteval reference(
        std::meta::info target,
        referential_action delete_action = referential_action::unspecified,
        referential_action update_action = referential_action::unspecified)
        : target_column(target),
          on_delete(delete_action),
          on_update(update_action) {}
};

} // namespace metal::mapping
