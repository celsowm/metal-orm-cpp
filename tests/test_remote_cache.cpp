#include <metal/cache_remote.hpp>
#include <metal/query_cache_manager.hpp>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

metal::QueryResult rich_result(std::int64_t id = 7) {
    metal::QueryResult result;
    result.affected_rows = 3;
    result.last_insert_id = 99;
    result.rows.push_back(metal::Row{
        {"null_value", metal::Value{nullptr}},
        {"id", metal::Value{id}},
        {"ratio", metal::Value{3.25}},
        {"text", metal::Value{std::string{"a\0b", 3}}},
        {"enabled", metal::Value{true}},
        {"payload", metal::Value{metal::Blob{
            std::byte{0x00}, std::byte{0xff}, std::byte{0x41}}}}
    });
    return result;
}

void assert_rich_result(const metal::QueryResult& result, std::int64_t id = 7) {
    assert(result.affected_rows == 3);
    assert(result.last_insert_id == 99);
    assert(result.rows.size() == 1);
    const auto& row = result.rows.front();
    assert(std::holds_alternative<std::nullptr_t>(row.at("null_value")));
    assert(metal::from_value<std::int64_t>(row.at("id")) == id);
    assert(metal::from_value<double>(row.at("ratio")) == 3.25);
    assert(metal::from_value<std::string>(row.at("text")) == std::string("a\0b", 3));
    assert(metal::from_value<bool>(row.at("enabled")));
    assert(metal::from_value<metal::Blob>(row.at("payload")) == metal::Blob({
        std::byte{0x00}, std::byte{0xff}, std::byte{0x41}}));
}

class FakeKeyValueBackend final :
    public metal::KeyValueBackend,
    public metal::KeyValuePrefixBackend {
public:
    std::optional<std::string> get(std::string_view key) override {
        const auto found = values.find(std::string(key));
        if (found == values.end()) return std::nullopt;
        return found->second;
    }

    void set(
        std::string key,
        std::string value,
        std::optional<std::chrono::milliseconds> ttl) override {
        last_ttl = ttl;
        values[std::move(key)] = std::move(value);
    }

    void delete_key(std::string_view key) override {
        values.erase(std::string(key));
    }

    std::vector<std::string> keys_with_prefix(std::string_view prefix) override {
        std::vector<std::string> result;
        for (const auto& [key, _] : values) {
            if (key.starts_with(prefix)) result.push_back(key);
        }
        return result;
    }

    void dispose() override { ++dispose_count; }

    std::unordered_map<std::string, std::string> values;
    std::optional<std::chrono::milliseconds> last_ttl;
    int dispose_count{};
};

class FakeNoPrefixBackend final : public metal::KeyValueBackend {
public:
    std::optional<std::string> get(std::string_view key) override {
        const auto found = values.find(std::string(key));
        if (found == values.end()) return std::nullopt;
        return found->second;
    }

    void set(
        std::string key,
        std::string value,
        std::optional<std::chrono::milliseconds>) override {
        values[std::move(key)] = std::move(value);
    }

    void delete_key(std::string_view key) override {
        values.erase(std::string(key));
    }

    std::unordered_map<std::string, std::string> values;
};

class FakeRedisBackend final : public metal::RedisBackend {
public:
    std::optional<std::string> get(std::string_view key) override {
        const auto found = values.find(std::string(key));
        if (found == values.end()) return std::nullopt;
        return found->second;
    }

    void set(
        std::string key,
        std::string value,
        std::optional<std::chrono::milliseconds> ttl) override {
        last_ttl = ttl;
        values[std::move(key)] = std::move(value);
    }

    void delete_key(std::string_view key) override {
        const std::string owned(key);
        values.erase(owned);
        sets.erase(owned);
    }

    std::vector<std::string> keys_with_prefix(std::string_view prefix) override {
        std::set<std::string> unique;
        for (const auto& [key, _] : values) {
            if (key.starts_with(prefix)) unique.insert(key);
        }
        for (const auto& [key, _] : sets) {
            if (key.starts_with(prefix)) unique.insert(key);
        }
        return {unique.begin(), unique.end()};
    }

    void add_set_members(
        std::string_view key,
        const std::vector<std::string>& members) override {
        auto& target = sets[std::string(key)];
        target.insert(members.begin(), members.end());
    }

    std::vector<std::string> set_members(std::string_view key) override {
        const auto found = sets.find(std::string(key));
        if (found == sets.end()) return {};
        return {found->second.begin(), found->second.end()};
    }

    void delete_keys(const std::vector<std::string>& keys) override {
        ++delete_batches;
        for (const auto& key : keys) delete_key(key);
    }

    void dispose() override { ++dispose_count; }

    std::unordered_map<std::string, std::string> values;
    std::unordered_map<std::string, std::set<std::string>> sets;
    std::optional<std::chrono::milliseconds> last_ttl;
    int delete_batches{};
    int dispose_count{};
};

} // namespace

