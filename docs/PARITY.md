# MetalORM C++ parity matrix

The TypeScript `celsowm/metal-orm` repository is the behavioral and architectural reference for this port.

C++26 reflection may replace metadata plumbing, improve compile-time safety, and make APIs more strongly typed. It must not silently redefine MetalORM behavior.

SQLite is intentionally the only backend while semantic parity is being built.

Legend:

- ✅ parity for the supported SQLite execution model
- 🟡 implemented with an explicit remaining edge/sub-gap
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
| Interceptors/hooks | ❌ | next runtime family |
| Domain events | ❌ | next runtime family |
| saveGraph/updateGraph/patchGraph | ❌ | later runtime family |

## Transaction parity — 0.0.14

The executor now exposes transaction capabilities explicitly:

```cpp
struct ExecutorCapabilities {
    bool transactions;
    bool savepoints;
};
```

SQLite advertises both capabilities and implements `BEGIN`, `COMMIT`, `ROLLBACK`, `SAVEPOINT`, `RELEASE SAVEPOINT`, and `ROLLBACK TO SAVEPOINT`. Savepoint identifiers are validated before being interpolated into control SQL.

The scoped runtime API mirrors TypeScript semantics:

```cpp
session.transaction([](metal::Session& tx) {
    // mutations

    tx.transaction([](metal::Session& nested) {
        // nested scope => SAVEPOINT metalorm_sp_1
    });
});
```

A successful nested scope flushes and releases its savepoint. If a nested scope throws, MetalORM rolls back to the savepoint and marks the session rollback-only. Catching that inner exception does **not** make the outer transaction committable; the outer scope subsequently rolls back.

C++ additionally checkpoints the synchronous in-memory state mutated by its Unit of Work. Every transaction/savepoint checkpoint records:

- whether each entity was already tracked;
- `EntityStatus` and dirty-check `original` snapshot;
- current persistent scalar values through reflection;
- reflected relation-wrapper state, including collection baselines/pivot patches/morph references.

That means rollback restores more than the SQLite rows. It also:

- restores an updated object's scalar values to the transaction-entry state;
- resurrects a tracked entity removed and flushed inside the failed transaction;
- removes objects that became tracked only inside the failed scope;
- removes rolled-back identities from the Identity Map and rebuilds valid identities;
- resets a generated primary key assigned by an INSERT that did not commit;
- restores relation collection/baseline state even when an inner savepoint had already successfully flushed and accepted the relation before the outer transaction failed.

The outer checkpoint survives successful nested savepoint releases, so outer rollback remains capable of undoing all work performed by successful inner scopes.

`commit()` uses the same checkpoint mechanism. If the database `COMMIT` fails, SQLite is rolled back and the pre-commit UoW/entity state is restored rather than leaving snapshots or generated keys falsely marked as committed.

Nested transactions on an executor that reports `savepoints == false` fail deterministically instead of attempting a second `BEGIN`.

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

### Collection parity

`has_many_collection<T>` supports `load`, `get_items`, `add`, `attach`, `remove`, `clear`, Session-bound lazy loading and reflected FK assignment.

`many_to_many_collection<T, Pivot>` supports entity/ID attach and detach, `sync_by_ids`, typed pivot hydration, partial `pivot_patch<Pivot>`, alternate non-primary target keys and Identity Map integration after hydration.

### Polymorphic relations

`morph_to` encodes discriminator targets at compile time:

```cpp
[[=metal::mapping::morph_to<
    ^^Activity::subject_type,
    ^^Activity::subject_id,
    metal::mapping::cascade_mode::persist,
    metal::mapping::morph_target<"post", ^^Post>,
    metal::mapping::morph_target<"video", ^^Video>>{}]]
metal::morph_to_reference<Post, Video> subject;
```

The discriminator set and target-key compatibility are `consteval` validated. `MorphTo` intentionally has no single-table JOIN representation; lazy polymorphic resolution is the parity path, matching the TypeScript restriction.

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

0.0.13 moved correlation into the SELECT `WHERE` compilation point. Callback-local `ORDER BY / LIMIT / OFFSET` therefore runs after correlation, and root relation predicates are applied before root pagination. Nested correlation aliases (`t0`, `t0_rel`, ...) prevent alias shadowing while ordinary queries retain stable `t0/t1/p0` aliases.

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

With transaction/savepoint semantics and rollback-safe runtime state closed in 0.0.14, the next reference gaps are:

1. **0.0.15:** table hooks / Session interceptors and domain events, with dispatch timing tied to successful outermost commit.
2. Then: `saveGraph` / `updateGraph` / `patchGraph`.
3. Then: schema/tooling/ecosystem modules such as introspection/diff, bulk operations, DTO/OpenAPI, cache, Tree/MPTT, pooling and code generation.

A later query-performance pass may replace in-memory root deduplication with a root-aware SQL page plan, provided it preserves the tested 0.0.13 semantics.
