#pragma once

#include "metal/cache_codec.hpp"
#include "metal/cache_provider.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace metal {

/**
 * Minimal dependency-inversion boundary for a remote key/value store.
 * Implementations can wrap KeyDB, Redis, Memcached, a Keyv-like service,
 * a cloud cache client, or an application-specific transport.
 */
class KeyValueBackend {
public:
    virtual ~KeyValueBackend() = default;

    [[nodiscard]] virtual std::optional<std::string> get(std::string_view key) = 0;
    virtual void set(
        std::string key,
        std::string value,
        std::optional<std::chrono::milliseconds> ttl = std::nullopt) = 0;
    virtual void delete_key(std::string_view key) = 0;
    virtual void dispose() {}
};

/** Optional capability for stores that can enumerate keys by prefix. */
class KeyValuePrefixBackend {
public:
    virtual ~KeyValuePrefixBackend() = default;
    [[nodiscard]] virtual std::vector<std::string> keys_with_prefix(
        std::string_view prefix) = 0;
};

struct KeyValueCacheAdapterOptions {
    bool dispose_backend{false};
};

/**
 * C++ equivalent of the TypeScript Keyv adapter: TTL + optional prefix
 * invalidation, deliberately without tag support.
 */
class KeyValueCacheAdapter final : public CacheProvider {
public:
    explicit KeyValueCacheAdapter(
        std::shared_ptr<KeyValueBackend> backend,
        KeyValueCacheAdapterOptions options = {})
        : backend_(std::move(backend)), options_(options) {
        if (!backend_) {
            throw std::invalid_argument("MetalORM: key/value cache backend cannot be null");
        }
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "key-value";
    }

    [[nodiscard]] CacheCapabilities capabilities() const noexcept override {
        return CacheCapabilities{
            false,
            dynamic_cast<KeyValuePrefixBackend*>(backend_.get()) != nullptr,
            true};
    }

    [[nodiscard]] std::optional<QueryResult> get(std::string_view key) override {
        const auto payload = backend_->get(key);
        if (!payload) return std::nullopt;
        return decode_query_result(*payload);
    }

    [[nodiscard]] bool has(std::string_view key) override {
        return backend_->get(key).has_value();
    }

    void set(
        std::string key,
        QueryResult value,
        std::optional<std::chrono::milliseconds> ttl = std::nullopt) override {
        backend_->set(
            std::move(key),
            encode_query_result(value),
            normalize_ttl(ttl));
    }

    void delete_key(std::string_view key) override {
        backend_->delete_key(key);
    }

    void invalidate(std::string_view key) override {
        delete_key(key);
    }

    void invalidate_tags(const std::vector<std::string>&) override {
        throw std::logic_error(
            "MetalORM: KeyValueCacheAdapter does not support tag invalidation; "
            "use RedisCacheAdapter or another tag-capable provider");
    }

    void invalidate_prefix(std::string_view prefix) override {
        auto* scanner = dynamic_cast<KeyValuePrefixBackend*>(backend_.get());
        if (!scanner) {
            throw std::logic_error(
                "MetalORM: key/value backend does not support prefix invalidation");
        }
        const auto keys = scanner->keys_with_prefix(prefix);
        for (const auto& key : keys) backend_->delete_key(key);
    }

    void dispose() override {
        if (options_.dispose_backend) backend_->dispose();
    }

    [[nodiscard]] KeyValueBackend& backend() noexcept { return *backend_; }
    [[nodiscard]] const KeyValueBackend& backend() const noexcept { return *backend_; }

private:
    static std::optional<std::chrono::milliseconds> normalize_ttl(
        std::optional<std::chrono::milliseconds> ttl) {
        if (ttl && ttl->count() == 0) return std::nullopt;
        return ttl;
    }

    std::shared_ptr<KeyValueBackend> backend_;
    KeyValueCacheAdapterOptions options_;
};

/**
 * Minimal Redis-shaped capability boundary used by RedisCacheAdapter.
 * A concrete integration can wrap hiredis, redis-plus-plus, Boost.Redis,
 * or an existing application client without making it a MetalORM dependency.
 */