int main() {
    const auto encoded = metal::encode_query_result(rich_result());
    assert(encoded.size() > 8);
    assert_rich_result(metal::decode_query_result(encoded));
    assert(metal::encode_query_result(rich_result()) == encoded);

    bool bad_magic = false;
    try {
        auto corrupted = encoded;
        corrupted[0] = 'X';
        (void)metal::decode_query_result(corrupted);
    } catch (const metal::CacheCodecError&) {
        bad_magic = true;
    }
    assert(bad_magic);

    bool trailing = false;
    try {
        (void)metal::decode_query_result(encoded + "x");
    } catch (const metal::CacheCodecError&) {
        trailing = true;
    }
    assert(trailing);

    auto key_value_backend = std::make_shared<FakeKeyValueBackend>();
    metal::KeyValueCacheAdapter key_value{
        key_value_backend,
        metal::KeyValueCacheAdapterOptions{.dispose_backend = true}};
    assert(key_value.name() == "key-value");
    assert(!key_value.capabilities().tags);
    assert(key_value.capabilities().prefix);
    assert(key_value.capabilities().ttl);

    key_value.set("tenant:1:a", rich_result(1), 5min);
    assert(key_value_backend->last_ttl == std::optional<std::chrono::milliseconds>{5min});
    assert_rich_result(*key_value.get("tenant:1:a"), 1);
    assert(key_value.has("tenant:1:a"));

    key_value.set("tenant:1:b", rich_result(2), std::chrono::milliseconds{0});
    assert(!key_value_backend->last_ttl.has_value());
    key_value.set("tenant:2:a", rich_result(3), 1h);
    key_value.invalidate_prefix("tenant:1:");
    assert(!key_value.has("tenant:1:a"));
    assert(!key_value.has("tenant:1:b"));
    assert(key_value.has("tenant:2:a"));

    bool key_value_tags_rejected = false;
    try {
        key_value.invalidate_tags({"users"});
    } catch (const std::logic_error&) {
        key_value_tags_rejected = true;
    }
    assert(key_value_tags_rejected);
    key_value.dispose();
    assert(key_value_backend->dispose_count == 1);

    auto no_prefix_backend = std::make_shared<FakeNoPrefixBackend>();
    metal::KeyValueCacheAdapter no_prefix{no_prefix_backend};
    assert(!no_prefix.capabilities().prefix);
    bool prefix_rejected = false;
    try {
        no_prefix.invalidate_prefix("tenant:");
    } catch (const std::logic_error&) {
        prefix_rejected = true;
    }
    assert(prefix_rejected);

    auto redis_backend = std::make_shared<FakeRedisBackend>();
    auto redis = std::make_shared<metal::RedisCacheAdapter>(
        redis_backend,
        metal::RedisCacheAdapterOptions{
            .tag_prefix = "metal:tag:",
            .dispose_backend = true});
    assert(redis->name() == "redis");
    assert(redis->capabilities().tags);
    assert(redis->capabilities().prefix);
    assert(redis->capabilities().ttl);

    metal::QueryCacheManager manager{
        redis,
        std::make_shared<metal::DefaultCacheStrategy>(),
        metal::Duration{"1h"}};

    metal::CacheOptions options;
    options.key = "dashboard";
    options.ttl = metal::Duration{"30m"};
    options.tags = {"users", "dashboard"};

    int calls = 0;
    const auto first = manager.get_or_execute(
        options,
        [&] {
            ++calls;
            return rich_result(10);
        },
        metal::CacheTenantId{std::int64_t{7}});
    const auto second = manager.get_or_execute(
        options,
        [&] {
            ++calls;
            return rich_result(20);
        },
        metal::CacheTenantId{std::int64_t{7}});

    assert(calls == 1);
    assert_rich_result(first, 10);
    assert_rich_result(second, 10);
    assert(redis_backend->last_ttl == std::optional<std::chrono::milliseconds>{30min});
    assert(redis_backend->sets.at("metal:tag:users").contains("tenant:7:dashboard"));
    assert(redis_backend->sets.at("metal:tag:dashboard").contains("tenant:7:dashboard"));

    manager.invalidate_tags({"users"});
    assert(!redis->has("tenant:7:dashboard"));
    assert(!redis_backend->sets.contains("metal:tag:users"));

    (void)manager.get_or_execute(
        options,
        [&] {
            ++calls;
            return rich_result(30);
        },
        metal::CacheTenantId{std::int64_t{7}});
    assert(calls == 2);

    redis->set("tenant:7:a", rich_result(1), 1h);
    redis->set("tenant:7:b", rich_result(2), 1h);
    redis->set("tenant:8:a", rich_result(3), 1h);
    redis->invalidate_prefix("tenant:7:");
    assert(!redis->has("tenant:7:a"));
    assert(!redis->has("tenant:7:b"));
    assert(redis->has("tenant:8:a"));
    assert(redis_backend->delete_batches > 0);

    manager.dispose();
    assert(redis_backend->dispose_count == 1);
}
