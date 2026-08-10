#pragma once

#include "metal/mapping_constraints.hpp"
#include "metal/reflection.hpp"

#include <type_traits>

namespace metal::reflect {

template <info Member>
consteval info physical_reference_annotation_info() {
    info result{};
    std::size_t count = 0;

    template for (constexpr auto candidate :
                  std::define_static_array(std::meta::annotations_of(Member))) {
        using Raw = [: std::meta::type_of(candidate) :];
        using A = std::remove_cv_t<Raw>;
        if constexpr (mapping::is_reference_annotation_v<A>) {
            result = candidate;
            ++count;
        }
    }

    if (count > 1) {
        throw "MetalORM: a column cannot declare more than one physical reference annotation";
    }
    return result;
}

template <info Member>
consteval bool has_physical_reference() {
    return physical_reference_annotation_info<Member>() != info{};
}

template <info Member>
using physical_reference_annotation_t =
    std::remove_cv_t<[: std::meta::type_of(physical_reference_annotation_info<Member>()) :]>;

template <info Member>
consteval bool validate_physical_reference() {
    static_assert(std::meta::is_nonstatic_data_member(Member),
                  "MetalORM: physical references require a reflected data member");

    if constexpr (has_physical_reference<Member>()) {
        static_assert(is_persistent_member<Member>(),
                      "MetalORM: physical references are valid only on persistent scalar members");

        using Reference = physical_reference_annotation_t<Member>;
        using Traits = mapping::reference_annotation_traits<Reference>;
        constexpr auto target = Traits::target_column();
        static_assert(std::meta::is_nonstatic_data_member(target),
                      "MetalORM: reference target must reflect a data member");

        using Target = owner_type_t<target>;
        static_assert(Mapped<Target>,
                      "MetalORM: reference target must belong to a mapped type");
        static_assert(is_persistent_member<target>(),
                      "MetalORM: reference target must be a persistent scalar column");
        static_assert(key_types_compatible<Member, target>(),
                      "MetalORM: physical foreign-key and target-column types are incompatible");
    }

    return true;
}

template <Mapped T>
consteval bool validate_physical_references() {
    template for (constexpr auto member : data_members<T>()) {
        static_assert(validate_physical_reference<member>());
    }
    return true;
}

} // namespace metal::reflect
