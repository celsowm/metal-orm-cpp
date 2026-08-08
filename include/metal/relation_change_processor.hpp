#pragma once

#include "metal/dml.hpp"
#include "metal/execution.hpp"
#include "metal/runtime_types.hpp"
#include "metal/unit_of_work.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
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
    template <reflect::Mapped Pivot, std::meta::info RootFk, std::meta::info TargetFk>
    static std::vector<DmlAssignment> pivot_payload(const Pivot& pivot) {
        std::vector<DmlAssignment> assignments;
        reflect::for_each_column<Pivot>([&]<std::meta::info Member>() {
            if constexpr (Member != RootFk && Member != TargetFk) {
                assignments.push_back({
                    reflect::column_name<Member>(),
                    to_value(pivot.[:Member:])
                });
            }
        });
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
        if (is_empty_generated_value(root_key) &&
            (!values._metal_added().empty() || !values._metal_removed().empty())) {
            throw std::runtime_error("MetalORM: cannot flush has_many for a root without a persisted key");
        }

        for (const auto& target : values._metal_added()) {
            const Value target_key = to_value((*target).[:target_pk:]);
            if (is_empty_generated_value(target_key)) {
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

        auto& values = root.[:Relation:];
        const Value root_key = to_value(root.[:local_key:]);
        if (is_empty_generated_value(root_key) &&
            (!values._metal_added().empty() || !values._metal_removed().empty())) {
            throw std::runtime_error("MetalORM: cannot flush many_to_many for a root without a persisted key");
        }

        for (const auto& target : values._metal_added()) {
            const Value target_key = to_value((*target).[:target_key_member:]);
            if (is_empty_generated_value(target_key)) {
                throw std::runtime_error(
                    "MetalORM: attached many_to_many target is not persisted; enable cascade persist or persist it explicitly");
            }

            std::vector<DmlAssignment> assignments{
                {reflect::column_name<pivot_root_fk>(), root_key},
                {reflect::column_name<pivot_target_fk>(), target_key}
            };
            if (const auto* pivot = values._metal_pivot(target)) {
                auto extra = pivot_payload<Pivot, pivot_root_fk, pivot_target_fk>(*pivot);
                assignments.insert(assignments.end(), extra.begin(), extra.end());
            }

            const auto compiled = InsertQueryBuilder{reflect::table_name<Pivot>()}
                .values(std::move(assignments))
                .on_conflict_do_nothing()
                .compile(dialect_);
            executor_.execute(compiled.sql, compiled.params);
        }

        for (const auto& target : values._metal_pivot_updates()) {
            const auto* pivot = values._metal_pivot(target);
            if (!pivot) continue;
            auto assignments = pivot_payload<Pivot, pivot_root_fk, pivot_target_fk>(*pivot);
            if (assignments.empty()) continue;

            const Value target_key = to_value((*target).[:target_key_member:]);
            const auto compiled = UpdateQueryBuilder{reflect::table_name<Pivot>()}
                .set(std::move(assignments))
                .where({
                    DmlPredicate{reflect::column_name<pivot_root_fk>(), CompareOp::Eq, root_key},
                    DmlPredicate{reflect::column_name<pivot_target_fk>(), CompareOp::Eq, target_key}
                })
                .compile(dialect_);
            executor_.execute(compiled.sql, compiled.params);
        }

        for (const auto& target : values._metal_removed()) {
            const Value target_key = to_value((*target).[:target_key_member:]);
            const auto compiled = DeleteQueryBuilder{reflect::table_name<Pivot>()}
                .where({
                    DmlPredicate{reflect::column_name<pivot_root_fk>(), CompareOp::Eq, root_key},
                    DmlPredicate{reflect::column_name<pivot_target_fk>(), CompareOp::Eq, target_key}
                })
                .compile(dialect_);
            executor_.execute(compiled.sql, compiled.params);

            if constexpr (mapping::cascades_remove(Traits::cascade)) remove(target);
        }
    }

    UnitOfWork& unit_of_work_;
    DbExecutor& executor_;
    const Dialect& dialect_;
};

} // namespace metal
