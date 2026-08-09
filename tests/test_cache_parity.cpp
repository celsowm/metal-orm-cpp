#include <metal/metal.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;

struct [[=metal::mapping::table{"cache_users"}]] CacheUser {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::string name;
};

static_assert(metal::reflect::validate_mapping<CacheUser>());

class CountingExecutor final : public metal::DbExecutor {
public:
    explicit CountingExecutor(std::shared_ptr<metal::SQLiteExecutor> inner)
        : inner_(std::move(inner)) {}

    metal::QueryResult execute(
        const std::string& sql,
        const std::vector<metal::Value>& params = {}) override {
        if (starts_with_select(sql)) ++select_count;
        return inner_->execute(sql, params);
    }

    [[nodiscard]] metal::ExecutorCapabilities capabilities() const noexcept override {
        return inner_->capabilities();
    }

    void begin_transaction() override { inner_->begin_transaction(); }
    void commit_transaction() override { inner_->commit_transaction(); }
    void rollback_transaction() override { inner_->rollback_transaction(); }
    void savepoint(std::string_view name) override { inner_->savepoint(name); }
    void release_savepoint(std::string_view name) override { inner_->release_savepoint(name); }
    void rollback_to_savepoint(std::string_view name) override {
        inner_->rollback_to_savepoint(name);
    }

    std::size_t select_count{};

private:
    static bool starts_with_select(std::string_view sql) {
        const auto first = sql.find_first_not_of(" \t\r\n(");
        if (first == std::string_view::npos) return false;
        return sql.substr(first, 6) == "SELECT";
    }

    std::shared_ptr<metal::SQLiteExecutor> inner_;
};

static metal::QueryResult sample_result(std::int64_t id = 1) {
    metal::QueryResult result;
    result.rows.push_back(metal::Row{
        {"id", metal::Value{id}},
        {"name", metal::Value{std::string{"cached"}}}
    });
    return result;
}

