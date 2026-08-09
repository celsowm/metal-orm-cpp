#pragma once

#include "metal/cache_execute.hpp"

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace metal {

class CacheSession {
public:
    CacheSession(
        Session& session,
        QueryCacheManager& cache_manager,
        std::optional<CacheTenantId> tenant_id = std::nullopt)
        : session_(session),
          cache_manager_(cache_manager),
          tenant_id_(std::move(tenant_id)) {}

    template <detail::CacheableSelectQuery Query>
    [[nodiscard]] auto execute(const CachedQuery<Query>& cached)
        -> std::vector<std::shared_ptr<detail::query_root_t<Query>>> {
        const auto tenant = cached.tenant_id().has_value()
            ? cached.tenant_id()
            : tenant_id_;
        return execute_cached(
            cached.query(),
            session_,
            cache_manager_,
            cached.options(),
            tenant);
    }

    template <detail::CacheableSelectQuery Query>
    [[nodiscard]] std::vector<Row> execute_rows(const CachedQuery<Query>& cached) {
        const auto tenant = cached.tenant_id().has_value()
            ? cached.tenant_id()
            : tenant_id_;
        return execute_cached_rows(
            cached.query(),
            session_.executor(),
            session_.dialect(),
            cache_manager_,
            cached.options(),
            tenant);
    }

    void invalidate_cache_key(std::string_view key) {
        cache_manager_.invalidate_key(key, tenant_id_);
    }

    void invalidate_cache_tags(const std::vector<std::string>& tags) {
        cache_manager_.invalidate_tags(tags);
    }

    void invalidate_cache_prefix(std::string_view prefix) {
        cache_manager_.invalidate_prefix(prefix);
    }

    [[nodiscard]] Session& session() noexcept { return session_; }
    [[nodiscard]] QueryCacheManager& cache_manager() noexcept { return cache_manager_; }
    [[nodiscard]] const std::optional<CacheTenantId>& tenant_id() const noexcept {
        return tenant_id_;
    }

private:
    Session& session_;
    QueryCacheManager& cache_manager_;
    std::optional<CacheTenantId> tenant_id_;
};

} // namespace metal
