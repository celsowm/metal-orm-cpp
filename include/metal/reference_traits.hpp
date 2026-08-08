#pragma once

#include "metal/reference.hpp"
#include "metal/reflection.hpp"

namespace metal::reflect {

template <typename Target>
struct single_relation_traits<metal::belongs_to_reference<Target>> {
    static constexpr bool value = true;
    using target_type = Target;
};

template <typename Target>
struct single_relation_traits<metal::has_one_reference<Target>> {
    static constexpr bool value = true;
    using target_type = Target;
};

template <typename Target>
struct belongs_to_reference_traits<metal::belongs_to_reference<Target>> {
    static constexpr bool value = true;
    using target_type = Target;
};

template <typename Target>
struct has_one_reference_traits<metal::has_one_reference<Target>> {
    static constexpr bool value = true;
    using target_type = Target;
};

} // namespace metal::reflect
