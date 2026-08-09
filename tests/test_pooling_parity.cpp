#include <metal/metal.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace std::chrono_literals;

struct FakeResource {
    int id{};
    bool valid{true};
};

struct PoolCounters {
    std::atomic<int> created{};
    std::atomic<int> destroyed{};
};

static metal::PoolAdapter<FakeResource> fake_resource_adapter(
    const std::shared_ptr<PoolCounters>& counters) {
    metal::PoolAdapter<FakeResource> adapter;
    adapter.create = [counters] {
        const int id = ++counters->created;
        return std::make_unique<FakeResource>(FakeResource{id, true});
    };
    adapter.destroy = [counters](std::unique_ptr<FakeResource> resource) {
        assert(resource);
        ++counters->destroyed;
    };
    adapter.validate = [](FakeResource& resource) { return resource.valid; };
    return adapter;
}

struct FakeExecutorState {
    std::atomic<int> next_id{};
    std::atomic<int> begin_count{};
    std::atomic<int> commit_count{};
    std::atomic<int> rollback_count{};
    std::atomic<int> savepoint_count{};
};

class FakeExecutor final : public metal::DbExecutor {
public:
    explicit FakeExecutor(std::shared_ptr<FakeExecutorState> state)
        : state_(std::move(state)), id_(++state_->next_id) {}

    metal::QueryResult execute(
        const std::string& sql,
        const std::vector<metal::Value>& = {}) override {
        metal::QueryResult result;
        result.rows.push_back({
            {"connection_id", metal::Value{static_cast<std::int64_t>(id_)}},
            {"sql", metal::Value{sql}}
        });
        return result;
    }

    [[nodiscard]] metal::ExecutorCapabilities capabilities() const noexcept override {
        return {true, true};
    }

    void begin_transaction() override {
        if (transaction_active_) throw std::logic_error("fake transaction already active");
        transaction_active_ = true;
        ++state_->begin_count;
    }

    void commit_transaction() override {
        if (!transaction_active_) throw std::logic_error("fake transaction is not active");
        transaction_active_ = false;
        ++state_->commit_count;
    }

    void rollback_transaction() override {
        if (!transaction_active_) return;
        transaction_active_ = false;
        ++state_->rollback_count;
    }

    void savepoint(std::string_view) override {
        if (!transaction_active_) throw std::logic_error("fake savepoint outside transaction");
        ++state_->savepoint_count;
    }

    void release_savepoint(std::string_view) override {
        if (!transaction_active_) throw std::logic_error("fake savepoint outside transaction");
    }

    void rollback_to_savepoint(std::string_view) override {
        if (!transaction_active_) throw std::logic_error("fake savepoint outside transaction");
    }

private:
    std::shared_ptr<FakeExecutorState> state_;
    int id_{};
    bool transaction_active_{};
};

struct [[=metal::mapping::table{"pooled_users"}]] PooledUser {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;
};

static std::int64_t connection_id(const metal::QueryResult& result) {
    assert(result.rows.size() == 1);
    return metal::from_value<std::int64_t>(result.rows.front().at("connection_id"));
}

