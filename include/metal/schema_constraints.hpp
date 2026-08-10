#pragma once

#include "metal/mapping_constraints.hpp"
#include "metal/reflection.hpp"

namespace metal::reflect {

template <info Member>
consteval bool validate_physical_reference() {
    static_assert(std::meta::is_nonstatic_data_member(Member),
                  "MetalORM: physical references require a reflected data member");

    if constexpr (has<mapping::reference>(Member)) {
        static_assert(is_persistent_member<Member>(),
                      "MetalORM: physical references are valid only on persistent scalar members");

        constexpr auto ref = annotation<mapping::reference>(Member);
        constexpr auto target = ref.target_column;
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
