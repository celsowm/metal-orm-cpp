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

template <typename Reference>
consteval info resolve_physical_reference_target() {
    using Traits = mapping::reference_annotation_traits<Reference>;
    if constexpr (Traits::by_member) {
        return Traits::target_column();
    } else {
        constexpr auto target_type = Traits::target_type();
        static_assert(std::meta::is_type(target_type),
                      "MetalORM: reference_to target must reflect a mapped type");
        using Target = [: target_type :];
        static_assert(Mapped<Target>,
                      "MetalORM: reference_to target must be a mapped type");

        info result{};
        std::size_t count = 0;
        template for (constexpr auto candidate : data_members<Target>()) {
            if constexpr (is_persistent_member<candidate>()) {
                if constexpr (column_name_view<candidate>() == Traits::target_column_name.view()) {
                    result = candidate;
                    ++count;
                }
            }
        }
        if (count != 1) {
            throw "MetalORM: reference_to target column must resolve to exactly one persistent member";
        }
        return result;
    }
}

template <info Member>
consteval info physical_reference_target() {
    static_assert(has_physical_reference<Member>(),
                  "MetalORM: physical_reference_target requires a physical reference annotation");
    using Reference = physical_reference_annotation_t<Member>;
    return resolve_physical_reference_target<Reference>();
}

template <info Member>
consteval bool validate_physical_reference() {
    static_assert(std::meta::is_nonstatic_data_member(Member),
                  "MetalORM: physical references require a reflected data member");

    if constexpr (has_physical_reference<Member>()) {
        static_assert(is_persistent_member<Member>(),
                      "MetalORM: physical references are valid only on persistent scalar members");

        constexpr auto target = physical_reference_target<Member>();
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

template <info Member>
consteval info column_check_annotation_info() {
    info result{};
    std::size_t count = 0;

    template for (constexpr auto candidate :
                  std::define_static_array(std::meta::annotations_of(Member))) {
        using Raw = [: std::meta::type_of(candidate) :];
        using A = std::remove_cv_t<Raw>;
        if constexpr (mapping::is_check_annotation_v<A>) {
            result = candidate;
            ++count;
        }
    }

    if (count > 1) {
        throw "MetalORM: a column can declare at most one physical check annotation";
    }
    return result;
}

template <info Member>
consteval bool has_column_check() {
    return column_check_annotation_info<Member>() != info{};
}

template <info Member>
using column_check_annotation_t =
    std::remove_cv_t<[: std::meta::type_of(column_check_annotation_info<Member>()) :]>;

template <info Member>
consteval bool validate_column_check() {
    static_assert(std::meta::is_nonstatic_data_member(Member),
                  "MetalORM: column checks require a reflected data member");

    if constexpr (has_column_check<Member>()) {
        static_assert(is_persistent_member<Member>(),
                      "MetalORM: column checks are valid only on persistent scalar members");
        using Check = column_check_annotation_t<Member>;
        using Traits = mapping::check_annotation_traits<Check>;
        static_assert(!Traits::expression.view().empty(),
                      "MetalORM: column check expression cannot be empty");
    }
    return true;
}

template <Mapped T, typename F>
constexpr void for_each_table_check(F&& fn) {
    template for (constexpr auto candidate :
                  std::define_static_array(std::meta::annotations_of(^^T))) {
        using Raw = [: std::meta::type_of(candidate) :];
        using A = std::remove_cv_t<Raw>;
        if constexpr (mapping::is_table_check_annotation_v<A>) {
            fn.template operator()<A>();
        }
    }
}

template <Mapped T>
consteval bool validate_physical_checks() {
    template for (constexpr auto member : data_members<T>()) {
        static_assert(validate_column_check<member>());
    }

    template for (constexpr auto candidate :
                  std::define_static_array(std::meta::annotations_of(^^T))) {
        using Raw = [: std::meta::type_of(candidate) :];
        using A = std::remove_cv_t<Raw>;
        if constexpr (mapping::is_table_check_annotation_v<A>) {
            using Traits = mapping::table_check_annotation_traits<A>;
            static_assert(!Traits::expression.view().empty(),
                          "MetalORM: table check expression cannot be empty");
            if constexpr (Traits::named) {
                static_assert(!Traits::name.view().empty(),
                              "MetalORM: named table check constraint name cannot be empty");
            }
        }
    }
    return true;
}

} // namespace metal::reflect
