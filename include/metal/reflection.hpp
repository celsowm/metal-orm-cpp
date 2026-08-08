#pragma once

#include "metal/collection.hpp"
#include "metal/mapping.hpp"
#include "metal/value.hpp"

#include <meta>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

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
concept Mapped = std::is_class_v<T> && has<mapping::table>(^^T);

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
consteval info relation_annotation_info() {
    info result{};
    std::size_t count = 0;

    template for (constexpr auto candidate :
                  std::define_static_array(std::meta::annotations_of(Member))) {
        using Raw = [: std::meta::type_of(candidate) :];
        using A = std::remove_cv_t<Raw>;
        if constexpr (mapping::is_relation_annotation_v<A>) {
            result = candidate;
            ++count;
        }
    }

    if (count > 1) {
        throw "MetalORM: a member cannot declare more than one relationship annotation";
    }
    return result;
}

template <info Member>
consteval bool has_relation_annotation() {
    return relation_annotation_info<Member>() != info{};
}

template <info Member>
using relation_annotation_t =
    std::remove_cv_t<[: std::meta::type_of(relation_annotation_info<Member>()) :]>;

template <info Member>
consteval bool is_persistent_member() {
    using M = member_type_t<Member>;
    return PersistableValue<M> &&
           !has<mapping::ignore_t>(Member) &&
           !has_relation_annotation<Member>();
}

template <info Member>
consteval std::string_view column_name_view() {
    static_assert(std::meta::is_nonstatic_data_member(Member));
    if constexpr (has<mapping::column>(Member)) {
        constexpr auto mapped = annotation<mapping::column>(Member);
        return mapped.name.view();
    } else {
        return std::meta::identifier_of(Member);
    }
}

template <info Member>
std::string column_name() {
    return std::string(column_name_view<Member>());
}

template <Mapped T, typename F>
constexpr void for_each_column(F&& fn) {
    template for (constexpr auto member : data_members<T>()) {
        if constexpr (is_persistent_member<member>()) {
            fn.template operator()<member>();
        }
    }
}

template <Mapped T>
consteval std::size_t primary_key_count() {
    std::size_t count = 0;
    template for (constexpr auto member : data_members<T>()) {
        if constexpr (is_persistent_member<member>() && has<mapping::primary_key_t>(member)) {
            ++count;
        }
    }
    return count;
}

template <Mapped T>
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
        throw "MetalORM: this operation requires exactly one primary key";
    }
    return result;
}

template <typename T>
concept Entity = Mapped<T> && (primary_key_count<T>() == 1);

template <typename T>
struct single_relation_traits {
    static constexpr bool value = false;
};

template <typename Target>
struct single_relation_traits<std::shared_ptr<Target>> {
    static constexpr bool value = true;
    using target_type = Target;
};

template <typename T>
inline constexpr bool is_single_relation_v =
    single_relation_traits<std::remove_cvref_t<T>>::value;

template <typename T>
using single_target_t =
    typename single_relation_traits<std::remove_cvref_t<T>>::target_type;

template <typename T>
struct many_collection_traits {
    static constexpr bool value = false;
};

template <typename Target>
struct many_collection_traits<metal::collection<Target>> {
    static constexpr bool value = true;
    using target_type = Target;
};

template <typename T>
inline constexpr bool is_many_collection_v =
    many_collection_traits<std::remove_cvref_t<T>>::value;

template <typename T>
using many_target_t =
    typename many_collection_traits<std::remove_cvref_t<T>>::target_type;

template <info Left, info Right>
consteval bool key_types_compatible() {
    static_assert(std::meta::is_nonstatic_data_member(Left));
    static_assert(std::meta::is_nonstatic_data_member(Right));
    using L = std::remove_cv_t<optional_value_t<member_type_t<Left>>>;
    using R = std::remove_cv_t<optional_value_t<member_type_t<Right>>>;
    return std::same_as<L, R>;
}

template <Entity T>
consteval info key_or_primary(info candidate) {
    return candidate == info{} ? primary_key_member<T>() : candidate;
}

