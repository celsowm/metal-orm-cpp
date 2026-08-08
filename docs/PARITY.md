# MetalORM C++ parity matrix

The TypeScript `celsowm/metal-orm` repository is the behavioral and architectural reference for this port.

C++26 reflection may replace metadata plumbing, improve compile-time safety, and make APIs more strongly typed. It must not silently redefine MetalORM behavior.

SQLite is intentionally the only backend while semantic parity is being built.

Legend:

- ✅ parity for the supported SQLite execution model
- 🟡 implemented with an explicit remaining edge/sub-gap or deliberate binding adaptation
- ❌ not ported yet

## Runtime and mapping

| MetalORM capability | C++ status | Notes |
| --- | --- | --- |
| Entity/table metadata | ✅ | C++26 annotations + static reflection |
| Primary/generated columns | ✅ | `consteval` validated |
| Identity Map | ✅ | Separate runtime component |
| Unit of Work | ✅ | Separate component; shared DML AST |
| Session coordinator | ✅ | Coordinates UoW, Identity Map and relation processor |
| Dirty snapshots | ✅ | Reflection-generated |
| Persist/remove lifecycle | ✅ | Aligned with TS semantics |
| Transactional `commit()` | ✅ | executor capabilities + rollback restoration |
| Nested transactions/savepoints | ✅ | BEGIN outer; SAVEPOINT/RELEASE inner |
| rollback-only nested failure | ✅ | inner failure poisons outer scope |
| rollback-safe in-memory UoW state | ✅ | status/original/current scalar snapshot checkpoints |
| rollback-safe generated IDs | ✅ | generated PK returns to checkpoint value |
| rollback-safe relation state | ✅ | reflected relation-wrapper snapshots |
| Table lifecycle hooks | ✅/🟡 | lifecycle/timing parity; C++ registration is typed and Session-bound rather than stored in a runtime TableDef |
| Session interceptors | ✅ | `before_flush` / `after_flush` wrap the full flush pipeline |
| Domain events | ✅ | typed queues + handlers; dispatch only after successful outermost commit |
| saveGraph/updateGraph/patchGraph | ❌ | next runtime family |

## Transaction parity — 0.0.14

The executor exposes transaction capabilities explicitly:

```cpp
struct ExecutorCapabilities {
    bool transactions;
    bool savepoints;
};
```

SQLite implements `BEGIN`, `COMMIT`, `ROLLBACK`, `SAVEPOINT`, `RELEASE SAVEPOINT`, and `ROLLBACK TO SAVEPOINT`. Savepoint identifiers are validated before they are interpolated into control SQL.

`Session::transaction()` mirrors the TypeScript nested-transaction contract. A successful nested scope flushes and releases its savepoint. If a nested scope throws, MetalORM rolls back to that savepoint and marks the Session rollback-only. Catching the inner exception does not make the outer transaction committable.

Every transaction/savepoint checkpoint records whether an entity existed in tracking, `EntityStatus`, the dirty-check snapshot, reflected persistent scalar values and rollback-sensitive runtime members. Rollback therefore restores generated IDs, updates, deletes, Identity Map membership and relation wrapper state as well as database rows.

The outer checkpoint survives successful nested releases, so a later outer rollback can still undo work performed by a successfully released inner scope. A failed database `COMMIT` likewise restores the pre-commit ORM state.

## Lifecycle and domain events — 0.0.15

The TypeScript runtime has two distinct lifecycle surfaces and the C++ port keeps them distinct:

1. table/entity hooks execute inside the Unit of Work for INSERT/UPDATE/DELETE;
2. Session interceptors wrap the complete flush pipeline.

Typed table hooks are registered for an entity type:

```cpp
metal::TableHooks<User> hooks;
hooks.before_insert = [](metal::Session&, User& user) {
    user.name = normalize(user.name);
};
hooks.after_insert = [](metal::Session&, User& user) {
    user.domain_events.raise(UserCreated{user.id});
};

session.register_table_hooks<User>(std::move(hooks));
```

The current C++ binding stores that hook set on the Session. The TypeScript binding stores hooks on `TableDef`. This is intentionally recorded as a surface-level adaptation: INSERT/UPDATE/DELETE timing and transactional behavior are aligned, but hook registration lifetime is not claimed to be byte/API-identical.

Table-hook ordering follows the reference UoW:

