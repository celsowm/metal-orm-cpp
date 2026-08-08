#pragma once

#include "metal/dml.hpp"
#include "metal/execution.hpp"
#include "metal/identity_map.hpp"
#include "metal/query.hpp"
#include "metal/reflection.hpp"
#include "metal/relation_change_processor.hpp"
#include "metal/runtime_types.hpp"
#include "metal/unit_of_work.hpp"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace metal {

class Session;

template <reflect::Entity T>
class EntityQuery {
public:
    explicit EntityQuery(Session& session) : session_(session) {}

    EntityQuery& where(Expression<T> expression) {
        query_.where(std::move(expression));
        return *this;
    }

    template <std::meta::info Member>
    requires std::same_as<typename Field<Member>::owner_type, T>
    EntityQuery& order_by(Field<Member> field, bool ascending = true) {
        query_.order_by(field, ascending);
        return *this;
    }

    EntityQuery& limit(std::size_t n) {
        query_.limit(n);
        return *this;
    }

    template <std::meta::info Relation>
    EntityQuery& include();

    std::vector<std::shared_ptr<T>> all();

    std::shared_ptr<T> first() {
        query_.limit(1);
        auto values = all();
        return values.empty() ? std::shared_ptr<T>{} : values.front();
    }

private:
    Session& session_;
    SelectQuery<T> query_;
    std::vector<std::function<void(std::vector<std::shared_ptr<T>>& roots)>> includes_;
};

class Session {
public:
    explicit Session(
        std::shared_ptr<DbExecutor> executor,
        std::shared_ptr<Dialect> dialect = std::make_shared<SQLiteDialect>())
        : executor_(std::move(executor)),
          dialect_(std::move(dialect)),
          unit_of_work_(*executor_, *dialect_),
          relation_changes_(unit_of_work_) {}

    template <reflect::Entity T>
    EntityQuery<T> query() { return EntityQuery<T>{*this}; }

    template <reflect::Entity T, typename Key>
    std::shared_ptr<T> find(const Key& key) {
        static_assert(reflect::validate_mapping<T>());
        const Value pk = to_value(key);
        if (auto existing = identity_map_.get<T>(pk)) return existing;

        SelectQuery<T> query;
        query.where_column(reflect::primary_key_name<T>(), CompareOp::Eq, pk).limit(1);
        const auto compiled = query.compile(*dialect_);
        const auto result = executor_->execute(compiled.sql, compiled.params);
        if (result.rows.empty()) return {};
        return hydrate<T>(result.rows.front());
    }

    template <reflect::Entity T>
    void persist(const std::shared_ptr<T>& entity) {
        static_assert(reflect::validate_mapping<T>());
        if (!entity) throw std::invalid_argument("MetalORM: cannot persist a null entity");
        if (auto* tracked = unit_of_work_.find(entity.get())) {
            if (tracked->status == EntityStatus::Removed) tracked->status = EntityStatus::Managed;
            return;
        }
        track<T>(entity, EntityStatus::New);
    }

    template <reflect::Entity T>
    void remove(const std::shared_ptr<T>& entity) {
        static_assert(reflect::validate_mapping<T>());
        if (!entity) return;
        if (!unit_of_work_.contains(entity.get())) track<T>(entity, EntityStatus::Managed);
        if (auto* tracked = unit_of_work_.find(entity.get())) tracked->status = EntityStatus::Removed;
    }

    void flush() {
        unit_of_work_.flush();
    }

    void commit() {
        executor_->execute("BEGIN;");
        try {
            relation_changes_.prepare();
            unit_of_work_.flush();
            relation_changes_.process();
            unit_of_work_.flush();
            executor_->execute("COMMIT;");
            relation_changes_.accept();
        } catch (...) {
            try { executor_->execute("ROLLBACK;"); } catch (...) {}
            throw;
        }
    }

    void clear() {
        unit_of_work_.clear();
        identity_map_.clear();
        relation_changes_.reset();
    }

    DbExecutor& executor() noexcept { return *executor_; }
    const Dialect& dialect() const noexcept { return *dialect_; }
    IdentityMap& identity_map() noexcept { return identity_map_; }
    UnitOfWork& unit_of_work() noexcept { return unit_of_work_; }
    RelationChangeProcessor& relation_changes() noexcept { return relation_changes_; }

private:
    template <reflect::Entity T>
    friend class EntityQuery;