template <Mapped Root, info Member>
consteval void validate_relation() {
    static_assert(std::same_as<owner_type_t<Member>, Root>,
                  "MetalORM: relation member must belong to its mapped root type");
    static_assert(!has<mapping::ignore_t>(Member),
                  "MetalORM: relation cannot also be ignored");
    static_assert(!has<mapping::column>(Member),
                  "MetalORM: relation cannot also be a scalar column");
    static_assert(!has<mapping::primary_key_t>(Member),
                  "MetalORM: relation cannot be a primary key");
    static_assert(!has<mapping::generated_t>(Member),
                  "MetalORM: relation cannot be generated");

    using A = relation_annotation_t<Member>;
    using Traits = mapping::relation_annotation_traits<A>;
    using M = member_type_t<Member>;

    if constexpr (Traits::kind == mapping::relation_kind::belongs_to) {
        static_assert(is_single_relation_v<M>,
                      "MetalORM: belongs_to member must be std::shared_ptr<T>");
        using Target = single_target_t<M>;
        static_assert(Entity<Target>,
                      "MetalORM: belongs_to target must be a mapped entity with one primary key");

        constexpr auto foreign_key = Traits::foreign_key();
        constexpr auto target_key = key_or_primary<Target>(Traits::target_key());
        static_assert(std::meta::is_nonstatic_data_member(foreign_key),
                      "MetalORM: belongs_to foreign key must reflect a data member");
        static_assert(std::same_as<owner_type_t<foreign_key>, Root>,
                      "MetalORM: belongs_to foreign key must belong to the root entity");
        static_assert(std::same_as<owner_type_t<target_key>, Target>,
                      "MetalORM: belongs_to target key must belong to the target entity");
        static_assert(is_persistent_member<foreign_key>() && is_persistent_member<target_key>(),
                      "MetalORM: belongs_to keys must be persistent scalar columns");
        static_assert(key_types_compatible<foreign_key, target_key>(),
                      "MetalORM: belongs_to foreign-key and target-key types are incompatible");
    } else if constexpr (Traits::kind == mapping::relation_kind::has_one) {
        static_assert(is_single_relation_v<M>,
                      "MetalORM: has_one member must be std::shared_ptr<T>");
        using Target = single_target_t<M>;
        static_assert(Entity<Target>,
                      "MetalORM: has_one target must be a mapped entity with one primary key");

        constexpr auto target_fk = Traits::target_foreign_key();
        constexpr auto local_key = key_or_primary<Root>(Traits::local_key());
        static_assert(std::same_as<owner_type_t<target_fk>, Target>,
                      "MetalORM: has_one foreign key must belong to the target entity");
        static_assert(std::same_as<owner_type_t<local_key>, Root>,
                      "MetalORM: has_one local key must belong to the root entity");
        static_assert(is_persistent_member<target_fk>() && is_persistent_member<local_key>(),
                      "MetalORM: has_one keys must be persistent scalar columns");
        static_assert(key_types_compatible<target_fk, local_key>(),
                      "MetalORM: has_one key types are incompatible");
    } else if constexpr (Traits::kind == mapping::relation_kind::has_many) {
        static_assert(is_many_collection_v<M>,
                      "MetalORM: has_many member must be metal::collection<T>");
        using Target = many_target_t<M>;
        static_assert(Entity<Target>,
                      "MetalORM: has_many target must be a mapped entity with one primary key");

        constexpr auto target_fk = Traits::target_foreign_key();
        constexpr auto local_key = key_or_primary<Root>(Traits::local_key());
        static_assert(std::same_as<owner_type_t<target_fk>, Target>,
                      "MetalORM: has_many foreign key must belong to the target entity");
        static_assert(std::same_as<owner_type_t<local_key>, Root>,
                      "MetalORM: has_many local key must belong to the root entity");
        static_assert(is_persistent_member<target_fk>() && is_persistent_member<local_key>(),
                      "MetalORM: has_many keys must be persistent scalar columns");
        static_assert(key_types_compatible<target_fk, local_key>(),
                      "MetalORM: has_many key types are incompatible");
    } else if constexpr (Traits::kind == mapping::relation_kind::many_to_many) {
        static_assert(is_many_collection_v<M>,
                      "MetalORM: many_to_many member must be metal::collection<T>");
        static_assert(!mapping::cascades_remove(Traits::cascade),
                      "MetalORM: many_to_many does not allow cascade remove of shared targets");
        using Target = many_target_t<M>;
        static_assert(Entity<Target>,
                      "MetalORM: many_to_many target must be a mapped entity with one primary key");

        constexpr auto pivot_reflection = Traits::pivot();
        static_assert(std::meta::is_type(pivot_reflection),
                      "MetalORM: many_to_many pivot must reflect a mapped type");
        using Pivot = [: pivot_reflection :];
        static_assert(Mapped<Pivot>,
                      "MetalORM: many_to_many pivot must have a [[=table{...}]] annotation");

        constexpr auto pivot_root_fk = Traits::pivot_root_foreign_key();
        constexpr auto pivot_target_fk = Traits::pivot_target_foreign_key();
        constexpr auto local_key = key_or_primary<Root>(Traits::local_key());
        constexpr auto target_key = key_or_primary<Target>(Traits::target_key());

        static_assert(std::same_as<owner_type_t<pivot_root_fk>, Pivot>,
                      "MetalORM: pivot root FK must belong to the pivot type");
        static_assert(std::same_as<owner_type_t<pivot_target_fk>, Pivot>,
                      "MetalORM: pivot target FK must belong to the pivot type");
        static_assert(std::same_as<owner_type_t<local_key>, Root>,
                      "MetalORM: many_to_many local key must belong to the root entity");
        static_assert(std::same_as<owner_type_t<target_key>, Target>,
                      "MetalORM: many_to_many target key must belong to the target entity");
        static_assert(is_persistent_member<pivot_root_fk>() &&
                      is_persistent_member<pivot_target_fk>() &&
                      is_persistent_member<local_key>() &&
                      is_persistent_member<target_key>(),
                      "MetalORM: many_to_many keys must be persistent scalar columns");
        static_assert(key_types_compatible<pivot_root_fk, local_key>(),
                      "MetalORM: pivot root FK and local-key types are incompatible");
        static_assert(key_types_compatible<pivot_target_fk, target_key>(),
                      "MetalORM: pivot target FK and target-key types are incompatible");
    }
}

