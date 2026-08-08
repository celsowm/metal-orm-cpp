#pragma once

#include "metal/dml.hpp"
#include "metal/execution.hpp"
#include "metal/runtime_types.hpp"
#include "metal/unit_of_work.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace metal {

class RelationChangeProcessor {
public:
    RelationChangeProcessor(UnitOfWork& unit_of_work, DbExecutor& executor, const Dialect& dialect)
        : unit_of_work_(unit_of_work), executor_(executor), dialect_(dialect) {}

    void prepare() {
        std::unordered_set<void*> prepared;
        while (prepared.size() < unit_of_work_.keys().size()) {
            const auto keys = unit_of_work_.keys();
            bool progressed = false;
            for (void* key : keys) {
                if (prepared.contains(key)) continue;
                auto* tracked = unit_of_work_.find(key);
                if (!tracked) continue;
                prepared.insert(key);
                progressed = true;
                if (tracked->prepare_relations) tracked->prepare_relations();
            }
            if (!progressed) break;
        }
    }

    void process() {
        for (void* key : unit_of_work_.keys()) {
            auto* tracked = unit_of_work_.find(key);
            if (tracked && tracked->flush_relations) tracked->flush_relations();
        }
    }

    void accept() {
        for (void* key : unit_of_work_.keys()) {
            auto* tracked = unit_of_work_.find(key);
            if (tracked && tracked->accept_relations) tracked->accept_relations();
        }
    }

    template <reflect::Entity Root, typename PersistFn>
    void prepare_entity(Root& root, PersistFn&& persist) {
        template for (constexpr auto relation : reflect::data_members<Root>()) {
            if constexpr (reflect::has_relation_annotation<relation>()) {
                using A = reflect::relation_annotation_t<relation>;
                using Traits = mapping::relation_annotation_traits<A>;
                if constexpr (Traits::kind == mapping::relation_kind::has_many ||
                              Traits::kind == mapping::relation_kind::many_to_many) {
                    auto& values = root.[:relation:];
                    if constexpr (mapping::cascades_persist(Traits::cascade)) {
                        for (const auto& target : values._metal_added()) {
                            if (target && !unit_of_work_.contains(target.get())) persist(target);
                        }
                    }
                }
            }
        }
    }

    template <reflect::Entity Root, typename RemoveFn>
    void process_entity(Root& root, RemoveFn&& remove) {
        template for (constexpr auto relation : reflect::data_members<Root>()) {
            if constexpr (reflect::has_relation_annotation<relation>()) {
                using A = reflect::relation_annotation_t<relation>;
                using Traits = mapping::relation_annotation_traits<A>;
                if constexpr (Traits::kind == mapping::relation_kind::has_many) {
                    flush_has_many<Root, relation>(root, remove);
                } else if constexpr (Traits::kind == mapping::relation_kind::many_to_many) {
                    flush_many_to_many<Root, relation>(root, remove);
                }
            }
        }
    }

    template <reflect::Entity Root>
    static void accept_entity(Root& root) {
        template for (constexpr auto relation : reflect::data_members<Root>()) {
            if constexpr (reflect::has_relation_annotation<relation>()) {
                using A = reflect::relation_annotation_t<relation>;
                using Traits = mapping::relation_annotation_traits<A>;
                if constexpr (Traits::kind == mapping::relation_kind::has_many ||
                              Traits::kind == mapping::relation_kind::many_to_many) {
                    root.[:relation:]._metal_accept_changes();
                }
            }
        }
    }

    void reset() {}

private:
    template <reflect::Entity Owner, std::meta::info Key>
    static bool missing_relation_key(const Value& value) {
        if (std::holds_alternative<std::nullptr_t>(value)) return true;
        constexpr auto pk = reflect::primary_key_member<Owner>();
        if constexpr (Key == pk && reflect::primary_key_is_generated<Owner>()) {
            return is_empty_generated_value(value);
        }
        return false;
    }

    template <typename Patch>
    static std::vector<DmlAssignment> pivot_payload(
        const Patch& patch,
        std::string_view root_fk,
        std::string_view target_fk) {
        std::vector<DmlAssignment> assignments;
        assignments.reserve(patch.entries().size());
        for (const auto& value : patch.entries()) {
            if (value.column == root_fk || value.column == target_fk) continue;
            assignments.push_back({value.column, value.value});
        }
        return assignments;
    }