```text
INSERT: beforeInsert -> INSERT/generated id -> snapshot/identity -> afterInsert
UPDATE: dirty diff -> beforeUpdate -> UPDATE -> refreshed snapshot -> afterUpdate
DELETE: beforeDelete -> DELETE/remove tracking -> afterDelete
```

`afterDelete` retains a live strong reference through the callback even though the entity has already been removed from UoW tracking, matching the observable TypeScript lifecycle.

Session interceptors use:

```cpp
session.register_interceptor({
    .before_flush = [](metal::Session&) {},
    .after_flush = [](metal::Session&) {}
});
```

and the commit/transaction pipeline is:

```text
beforeFlush interceptors
  -> relation prepare
  -> UoW flush
  -> relation process
  -> second UoW flush
  -> afterFlush interceptors
  -> accept relation baselines
  -> COMMIT / RELEASE SAVEPOINT
```

A raw `Session::flush()` remains a UoW-only operation, like the reference runtime: table hooks execute, but Session interceptors, relation processing and domain-event dispatch do not.

Domain events use a C++-typed queue rather than a string discriminator API:

```cpp
using Events = metal::domain_event_queue<UserCreated, UserRenamed>;

[[=metal::mapping::ignore]]
Events domain_events;

session.register_domain_event_handler<UserCreated>(
    [](const UserCreated& event, metal::Session& session) {
        // database commit is already visible here
    });
```

The event queue is a non-persistent runtime member and is therefore explicitly `[[=ignore]]`. Event membership and handler signatures are checked by the C++ type system. Runtime type erasure is internal to `DomainEventBus`; users do not register string event names.

Dispatch timing follows the TypeScript contract:

- no dispatch at `flush()`;
- no dispatch at an inner `RELEASE SAVEPOINT`;
- dispatch only after the successful outermost database COMMIT;
- events raised inside a rolled-back transaction/savepoint are restored with the checkpoint and are not leaked to a later commit;
- if an event handler throws after COMMIT, the exception propagates as a post-commit failure and MetalORM does not pretend the already-committed database work was rolled back;
- as in the TS bus, an entity queue is cleared only after all its handlers complete, so a handler failure leaves the queue available for caller-defined recovery/retry policy.

The dedicated 0.0.15 E2E test covers INSERT/UPDATE/DELETE hook order, raw-flush boundaries, nested-savepoint dispatch timing, rollback of queued events, hook failure, `afterFlush` rollback and handler failure after COMMIT.

## Relations

| MetalORM capability | C++ status | Notes |
| --- | --- | --- |
| belongsTo | ✅ | reflected metadata + eager loading |
| hasOne | ✅ | reflected metadata + eager loading |
| hasMany | ✅/🟡 | dedicated collection; broader JS-object conveniences remain |
| belongsToMany | ✅ | lazy/eager loading, IDs, sync, typed pivot patches, alternate `targetKey` |
| morphTo | ✅ | typed target set, lazy resolution, switching/reset, cascade persist |
| morphOne | ✅ | dedicated reference, lazy/eager loading, mutation/cascade |
| morphMany | ✅ | dedicated collection, lazy/eager loading, mutation/cascade |
| cascade none/all/persist/remove/link | ✅ | aligned vocabulary and runtime semantics |

`has_many_collection<T>` supports `load`, `get_items`, `add`, `attach`, `remove`, `clear`, Session-bound lazy loading and reflected FK assignment.

`many_to_many_collection<T, Pivot>` supports entity/ID attach and detach, `sync_by_ids`, typed pivot hydration, partial `pivot_patch<Pivot>`, alternate non-primary target keys and Identity Map integration after hydration.

`morph_to` encodes discriminator targets at compile time. The discriminator set and target-key compatibility are `consteval` validated. MorphTo intentionally has no single-table JOIN representation; lazy polymorphic resolution is the parity path, matching the TypeScript restriction.

## Query builder and DML

