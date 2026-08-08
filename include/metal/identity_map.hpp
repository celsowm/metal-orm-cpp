#pragma once

#include "metal/reflection.hpp"

#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>

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

} // namespace metal
