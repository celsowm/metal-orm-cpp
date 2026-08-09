#pragma once

#include "metal/execution.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace metal {

[[nodiscard]] std::chrono::milliseconds parse_duration(std::string_view value);
[[nodiscard]] std::chrono::milliseconds parse_duration(std::int64_t milliseconds) noexcept;
[[nodiscard]] std::string format_duration(std::chrono::milliseconds value);
[[nodiscard]] bool is_valid_duration(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_duration(std::int64_t milliseconds) noexcept;

class Duration {
public:
    Duration() : milliseconds_(0) {}
    Duration(std::chrono::milliseconds value) : milliseconds_(value) {}
    Duration(std::int64_t milliseconds) : milliseconds_(milliseconds) {}
    Duration(std::string_view value) : milliseconds_(parse_duration(value)) {}

    [[nodiscard]] std::chrono::milliseconds milliseconds() const noexcept {
        return milliseconds_;
    }

private:
    std::chrono::milliseconds milliseconds_;
};

enum class InvalidationStrategy {
    tags,
    entity,
    prefix,
    key,
    ttl
};

struct CacheCapabilities {
    bool tags{false};
    bool prefix{false};
    bool ttl{false};
};

using CacheTenantId = std::variant<std::int64_t, std::string>;

struct CacheOptions {
    std::string key;
    std::optional<Duration> ttl;
    std::vector<std::string> tags;
    bool auto_invalidate{false};
    std::function<bool(const QueryResult&)> condition;
};

struct CacheState {
    std::optional<CacheOptions> options;
};

struct CacheStats {
    std::size_t size{};
    std::size_t tags{};
};

} // namespace metal
