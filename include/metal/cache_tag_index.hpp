#pragma once

#include "metal/cache_types.hpp"

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace metal {

class TagIndex {
public:
    void register_key(std::string key, const std::vector<std::string>& tags) {
        std::scoped_lock lock(mutex_);
        auto& key_tags = key_to_tags_[key];
        for (const auto& tag : tags) {
            if (tag.empty()) continue;
            tag_to_keys_[tag].insert(key);
            key_tags.insert(tag);
        }
        if (key_tags.empty()) key_to_tags_.erase(key);
    }

    void unregister(std::string_view key) {
        std::scoped_lock lock(mutex_);
        unregister_locked(key);
    }

    [[nodiscard]] std::vector<std::string> keys_by_tag(std::string_view tag) const {
        std::scoped_lock lock(mutex_);
        const auto found = tag_to_keys_.find(std::string(tag));
        if (found == tag_to_keys_.end()) return {};
        return {found->second.begin(), found->second.end()};
    }

    [[nodiscard]] std::vector<std::string> tags_by_key(std::string_view key) const {
        std::scoped_lock lock(mutex_);
        const auto found = key_to_tags_.find(std::string(key));
        if (found == key_to_tags_.end()) return {};
        return {found->second.begin(), found->second.end()};
    }

    [[nodiscard]] std::vector<std::string> invalidate_tags(
        const std::vector<std::string>& tags) {
        std::scoped_lock lock(mutex_);
        std::unordered_set<std::string> keys;
        for (const auto& tag : tags) {
            const auto found = tag_to_keys_.find(tag);
            if (found == tag_to_keys_.end()) continue;
            keys.insert(found->second.begin(), found->second.end());
        }
        for (const auto& key : keys) unregister_locked(key);
        return {keys.begin(), keys.end()};
    }

    [[nodiscard]] std::vector<std::string> invalidate_prefix(std::string_view prefix) {
        std::scoped_lock lock(mutex_);
        std::vector<std::string> keys;
        for (const auto& [key, _] : key_to_tags_) {
            if (key.starts_with(prefix)) keys.push_back(key);
        }
        for (const auto& key : keys) unregister_locked(key);
        return keys;
    }

    [[nodiscard]] std::vector<std::string> all_tags() const {
        std::scoped_lock lock(mutex_);
        std::vector<std::string> out;
        out.reserve(tag_to_keys_.size());
        for (const auto& [tag, _] : tag_to_keys_) out.push_back(tag);
        return out;
    }

    [[nodiscard]] std::vector<std::string> all_keys() const {
        std::scoped_lock lock(mutex_);
        std::vector<std::string> out;
        out.reserve(key_to_tags_.size());
        for (const auto& [key, _] : key_to_tags_) out.push_back(key);
        return out;
    }

    void clear() {
        std::scoped_lock lock(mutex_);
        tag_to_keys_.clear();
        key_to_tags_.clear();
    }

    [[nodiscard]] CacheStats stats() const {
        std::scoped_lock lock(mutex_);
        return CacheStats{key_to_tags_.size(), tag_to_keys_.size()};
    }

private:
    void unregister_locked(std::string_view key_view) {
        const std::string key(key_view);
        const auto found = key_to_tags_.find(key);
        if (found == key_to_tags_.end()) return;
        const auto tags = found->second;
        for (const auto& tag : tags) {
            auto tag_found = tag_to_keys_.find(tag);
            if (tag_found == tag_to_keys_.end()) continue;
            tag_found->second.erase(key);
            if (tag_found->second.empty()) tag_to_keys_.erase(tag_found);
        }
        key_to_tags_.erase(found);
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unordered_set<std::string>> tag_to_keys_;
    std::unordered_map<std::string, std::unordered_set<std::string>> key_to_tags_;
};

} // namespace metal
