#pragma once

#include "metal/query_cache_manager.hpp"
#include "metal/runtime_pagination.hpp"

#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace metal {

namespace detail {

template <typename Query>
concept CacheableSelectQuery = requires(
    const Query& query,
    const Dialect& dialect) {
    typename query_root_t<Query>;
    { query.compile(dialect) } -> std::same_as<CompiledQuery>;
};

} // namespace detail

template <detail::CacheableSelectQuery Query>
QueryResult execute_cached_result(
    const Query& query,
    DbExecutor& executor,
    const Dialect& dialect,
    QueryCacheManager& cache_manager,
    const CacheOptions& options,
    std::optional<CacheTenantId> tenant_id = std::nullopt) {
    const auto compiled = query.compile(dialect);
    return cache_manager.get_or_execute(
        options,
        [&]() { return executor.execute(compiled.sql, compiled.params); },
        std::move(tenant_id));
}

template <detail::CacheableSelectQuery Query>
std::vector<Row> execute_cached_rows(
    const Query& query,
    DbExecutor& executor,
    const Dialect& dialect,
    QueryCacheManager& cache_manager,
    const CacheOptions& options,
    std::optional<CacheTenantId> tenant_id = std::nullopt) {
    return execute_cached_result(
        query,
        executor,
        dialect,
        cache_manager,
        options,
        std::move(tenant_id)).rows;
}

template <detail::CacheableSelectQuery Query>
auto execute_cached(
    const Query& query,
    Session& session,
    QueryCacheManager& cache_manager,
    const CacheOptions& options,
    std::optional<CacheTenantId> tenant_id = std::nullopt)
    -> std::vector<std::shared_ptr<detail::query_root_t<Query>>> {
    using Root = detail::query_root_t<Query>;
    auto result = execute_cached_result(
        query,
        session.executor(),
        session.dialect(),
        cache_manager,
        options,
        std::move(tenant_id));

    std::vector<std::shared_ptr<Root>> out;
    out.reserve(result.rows.size());
    for (const auto& row : result.rows) {
        out.push_back(detail::hydrate_complete_entity_row<Root>(session, row));
    }
    return out;
}

template <detail::CacheableSelectQuery Query>
class CachedQuery {
public:
    CachedQuery(
        Query query,
        CacheOptions options,
        std::optional<CacheTenantId> tenant_id = std::nullopt)
        : query_(std::move(query)),
          options_(std::move(options)),
          tenant_id_(std::move(tenant_id)) {}

    [[nodiscard]] const Query& query() const noexcept { return query_; }
    [[nodiscard]] const CacheOptions& options() const noexcept { return options_; }
    [[nodiscard]] const std::optional<CacheTenantId>& tenant_id() const noexcept {
        return tenant_id_;
    }

    CachedQuery& tenant(CacheTenantId tenant_id) {
        tenant_id_ = std::move(tenant_id);
        return *this;
    }

    CachedQuery& condition(std::function<bool(const QueryResult&)> predicate) {
        options_.condition = std::move(predicate);
        return *this;
    }

    [[nodiscard]] QueryResult execute_result(
        DbExecutor& executor,
        const Dialect& dialect,
        QueryCacheManager& cache_manager) const {
        return execute_cached_result(
            query_, executor, dialect, cache_manager, options_, tenant_id_);
    }

    [[nodiscard]] std::vector<Row> execute_rows(
        DbExecutor& executor,
        const Dialect& dialect,
        QueryCacheManager& cache_manager) const {
        return execute_cached_rows(
            query_, executor, dialect, cache_manager, options_, tenant_id_);
    }

    [[nodiscard]] auto execute(
        Session& session,
        QueryCacheManager& cache_manager) const
        -> std::vector<std::shared_ptr<detail::query_root_t<Query>>> {
        return execute_cached(query_, session, cache_manager, options_, tenant_id_);
    }

private:
    Query query_;
    CacheOptions options_;
    std::optional<CacheTenantId> tenant_id_;
};

template <detail::CacheableSelectQuery Query>
auto cache_query(
    Query query,
    CacheOptions options,
    std::optional<CacheTenantId> tenant_id = std::nullopt) {
    if (options.key.empty()) {
        throw std::invalid_argument("MetalORM: cache key cannot be empty");
    }
    return CachedQuery<Query>{
        std::move(query), std::move(options), std::move(tenant_id)};
}

template <detail::CacheableSelectQuery Query>
auto cache_query(
    Query query,
    std::string key,
    Duration ttl,
    std::vector<std::string> tags = {},
    bool auto_invalidate = false) {
    CacheOptions options;
    options.key = std::move(key);
    options.ttl = ttl;
    options.tags = std::move(tags);
    options.auto_invalidate = auto_invalidate;
    return cache_query(std::move(query), std::move(options));
}

template <detail::CacheableSelectQuery Query>
auto cache(
    Query query,
    CacheOptions options,
    std::optional<CacheTenantId> tenant_id = std::nullopt) {
    return cache_query(
        std::move(query), std::move(options), std::move(tenant_id));
}

template <detail::CacheableSelectQuery Query>
auto cache(
    Query query,
    std::string key,
    Duration ttl,
    std::vector<std::string> tags = {},
    bool auto_invalidate = false) {
    return cache_query(
        std::move(query),
        std::move(key),
        ttl,
        std::move(tags),
        auto_invalidate);
}

} // namespace metal
