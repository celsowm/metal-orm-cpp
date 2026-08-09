#pragma once

#include "metal/execution.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace metal {

[[nodiscard]] inline bool is_valid_duration(std::string_view value) noexcept {
    if (value.size() < 2) return false;
    const char unit = value.back();
    if (unit != 's' && unit != 'm' && unit != 'h' && unit != 'd' && unit != 'w') return false;
    for (std::size_t i = 0; i + 1 < value.size(); ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
    }
    return true;
}

[[nodiscard]] inline bool is_valid_duration(std::int64_t milliseconds) noexcept {
    return milliseconds >= 0;
}

[[nodiscard]] inline std::chrono::milliseconds parse_duration(std::int64_t milliseconds) noexcept {
    return std::chrono::milliseconds{milliseconds};
}

[[nodiscard]] inline std::chrono::milliseconds parse_duration(std::string_view value) {
    if (!is_valid_duration(value)) {
        throw std::invalid_argument(
            "MetalORM: invalid cache duration '" + std::string(value) +
            "'; use 30s, 10m, 2h, 1d, 1w or milliseconds");
    }

    std::uint64_t amount = 0;
    for (std::size_t i = 0; i + 1 < value.size(); ++i) {
        const auto digit = static_cast<std::uint64_t>(value[i] - '0');
        if (amount > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            throw std::out_of_range("MetalORM: cache duration is too large");
        }
        amount = amount * 10 + digit;
    }

    std::uint64_t multiplier = 1000;
    switch (value.back()) {
        case 's': multiplier = 1000; break;
        case 'm': multiplier = 60'000; break;
        case 'h': multiplier = 3'600'000; break;
        case 'd': multiplier = 86'400'000; break;
        case 'w': multiplier = 604'800'000; break;
        default: break;
    }

    constexpr auto max_ms = static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max());
    if (amount > max_ms / multiplier) {
        throw std::out_of_range("MetalORM: cache duration is too large");
    }
    return std::chrono::milliseconds{
        static_cast<std::int64_t>(amount * multiplier)};
}

[[nodiscard]] inline std::string format_duration(std::chrono::milliseconds value) {
    const auto ms = value.count();
    if (ms < 1000) return std::to_string(ms) + "ms";
    if (ms < 60'000) return std::to_string(ms / 1000) + "s";
    if (ms < 3'600'000) return std::to_string(ms / 60'000) + "m";
    if (ms < 86'400'000) return std::to_string(ms / 3'600'000) + "h";
    if (ms < 604'800'000) return std::to_string(ms / 86'400'000) + "d";
    return std::to_string(ms / 604'800'000) + "w";
}

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
