#pragma once

#include "metal/cache_types.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace metal {

class CacheReader {
public:
    virtual ~CacheReader() = default;
    [[nodiscard]] virtual std::optional<QueryResult> get(std::string_view key) = 0;
    [[nodiscard]] virtual bool has(std::string_view key) = 0;
};

class CacheWriter {
public:
    virtual ~CacheWriter() = default;
    virtual void set(
        std::string key,
        QueryResult value,
        std::optional<std::chrono::milliseconds> ttl = std::nullopt) = 0;
    virtual void delete_key(std::string_view key) = 0;
};

class CacheInvalidator {
public:
    virtual ~CacheInvalidator() = default;
    virtual void invalidate(std::string_view key) = 0;
    virtual void invalidate_tags(const std::vector<std::string>& tags) = 0;
    virtual void invalidate_prefix(std::string_view prefix) = 0;
};

class CacheProvider : public CacheReader, public CacheWriter, public CacheInvalidator {
public:
    ~CacheProvider() override = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual CacheCapabilities capabilities() const noexcept = 0;
    virtual void dispose() {}
};

class CacheTagRegistrar {
public:
    virtual ~CacheTagRegistrar() = default;
    virtual void register_tags(std::string_view key, const std::vector<std::string>& tags) = 0;
};

class CacheClearable {
public:
    virtual ~CacheClearable() = default;
    virtual void clear_cache() = 0;
};

class CacheStatsProvider {
public:
    virtual ~CacheStatsProvider() = default;
    [[nodiscard]] virtual CacheStats cache_stats() const = 0;
};

} // namespace metal
