#pragma once

#include "metal/execution.hpp"
#include "metal/pool.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace metal {

using ExecutorPool = Pool<DbExecutor>;

enum class PooledExecutorMode {
    Session,
    Sticky
};

/**
 * DbExecutor backed by a Pool<DbExecutor>.
 *
 * Session mode acquires/releases per execute() outside a transaction and pins
 * one lease for the whole transaction. Sticky mode keeps one lease between
 * calls until commit/rollback/destruction, mirroring the TypeScript factory's
 * transactional-executor behavior.
 */
class PooledExecutor final : public DbExecutor {
public:
    PooledExecutor(
        std::shared_ptr<ExecutorPool> pool,
        ExecutorCapabilities capabilities,
        PooledExecutorMode mode = PooledExecutorMode::Session);
    ~PooledExecutor() override;

    PooledExecutor(const PooledExecutor&) = delete;
    PooledExecutor& operator=(const PooledExecutor&) = delete;
    PooledExecutor(PooledExecutor&&) = delete;
    PooledExecutor& operator=(PooledExecutor&&) = delete;

    QueryResult execute(const std::string& sql, const std::vector<Value>& params = {}) override;
    [[nodiscard]] ExecutorCapabilities capabilities() const noexcept override;

    void begin_transaction() override;
    void commit_transaction() override;
    void rollback_transaction() override;
    void savepoint(std::string_view name) override;
    void release_savepoint(std::string_view name) override;
    void rollback_to_savepoint(std::string_view name) override;

private:
    DbExecutor& ensure_persistent_executor_locked();
    void destroy_persistent_lease_locked() noexcept;
    void release_persistent_lease_locked() noexcept;
    void dispose_noexcept() noexcept;

    std::shared_ptr<ExecutorPool> pool_;
    ExecutorCapabilities capabilities_;
    PooledExecutorMode mode_;
    mutable std::mutex mutex_;
    std::optional<ExecutorPool::Lease> lease_;
    bool transaction_active_{};
};

/** First-class factory equivalent to TypeScript createPooledExecutorFactory(). */
class PooledExecutorFactory {
public:
    PooledExecutorFactory(
        std::shared_ptr<ExecutorPool> pool,
        ExecutorCapabilities capabilities);

    [[nodiscard]] std::shared_ptr<DbExecutor> create_executor() const;
    [[nodiscard]] std::shared_ptr<DbExecutor> create_transactional_executor() const;
    [[nodiscard]] std::shared_ptr<ExecutorPool> pool() const noexcept { return pool_; }

    void destroy();

private:
    std::shared_ptr<ExecutorPool> pool_;
    ExecutorCapabilities capabilities_;
};

/** Create a pool of independent SQLite connections to the same database file. */
std::shared_ptr<ExecutorPool> create_sqlite_executor_pool(
    std::string filename,
    PoolOptions options);

/** Convenience composition of SQLite connection pool + pooled executor factory. */
PooledExecutorFactory create_sqlite_pooled_executor_factory(
    std::string filename,
    PoolOptions options);

} // namespace metal