    template <reflect::Entity T>
    std::unordered_map<std::string, Value> snapshot_of(const T& entity) const {
        std::unordered_map<std::string, Value> snapshot;
        reflect::for_each_column<T>([&]<std::meta::info Member>() {
            snapshot.emplace(reflect::column_name<Member>(), to_value(entity.[:Member:]));
        });
        return snapshot;
    }

    template <reflect::Entity T>
    void track(const std::shared_ptr<T>& entity, EntityStatus status) {
        TrackedEntity tracked;
        tracked.object = entity;
        tracked.status = status;
        tracked.table = reflect::table_name<T>();
        tracked.primary_key = reflect::primary_key_name<T>();
        tracked.original = status == EntityStatus::New
            ? std::unordered_map<std::string, Value>{}
            : snapshot_of(*entity);
        tracked.snapshot = [this, weak = std::weak_ptr<T>(entity)] {
            if (auto locked = weak.lock()) return snapshot_of(*locked);
            return std::unordered_map<std::string, Value>{};
        };
        tracked.get_pk = [weak = std::weak_ptr<T>(entity)] {
            if (auto locked = weak.lock()) return reflect::primary_key_value(*locked);
            return Value{nullptr};
        };
        tracked.set_pk = [weak = std::weak_ptr<T>(entity)](const Value& value) {
            if (auto locked = weak.lock()) reflect::set_primary_key_value(*locked, value);
        };
        tracked.generated_pk = reflect::primary_key_is_generated<T>();
        tracked.register_identity = [this, weak = std::weak_ptr<T>(entity)] {
            if (auto locked = weak.lock()) {
                const auto pk = reflect::primary_key_value(*locked);
                if (!is_empty_generated_value(pk)) identity_map_.put<T>(pk, locked);
            }
        };
        tracked.erase_identity = [this](const Value& pk) { identity_map_.erase<T>(pk); };
        tracked.prepare_relations = [this, weak = std::weak_ptr<T>(entity)] {
            if (auto locked = weak.lock()) prepare_collections(*locked);
        };
        tracked.flush_relations = [this, weak = std::weak_ptr<T>(entity)] {
            if (auto locked = weak.lock()) flush_collections(*locked);
        };
        tracked.accept_relations = [weak = std::weak_ptr<T>(entity)] {
            if (auto locked = weak.lock()) accept_collections(*locked);
        };
        unit_of_work_.track(entity.get(), std::move(tracked));

        const auto pk = reflect::primary_key_value(*entity);
        if (!is_empty_generated_value(pk)) identity_map_.put<T>(pk, entity);
    }

    template <reflect::Entity T>
    std::shared_ptr<T> hydrate(const Row& row) {
        const auto pk_name = reflect::primary_key_name<T>();
        if (auto pk = row.find(pk_name); pk != row.end()) {
            if (auto existing = identity_map_.get<T>(pk->second)) return existing;
        }

        auto entity = std::make_shared<T>();
        reflect::for_each_column<T>([&]<std::meta::info Member>() {
            auto value = row.find(reflect::column_name<Member>());
            if (value == row.end()) return;
            using M = reflect::member_type_t<Member>;
            (*entity).[:Member:] = from_value<M>(value->second);
        });
        track<T>(entity, EntityStatus::Managed);
        return entity;
    }

    template <reflect::Entity T>
    std::vector<std::shared_ptr<T>> execute_query(const SelectQuery<T>& query) {
        const auto compiled = query.compile(*dialect_);
        const auto result = executor_->execute(compiled.sql, compiled.params);
        std::vector<std::shared_ptr<T>> values;
        values.reserve(result.rows.size());
        for (const auto& row : result.rows) values.push_back(hydrate<T>(row));
        return values;
    }

    template <reflect::Entity Target>
    void append_target_columns(std::string& sql, std::string_view alias) const {
        bool first = true;
        reflect::for_each_column<Target>([&]<std::meta::info Member>() {
            if (!first) sql += ", ";
            first = false;
            sql += std::string(alias) + "." + dialect_->quote_identifier(reflect::column_name<Member>());
        });
    }