template <Mapped T>
consteval bool validate_mapping() {
    static_assert(std::meta::annotations_of_with_type(^^T, ^^mapping::table).size() == 1,
                  "MetalORM: mapped type must have exactly one table annotation");

    template for (constexpr auto member : data_members<T>()) {
        using M = member_type_t<member>;
        if constexpr (has_relation_annotation<member>()) {
            validate_relation<T, member>();
        } else if constexpr (has<mapping::ignore_t>(member)) {
            static_assert(!has<mapping::column>(member) &&
                          !has<mapping::primary_key_t>(member) &&
                          !has<mapping::generated_t>(member),
                          "MetalORM: ignored member cannot also carry column/PK/generated metadata");
        } else if constexpr (PersistableValue<M>) {
            if constexpr (has<mapping::generated_t>(member)) {
                static_assert(has<mapping::primary_key_t>(member),
                              "MetalORM: generated currently requires primary_key");
                static_assert(std::is_integral_v<optional_value_t<M>>,
                              "MetalORM: generated SQLite key must be integral");
                static_assert(primary_key_count<T>() == 1,
                              "MetalORM: generated is invalid on a composite primary key");
            }
        } else {
            static_assert(PersistableValue<M>,
                          "MetalORM: unsupported member must be annotated as a relation or [[=ignore]]");
        }
    }

    template for (constexpr auto left : data_members<T>()) {
        if constexpr (is_persistent_member<left>()) {
            template for (constexpr auto right : data_members<T>()) {
                if constexpr (left != right && is_persistent_member<right>()) {
                    static_assert(column_name_view<left>() != column_name_view<right>(),
                                  "MetalORM: duplicate mapped column name");
                }
            }
        }
    }

    return true;
}

template <Mapped T>
inline constexpr auto table_mapping = annotation<mapping::table>(^^T);

template <Mapped T>
std::string table_name() {
    static_assert(validate_mapping<T>());
    return std::string(table_mapping<T>.name.view());
}

template <Entity T>
Value primary_key_value(const T& entity) {
    static_assert(validate_mapping<T>());
    constexpr auto pk = primary_key_member<T>();
    return to_value(entity.[:pk:]);
}

template <Entity T>
void set_primary_key_value(T& entity, const Value& value) {
    static_assert(validate_mapping<T>());
    constexpr auto pk = primary_key_member<T>();
    using Key = member_type_t<pk>;
    entity.[:pk:] = from_value<Key>(value);
}

template <Entity T>
std::string primary_key_name() {
    static_assert(validate_mapping<T>());
    constexpr auto pk = primary_key_member<T>();
    return column_name<pk>();
}

template <Entity T>
consteval bool primary_key_is_generated() {
    static_assert(validate_mapping<T>());
    constexpr auto pk = primary_key_member<T>();
    return has<mapping::generated_t>(pk);
}

template <Mapped T>
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