int main() {
    assert(metal::parse_duration("30s").count() == 30'000);
    assert(metal::parse_duration("10m").count() == 600'000);
    assert(metal::parse_duration("2h").count() == 7'200'000);
    assert(metal::parse_duration("1d").count() == 86'400'000);
    assert(metal::parse_duration("1w").count() == 604'800'000);
    assert(metal::format_duration(500ms) == "500ms");
    assert(metal::format_duration(30s) == "30s");
    assert(metal::format_duration(10min) == "10m");
    assert(metal::is_valid_duration("15s"));
    assert(!metal::is_valid_duration("15ms"));
    assert(metal::is_valid_duration(std::int64_t{0}));
    assert(!metal::is_valid_duration(std::int64_t{-1}));

    bool bad_duration = false;
    try {
        (void)metal::parse_duration("oops");
    } catch (const std::invalid_argument&) {
        bad_duration = true;
    }
    assert(bad_duration);

    metal::TagIndex tag_index;
    tag_index.register_key("tenant:1:users", {"users", "dashboard"});
    tag_index.register_key("tenant:1:roles", {"roles", "dashboard"});
    assert(tag_index.keys_by_tag("dashboard").size() == 2);
    assert(tag_index.tags_by_key("tenant:1:users").size() == 2);
    const auto dashboard_keys = tag_index.invalidate_tags({"dashboard"});
    assert(dashboard_keys.size() == 2);
    assert(tag_index.all_keys().empty());
    assert(tag_index.all_tags().empty());

    auto memory = std::make_shared<metal::MemoryCacheAdapter>();
    assert(memory->capabilities().tags);
    assert(memory->capabilities().prefix);
    assert(memory->capabilities().ttl);

    memory->set("short", sample_result(), std::chrono::milliseconds{-1});
    assert(!memory->get("short").has_value());

    memory->set("tenant:1:a", sample_result(1), 1h);
    memory->set("tenant:1:b", sample_result(2), 1h);
    memory->set("tenant:2:a", sample_result(3), 1h);
    memory->register_tags("tenant:1:a", {"users"});
    memory->register_tags("tenant:1:b", {"users"});
    assert(memory->cache_stats().size == 3);
    assert(memory->cache_stats().tags == 1);
    memory->invalidate_prefix("tenant:1:");
    assert(!memory->has("tenant:1:a"));
    assert(!memory->has("tenant:1:b"));
    assert(memory->has("tenant:2:a"));
    memory->clear_cache();
    assert(memory->cache_stats().size == 0);

    auto manager_provider = std::make_shared<metal::MemoryCacheAdapter>();
    metal::QueryCacheManager manager{
        manager_provider,
        std::make_shared<metal::DefaultCacheStrategy>(),
        metal::Duration{"1h"}};

    metal::CacheOptions options;
    options.key = "users";
    options.ttl = metal::Duration{"30m"};
    options.tags = {"users", "dashboard"};

    int calls = 0;
    auto first = manager.get_or_execute(options, [&] {
        ++calls;
        return sample_result(10);
    });
    auto second = manager.get_or_execute(options, [&] {
        ++calls;
        return sample_result(20);
    });
    assert(calls == 1);
    assert(metal::from_value<std::int64_t>(first.rows[0].at("id")) == 10);
    assert(metal::from_value<std::int64_t>(second.rows[0].at("id")) == 10);

    const auto stats = manager.stats();
    assert(stats.has_value());
    assert(stats->size == 1);
    assert(stats->tags == 2);

    manager.invalidate_tags({"users"});
    (void)manager.get_or_execute(options, [&] {
        ++calls;
        return sample_result(30);
    });
    assert(calls == 2);

    metal::CacheOptions conditional;
    conditional.key = "conditional";
    conditional.ttl = metal::Duration{"1h"};
    conditional.condition = [](const metal::QueryResult& result) {
        return result.rows.size() > 1;
    };
    int conditional_calls = 0;
    (void)manager.get_or_execute(conditional, [&] {
        ++conditional_calls;
        return sample_result();
    });
    (void)manager.get_or_execute(conditional, [&] {
        ++conditional_calls;
        return sample_result();
    });
    assert(conditional_calls == 2);

    metal::CacheOptions tenant_options;
    tenant_options.key = "dashboard";
    tenant_options.ttl = metal::Duration{"1h"};
    int tenant_calls = 0;
    (void)manager.get_or_execute(
        tenant_options,
        [&] { ++tenant_calls; return sample_result(1); },
        metal::CacheTenantId{std::int64_t{7}});
    (void)manager.get_or_execute(
        tenant_options,
        [&] { ++tenant_calls; return sample_result(2); },
        metal::CacheTenantId{std::int64_t{8}});
    (void)manager.get_or_execute(
        tenant_options,
        [&] { ++tenant_calls; return sample_result(3); },
        metal::CacheTenantId{std::int64_t{7}});
    assert(tenant_calls == 2);
    manager.invalidate_key("dashboard", metal::CacheTenantId{std::int64_t{7}});
    (void)manager.get_or_execute(
        tenant_options,
        [&] { ++tenant_calls; return sample_result(4); },
        metal::CacheTenantId{std::int64_t{7}});
    assert(tenant_calls == 3);

    auto sqlite = std::make_shared<metal::SQLiteExecutor>(":memory:");
    auto counting = std::make_shared<CountingExecutor>(sqlite);
    metal::SQLiteDialect dialect;
    counting->execute(metal::create_table_sql<CacheUser>(dialect));
    counting->execute(
        "INSERT INTO cache_users(id, name) VALUES (1, 'Ada'), (2, 'Grace');");

    metal::Session session{counting};
    auto query = metal::select<CacheUser>();
    query.order_by(metal::field<^^CacheUser::id>);
    auto cached_query = metal::cache_query(
        query,
        "cache-users",
        metal::Duration{"1h"},
        {"cache_users"});

    const auto users1 = cached_query.execute(session, manager);
    assert(users1.size() == 2);
    assert(users1[0]->name == "Ada");
    assert(counting->select_count == 1);
    auto* identity = users1[0].get();

    counting->execute("UPDATE cache_users SET name = 'Ada changed' WHERE id = 1;");
    const auto users2 = cached_query.execute(session, manager);
    assert(counting->select_count == 1);
    assert(users2[0].get() == identity);
    assert(users2[0]->name == "Ada");

    session.clear();
    const auto users3 = cached_query.execute(session, manager);
    assert(counting->select_count == 1);
    assert(users3[0]->name == "Ada");

    manager.invalidate_tags({"cache_users"});
    session.clear();
    const auto users4 = cached_query.execute(session, manager);
    assert(counting->select_count == 2);
    assert(users4[0]->name == "Ada changed");

    const auto auto_options = metal::cache_query(
        metal::select<CacheUser>(),
        "auto-flag",
        metal::Duration{"1m"},
        {},
        true).options();
    assert(auto_options.auto_invalidate);

    manager.clear();
    assert(manager.stats()->size == 0);
    manager.dispose();
}