    void append_in_placeholders(std::string& sql, std::size_t count) const {
        for (std::size_t i = 0; i < count; ++i) {
            if (i) sql += ", ";
            sql += dialect_->placeholder(i + 1);
        }
    }

    template <reflect::Entity Root, std::meta::info Relation>
    void load_has_many_batch(std::vector<std::shared_ptr<Root>>& roots) {
        using A = reflect::relation_annotation_t<Relation>;
        using Traits = mapping::relation_annotation_traits<A>;
        using Target = reflect::many_target_t<reflect::member_type_t<Relation>>;
        if (roots.empty()) return;

        constexpr auto target_fk = Traits::target_foreign_key();
        constexpr auto local_key = reflect::key_or_primary<Root>(Traits::local_key());
        std::unordered_map<std::string, std::shared_ptr<Root>> roots_by_key;
        std::unordered_map<Root*, std::vector<std::shared_ptr<Target>>> loaded;
        std::vector<Value> keys;

        for (auto& root : roots) {
            const auto key = to_value((*root).[:local_key:]);
            roots_by_key[value_key(key)] = root;
            loaded[root.get()] = {};
            keys.push_back(key);
        }

        std::string sql = "SELECT ";
        append_target_columns<Target>(sql, "t");
        sql += ", t." + dialect_->quote_identifier(reflect::column_name<target_fk>()) + " AS \"__metal_root_key\"";
        sql += " FROM " + dialect_->quote_identifier(reflect::table_name<Target>()) + " t";
        sql += " WHERE t." + dialect_->quote_identifier(reflect::column_name<target_fk>()) + " IN (";
        append_in_placeholders(sql, keys.size());
        sql += ");";

        const auto result = executor_->execute(sql, keys);
        for (const auto& row : result.rows) {
            auto root_key = row.find("__metal_root_key");
            if (root_key == row.end()) continue;
            auto root = roots_by_key.find(value_key(root_key->second));
            if (root == roots_by_key.end()) continue;
            loaded[root->second.get()].push_back(hydrate<Target>(row));
        }

        for (auto& root : roots) {
            (*root).[:Relation:]._metal_hydrate(std::move(loaded[root.get()]));
        }
    }

    template <reflect::Entity Root, std::meta::info Relation>
    void load_has_one_batch(std::vector<std::shared_ptr<Root>>& roots) {
        using A = reflect::relation_annotation_t<Relation>;
        using Traits = mapping::relation_annotation_traits<A>;
        using Target = reflect::single_target_t<reflect::member_type_t<Relation>>;
        if (roots.empty()) return;

        constexpr auto target_fk = Traits::target_foreign_key();
        constexpr auto local_key = reflect::key_or_primary<Root>(Traits::local_key());
        std::unordered_map<std::string, std::shared_ptr<Root>> roots_by_key;
        std::vector<Value> keys;
        for (auto& root : roots) {
            (*root).[:Relation:].reset();
            const auto key = to_value((*root).[:local_key:]);
            roots_by_key[value_key(key)] = root;
            keys.push_back(key);
        }

        std::string sql = "SELECT ";
        append_target_columns<Target>(sql, "t");
        sql += ", t." + dialect_->quote_identifier(reflect::column_name<target_fk>()) + " AS \"__metal_root_key\"";
        sql += " FROM " + dialect_->quote_identifier(reflect::table_name<Target>()) + " t";
        sql += " WHERE t." + dialect_->quote_identifier(reflect::column_name<target_fk>()) + " IN (";
        append_in_placeholders(sql, keys.size());
        sql += ");";

        const auto result = executor_->execute(sql, keys);
        for (const auto& row : result.rows) {
            auto root_key = row.find("__metal_root_key");
            if (root_key == row.end()) continue;
            auto root = roots_by_key.find(value_key(root_key->second));
            if (root != roots_by_key.end()) (*root->second).[:Relation:] = hydrate<Target>(row);
        }
    }

