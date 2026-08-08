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
| Table lifecycle hooks | ✅ | both implementations are Session-bound; C++ registration is entity-type-safe |
| Session interceptors | ✅ | `before_flush` / `after_flush` wrap the full flush pipeline |
| Domain events | ✅ | typed queues + handlers; dispatch only after successful outermost commit |
| saveGraph/updateGraph/patchGraph | ✅ | typed nested graph payloads, pivots, pruning, single references and transaction integration |

## Transaction parity — 0.0.14

The executor exposes transaction capabilities explicitly. SQLite implements `BEGIN`, `COMMIT`, `ROLLBACK`, `SAVEPOINT`, `RELEASE SAVEPOINT`, and `ROLLBACK TO SAVEPOINT`; savepoint identifiers are validated before interpolation.

`Session::transaction()` mirrors the TypeScript nested-transaction contract. A successful nested scope flushes and releases its savepoint. If a nested scope throws, MetalORM rolls back to that savepoint and marks the Session rollback-only. Catching the inner exception does not make the outer transaction committable.

Every transaction/savepoint checkpoint records whether an entity existed in tracking, `EntityStatus`, the dirty-check snapshot, reflected persistent scalar values and rollback-sensitive runtime members. Rollback restores generated IDs, updates, deletes, Identity Map membership and relation wrapper state as well as database rows.

## Lifecycle and domain events — 0.0.15

The runtime has two distinct lifecycle surfaces:

1. entity/table lifecycle hooks execute inside the Unit of Work for INSERT/UPDATE/DELETE;
2. Session interceptors wrap the complete flush pipeline.

```cpp
metal::TableHooks<User> hooks;
hooks.before_insert = [](metal::Session&, User& user) {
    user.name = normalize(user.name);
};

session.register_table_hooks<User>(std::move(hooks));
```

After the corresponding TypeScript refactor on 2026-08-08, lifecycle policy is Session-bound in **both** repositories. `TableDef`/mapping metadata no longer owns runtime hooks in the TypeScript reference either. The C++ API additionally uses the entity type as its public registration key.

Table-hook ordering follows the reference UoW:

```text
INSERT: beforeInsert -> INSERT/generated id -> snapshot/identity -> afterInsert
UPDATE: dirty diff -> beforeUpdate -> UPDATE -> refreshed snapshot -> afterUpdate
DELETE: beforeDelete -> DELETE/remove tracking -> afterDelete
```

Raw `Session::flush()` remains UoW-only: table hooks execute, but Session interceptors, relation processing and domain-event dispatch do not.

Domain events use typed queues and dispatch only after a successful outermost COMMIT. SAVEPOINT release never dispatches. Event queues participate in transaction checkpoints, and post-COMMIT handler errors propagate without a fake database rollback.

## Graph persistence — 0.0.16

The JavaScript/TypeScript implementation accepts DTO-like object payloads. The C++ port expresses the same semantic payload through reflected builders so invalid entity fields, relation targets and scalar value types can fail at compile time.

```cpp
auto payload = metal::graph<User>()
    .set<^^User::name>(std::string{"Celso"})
    .relation<^^User::profile>(
        metal::graph<Profile>()
            .set<^^Profile::bio>(std::string{"C++26"}))
    .relation<^^User::posts>([](auto& posts) {
        posts.add(
            metal::graph<Post>()
                .set<^^Post::title>(std::string{"Reflection"}));
        posts.add_id(42);
    });

auto user = metal::save_graph(session, payload);
```

The public operations are `save_graph`, `update_graph` and `patch_graph`. Omitted fields and relations are untouched. `GraphOptions::prune_missing` removes/detaches collection members not represented in the graph. Collection payloads support nested graph values, existing entities and IDs; N:N entries additionally accept `pivot_patch<Pivot>` and respect alternate `targetKey` metadata.

Graph execution composes with `Session::transaction()` by default, therefore database writes, generated IDs, runtime relation state and queued domain events remain under the transaction checkpoint.

The C++ graph API is intentionally not a dynamic `Record<string, unknown>` clone. Its stronger reflected shape is a language-binding adaptation; graph behavior remains the parity target.

## Dedicated single references — 0.0.17

`belongsTo` and `hasOne` now use dedicated wrappers exclusively:

```cpp
metal::belongs_to_reference<User> author;
metal::has_one_reference<Profile> profile;
```

A raw `std::shared_ptr<T>` member annotated as either relation is rejected by mapping validation. There is no legacy compatibility specialization.

Both wrappers expose:

```text
load / loaded
get
set / reset
dirty
```

Session binding supplies lazy loaders. Eager and lazy hydration call `_metal_hydrate()` internally, which establishes `current == baseline`; reading a relation therefore never creates a false mutation. User `set/reset` changes `current` while preserving the baseline until a successful commit accepts the relation state.

For `belongsTo`, `set()` synchronizes the root FK immediately when the target relation key is already available. If a configured cascade-persist target receives a generated key during the first UoW flush, relation processing resolves the key afterward and the second UoW flush persists the root FK. Clearing requires a nullable root FK in the C++ type.

For `hasOne`, the child carries the FK. Replacing a loaded reference attaches the new child and detaches the old child; detach nulls a nullable child FK or applies cascade remove when configured. Newly attached targets participate in cascade persist.

The same generic transaction checkpoint introduced in 0.0.14 copies relation wrappers. A rollback therefore restores both the pointer and its baseline/dirty state; no single-reference-specific rollback side channel was added.

The dedicated 0.0.17 SQLite E2E covers new-root/generated-key attachment, lazy loading, Identity Map reuse, belongs-to set/reset, has-one replacement/reset, nullable detach, cascade persist and transaction rollback. Compile-fail tests separately reject raw `shared_ptr` belongs-to and has-one shapes.

## Relations

| MetalORM capability | C++ status | Notes |
| --- | --- | --- |
| belongsTo | ✅ | dedicated reference, lazy/eager hydration, target-key FK sync, set/reset, rollback |
| hasOne | ✅ | dedicated reference, lazy/eager hydration, replacement/detach, cascade and rollback |
| hasMany | ✅/🟡 | dedicated collection; broader JS-object conveniences remain language-specific |
| belongsToMany | ✅ | lazy/eager loading, IDs, sync, typed pivot patches, alternate `targetKey` |
| morphTo | ✅ | typed target set, lazy resolution, switching/reset, cascade persist |
| morphOne | ✅ | dedicated reference, lazy/eager loading, mutation/cascade |
| morphMany | ✅ | dedicated collection, lazy/eager loading, mutation/cascade |
| cascade none/all/persist/remove/link | ✅ | aligned vocabulary and runtime semantics for supported relation operations |

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

Correlation lives in the SELECT `WHERE` compilation point. Callback-local `ORDER BY / LIMIT / OFFSET` runs after correlation, and root relation predicates run before root pagination. Nested correlation aliases prevent alias shadowing while ordinary queries retain stable `t0/t1/p0` aliases.

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

The narrow runtime gaps through graph persistence and single-reference semantics are now closed through 0.0.17. The next version should be selected from a **fresh audit of current TypeScript capabilities**, rather than preserving an old checklist after the reference implementation changes.

Known larger gaps currently include schema introspection/diff/migrations, bulk operations, DTO/OpenAPI, cache, Tree/MPTT, pooling and DB-to-entity generation. SQLite remains the only backend while these parity layers are evaluated.

A later query-performance pass may replace in-memory root deduplication with a root-aware SQL page plan, provided it preserves the tested 0.0.13 semantics.
