#pragma once

#include "metal/execution.hpp"
#include "metal/identity_map.hpp"
#include "metal/polymorphic.hpp"
#include "metal/query.hpp"
#include "metal/reflection.hpp"
#include "metal/relation_change_processor.hpp"
#include "metal/runtime_types.hpp"
#include "metal/unit_of_work.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <variant>
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
          relation_changes_(unit_of_work_, *executor_, *dialect_) {}

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
        const Value pk = reflect::primary_key_value(*entity);
        track<T>(entity, has_identity_key<T>(pk) ? EntityStatus::Managed : EntityStatus::New);
    }

    template <reflect::Entity T>
    void remove(const std::shared_ptr<T>& entity) {
        static_assert(reflect::validate_mapping<T>());
        if (!entity) return;
        if (auto* tracked = unit_of_work_.find(entity.get())) tracked->status = EntityStatus::Removed;
    }

    void flush() { unit_of_work_.flush(); }

    void commit() {
        if (transaction_depth_ != 0) {
            throw std::logic_error(
                "MetalORM: commit() cannot be called inside Session::transaction(); the transaction scope flushes automatically");
        }

        if (!executor_->capabilities().transactions) {
            flush_pipeline();
            relation_changes_.accept();
            return;
        }

        unit_of_work_.begin_checkpoint();
        try {
            executor_->begin_transaction();
            flush_pipeline();
            relation_changes_.accept();
            executor_->commit_transaction();
            unit_of_work_.commit_checkpoint();
        } catch (...) {
            try { executor_->rollback_transaction(); } catch (...) {}
            if (unit_of_work_.has_checkpoint()) {
                try { unit_of_work_.rollback_checkpoint(); } catch (...) {}
            }
            rebuild_identity_map();
            relation_changes_.reset();
            throw;
        }
    }

    template <typename Fn>
    auto transaction(Fn&& fn) -> std::invoke_result_t<Fn, Session&> {
        using Result = std::invoke_result_t<Fn, Session&>;

        if (!executor_->capabilities().transactions) {
            if constexpr (std::is_void_v<Result>) {
                std::invoke(std::forward<Fn>(fn), *this);
                commit();
                return;
            } else {
                Result result = std::invoke(std::forward<Fn>(fn), *this);
                commit();
                return result;
            }
        }

        const bool outermost = transaction_depth_ == 0;
        std::string savepoint_name;
        if (outermost) {
            rollback_only_ = false;
            executor_->begin_transaction();
        } else {
            assert_savepoint_support();
            savepoint_name = next_savepoint_name();
            executor_->savepoint(savepoint_name);
        }

        unit_of_work_.begin_checkpoint();
        ++transaction_depth_;

        try {
            if constexpr (std::is_void_v<Result>) {
                std::invoke(std::forward<Fn>(fn), *this);
                throw_if_rollback_only();
                flush_pipeline();
                throw_if_rollback_only();
                relation_changes_.accept();
                if (outermost) executor_->commit_transaction();
                else executor_->release_savepoint(savepoint_name);
                unit_of_work_.commit_checkpoint();
                finish_transaction_scope();
                return;
            } else {
                Result result = std::invoke(std::forward<Fn>(fn), *this);
                throw_if_rollback_only();
                flush_pipeline();
                throw_if_rollback_only();
                relation_changes_.accept();
                if (outermost) executor_->commit_transaction();
                else executor_->release_savepoint(savepoint_name);
                unit_of_work_.commit_checkpoint();
                finish_transaction_scope();
                return result;
            }
        } catch (...) {
            if (outermost) {
                try { executor_->rollback_transaction(); } catch (...) {}
            } else {
                rollback_only_ = true;
                try { executor_->rollback_to_savepoint(savepoint_name); } catch (...) {}
            }
            if (unit_of_work_.has_checkpoint()) {
                try { unit_of_work_.rollback_checkpoint(); } catch (...) {}
            }
            rebuild_identity_map();
            relation_changes_.reset();
            finish_transaction_scope();
            throw;
        }
    }

    void rollback() {
        if (executor_->capabilities().transactions && transaction_depth_ != 0) {
            try { executor_->rollback_transaction(); } catch (...) {}
        }

        if (unit_of_work_.has_checkpoint()) {
            unit_of_work_.rollback_all_checkpoints();
            rebuild_identity_map();
        } else {
            unit_of_work_.clear();
            identity_map_.clear();
        }
        relation_changes_.reset();
        transaction_depth_ = 0;
        savepoint_counter_ = 0;
        rollback_only_ = false;
    }

    void clear() {
        unit_of_work_.clear();
        identity_map_.clear();
        relation_changes_.reset();
        transaction_depth_ = 0;
        savepoint_counter_ = 0;
        rollback_only_ = false;
    }

    DbExecutor& executor() noexcept { return *executor_; }
    const Dialect& dialect() const noexcept { return *dialect_; }
    IdentityMap& identity_map() noexcept { return identity_map_; }
    UnitOfWork& unit_of_work() noexcept { return unit_of_work_; }
    RelationChangeProcessor& relation_changes() noexcept { return relation_changes_; }
    [[nodiscard]] std::size_t transaction_depth() const noexcept { return transaction_depth_; }
    [[nodiscard]] bool rollback_only() const noexcept { return rollback_only_; }