    template <reflect::Entity Root, std::meta::info Relation>
    void load_belongs_to_batch(std::vector<std::shared_ptr<Root>>& roots) {
        using A = reflect::relation_annotation_t<Relation>;
        using Traits = mapping::relation_annotation_traits<A>;
        using Target = reflect::single_target_t<reflect::member_type_t<Relation>>;
        if (roots.empty()) return;

        constexpr auto foreign_key = Traits::foreign_key();
        constexpr auto target_key = reflect::key_or_primary<Target>(Traits::target_key());
        std::vector<Value> keys;
        for (auto& root : roots) {
            (*root).[:Relation:].reset();
            keys.push_back(to_value((*root).[:foreign_key:]));
        }

        std::string sql = "SELECT ";
        append_target_columns<Target>(sql, "t");
        sql += " FROM " + dialect_->quote_identifier(reflect::table_name<Target>()) + " t";
        sql += " WHERE t." + dialect_->quote_identifier(reflect::column_name<target_key>()) + " IN (";
        append_in_placeholders(sql, keys.size());
        sql += ");";

        const auto result = executor_->execute(sql, keys);
        std::unordered_map<std::string, std::shared_ptr<Target>> targets_by_key;
        for (const auto& row : result.rows) {
            auto target = hydrate<Target>(row);
            targets_by_key[value_key(to_value((*target).[:target_key:]))] = target;
        }
        for (auto& root : roots) {
            const auto key = value_key(to_value((*root).[:foreign_key:]));
            if (auto target = targets_by_key.find(key); target != targets_by_key.end()) {
                (*root).[:Relation:] = target->second;
            }
        }
    }

    template <reflect::Entity Root, std::meta::info Relation>
    void load_many_to_many_batch(std::vector<std::shared_ptr<Root>>& roots) {
        using A = reflect::relation_annotation_t<Relation>;
        using Traits = mapping::relation_annotation_traits<A>;
        using Target = reflect::many_target_t<reflect::member_type_t<Relation>>;
        if (roots.empty()) return;

        using Pivot = [: Traits::pivot() :];
        constexpr auto pivot_root_fk = Traits::pivot_root_foreign_key();
        constexpr auto pivot_target_fk = Traits::pivot_target_foreign_key();
        constexpr auto local_key = reflect::key_or_primary<Root>(Traits::local_key());
        constexpr auto target_key = reflect::key_or_primary<Target>(Traits::target_key());

        std::unordered_map<std::string, std::shared_ptr<Root>> roots_by_key;
        std::unordered_map<Root*, std::vector<std::shared_ptr<Target>>> loaded;
        std::vector<Value> keys;
        for (auto& root : roots) {
            const auto key = to_value((*root).[:local_key:]);
            roots_by_key[value_key(key)] = root;
            loaded[root.get()] = {};
            keys.push_back(key);
        }

        std::string sql = "SELECT ";
        append_target_columns<Target>(sql, "t");
        sql += ", p." + dialect_->quote_identifier(reflect::column_name<pivot_root_fk>()) + " AS \"__metal_root_key\"";
        sql += " FROM " + dialect_->quote_identifier(reflect::table_name<Target>()) + " t";
        sql += " JOIN " + dialect_->quote_identifier(reflect::table_name<Pivot>()) + " p ON p." +
               dialect_->quote_identifier(reflect::column_name<pivot_target_fk>()) + " = t." +
               dialect_->quote_identifier(reflect::column_name<target_key>());
        sql += " WHERE p." + dialect_->quote_identifier(reflect::column_name<pivot_root_fk>()) + " IN (";
        append_in_placeholders(sql, keys.size());
        sql += ");";

        const auto result = executor_->execute(sql, keys);
        for (const auto& row : result.rows) {
            auto root_key = row.find("__metal_root_key");
            if (root_key == row.end()) continue;
            auto root = roots_by_key.find(value_key(root_key->second));
            if (root == roots_by_key.end()) continue;
            loaded[root->second.get()].push_back(hydrate<Target>(row));
        }

        for (auto& root : roots) {
            (*root).[:Relation:]._metal_hydrate(std::move(loaded[root.get()]));
        }
    }

