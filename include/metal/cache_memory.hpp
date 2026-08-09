#pragma once

#include "metal/cache_provider.hpp"
#include "metal/cache_tag_index.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace metal {

class MemoryCacheAdapter final :
    public CacheProvider,
    public CacheTagRegistrar,
    public CacheClearable,
    public CacheStatsProvider {
public:
    [[nodiscard]] std::string_view name() const noexcept override {
        return "memory";
    }

    [[nodiscard]] CacheCapabilities capabilities() const noexcept override {
        return CacheCapabilities{true, true, true};
    }

    [[nodiscard]] std::optional<QueryResult> get(std::string_view key_view) override {
        const std::string key(key_view);
        bool expired = false;
        std::optional<QueryResult> value;
        {
            std::scoped_lock lock(mutex_);
            const auto found = storage_.find(key);
            if (found == storage_.end()) return std::nullopt;
            if (found->second.expires_at &&
                std::chrono::steady_clock::now() > *found->second.expires_at) {
                storage_.erase(found);
                expired = true;
            } else {
                value = found->second.value;
            }
        }
        if (expired) tag_index_.unregister(key);
        return value;
    }

    [[nodiscard]] bool has(std::string_view key) override {
        return get(key).has_value();
    }

    void set(
        std::string key,
        QueryResult value,
        std::optional<std::chrono::milliseconds> ttl = std::nullopt) override {
        CacheEntry entry;
        entry.value = std::move(value);
        if (ttl && ttl->count() != 0) {
            entry.expires_at = std::chrono::steady_clock::now() + *ttl;
        }
        std::scoped_lock lock(mutex_);
        storage_[std::move(key)] = std::move(entry);
    }

    void delete_key(std::string_view key_view) override {
        const std::string key(key_view);
        {
            std::scoped_lock lock(mutex_);
            storage_.erase(key);
        }
        tag_index_.unregister(key);
    }

    void invalidate(std::string_view key) override {
        delete_key(key);
    }

    void invalidate_tags(const std::vector<std::string>& tags) override {
        const auto keys = tag_index_.invalidate_tags(tags);
        std::scoped_lock lock(mutex_);
        for (const auto& key : keys) storage_.erase(key);
    }

    void invalidate_prefix(std::string_view prefix) override {
        std::vector<std::string> keys;
        {
            std::scoped_lock lock(mutex_);
            for (const auto& [key, _] : storage_) {
                if (key.starts_with(prefix)) keys.push_back(key);
            }
        }
        for (const auto& key : keys) delete_key(key);
    }

    void register_tags(
        std::string_view key,
        const std::vector<std::string>& tags) override {
        tag_index_.register_key(std::string(key), tags);
    }

    void clear_cache() override {
        {
            std::scoped_lock lock(mutex_);
            storage_.clear();
        }
        tag_index_.clear();
    }

    [[nodiscard]] CacheStats cache_stats() const override {
        std::size_t size = 0;
        {
            std::scoped_lock lock(mutex_);
            size = storage_.size();
        }
        const auto tag_stats = tag_index_.stats();
        return CacheStats{size, tag_stats.tags};
    }

    void dispose() override {
        clear_cache();
    }

private:
    struct CacheEntry {
        QueryResult value;
        std::optional<std::chrono::steady_clock::time_point> expires_at;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, CacheEntry> storage_;
    TagIndex tag_index_;
};

} // namespace metal
