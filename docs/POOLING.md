# Connection pooling — 0.0.27

MetalORM C++ mirrors the TypeScript `Pool<TResource>` and pooled-executor behavior while using C++ ownership instead of promise-managed leases.

## Generic pool

```cpp
metal::PoolAdapter<MyConnection> adapter;
adapter.create = [] {
    return std::make_unique<MyConnection>();
};
adapter.validate = [](MyConnection& connection) {
    return connection.ping();
};

metal::Pool<MyConnection> pool{
    std::move(adapter),
    metal::PoolOptions{
        .max = 10,
        .min = 2,
        .idle_timeout = std::chrono::seconds{30},
        .acquire_timeout = std::chrono::seconds{5}
    }};

{
    auto lease = pool.acquire();
    lease->query("SELECT 1");
} // RAII: returned automatically
```

`PoolOptions` preserves the reference semantics:

- `max`: maximum live resources, counting idle, leased and currently being created resources;
- `min`: best-effort warm idle floor, clamped to `max`;
- `idle_timeout`: idle lifetime before reaping;
- `reap_interval`: optional explicit reaper cadence; otherwise `idle_timeout / 2` with a one-second minimum;
- `acquire_timeout`: maximum blocking acquisition time; zero means no timeout.

`PoolAdapter<T>` exposes `create`, optional `destroy`, and optional `validate`. Invalid idle resources are destroyed and replaced on demand.

## RAII leases

`Pool<T>::Lease` is move-only. Its destructor returns the resource automatically. Explicit operations are idempotent:

- `release()` returns the resource to the pool;
- `destroy()` permanently removes it and frees capacity.

Destroying a pool immediately rejects future acquisitions and destroys idle resources. Resources that are still leased are destroyed when their leases are returned rather than being reinserted into a dead pool.

## Pooled DbExecutor

```cpp
auto factory = metal::create_sqlite_pooled_executor_factory(
    "app.sqlite",
    metal::PoolOptions{
        .max = 8,
        .min = 1,
        .acquire_timeout = std::chrono::seconds{5}
    });

metal::Session session{factory.create_executor()};
```

A normal pooled executor follows the TypeScript session-mode contract:

- outside a transaction, each `execute()` acquires and releases a lease around the call;
- `begin_transaction()` pins one lease;
- every query/savepoint inside that transaction uses the same underlying connection;
- commit/rollback returns the lease;
- a failed begin/commit/rollback destroys the pinned connection instead of returning an unknown transaction state to the pool;
- destroying an executor with an active transaction attempts rollback before releasing the lease.

`create_transactional_executor()` returns the reference-style sticky executor. It retains one lease across calls until commit, rollback or executor destruction.

## SQLite

`create_sqlite_executor_pool()` creates independent `SQLiteExecutor` connections and validates idle connections with `SELECT 1` before reuse.

A filename-backed SQLite database can safely use multiple pooled connections. `:memory:` is intentionally rejected when `max > 1`, because ordinary SQLite in-memory connections are isolated databases; silently pooling them would make different requests observe different schemas/data.

The generic `Pool<T>` is database-independent. SQLite remains MetalORM C++'s only production executor while semantic parity is being completed.