    template <reflect::Entity Root, std::meta::info Relation, typename RemoveFn>
    void flush_has_many(Root& root, RemoveFn&& remove) {
        using A = reflect::relation_annotation_t<Relation>;
        using Traits = mapping::relation_annotation_traits<A>;
        using Target = reflect::has_many_target_t<reflect::member_type_t<Relation>>;
        constexpr auto target_fk = Traits::target_foreign_key();
        constexpr auto local_key = reflect::key_or_primary<Root>(Traits::local_key());
        constexpr auto target_pk = reflect::primary_key_member<Target>();
        using ForeignKey = reflect::member_type_t<target_fk>;

        auto& values = root.[:Relation:];
        const Value root_key = to_value(root.[:local_key:]);
        if (missing_relation_key<Root, local_key>(root_key) &&
            (!values._metal_added().empty() || !values._metal_removed().empty())) {
            throw std::runtime_error("MetalORM: cannot flush has_many for a root without a persisted key");
        }

        for (const auto& target : values._metal_added()) {
            const Value target_key = to_value((*target).[:target_pk:]);
            if (missing_relation_key<Target, target_pk>(target_key)) {
                throw std::runtime_error(
                    "MetalORM: attached has_many target is not persisted; enable cascade persist or persist it explicitly");
            }
            (*target).[:target_fk:] = from_value<ForeignKey>(root_key);
            const auto compiled = UpdateQueryBuilder{reflect::table_name<Target>()}
                .set({DmlAssignment{reflect::column_name<target_fk>(), root_key}})
                .where_eq(reflect::column_name<target_pk>(), target_key)
                .compile(dialect_);
            executor_.execute(compiled.sql, compiled.params);
            unit_of_work_.refresh_snapshot(target.get());
        }

        for (const auto& target : values._metal_removed()) {
            if constexpr (mapping::cascades_remove(Traits::cascade)) {
                remove(target);
            } else if constexpr (is_optional_v<ForeignKey>) {
                const Value target_key = to_value((*target).[:target_pk:]);
                (*target).[:target_fk:] = std::nullopt;
                const auto compiled = UpdateQueryBuilder{reflect::table_name<Target>()}
                    .set({DmlAssignment{reflect::column_name<target_fk>(), Value{nullptr}}})
                    .where_eq(reflect::column_name<target_pk>(), target_key)
                    .compile(dialect_);
                executor_.execute(compiled.sql, compiled.params);
                unit_of_work_.refresh_snapshot(target.get());
            } else {
                throw std::runtime_error(
                    "MetalORM: detaching from a non-nullable has_many requires cascade remove");
            }
        }
    }

    template <reflect::Entity Root, std::meta::info Relation, typename RemoveFn>
    void flush_many_to_many(Root& root, RemoveFn&& remove) {
        using A = reflect::relation_annotation_t<Relation>;
        using Traits = mapping::relation_annotation_traits<A>;
        using Collection = reflect::member_type_t<Relation>;
        using Target = reflect::many_to_many_target_t<Collection>;
        using Pivot = reflect::many_to_many_pivot_t<Collection>;
        constexpr auto pivot_root_fk = Traits::pivot_root_foreign_key();
        constexpr auto pivot_target_fk = Traits::pivot_target_foreign_key();
        constexpr auto local_key = reflect::key_or_primary<Root>(Traits::local_key());
        constexpr auto target_key_member = reflect::key_or_primary<Target>(Traits::target_key());

        const auto pivot_root_column = reflect::column_name<pivot_root_fk>();
        const auto pivot_target_column = reflect::column_name<pivot_target_fk>();

        auto& values = root.[:Relation:];
        const Value root_key = to_value(root.[:local_key:]);
        if (missing_relation_key<Root, local_key>(root_key) &&
            (!values._metal_added().empty() || !values._metal_removed().empty())) {
            throw std::runtime_error("MetalORM: cannot flush many_to_many for a root without a persisted key");
        }

        for (const auto& target : values._metal_added()) {
            const Value target_key = to_value((*target).[:target_key_member:]);
            if (missing_relation_key<Target, target_key_member>(target_key)) {
                throw std::runtime_error(
                    "MetalORM: attached many_to_many target does not have a relation target key");
            }

            std::vector<DmlAssignment> assignments{
                {pivot_root_column, root_key},
                {pivot_target_column, target_key}
            };
            if (const auto* patch = values._metal_pivot_patch(target)) {
                auto extra = pivot_payload(*patch, pivot_root_column, pivot_target_column);
                assignments.insert(assignments.end(), extra.begin(), extra.end());
            }

            const auto compiled = InsertQueryBuilder{reflect::table_name<Pivot>()}
                .values(std::move(assignments))
                .compile(dialect_);
            executor_.execute(compiled.sql, compiled.params);
        }

        for (const auto& target : values._metal_pivot_updates()) {
            const auto* patch = values._metal_pivot_patch(target);
            if (!patch || patch->empty()) continue;
            auto assignments = pivot_payload(*patch, pivot_root_column, pivot_target_column);
            if (assignments.empty()) continue;

            const Value target_key = to_value((*target).[:target_key_member:]);
            const auto compiled = UpdateQueryBuilder{reflect::table_name<Pivot>()}
                .set(std::move(assignments))
                .where({
                    DmlPredicate{pivot_root_column, CompareOp::Eq, root_key},
                    DmlPredicate{pivot_target_column, CompareOp::Eq, target_key}
                })
                .compile(dialect_);
            executor_.execute(compiled.sql, compiled.params);
        }

        for (const auto& target : values._metal_removed()) {
            const Value target_key = to_value((*target).[:target_key_member:]);
            const auto compiled = DeleteQueryBuilder{reflect::table_name<Pivot>()}
                .where({
                    DmlPredicate{pivot_root_column, CompareOp::Eq, root_key},
                    DmlPredicate{pivot_target_column, CompareOp::Eq, target_key}
                })
                .compile(dialect_);
            executor_.execute(compiled.sql, compiled.params);

            if constexpr (mapping::cascades_remove(Traits::cascade)) {
                if (unit_of_work_.contains(target.get())) {
                    remove(target);
                } else {
                    const auto target_delete = DeleteQueryBuilder{reflect::table_name<Target>()}
                        .where_eq(reflect::column_name<target_key_member>(), target_key)
                        .compile(dialect_);
                    executor_.execute(target_delete.sql, target_delete.params);
                }
            }
        }
    }

    UnitOfWork& unit_of_work_;
    DbExecutor& executor_;
    const Dialect& dialect_;
};

} // namespace metal