class RedisBackend : public KeyValueBackend, public KeyValuePrefixBackend {
public:
    ~RedisBackend() override = default;

    virtual void add_set_members(
        std::string_view key,
        const std::vector<std::string>& members) = 0;
    [[nodiscard]] virtual std::vector<std::string> set_members(
        std::string_view key) = 0;

    virtual void delete_keys(const std::vector<std::string>& keys) {
        for (const auto& key : keys) delete_key(key);
    }
};

struct RedisCacheAdapterOptions {
    std::string tag_prefix{"tag:"};
    bool dispose_backend{false};
};

/**
 * Full remote cache adapter matching the TypeScript Redis feature set:
 * native backend TTL, prefix invalidation, and tag invalidation through sets.
 */
class RedisCacheAdapter final :
    public CacheProvider,
    public CacheTagRegistrar {
public:
    explicit RedisCacheAdapter(
        std::shared_ptr<RedisBackend> backend,
        RedisCacheAdapterOptions options = {})
        : backend_(std::move(backend)), options_(std::move(options)) {
        if (!backend_) {
            throw std::invalid_argument("MetalORM: Redis cache backend cannot be null");
        }
        if (options_.tag_prefix.empty()) {
            throw std::invalid_argument("MetalORM: Redis cache tag prefix cannot be empty");
        }
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "redis";
    }

    [[nodiscard]] CacheCapabilities capabilities() const noexcept override {
        return CacheCapabilities{true, true, true};
    }

    [[nodiscard]] std::optional<QueryResult> get(std::string_view key) override {
        const auto payload = backend_->get(key);
        if (!payload) return std::nullopt;
        return decode_query_result(*payload);
    }

    [[nodiscard]] bool has(std::string_view key) override {
        return backend_->get(key).has_value();
    }

    void set(
        std::string key,
        QueryResult value,
        std::optional<std::chrono::milliseconds> ttl = std::nullopt) override {
        backend_->set(
            std::move(key),
            encode_query_result(value),
            normalize_ttl(ttl));
    }

    void delete_key(std::string_view key) override {
        backend_->delete_key(key);
    }

    void invalidate(std::string_view key) override {
        delete_key(key);
    }

    void register_tags(
        std::string_view key,
        const std::vector<std::string>& tags) override {
        for (const auto& tag : tags) {
            if (tag.empty()) continue;
            backend_->add_set_members(
                tag_key(tag),
                std::vector<std::string>{std::string(key)});
        }
    }

    void invalidate_tags(const std::vector<std::string>& tags) override {
        std::unordered_set<std::string> unique_keys;
        std::vector<std::string> tag_keys;
        tag_keys.reserve(tags.size());

        for (const auto& tag : tags) {
            if (tag.empty()) continue;
            const auto key = tag_key(tag);
            for (auto cached_key : backend_->set_members(key)) {
                unique_keys.insert(std::move(cached_key));
            }
            tag_keys.push_back(key);
        }

        if (!tag_keys.empty()) backend_->delete_keys(tag_keys);
        if (!unique_keys.empty()) {
            std::vector<std::string> keys;
            keys.reserve(unique_keys.size());
            for (const auto& key : unique_keys) keys.push_back(key);
            backend_->delete_keys(keys);
        }
    }

    void invalidate_prefix(std::string_view prefix) override {
        auto keys = backend_->keys_with_prefix(prefix);
        if (!keys.empty()) backend_->delete_keys(keys);
    }

    void dispose() override {
        if (options_.dispose_backend) backend_->dispose();
    }

    [[nodiscard]] RedisBackend& backend() noexcept { return *backend_; }
    [[nodiscard]] const RedisBackend& backend() const noexcept { return *backend_; }

private:
    static std::optional<std::chrono::milliseconds> normalize_ttl(
        std::optional<std::chrono::milliseconds> ttl) {
        if (ttl && ttl->count() == 0) return std::nullopt;
        return ttl;
    }

    [[nodiscard]] std::string tag_key(std::string_view tag) const {
        return options_.tag_prefix + std::string(tag);
    }

    std::shared_ptr<RedisBackend> backend_;
    RedisCacheAdapterOptions options_;
};

} // namespace metal