| MetalORM capability | C++ status | Notes |
| --- | --- | --- |
| Typed SELECT AST | ✅ | compile-time entity scope |
| comparisons/logical predicates | ✅ | typed scalar operands |
| IN / NULL / LIKE | ✅ | values and subqueries |
| BETWEEN / NOT BETWEEN | ✅ | first-class expression AST |
| EXISTS / NOT EXISTS | ✅ | typed SELECT subqueries |
| reflected JOINs | ✅ | N:1 / 1:1 / 1:N / N:N |
| projections/aliases | ✅ | columns, aggregates, functions, CASE, windows |
| aggregates/GROUP BY/HAVING | ✅/🟡 | core set; optional SQLite extension functions vary by build |
| CTE / recursive CTE | ✅ | recursive traversal tested on SQLite |
| UNION / UNION ALL / INTERSECT / EXCEPT | ✅ | projection arity validated |
| window functions | ✅ | ranking, NTILE, LAG/LEAD, FIRST/LAST VALUE |
| derived tables / fromSubquery | ✅ | SQLite alias-list restriction diagnosed explicitly |
| CASE | ✅ | searched CASE in projection/predicates |
| SQL function AST | ✅ | recursive typed scalar node + validated generic function helper |
| text/control/date/JSON functions | ✅/🟡 | broad SQLite catalog; TS cross-dialect-only helpers remain backend-specific |
| numeric function catalog | ✅/🟡 | AST surface broad; optional SQLite math support depends on linked build |
| INSERT/UPDATE/DELETE AST | ✅ | public builders + UoW + relation processor share it |
| multi-row INSERT | ✅ | accumulated VALUES rows |
| INSERT ... SELECT | ✅ | typed SELECT source |
| RETURNING | ✅ | INSERT/UPDATE/DELETE |
| SQLite UPSERT | ✅ | conflict target, DO NOTHING/UPDATE, `excluded()` |

## Relation-query parity — 0.0.13

| Capability | Status | Notes |
| --- | --- | --- |
| whereHas | ✅ | reflected correlated EXISTS injected before child/root pagination |
| whereHasNot | ✅ | reflected NOT EXISTS with the same correlation pipeline |
| relation conditions | ✅ | `where_relation<^^Relation>(..., targetPredicate)` |
| relation match | ✅ behavioral | shared correlation engine; SQL shape intentionally differs from TS `INNER JOIN + DISTINCT` |
| N:N relation predicates | ✅ | reflected pivot and target keys |
| MorphOne/MorphMany relation predicates | ✅ | reflected id/type correlation |
| MorphTo whereHas | intentionally unsupported | same physical-target ambiguity as TS |

0.0.13 moved correlation into the SELECT `WHERE` compilation point. Callback-local `ORDER BY / LIMIT / OFFSET` runs after correlation, and root relation predicates run before root pagination. Nested correlation aliases (`t0`, `t0_rel`, ...) prevent alias shadowing while ordinary queries retain stable `t0/t1/p0` aliases.

## Pagination parity — 0.0.13

The raw executor overload remains row-oriented. The Session overload is root-oriented and deduplicates explicit row-multiplying joins by reflected root PK while preserving result order. Cursor pagination supports forward/backward mode, `limit + 1`, multi-column lexicographic keys, mixed ASC/DESC, non-null keys, ordering signatures and tracked root deduplication.

Tracked root pagination currently chooses semantic correctness over a SQL-only optimization and may materialize more candidate rows before deduplication. That is a performance opportunity, not a behavior gap.

## Query architecture

`query.hpp` remains a façade:

```text
query.hpp
  ├── core.hpp
  │    ├── core_types.hpp
  │    └── compiler.hpp -> sqlite_compiler.hpp
  ├── expressions.hpp
  ├── functions.hpp
  ├── select.hpp
  ├── relation_queries.hpp
  ├── relation_match.hpp
  └── pagination.hpp
```

Session-specific tracked pagination remains isolated in `runtime_pagination.hpp`.

## Schema/tooling/ecosystem

| MetalORM capability | C++ status |
| --- | --- |
| SQLite DDL generation | ✅ foundational |
| composite PK DDL | ✅ |
| schema introspection | ❌ |
| schema diff | ❌ |
| migrations/tooling | ❌ |
| DTO/OpenAPI | ❌ |
| Tree/MPTT | ❌ |
| cache layer | ❌ |
| bulk operations | ❌ |
| procedure calls | ❌ |
| pooling | ❌ |
| DB-to-entity code generation | ❌ |

## Ordered parity roadmap

With lifecycle hooks, interceptors and transaction-aware domain-event timing closed in 0.0.15, the next reference gaps are:

1. **0.0.16:** `saveGraph` / `updateGraph` / `patchGraph` and their typed nested relation payload semantics.
2. Then: schema/tooling/ecosystem modules such as introspection/diff, bulk operations, DTO/OpenAPI, cache, Tree/MPTT, pooling and code generation.

A later query-performance pass may replace in-memory root deduplication with a root-aware SQL page plan, provided it preserves the tested 0.0.13 semantics.
