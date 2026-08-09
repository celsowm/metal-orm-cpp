#pragma once

#include "metal/cache_memory.hpp"
#include "metal/cache_strategy.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace metal {

class QueryCacheManager {
public:
    explicit QueryCacheManager(
        std::shared_ptr<CacheProvider> provider = std::make_shared<MemoryCacheAdapter>(),
        std::shared_ptr<CacheStrategy> strategy = std::make_shared<DefaultCacheStrategy>(),
        Duration default_ttl = Duration{"1h"})
        : provider_(std::move(provider)),
          strategy_(std::move(strategy)),
          default_ttl_(default_ttl) {
        if (!provider_) throw std::invalid_argument("MetalORM: cache provider cannot be null");
        if (!strategy_) throw std::invalid_argument("MetalORM: cache strategy cannot be null");
    }

    [[nodiscard]] QueryResult get_or_execute(
        const CacheOptions& options,
        std::function<QueryResult()> executor,
        std::optional<CacheTenantId> tenant_id = std::nullopt) {
        if (options.key.empty()) {
            throw std::invalid_argument("MetalORM: cache key cannot be empty");
        }
        if (!executor) {
            throw std::invalid_argument("MetalORM: cached query executor cannot be empty");
        }

        const auto key = strategy_->generate_key(options.key, tenant_id);
        const auto ttl = options.ttl.value_or(default_ttl_).milliseconds();

        if (auto cached = provider_->get(key)) return *cached;

        auto result = executor();
        if (!strategy_->should_cache(result, options)) return result;

        provider_->set(key, result, ttl);
        if (!options.tags.empty()) register_tags(key, options.tags);
        return result;
    }

    void invalidate_key(
        std::string_view key,
        std::optional<CacheTenantId> tenant_id = std::nullopt) {
        provider_->invalidate(strategy_->generate_key(key, tenant_id));
    }

    void invalidate_tags(const std::vector<std::string>& tags) {
        provider_->invalidate_tags(tags);
    }

    void invalidate_prefix(std::string_view prefix) {
        provider_->invalidate_prefix(prefix);
    }

    void clear() {
        auto* clearable = dynamic_cast<CacheClearable*>(provider_.get());
        if (!clearable) {
            throw std::logic_error("MetalORM: cache provider does not support clear operation");
        }
        clearable->clear_cache();
    }

    [[nodiscard]] std::optional<CacheStats> stats() const {
        const auto* provider = dynamic_cast<const CacheStatsProvider*>(provider_.get());
        if (!provider) return std::nullopt;
        return provider->cache_stats();
    }

    void dispose() {
        provider_->dispose();
    }

    [[nodiscard]] CacheProvider& provider() noexcept { return *provider_; }
    [[nodiscard]] const CacheProvider& provider() const noexcept { return *provider_; }
    [[nodiscard]] const CacheStrategy& strategy() const noexcept { return *strategy_; }
    [[nodiscard]] Duration default_ttl() const noexcept { return default_ttl_; }

private:
    void register_tags(std::string_view key, const std::vector<std::string>& tags) {
        if (auto* registrar = dynamic_cast<CacheTagRegistrar*>(provider_.get())) {
            registrar->register_tags(key, tags);
        }
    }

    std::shared_ptr<CacheProvider> provider_;
    std::shared_ptr<CacheStrategy> strategy_;
    Duration default_ttl_;
};

} // namespace metal
