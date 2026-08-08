#pragma once

#include "metal/dml.hpp"
#include "metal/execution.hpp"
#include "metal/runtime_types.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace metal {

class UnitOfWork {
public:
    UnitOfWork(DbExecutor& executor, const Dialect& dialect)
        : executor_(executor), dialect_(dialect) {}

    [[nodiscard]] bool contains(void* key) const { return tracked_.contains(key); }

    TrackedEntity* find(void* key) {
        auto it = tracked_.find(key);
        return it == tracked_.end() ? nullptr : &it->second;
    }

    const TrackedEntity* find(void* key) const {
        auto it = tracked_.find(key);
        return it == tracked_.end() ? nullptr : &it->second;
    }

    void track(void* key, TrackedEntity tracked) {
        for (auto& checkpoint : checkpoints_) {
            if (!checkpoint.contains(key)) {
                checkpoint.emplace(key, capture_entry(false, tracked));
            }
        }
        tracked_[key] = std::move(tracked);
    }

    void erase(void* key) { tracked_.erase(key); }

    [[nodiscard]] std::vector<void*> keys() const {
        std::vector<void*> out;
        out.reserve(tracked_.size());
        for (const auto& [key, _] : tracked_) out.push_back(key);
        return out;
    }

    void clear() {
        tracked_.clear();
        checkpoints_.clear();
    }

    void refresh_snapshot(void* key) {
        if (auto* tracked = find(key); tracked && tracked->snapshot) {
            tracked->original = tracked->snapshot();
        }
    }

    void begin_checkpoint() {
        Checkpoint checkpoint;
        checkpoint.reserve(tracked_.size());
        for (const auto& [key, tracked] : tracked_) {
            checkpoint.emplace(key, capture_entry(true, tracked));
        }
        checkpoints_.push_back(std::move(checkpoint));
    }

    void commit_checkpoint() {
        if (checkpoints_.empty()) {
            throw std::logic_error("MetalORM: no UnitOfWork checkpoint to commit");
        }
        checkpoints_.pop_back();
    }

    void rollback_checkpoint() {
        if (checkpoints_.empty()) {
            throw std::logic_error("MetalORM: no UnitOfWork checkpoint to roll back");
        }

        auto checkpoint = std::move(checkpoints_.back());
        checkpoints_.pop_back();

        for (auto& [key, entry] : checkpoint) {
            if (entry.tracked.restore_snapshot) {
                entry.tracked.restore_snapshot(entry.current);
            }
            if (entry.restore_relations) entry.restore_relations();

            if (entry.existed) {
                tracked_[key] = std::move(entry.tracked);
            } else {
                tracked_.erase(key);
            }
        }
    }

    void rollback_all_checkpoints() {
        while (!checkpoints_.empty()) rollback_checkpoint();
    }

    [[nodiscard]] bool has_checkpoint() const noexcept { return !checkpoints_.empty(); }
    [[nodiscard]] std::size_t checkpoint_depth() const noexcept { return checkpoints_.size(); }

    void register_all_identities() {
        for (void* key : keys()) {
            auto* tracked = find(key);
            if (tracked && tracked->register_identity) tracked->register_identity();
        }
    }

    void flush() {
        for (void* key : keys()) {
            auto* tracked = find(key);
            if (!tracked) continue;
            switch (tracked->status) {
                case EntityStatus::New:
                    flush_insert(*tracked);
                    break;
                case EntityStatus::Managed:
                    flush_update_if_dirty(*tracked);
                    break;
                case EntityStatus::Removed:
                    flush_delete(key, *tracked);
                    break;
                case EntityStatus::Detached:
                    break;
            }
        }
    }

private:
    struct CheckpointEntry {
        bool existed{false};
        TrackedEntity tracked;
        std::unordered_map<std::string, Value> current;
        std::function<void()> restore_relations;
    };

    using Checkpoint = std::unordered_map<void*, CheckpointEntry>;

    static CheckpointEntry capture_entry(bool existed, const TrackedEntity& tracked) {
        CheckpointEntry entry;
        entry.existed = existed;
        entry.tracked = tracked;
        if (tracked.snapshot) entry.current = tracked.snapshot();
        if (tracked.capture_relation_restore) {
            entry.restore_relations = tracked.capture_relation_restore();
        }
        return entry;
    }

    void flush_insert(TrackedEntity& tracked) {
        const auto current = tracked.snapshot();
        std::vector<std::string> names;
        for (const auto& [name, value] : current) {
            if (name == tracked.primary_key && tracked.generated_pk && is_empty_generated_value(value)) continue;
            names.push_back(name);
        }
        std::sort(names.begin(), names.end());

        std::vector<DmlAssignment> values;
        values.reserve(names.size());
        for (const auto& name : names) values.push_back({name, current.at(name)});

        const auto compiled = InsertQueryBuilder{tracked.table}
            .values(std::move(values))
            .compile(dialect_);
        const auto result = executor_.execute(compiled.sql, compiled.params);

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

        std::vector<DmlAssignment> assignments;
        assignments.reserve(changed.size());
        for (const auto& name : changed) assignments.push_back({name, current.at(name)});

        const auto compiled = UpdateQueryBuilder{tracked.table}
            .set(std::move(assignments))
            .where_eq(tracked.primary_key, tracked.get_pk())
            .compile(dialect_);
        executor_.execute(compiled.sql, compiled.params);
        tracked.original = current;
    }

    void flush_delete(void* key, TrackedEntity& tracked) {
        const Value pk = tracked.get_pk();
        const auto compiled = DeleteQueryBuilder{tracked.table}
            .where_eq(tracked.primary_key, pk)
            .compile(dialect_);
        executor_.execute(compiled.sql, compiled.params);
        tracked.erase_identity(pk);
        tracked.status = EntityStatus::Detached;
        tracked_.erase(key);
    }

    DbExecutor& executor_;
    const Dialect& dialect_;
    std::unordered_map<void*, TrackedEntity> tracked_;
    std::vector<Checkpoint> checkpoints_;
};

} // namespace metal