int main() {
    {
        bool invalid_max = false;
        try {
            auto counters = std::make_shared<PoolCounters>();
            metal::Pool<FakeResource> pool{
                fake_resource_adapter(counters),
                metal::PoolOptions{.max = 0}};
            (void)pool;
        } catch (const std::invalid_argument&) {
            invalid_max = true;
        }
        assert(invalid_max);
    }

    {
        auto counters = std::make_shared<PoolCounters>();
        metal::Pool<FakeResource> pool{
            fake_resource_adapter(counters),
            metal::PoolOptions{.max = 3, .min = 2}};
        assert(counters->created.load() == 2);

        int first_id = 0;
        {
            auto lease = pool.acquire();
            first_id = lease->id;
        }
        auto reused = pool.acquire();
        assert(reused->id == first_id);
        reused.release();
    }

    {
        auto counters = std::make_shared<PoolCounters>();
        metal::Pool<FakeResource> pool{
            fake_resource_adapter(counters),
            metal::PoolOptions{
                .max = 1,
                .acquire_timeout = 25ms
            }};

        auto held = pool.acquire();
        bool timed_out = false;
        try {
            auto blocked = pool.acquire();
            (void)blocked;
        } catch (const std::runtime_error& error) {
            timed_out = std::string{error.what()}.find("timeout") != std::string::npos;
        }
        assert(timed_out);
        held.release();
    }

    {
        auto counters = std::make_shared<PoolCounters>();
        metal::Pool<FakeResource> pool{
            fake_resource_adapter(counters),
            metal::PoolOptions{.max = 1}};

        int invalid_id = 0;
        {
            auto lease = pool.acquire();
            invalid_id = lease->id;
            lease->valid = false;
        }

        auto replacement = pool.acquire();
        assert(replacement->id != invalid_id);
        assert(counters->destroyed.load() == 1);
        replacement.release();
    }

    {
        auto counters = std::make_shared<PoolCounters>();
        metal::Pool<FakeResource> pool{
            fake_resource_adapter(counters),
            metal::PoolOptions{
                .max = 2,
                .idle_timeout = 20ms,
                .reap_interval = 5ms
            }};
        {
            auto first = pool.acquire();
            auto second = pool.acquire();
            (void)first;
            (void)second;
        }
        std::this_thread::sleep_for(80ms);
        assert(counters->destroyed.load() >= 1);
    }

    {
        auto counters = std::make_shared<PoolCounters>();
        metal::Pool<FakeResource> pool{
            fake_resource_adapter(counters),
            metal::PoolOptions{.max = 1}};
        auto lease = pool.acquire();
        pool.destroy();
        lease.release();
        assert(counters->destroyed.load() == 1);

        bool destroyed_pool_rejected = false;
        try {
            auto another = pool.acquire();
            (void)another;
        } catch (const std::runtime_error&) {
            destroyed_pool_rejected = true;
        }
        assert(destroyed_pool_rejected);
    }

    {
        auto state = std::make_shared<FakeExecutorState>();
        metal::PoolAdapter<metal::DbExecutor> adapter;
        adapter.create = [state]() -> std::unique_ptr<metal::DbExecutor> {
            return std::make_unique<FakeExecutor>(state);
        };
        adapter.validate = [](metal::DbExecutor&) { return true; };

        auto pool = std::make_shared<metal::ExecutorPool>(
            std::move(adapter),
            metal::PoolOptions{
                .max = 1,
                .acquire_timeout = 25ms
            });
        metal::PooledExecutorFactory factory{pool, {true, true}};
        auto executor = factory.create_executor();

        const auto first_id = connection_id(executor->execute("outside-transaction"));
        {
            auto probe = pool->acquire();
            auto* fake = dynamic_cast<FakeExecutor*>(&probe.resource());
            assert(fake != nullptr);
        }

        executor->begin_transaction();
        const auto transaction_id = connection_id(executor->execute("inside-transaction"));
        assert(transaction_id == first_id);
        executor->savepoint("sp1");
        executor->release_savepoint("sp1");

        bool transaction_pins_connection = false;
        try {
            auto blocked = pool->acquire();
            (void)blocked;
        } catch (const std::runtime_error&) {
            transaction_pins_connection = true;
        }
        assert(transaction_pins_connection);
        executor->commit_transaction();
        assert(state->begin_count.load() == 1);
        assert(state->commit_count.load() == 1);
        assert(state->savepoint_count.load() == 1);

        {
            auto available_again = pool->acquire();
            (void)available_again;
        }

        bool savepoint_without_transaction = false;
        try {
            executor->savepoint("no_tx");
        } catch (const std::logic_error&) {
            savepoint_without_transaction = true;
        }
        assert(savepoint_without_transaction);

        auto sticky = factory.create_transactional_executor();
        (void)sticky->execute("sticky");
        bool sticky_pins_connection = false;
        try {
            auto blocked = pool->acquire();
            (void)blocked;
        } catch (const std::runtime_error&) {
            sticky_pins_connection = true;
        }
        assert(sticky_pins_connection);
        sticky.reset();
        {
            auto available_after_sticky = pool->acquire();
            (void)available_after_sticky;
        }
    }

    {
        bool isolated_memory_rejected = false;
        try {
            auto pool = metal::create_sqlite_executor_pool(
                ":memory:",
                metal::PoolOptions{.max = 2});
            (void)pool;
        } catch (const std::invalid_argument&) {
            isolated_memory_rejected = true;
        }
        assert(isolated_memory_rejected);
    }

    {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto path = std::filesystem::temp_directory_path()
            / ("metal-orm-pool-" + std::to_string(suffix) + ".sqlite");
        std::filesystem::remove(path);

        auto factory = metal::create_sqlite_pooled_executor_factory(
            path.string(),
            metal::PoolOptions{
                .max = 2,
                .min = 1,
                .acquire_timeout = 250ms
            });
        auto setup = factory.create_executor();
        metal::SQLiteDialect dialect;
        setup->execute(metal::create_table_sql<PooledUser>(dialect));

        metal::Session session{factory.create_executor()};
        auto user = std::make_shared<PooledUser>();
        user->name = "pooled";
        session.persist(user);
        session.commit();
        assert(user->id != 0);

        session.clear();
        const auto loaded = session.find<PooledUser>(user->id);
        assert(loaded);
        assert(loaded->name == "pooled");

        auto second = factory.create_executor();
        const auto count = second->execute("SELECT COUNT(*) AS total FROM pooled_users;");
        assert(metal::from_value<std::int64_t>(count.rows.front().at("total")) == 1);

        setup.reset();
        second.reset();
        factory.destroy();
        std::filesystem::remove(path);
    }
}
