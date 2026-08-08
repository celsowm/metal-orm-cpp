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
| Nested transactions/savepoints | ❌ | Next runtime family |
| Interceptors/hooks | ❌ | Next runtime family |
| Domain events | ❌ | Next runtime family |
| saveGraph/updateGraph/patchGraph | ❌ | Next runtime family |

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

## Relation-query parity — 0.0.12

| Capability | Status | Notes |
| --- | --- | --- |
| whereHas | ✅/🟡 | reflected correlated EXISTS; normal child filtering/joins supported |
| whereHasNot | ✅/🟡 | reflected NOT EXISTS |
| relation conditions | ✅ | `where_relation<^^Relation>(..., targetPredicate)` |
| relation match | ✅/🟡 | same root-filtering behavior through the shared EXISTS correlation engine |
| N:N relation predicates | ✅ | reflected pivot and target keys |
| MorphOne/MorphMany relation predicates | ✅ | reflected id/type correlation |
| MorphTo whereHas | intentionally unsupported | same physical-target ambiguity as TS |

The canonical API does not accept relation names or key names as strings:

```cpp
auto users = metal::where_has<^^User::posts>(
    metal::select<User>(),
    [](auto& posts) {
        posts.where(metal::field<^^Post::published> == true);
    });
```

`where_relation` is the concise target-predicate form:

```cpp
auto admins = metal::where_relation<^^User::roles>(
    metal::select<User>(),
    metal::field<^^Role::name> == "admin");
```

Relations can be filtered repeatedly by composing the free helpers.

**Remaining relation-query edge:** callback-local `LIMIT/OFFSET` is not claimed as complete parity yet. The current C++ correlation engine wraps the configured child query, so ordinary child predicates/joins are correct but pagination inside the child callback can differ from the TS correlation-before-pagination order. This is deliberately marked rather than hidden.

`match_relation` uses the same correlation engine. TypeScript currently renders `match()` as `INNER JOIN + DISTINCT`; C++ renders EXISTS because keeping one reflected correlation compiler avoids two semantically overlapping implementations. Observable root filtering is equivalent for the supported predicates, but SQL shape parity is not claimed.

## Pagination parity — 0.0.12

### Offset pagination

Two execution levels mirror the TypeScript architecture:

- `execute_paged(query, executor, dialect, options)` returns raw `Row` results and counts rows;
- `execute_paged(query, session, options)` returns tracked root entities and counts `DISTINCT` reflected root primary keys.

The Session overload refuses partial root projections rather than creating incomplete managed entities. Hydrated pages reuse the Identity Map.

### Cursor pagination

Implemented:

- `first` / `after` forward pagination;
- `last` / `before` backward pagination;
- `limit + 1` next/previous-page detection;
- reflected `cursor_order(field<^^T::member>, direction)` terms;
- lexicographic multi-column keyset predicates;
- ASC/DESC-aware break operators;
- non-null cursor keys;
- order signature validation so cursors cannot be reused with a different ordering;
- Session overload returning Identity-Map-managed entities.

Cursor payloads are opaque. The C++ encoder preserves the same semantic payload — version, ordered values and ordering signature — but does not promise TypeScript/C++ wire-format interchange.

**Remaining pagination edge:** the Session count is distinct-root aware, but an explicitly joined query that physically duplicates root rows can still require root-aware page extraction as well as root-aware counting. Session `.include()` remains batch-loaded and does not create that duplication. This explicit-join edge is tracked for the next pagination hardening pass.

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

Session-specific tracked pagination is isolated in `runtime_pagination.hpp` rather than growing the SELECT builder into a runtime monolith.

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

The next releases should close reference gaps rather than add unrelated capabilities:

1. Harden the two explicit 0.0.12 edges: callback-local relation pagination and distinct-root page extraction for explicit row-multiplying joins.
2. Port nested transactions/savepoints and rollback-safe in-memory Unit of Work state.
3. Port interceptors/hooks and domain events.
4. Port saveGraph/updateGraph/patchGraph.
5. Then move into schema/tooling/ecosystem modules: introspection/diff, bulk operations, DTO/OpenAPI, cache, Tree/MPTT, pooling and code generation.

This ordering may change when comparison with the TypeScript reference exposes a more fundamental dependency.