    template <reflect::Entity Root>
    void prepare_collections(Root& root) {
        template for (constexpr auto relation : reflect::data_members<Root>()) {
            if constexpr (reflect::has_relation_annotation<relation>()) {
                using A = reflect::relation_annotation_t<relation>;
                using Traits = mapping::relation_annotation_traits<A>;
                if constexpr (Traits::kind == mapping::relation_kind::has_many ||
                              Traits::kind == mapping::relation_kind::many_to_many) {
                    using Target = reflect::many_target_t<reflect::member_type_t<relation>>;
                    auto& values = root.[:relation:];
                    if constexpr (mapping::cascades_persist(Traits::cascade)) {
                        for (const auto& target : values._metal_added()) {
                            if (target && is_empty_generated_value(reflect::primary_key_value(*target)) &&
                                !unit_of_work_.contains(target.get())) {
                                persist<Target>(target);
                            }
                        }
                    }
                }
            }
        }
    }

    template <reflect::Entity Root>
    void flush_collections(Root& root) {
        template for (constexpr auto relation : reflect::data_members<Root>()) {
            if constexpr (reflect::has_relation_annotation<relation>()) {
                using A = reflect::relation_annotation_t<relation>;
                using Traits = mapping::relation_annotation_traits<A>;
                if constexpr (Traits::kind == mapping::relation_kind::has_many) {
                    flush_has_many_collection<Root, relation>(root);
                } else if constexpr (Traits::kind == mapping::relation_kind::many_to_many) {
                    flush_many_to_many_collection<Root, relation>(root);
                }
            }
        }
    }

