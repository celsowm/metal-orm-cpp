#pragma once

#include "metal/value.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace metal {

enum class EntityStatus { New, Managed, Removed, Detached };

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
    std::function<void()> prepare_relations;
    std::function<void()> flush_relations;
    std::function<void()> accept_relations;
};

} // namespace metal
