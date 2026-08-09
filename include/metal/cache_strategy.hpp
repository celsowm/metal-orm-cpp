#pragma once

#include "metal/cache_types.hpp"

#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace metal {

class CacheStrategy {
public:
    virtual ~CacheStrategy() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual std::string generate_key(
        std::string_view query_key,
        const std::optional<CacheTenantId>& tenant_id = std::nullopt) const = 0;
    [[nodiscard]] virtual bool should_cache(
        const QueryResult& result,
        const CacheOptions& options) const = 0;
};

class DefaultCacheStrategy final : public CacheStrategy {
public:
    [[nodiscard]] std::string_view name() const noexcept override {
        return "default";
    }

    [[nodiscard]] std::string generate_key(
        std::string_view query_key,
        const std::optional<CacheTenantId>& tenant_id = std::nullopt) const override {
        if (!tenant_id) return std::string(query_key);
        return std::visit(
            [&](const auto& tenant) {
                if constexpr (std::same_as<std::remove_cvref_t<decltype(tenant)>, std::string>) {
                    return std::string("tenant:") + tenant + ":" + std::string(query_key);
                } else {
                    return std::string("tenant:") + std::to_string(tenant) + ":" + std::string(query_key);
                }
            },
            *tenant_id);
    }

    [[nodiscard]] bool should_cache(
        const QueryResult& result,
        const CacheOptions& options) const override {
        return !options.condition || options.condition(result);
    }
};

} // namespace metal