    template <reflect::Entity Root>
    static void accept_collections(Root& root) {
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

    template <reflect::Entity Root, std::meta::info Relation>
    void flush_has_many_collection(Root& root) {
        using A = reflect::relation_annotation_t<Relation>;
        using Traits = mapping::relation_annotation_traits<A>;
        using Target = reflect::many_target_t<reflect::member_type_t<Relation>>;
        constexpr auto target_fk = Traits::target_foreign_key();
        constexpr auto local_key = reflect::key_or_primary<Root>(Traits::local_key());
        constexpr auto target_pk = reflect::primary_key_member<Target>();
        using ForeignKey = reflect::member_type_t<target_fk>;

        auto& values = root.[:Relation:];
        const Value root_key = to_value(root.[:local_key:]);
        if (is_empty_generated_value(root_key) && (!values._metal_added().empty() || !values._metal_removed().empty())) {
            throw std::runtime_error("MetalORM: cannot flush has_many for a root without a persisted key");
        }

        for (const auto& target : values._metal_added()) {
            const Value target_key = to_value((*target).[:target_pk:]);
            if (is_empty_generated_value(target_key)) {
                throw std::runtime_error("MetalORM: attached has_many target is not persisted; enable cascade persist or persist it explicitly");
            }
            (*target).[:target_fk:] = from_value<ForeignKey>(root_key);
            const auto compiled = UpdateQueryBuilder{reflect::table_name<Target>()}
                .set({DmlAssignment{reflect::column_name<target_fk>(), root_key}})
                .where_eq(reflect::column_name<target_pk>(), target_key)
                .compile(*dialect_);
            executor_->execute(compiled.sql, compiled.params);
            unit_of_work_.refresh_snapshot(target.get());
        }

        for (const auto& target : values._metal_removed()) {
            if constexpr (mapping::cascades_remove(Traits::cascade)) {
                remove<Target>(target);
            } else if constexpr (is_optional_v<ForeignKey>) {
                const Value target_key = to_value((*target).[:target_pk:]);
                (*target).[:target_fk:] = std::nullopt;
                const auto compiled = UpdateQueryBuilder{reflect::table_name<Target>()}
                    .set({DmlAssignment{reflect::column_name<target_fk>(), Value{nullptr}}})
                    .where_eq(reflect::column_name<target_pk>(), target_key)
                    .compile(*dialect_);
                executor_->execute(compiled.sql, compiled.params);
                unit_of_work_.refresh_snapshot(target.get());
            } else {
                throw std::runtime_error(
                    "MetalORM: detaching from a non-nullable has_many requires cascade remove");
            }
        }
    }

    template <reflect::Entity Root, std::meta::info Relation>
    void flush_many_to_many_collection(Root& root) {
        using A = reflect::relation_annotation_t<Relation>;
        using Traits = mapping::relation_annotation_traits<A>;
        using Target = reflect::many_target_t<reflect::member_type_t<Relation>>;
        using Pivot = [: Traits::pivot() :];
        constexpr auto pivot_root_fk = Traits::pivot_root_foreign_key();
        constexpr auto pivot_target_fk = Traits::pivot_target_foreign_key();
        constexpr auto local_key = reflect::key_or_primary<Root>(Traits::local_key());
        constexpr auto target_key_member = reflect::key_or_primary<Target>(Traits::target_key());

        auto& values = root.[:Relation:];
        const Value root_key = to_value(root.[:local_key:]);
        if (is_empty_generated_value(root_key) && (!values._metal_added().empty() || !values._metal_removed().empty())) {
            throw std::runtime_error("MetalORM: cannot flush many_to_many for a root without a persisted key");
        }

        for (const auto& target : values._metal_added()) {
            const Value target_key = to_value((*target).[:target_key_member:]);
            if (is_empty_generated_value(target_key)) {
                throw std::runtime_error("MetalORM: attached many_to_many target is not persisted; enable cascade persist or persist it explicitly");
            }
            const auto compiled = InsertQueryBuilder{reflect::table_name<Pivot>()}
                .values({
                    DmlAssignment{reflect::column_name<pivot_root_fk>(), root_key},
                    DmlAssignment{reflect::column_name<pivot_target_fk>(), target_key}
                })
                .on_conflict_do_nothing()
                .compile(*dialect_);
            executor_->execute(compiled.sql, compiled.params);
        }

        for (const auto& target : values._metal_removed()) {
            const Value target_key = to_value((*target).[:target_key_member:]);
            const auto compiled = DeleteQueryBuilder{reflect::table_name<Pivot>()}
                .where({
                    DmlPredicate{reflect::column_name<pivot_root_fk>(), CompareOp::Eq, root_key},
                    DmlPredicate{reflect::column_name<pivot_target_fk>(), CompareOp::Eq, target_key}
                })
                .compile(*dialect_);
            executor_->execute(compiled.sql, compiled.params);

            if constexpr (mapping::cascades_remove(Traits::cascade)) {
                remove<Target>(target);
            }
        }
    }

    std::shared_ptr<DbExecutor> executor_;
    std::shared_ptr<Dialect> dialect_;
    IdentityMap identity_map_;
    UnitOfWork unit_of_work_;
    RelationChangeProcessor relation_changes_;
};

template <reflect::Entity T>
std::vector<std::shared_ptr<T>> EntityQuery<T>::all() {
    static_assert(reflect::validate_mapping<T>());
    auto roots = session_.execute_query(query_);
    for (auto& include : includes_) include(roots);
    return roots;
}

template <reflect::Entity T>
template <std::meta::info Relation>
EntityQuery<T>& EntityQuery<T>::include() {
    static_assert(std::same_as<reflect::owner_type_t<Relation>, T>,
                  "MetalORM: included relation must belong to the queried entity");
    static_assert(reflect::has_relation_annotation<Relation>(),
                  "MetalORM: include<> requires a reflected relationship annotation");
    static_assert(reflect::validate_mapping<T>());

    using A = reflect::relation_annotation_t<Relation>;
    using Traits = mapping::relation_annotation_traits<A>;
    auto* session = &session_;

    if constexpr (Traits::kind == mapping::relation_kind::belongs_to) {
        includes_.push_back([session](auto& roots) {
            session->template load_belongs_to_batch<T, Relation>(roots);
        });
    } else if constexpr (Traits::kind == mapping::relation_kind::has_one) {
        includes_.push_back([session](auto& roots) {
            session->template load_has_one_batch<T, Relation>(roots);
        });
    } else if constexpr (Traits::kind == mapping::relation_kind::has_many) {
        includes_.push_back([session](auto& roots) {
            session->template load_has_many_batch<T, Relation>(roots);
        });
    } else if constexpr (Traits::kind == mapping::relation_kind::many_to_many) {
        includes_.push_back([session](auto& roots) {
            session->template load_many_to_many_batch<T, Relation>(roots);
        });
    }
    return *this;
}

} // namespace metal