private:
    template <reflect::Entity T>
    friend class EntityQuery;

    template <reflect::Entity T>
    static bool has_identity_key(const Value& pk) {
        if (std::holds_alternative<std::nullptr_t>(pk)) return false;
        if constexpr (reflect::primary_key_is_generated<T>()) return !is_empty_generated_value(pk);
        return true;
    }

    void flush_pipeline() {
        relation_changes_.prepare();
        unit_of_work_.flush();
        relation_changes_.process();
        unit_of_work_.flush();
    }

    void rebuild_identity_map() {
        identity_map_.clear();
        unit_of_work_.register_all_identities();
    }

    void assert_savepoint_support() const {
        if (!executor_->capabilities().savepoints) {
            throw std::logic_error(
                "MetalORM: nested Session::transaction() calls require savepoint support in this executor");
        }
    }

    std::string next_savepoint_name() {
        ++savepoint_counter_;
        return "metalorm_sp_" + std::to_string(savepoint_counter_);
    }

    void throw_if_rollback_only() const {
        if (rollback_only_) {
            throw std::logic_error(
                "MetalORM: cannot commit transaction because an inner transaction failed");
        }
    }

    void finish_transaction_scope() noexcept {
        if (transaction_depth_ != 0) --transaction_depth_;
        if (transaction_depth_ == 0) {
            rollback_only_ = false;
            savepoint_counter_ = 0;
        }
    }

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
        tracked.restore_snapshot = [weak = std::weak_ptr<T>(entity)](
            const std::unordered_map<std::string, Value>& values) {
            if (auto locked = weak.lock()) {
                reflect::for_each_column<T>([&]<std::meta::info Member>() {
                    const auto found = values.find(reflect::column_name<Member>());
                    if (found == values.end()) return;
                    using M = reflect::member_type_t<Member>;
                    (*locked).[:Member:] = from_value<M>(found->second);
                });
            }
        };
        tracked.capture_relation_restore = [weak = std::weak_ptr<T>(entity)]() -> std::function<void()> {
            std::vector<std::function<void()>> restores;
            if (auto locked = weak.lock()) {
                template for (constexpr auto relation : reflect::data_members<T>()) {
                    if constexpr (reflect::has_relation_annotation<relation>()) {
                        using Relation = reflect::member_type_t<relation>;
                        static_assert(std::is_copy_constructible_v<Relation> && std::is_copy_assignable_v<Relation>,
                                      "MetalORM: rollback-safe relation members must be copyable");
                        Relation saved = (*locked).[:relation:];
                        restores.push_back(
                            [weak, saved = std::move(saved)]() mutable {
                                if (auto current = weak.lock()) (*current).[:relation:] = saved;
                            });
                    }
                }
            }
            return [restores = std::move(restores)]() mutable {
                for (auto& restore : restores) restore();
            };
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
                if (has_identity_key<T>(pk)) identity_map_.put<T>(pk, locked);
            }
        };
        tracked.erase_identity = [this](const Value& pk) { identity_map_.erase<T>(pk); };
        tracked.prepare_relations = [this, weak = std::weak_ptr<T>(entity)] {
            if (auto locked = weak.lock()) {
                relation_changes_.prepare_entity(*locked, [this](const auto& target) { this->persist(target); });
            }
        };
        tracked.flush_relations = [this, weak = std::weak_ptr<T>(entity)] {
            if (auto locked = weak.lock()) {
                relation_changes_.process_entity(*locked, [this](const auto& target) { this->remove(target); });
            }
        };
        tracked.accept_relations = [weak = std::weak_ptr<T>(entity)] {
            if (auto locked = weak.lock()) RelationChangeProcessor::accept_entity(*locked);
        };
        unit_of_work_.track(entity.get(), std::move(tracked));

        bind_relation_collections(entity);

        const auto pk = reflect::primary_key_value(*entity);
        if (has_identity_key<T>(pk)) identity_map_.put<T>(pk, entity);
    }

    template <reflect::Entity Root>
    void bind_relation_collections(const std::shared_ptr<Root>& root) {
        template for (constexpr auto relation : reflect::data_members<Root>()) {
            if constexpr (reflect::has_relation_annotation<relation>()) {
                using A = reflect::relation_annotation_t<relation>;
                using Traits = mapping::relation_annotation_traits<A>;
                if constexpr (Traits::kind == mapping::relation_kind::has_many) {
                    using Target = reflect::has_many_target_t<reflect::member_type_t<relation>>;
                    constexpr auto target_fk = Traits::target_foreign_key();
                    constexpr auto local_key = reflect::key_or_primary<Root>(Traits::local_key());
                    using ForeignKey = reflect::member_type_t<target_fk>;
                    auto weak = std::weak_ptr<Root>(root);
                    auto& collection = root->[:relation:];
                    collection._metal_bind_loader([this, weak] {
                        if (auto locked = weak.lock()) {
                            std::vector<std::shared_ptr<Root>> roots{locked};
                            load_has_many_batch<Root, relation>(roots);
                        }
                    });
                    collection._metal_bind_attach([weak](Target& target) {
                        if (auto locked = weak.lock()) {
                            target.[:target_fk:] = from_value<ForeignKey>(to_value((*locked).[:local_key:]));
                        }
                    });
                } else if constexpr (Traits::kind == mapping::relation_kind::many_to_many) {
                    using Collection = reflect::member_type_t<relation>;
                    using Target = reflect::many_to_many_target_t<Collection>;
                    constexpr auto target_key = reflect::key_or_primary<Target>(Traits::target_key());
                    constexpr auto target_pk = reflect::primary_key_member<Target>();
                    constexpr bool target_key_is_pk = target_key == target_pk;

                    auto weak = std::weak_ptr<Root>(root);
                    auto& collection = root->[:relation:];
                    collection._metal_bind_loader([this, weak] {
                        if (auto locked = weak.lock()) {
                            std::vector<std::shared_ptr<Root>> roots{locked};
                            load_many_to_many_batch<Root, relation>(roots);
                        }
                    });
                    collection._metal_bind_identity(
                        [](const Target& target) -> std::optional<std::string> {
                            const Value key = to_value(target.[:target_key:]);
                            if (std::holds_alternative<std::nullptr_t>(key)) return std::nullopt;
                            if constexpr (target_key_is_pk && reflect::primary_key_is_generated<Target>()) {
                                if (is_empty_generated_value(key)) return std::nullopt;
                            }
                            return value_key(key);
                        },
                        [this](const Value& key) -> std::shared_ptr<Target> {
                            if constexpr (target_key_is_pk) {
                                if (auto existing = identity_map_.get<Target>(key)) return existing;
                            }
                            auto target = std::make_shared<Target>();
                            using KeyType = reflect::member_type_t<target_key>;
                            target->[:target_key:] = from_value<KeyType>(key);
                            if constexpr (target_key_is_pk) track<Target>(target, EntityStatus::Managed);
                            return target;
                        });
                } else if constexpr (Traits::kind == mapping::relation_kind::morph_one) {
                    using Target = reflect::morph_one_target_t<reflect::member_type_t<relation>>;
                    constexpr auto type_field = Traits::type_field();
                    constexpr auto id_field = Traits::id_field();
                    constexpr auto local_key = reflect::key_or_primary<Root>(Traits::local_key());
                    using TypeField = reflect::member_type_t<type_field>;
                    using IdField = reflect::member_type_t<id_field>;

                    auto weak = std::weak_ptr<Root>(root);
                    auto& reference = root->[:relation:];
                    reference._metal_bind_loader([this, weak] {
                        if (auto locked = weak.lock()) {
                            std::vector<std::shared_ptr<Root>> roots{locked};
                            load_morph_one_batch<Root, relation>(roots);
                        }
                    });
                    reference._metal_bind_attach([weak](Target& target) {
                        if (auto locked = weak.lock()) {
                            target.[:id_field:] = from_value<IdField>(to_value((*locked).[:local_key:]));
                            target.[:type_field:] = from_value<TypeField>(
                                Value{std::string(Traits::type_value.view())});
                        }
                    });
                } else if constexpr (Traits::kind == mapping::relation_kind::morph_many) {
                    using Target = reflect::morph_many_target_t<reflect::member_type_t<relation>>;
                    constexpr auto type_field = Traits::type_field();
                    constexpr auto id_field = Traits::id_field();
                    constexpr auto local_key = reflect::key_or_primary<Root>(Traits::local_key());
                    using TypeField = reflect::member_type_t<type_field>;
                    using IdField = reflect::member_type_t<id_field>;

                    auto weak = std::weak_ptr<Root>(root);
                    auto& collection = root->[:relation:];
                    collection._metal_bind_loader([this, weak] {
                        if (auto locked = weak.lock()) {
                            std::vector<std::shared_ptr<Root>> roots{locked};
                            load_morph_many_batch<Root, relation>(roots);
                        }
                    });
                    collection._metal_bind_attach([weak](Target& target) {
                        if (auto locked = weak.lock()) {
                            target.[:id_field:] = from_value<IdField>(to_value((*locked).[:local_key:]));
                            target.[:type_field:] = from_value<TypeField>(
                                Value{std::string(Traits::type_value.view())});
                        }
                    });
                } else if constexpr (Traits::kind == mapping::relation_kind::morph_to) {
                    constexpr auto type_field = Traits::type_field();
                    constexpr auto id_field = Traits::id_field();
                    using TypeField = reflect::member_type_t<type_field>;
                    using IdField = reflect::member_type_t<id_field>;

                    auto weak = std::weak_ptr<Root>(root);
                    auto& reference = root->[:relation:];
                    reference._metal_bind_loader([this, weak] {
                        if (auto locked = weak.lock()) {
                            std::vector<std::shared_ptr<Root>> roots{locked};
                            load_morph_to_batch<Root, relation>(roots);
                        }
                    });
                    reference._metal_bind_attach([weak](const auto& current) {
                        if (!std::holds_alternative<std::monostate>(current)) return;
                        if (auto locked = weak.lock()) {
                            if constexpr (is_optional_v<TypeField>) (*locked).[:type_field:] = std::nullopt;
                            if constexpr (is_optional_v<IdField>) (*locked).[:id_field:] = std::nullopt;
                        }
                    });
                }
            }
        }
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

    template <reflect::Mapped T>
    static T hydrate_prefixed(const Row& row, std::string_view prefix) {
        T value{};
        reflect::for_each_column<T>([&]<std::meta::info Member>() {
            const std::string key = std::string(prefix) + reflect::column_name<Member>();
            auto found = row.find(key);
            if (found == row.end()) return;
            using M = reflect::member_type_t<Member>;
            value.[:Member:] = from_value<M>(found->second);
        });
        return value;
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

    template <reflect::Mapped Pivot>
    void append_pivot_columns(std::string& sql, std::string_view alias) const {
        reflect::for_each_column<Pivot>([&]<std::meta::info Member>() {
            const auto name = reflect::column_name<Member>();
            sql += ", " + std::string(alias) + "." + dialect_->quote_identifier(name) +
                   " AS " + dialect_->quote_identifier("__metal_pivot_" + name);
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
        using Target = reflect::has_many_target_t<reflect::member_type_t<Relation>>;
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
        for (auto& root : roots) (*root).[:Relation:]._metal_hydrate(std::move(loaded[root.get()]));
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
            if (auto target = targets_by_key.find(key); target != targets_by_key.end()) (*root).[:Relation:] = target->second;
        }
    }

    template <reflect::Entity Root, std::meta::info Relation>
    void load_many_to_many_batch(std::vector<std::shared_ptr<Root>>& roots) {
        using A = reflect::relation_annotation_t<Relation>;
        using Traits = mapping::relation_annotation_traits<A>;
        using Collection = reflect::member_type_t<Relation>;
        using Target = reflect::many_to_many_target_t<Collection>;
        using Pivot = reflect::many_to_many_pivot_t<Collection>;
        if (roots.empty()) return;
        constexpr auto pivot_root_fk = Traits::pivot_root_foreign_key();
        constexpr auto pivot_target_fk = Traits::pivot_target_foreign_key();
        constexpr auto local_key = reflect::key_or_primary<Root>(Traits::local_key());
        constexpr auto target_key = reflect::key_or_primary<Target>(Traits::target_key());
        using LoadedValue = std::pair<std::shared_ptr<Target>, std::optional<Pivot>>;
        std::unordered_map<std::string, std::shared_ptr<Root>> roots_by_key;
        std::unordered_map<Root*, std::vector<LoadedValue>> loaded;
        std::vector<Value> keys;
        for (auto& root : roots) {
            const auto key = to_value((*root).[:local_key:]);
            roots_by_key[value_key(key)] = root;
            loaded[root.get()] = {};
            keys.push_back(key);
        }
        std::string sql = "SELECT ";
        append_target_columns<Target>(sql, "t");
        append_pivot_columns<Pivot>(sql, "p");
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
            loaded[root->second.get()].push_back({
                hydrate<Target>(row),
                hydrate_prefixed<Pivot>(row, "__metal_pivot_")
            });
        }
        for (auto& root : roots) (*root).[:Relation:]._metal_hydrate(std::move(loaded[root.get()]));
    }

    template <reflect::Entity Root, std::meta::info Relation>
    void load_morph_one_batch(std::vector<std::shared_ptr<Root>>& roots) {
        using A = reflect::relation_annotation_t<Relation>;
        using Traits = mapping::relation_annotation_traits<A>;
        using Target = reflect::morph_one_target_t<reflect::member_type_t<Relation>>;
        if (roots.empty()) return;
        constexpr auto type_field = Traits::type_field();
        constexpr auto id_field = Traits::id_field();
        constexpr auto local_key = reflect::key_or_primary<Root>(Traits::local_key());

        std::unordered_map<std::string, std::shared_ptr<Root>> roots_by_key;
        std::unordered_map<Root*, std::shared_ptr<Target>> loaded;
        std::vector<Value> keys;
        for (auto& root : roots) {
            const auto key = to_value((*root).[:local_key:]);
            roots_by_key[value_key(key)] = root;
            loaded[root.get()] = {};
            keys.push_back(key);
        }

        std::string sql = "SELECT ";
        append_target_columns<Target>(sql, "t");
        sql += ", t." + dialect_->quote_identifier(reflect::column_name<id_field>()) + " AS \"__metal_root_key\"";
        sql += " FROM " + dialect_->quote_identifier(reflect::table_name<Target>()) + " t";
        sql += " WHERE t." + dialect_->quote_identifier(reflect::column_name<id_field>()) + " IN (";
        append_in_placeholders(sql, keys.size());
        sql += ") AND t." + dialect_->quote_identifier(reflect::column_name<type_field>()) + " = ?;";
        keys.push_back(Value{std::string(Traits::type_value.view())});

        const auto result = executor_->execute(sql, keys);
        for (const auto& row : result.rows) {
            auto root_key = row.find("__metal_root_key");
            if (root_key == row.end()) continue;
            auto root = roots_by_key.find(value_key(root_key->second));
            if (root != roots_by_key.end()) loaded[root->second.get()] = hydrate<Target>(row);
        }
        for (auto& root : roots) (*root).[:Relation:]._metal_hydrate(std::move(loaded[root.get()]));
    }

    template <reflect::Entity Root, std::meta::info Relation>
    void load_morph_many_batch(std::vector<std::shared_ptr<Root>>& roots) {
        using A = reflect::relation_annotation_t<Relation>;
        using Traits = mapping::relation_annotation_traits<A>;
        using Target = reflect::morph_many_target_t<reflect::member_type_t<Relation>>;
        if (roots.empty()) return;
        constexpr auto type_field = Traits::type_field();
        constexpr auto id_field = Traits::id_field();
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
        sql += ", t." + dialect_->quote_identifier(reflect::column_name<id_field>()) + " AS \"__metal_root_key\"";
        sql += " FROM " + dialect_->quote_identifier(reflect::table_name<Target>()) + " t";
        sql += " WHERE t." + dialect_->quote_identifier(reflect::column_name<id_field>()) + " IN (";
        append_in_placeholders(sql, keys.size());
        sql += ") AND t." + dialect_->quote_identifier(reflect::column_name<type_field>()) + " = ?;";
        keys.push_back(Value{std::string(Traits::type_value.view())});

        const auto result = executor_->execute(sql, keys);
        for (const auto& row : result.rows) {
            auto root_key = row.find("__metal_root_key");
            if (root_key == row.end()) continue;
            auto root = roots_by_key.find(value_key(root_key->second));
            if (root == roots_by_key.end()) continue;
            loaded[root->second.get()].push_back(hydrate<Target>(row));
        }
        for (auto& root : roots) (*root).[:Relation:]._metal_hydrate(std::move(loaded[root.get()]));
    }

    template <reflect::Entity Root, std::meta::info Relation>
    void load_morph_to_batch(std::vector<std::shared_ptr<Root>>& roots) {
        using A = reflect::relation_annotation_t<Relation>;
        using Traits = mapping::relation_annotation_traits<A>;
        using Reference = reflect::member_type_t<Relation>;
        using Variant = typename Reference::variant_type;
        if (roots.empty()) return;
        constexpr auto type_field = Traits::type_field();
        constexpr auto id_field = Traits::id_field();

        std::unordered_map<Root*, Variant> loaded;
        for (auto& root : roots) loaded.emplace(root.get(), Variant{std::monostate{}});

        auto load_case = [&]<typename Case>() {
            using CaseTraits = mapping::morph_target_traits<Case>;
            constexpr auto target_reflection = CaseTraits::target();
            using Target = [: target_reflection :];
            constexpr auto target_key = reflect::key_or_primary<Target>(CaseTraits::target_key());

            std::unordered_map<std::string, std::vector<std::shared_ptr<Root>>> roots_by_key;
            std::vector<Value> keys;
            for (auto& root : roots) {
                const Value type_value = to_value((*root).[:type_field:]);
                const Value id_value = to_value((*root).[:id_field:]);
                if (std::holds_alternative<std::nullptr_t>(type_value) ||
                    std::holds_alternative<std::nullptr_t>(id_value)) continue;
                if (from_value<std::string>(type_value) != CaseTraits::type_value.view()) continue;
                const auto key = value_key(id_value);
                if (!roots_by_key.contains(key)) keys.push_back(id_value);
                roots_by_key[key].push_back(root);
            }
            if (keys.empty()) return;

            std::string sql = "SELECT ";
            append_target_columns<Target>(sql, "t");
            sql += " FROM " + dialect_->quote_identifier(reflect::table_name<Target>()) + " t";
            sql += " WHERE t." + dialect_->quote_identifier(reflect::column_name<target_key>()) + " IN (";
            append_in_placeholders(sql, keys.size());
            sql += ");";
            const auto result = executor_->execute(sql, keys);
            for (const auto& row : result.rows) {
                auto target = hydrate<Target>(row);
                const auto key = value_key(to_value((*target).[:target_key:]));
                auto matched = roots_by_key.find(key);
                if (matched == roots_by_key.end()) continue;
                for (auto& root : matched->second) loaded[root.get()] = target;
            }
        };

        [&]<typename... Cases>(mapping::type_list<Cases...>) {
            (load_case.template operator()<Cases>(), ...);
        }(typename Traits::targets{});

        for (auto& root : roots) (*root).[:Relation:]._metal_hydrate(std::move(loaded[root.get()]));
    }

    std::shared_ptr<DbExecutor> executor_;
    std::shared_ptr<Dialect> dialect_;
    IdentityMap identity_map_;
    UnitOfWork unit_of_work_;
    RelationChangeProcessor relation_changes_;
    std::size_t transaction_depth_{0};
    std::size_t savepoint_counter_{0};
    bool rollback_only_{false};
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
        includes_.push_back([session](auto& roots) { session->template load_belongs_to_batch<T, Relation>(roots); });
    } else if constexpr (Traits::kind == mapping::relation_kind::has_one) {
        includes_.push_back([session](auto& roots) { session->template load_has_one_batch<T, Relation>(roots); });
    } else if constexpr (Traits::kind == mapping::relation_kind::has_many) {
        includes_.push_back([session](auto& roots) { session->template load_has_many_batch<T, Relation>(roots); });
    } else if constexpr (Traits::kind == mapping::relation_kind::many_to_many) {
        includes_.push_back([session](auto& roots) { session->template load_many_to_many_batch<T, Relation>(roots); });
    } else if constexpr (Traits::kind == mapping::relation_kind::morph_one) {
        includes_.push_back([session](auto& roots) { session->template load_morph_one_batch<T, Relation>(roots); });
    } else if constexpr (Traits::kind == mapping::relation_kind::morph_many) {
        includes_.push_back([session](auto& roots) { session->template load_morph_many_batch<T, Relation>(roots); });
    } else if constexpr (Traits::kind == mapping::relation_kind::morph_to) {
        includes_.push_back([session](auto& roots) { session->template load_morph_to_batch<T, Relation>(roots); });
    }
    return *this;
}

} // namespace metal
