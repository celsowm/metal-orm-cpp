#include "metal/pooling.hpp"

#include <stdexcept>
#include <utility>

namespace metal {

PooledExecutor::PooledExecutor(
    std::shared_ptr<ExecutorPool> pool,
    ExecutorCapabilities capabilities,
    PooledExecutorMode mode)
    : pool_(std::move(pool)), capabilities_(capabilities), mode_(mode) {
    if (!pool_) throw std::invalid_argument("MetalORM: pooled executor requires a pool");
}

PooledExecutor::~PooledExecutor() {
    dispose_noexcept();
}

ExecutorCapabilities PooledExecutor::capabilities() const noexcept {
    return capabilities_;
}

DbExecutor& PooledExecutor::ensure_persistent_executor_locked() {
    if (!lease_) {
        auto acquired = pool_->acquire();
        lease_.emplace(std::move(acquired));
    }
    return lease_->resource();
}

QueryResult PooledExecutor::execute(
    const std::string& sql,
    const std::vector<Value>& params) {
    // Serialize lease selection and execution for one pooled executor. This is
    // stronger than the single-threaded TypeScript runtime and prevents a
    // concurrent begin_transaction() from racing an otherwise temporary lease.
    std::lock_guard lock(mutex_);

    if (mode_ == PooledExecutorMode::Sticky || lease_) {
        return ensure_persistent_executor_locked().execute(sql, params);
    }

    auto lease = pool_->acquire();
    return lease.resource().execute(sql, params);
}

void PooledExecutor::begin_transaction() {
    if (!capabilities_.transactions) {
        throw std::logic_error("MetalORM: pooled executor does not support transactions");
    }

    std::lock_guard lock(mutex_);
    if (transaction_active_) {
        throw std::logic_error("MetalORM: pooled executor transaction is already active");
    }

    auto& executor = ensure_persistent_executor_locked();
    try {
        executor.begin_transaction();
        transaction_active_ = true;
    } catch (...) {
        destroy_persistent_lease_locked();
        throw;
    }
}

void PooledExecutor::commit_transaction() {
    if (!capabilities_.transactions) {
        throw std::logic_error("MetalORM: pooled executor does not support transactions");
    }

    ExecutorPool::Lease completed;
    {
        std::lock_guard lock(mutex_);
        if (!transaction_active_ || !lease_) {
            throw std::logic_error("MetalORM: commit_transaction called without an active transaction");
        }

        try {
            lease_->resource().commit_transaction();
        } catch (...) {
            destroy_persistent_lease_locked();
            throw;
        }

        transaction_active_ = false;
        completed = std::move(*lease_);
        lease_.reset();
    }
    completed.release();
}

void PooledExecutor::rollback_transaction() {
    if (!capabilities_.transactions) {
        throw std::logic_error("MetalORM: pooled executor does not support transactions");
    }

    ExecutorPool::Lease completed;
    {
        std::lock_guard lock(mutex_);
        if (!transaction_active_ || !lease_) return;

        try {
            lease_->resource().rollback_transaction();
        } catch (...) {
            destroy_persistent_lease_locked();
            throw;
        }

        transaction_active_ = false;
        completed = std::move(*lease_);
        lease_.reset();
    }
    completed.release();
}

void PooledExecutor::savepoint(std::string_view name) {
    if (!capabilities_.savepoints) {
        throw std::logic_error("MetalORM: savepoints are not supported by this pooled executor");
    }
    std::lock_guard lock(mutex_);
    if (!transaction_active_ || !lease_) {
        throw std::logic_error("MetalORM: savepoint called without an active transaction");
    }
    lease_->resource().savepoint(name);
}

void PooledExecutor::release_savepoint(std::string_view name) {
    if (!capabilities_.savepoints) {
        throw std::logic_error("MetalORM: savepoints are not supported by this pooled executor");
    }
    std::lock_guard lock(mutex_);
    if (!transaction_active_ || !lease_) {
        throw std::logic_error("MetalORM: release_savepoint called without an active transaction");
    }
    lease_->resource().release_savepoint(name);
}

void PooledExecutor::rollback_to_savepoint(std::string_view name) {
    if (!capabilities_.savepoints) {
        throw std::logic_error("MetalORM: savepoints are not supported by this pooled executor");
    }
    std::lock_guard lock(mutex_);
    if (!transaction_active_ || !lease_) {
        throw std::logic_error("MetalORM: rollback_to_savepoint called without an active transaction");
    }
    lease_->resource().rollback_to_savepoint(name);
}

void PooledExecutor::destroy_persistent_lease_locked() noexcept {
    transaction_active_ = false;
    if (!lease_) return;
    try {
        lease_->destroy();
    } catch (...) {}
    lease_.reset();
}

void PooledExecutor::release_persistent_lease_locked() noexcept {
    if (!lease_) return;
    try {
        lease_->release();
    } catch (...) {}
    lease_.reset();
}

void PooledExecutor::dispose_noexcept() noexcept {
    std::lock_guard lock(mutex_);
    if (!lease_) return;

    if (transaction_active_) {
        try {
            lease_->resource().rollback_transaction();
            transaction_active_ = false;
            release_persistent_lease_locked();
            return;
        } catch (...) {
            destroy_persistent_lease_locked();
            return;
        }
    }

    release_persistent_lease_locked();
}

PooledExecutorFactory::PooledExecutorFactory(
    std::shared_ptr<ExecutorPool> pool,
    ExecutorCapabilities capabilities)
    : pool_(std::move(pool)), capabilities_(capabilities) {
    if (!pool_) throw std::invalid_argument("MetalORM: pooled executor factory requires a pool");
}

std::shared_ptr<DbExecutor> PooledExecutorFactory::create_executor() const {
    return std::make_shared<PooledExecutor>(
        pool_, capabilities_, PooledExecutorMode::Session);
}

std::shared_ptr<DbExecutor> PooledExecutorFactory::create_transactional_executor() const {
    return std::make_shared<PooledExecutor>(
        pool_, capabilities_, PooledExecutorMode::Sticky);
}

void PooledExecutorFactory::destroy() {
    pool_->destroy();
}

std::shared_ptr<ExecutorPool> create_sqlite_executor_pool(
    std::string filename,
    PoolOptions options) {
    if (filename == ":memory:" && options.max > 1) {
        throw std::invalid_argument(
            "MetalORM: a SQLite :memory: pool cannot use max > 1 because each connection has an isolated database");
    }

    PoolAdapter<DbExecutor> adapter;
    adapter.create = [filename = std::move(filename)]() -> std::unique_ptr<DbExecutor> {
        return std::make_unique<SQLiteExecutor>(filename);
    };
    adapter.validate = [](DbExecutor& executor) {
        try {
            const auto result = executor.execute("SELECT 1 AS metalorm_pool_health;");
            return !result.rows.empty();
        } catch (...) {
            return false;
        }
    };

    return std::make_shared<ExecutorPool>(std::move(adapter), options);
}

PooledExecutorFactory create_sqlite_pooled_executor_factory(
    std::string filename,
    PoolOptions options) {
    return PooledExecutorFactory{
        create_sqlite_executor_pool(std::move(filename), options),
        ExecutorCapabilities{true, true}
    };
}

} // namespace metal
