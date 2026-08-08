#pragma once

#include "metal/execution.hpp"
#include "metal/query.hpp"
#include "metal/reflection.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace metal {

class IdentityMap {
public:
    template <reflect::Entity T>
    std::shared_ptr<T> get(const Value& pk) const {
        auto bucket = buckets_.find(std::type_index(typeid(T)));
        if (bucket == buckets_.end()) return {};
        auto item = bucket->second.find(value_key(pk));
        if (item == bucket->second.end()) return {};
        return std::static_pointer_cast<T>(item->second.lock());
    }

    template <reflect::Entity T>
    void put(const Value& pk, const std::shared_ptr<T>& entity) {
        buckets_[std::type_index(typeid(T))][value_key(pk)] = entity;
    }

    template <reflect::Entity T>
    void erase(const Value& pk) {
        auto bucket = buckets_.find(std::type_index(typeid(T)));
        if (bucket != buckets_.end()) bucket->second.erase(value_key(pk));
    }

    void clear() { buckets_.clear(); }

private:
    std::unordered_map<std::type_index, std::unordered_map<std::string, std::weak_ptr<void>>> buckets_;
};

enum class EntityStatus { New, Managed, Removed };

struct TrackedEntity {
    std::shared_ptr<void> object;
    EntityStatus status{EntityStatus::Managed};
    std::string table;
    std::string primary_key;
    std::unordered_map<std::string, Value> original;
    std::function<std::unordered_map<std::string, Value>()> snapshot;
    std::function<Value()> get_pk;
    std::function<void(const Value&)> set_pk;
    bool generated_pk{false};
    std::function<void()> register_identity;
    std::function<void(const Value&)> erase_identity;
};

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
    EntityQuery& order_by(Field<Member> f, bool ascending = true) {
        query_.order_by(f, ascending);
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
        : executor_(std::move(executor)), dialect_(std::move(dialect)) {}

    template <reflect::Entity T>
    EntityQuery<T> query() { return EntityQuery<T>{*this}; }

    template <reflect::Entity T, typename Key>
    std::shared_ptr<T> find(const Key& key) {
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
        if (!entity) throw std::invalid_argument("MetalORM: cannot persist a null entity");
        if (auto it = tracked_.find(entity.get()); it != tracked_.end()) {
            if (it->second.status == EntityStatus::Removed) it->second.status = EntityStatus::Managed;
            return;
        }
        track<T>(entity, EntityStatus::New);
    }

    template <reflect::Entity T>
    void remove(const std::shared_ptr<T>& entity) {
        if (!entity) return;
        if (!tracked_.contains(entity.get())) track<T>(entity, EntityStatus::Managed);
        tracked_.at(entity.get()).status = EntityStatus::Removed;
    }

    void commit() {
        executor_->execute("BEGIN;");
        try {
            std::vector<void*> keys;
            keys.reserve(tracked_.size());
            for (auto& [key, _] : tracked_) keys.push_back(key);

            for (void* key : keys) {
                auto it = tracked_.find(key);
                if (it == tracked_.end()) continue;
                auto& tracked = it->second;
                switch (tracked.status) {
                    case EntityStatus::New: flush_insert(tracked); break;
                    case EntityStatus::Managed: flush_update_if_dirty(tracked); break;
                    case EntityStatus::Removed: flush_delete(tracked); break;
                }
            }
            executor_->execute("COMMIT;");
        } catch (...) {
            try { executor_->execute("ROLLBACK;"); } catch (...) {}
            throw;
        }
    }

    void clear() {
        tracked_.clear();
        identity_map_.clear();
    }

    DbExecutor& executor() noexcept { return *executor_; }
    const Dialect& dialect() const noexcept { return *dialect_; }

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
        tracked_[entity.get()] = std::move(tracked);

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

    template <reflect::Entity Root, std::meta::info Relation>
    void load_many_to_many_batch(std::vector<std::shared_ptr<Root>>& roots) {
        static_assert(std::same_as<reflect::owner_type_t<Relation>, Root>);
        static_assert(reflect::has<mapping::many_to_many>(Relation),
                      "MetalORM: include<> requires a [[=metal::mapping::many_to_many{...}]] member");

        using Collection = reflect::member_type_t<Relation>;
        static_assert(reflect::is_many_collection_v<Collection>,
                      "MetalORM 0.0.1: many-to-many member must be std::vector<std::shared_ptr<T>>");
        using Target = reflect::many_target_t<Collection>;
        static_assert(reflect::Entity<Target>, "MetalORM: many-to-many target must be an entity");

        if (roots.empty()) return;
        constexpr auto relation = reflect::annotation<mapping::many_to_many>(Relation);

        std::unordered_map<std::string, std::shared_ptr<Root>> roots_by_id;
        std::vector<Value> root_ids;
        for (auto& root : roots) {
            (*root).[:Relation:].clear();
            const auto id = reflect::value_for_column(*root, relation.root_key.view());
            roots_by_id[value_key(id)] = root;
            root_ids.push_back(id);
        }

        std::string sql = "SELECT ";
        bool first = true;
        reflect::for_each_column<Target>([&]<std::meta::info Member>() {
            if (!first) sql += ", ";
            first = false;
            sql += "t." + dialect_->quote_identifier(reflect::column_name<Member>());
        });
        sql += ", p." + dialect_->quote_identifier(relation.pivot_root_fk.view()) + " AS \"__metal_root_id\"";
        sql += " FROM " + dialect_->quote_identifier(reflect::table_name<Target>()) + " t";
        sql += " JOIN " + dialect_->quote_identifier(relation.pivot_table.view()) + " p ON p." +
               dialect_->quote_identifier(relation.pivot_target_fk.view()) + " = t." +
               dialect_->quote_identifier(relation.target_key.view());
        sql += " WHERE p." + dialect_->quote_identifier(relation.pivot_root_fk.view()) + " IN (";
        for (std::size_t i = 0; i < root_ids.size(); ++i) {
            if (i) sql += ", ";
            sql += dialect_->placeholder(i + 1);
        }
        sql += ");";

        const auto result = executor_->execute(sql, root_ids);
        for (const auto& row : result.rows) {
            auto root_id = row.find("__metal_root_id");
            if (root_id == row.end()) continue;
            auto root = roots_by_id.find(value_key(root_id->second));
            if (root == roots_by_id.end()) continue;

            auto target = hydrate<Target>(row);
            auto& collection = (*root->second).[:Relation:];
            if (std::find(collection.begin(), collection.end(), target) == collection.end()) {
                collection.push_back(std::move(target));
            }
        }
    }

    void flush_insert(TrackedEntity& tracked) {
        const auto current = tracked.snapshot();
        std::vector<std::string> names;
        for (const auto& [name, value] : current) {
            if (name == tracked.primary_key && tracked.generated_pk && is_empty_generated_value(value)) continue;
            names.push_back(name);
        }
        std::sort(names.begin(), names.end());

        std::vector<Value> params;
        std::string sql = "INSERT INTO " + dialect_->quote_identifier(tracked.table) + " (";
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i) sql += ", ";
            sql += dialect_->quote_identifier(names[i]);
            params.push_back(current.at(names[i]));
        }
        sql += ") VALUES (";
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i) sql += ", ";
            sql += dialect_->placeholder(i + 1);
        }
        sql += ");";

        const auto result = executor_->execute(sql, params);
        if (tracked.generated_pk && is_empty_generated_value(tracked.get_pk())) {
            tracked.set_pk(Value{result.last_insert_id});
        }
        tracked.original = tracked.snapshot();
        tracked.status = EntityStatus::Managed;
        tracked.register_identity();
    }

    void flush_update_if_dirty(TrackedEntity& tracked) {
        const auto current = tracked.snapshot();
        std::vector<std::string> changed;
        for (const auto& [name, value] : current) {
            if (name == tracked.primary_key) continue;
            auto original = tracked.original.find(name);
            if (original == tracked.original.end() || original->second != value) changed.push_back(name);
        }
        if (changed.empty()) return;
        std::sort(changed.begin(), changed.end());

        std::vector<Value> params;
        std::string sql = "UPDATE " + dialect_->quote_identifier(tracked.table) + " SET ";
        for (std::size_t i = 0; i < changed.size(); ++i) {
            if (i) sql += ", ";
            sql += dialect_->quote_identifier(changed[i]) + " = " + dialect_->placeholder(i + 1);
            params.push_back(current.at(changed[i]));
        }
        sql += " WHERE " + dialect_->quote_identifier(tracked.primary_key) + " = " +
               dialect_->placeholder(params.size() + 1) + ";";
        params.push_back(tracked.get_pk());
        executor_->execute(sql, params);
        tracked.original = current;
    }

    void flush_delete(TrackedEntity& tracked) {
        const Value pk = tracked.get_pk();
        const std::string sql = "DELETE FROM " + dialect_->quote_identifier(tracked.table) +
            " WHERE " + dialect_->quote_identifier(tracked.primary_key) + " = " + dialect_->placeholder(1) + ";";
        executor_->execute(sql, {pk});
        tracked.erase_identity(pk);
        tracked_.erase(tracked.object.get());
    }

    std::shared_ptr<DbExecutor> executor_;
    std::shared_ptr<Dialect> dialect_;
    IdentityMap identity_map_;
    std::unordered_map<void*, TrackedEntity> tracked_;
};

template <reflect::Entity T>
std::vector<std::shared_ptr<T>> EntityQuery<T>::all() {
    auto roots = session_.execute_query(query_);
    for (auto& include : includes_) include(roots);
    return roots;
}

template <reflect::Entity T>
template <std::meta::info Relation>
EntityQuery<T>& EntityQuery<T>::include() {
    static_assert(std::same_as<reflect::owner_type_t<Relation>, T>,
                  "MetalORM: included relation must belong to the queried entity");
    static_assert(reflect::has<mapping::many_to_many>(Relation),
                  "MetalORM 0.0.1: include<> currently supports many-to-many relations");
    auto* session = &session_;
    includes_.push_back([session](auto& roots) {
        session->template load_many_to_many_batch<T, Relation>(roots);
    });
    return *this;
}

} // namespace metal
