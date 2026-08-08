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
| rollback-safe in-memory UoW state | ❌ | Next runtime family |
| Interceptors/hooks | ❌ | Follows transactions |
| Domain events | ❌ | Follows interceptors/hooks |
| saveGraph/updateGraph/patchGraph | ❌ | Later runtime family |

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

0.0.13 moves correlation into the SELECT `WHERE` compilation point instead of wrapping an already-compiled child query. Therefore callback-local `ORDER BY / LIMIT / OFFSET` runs **after** correlation, matching the TypeScript `applyRelationCorrelation()` ordering. A relation filter attached to a root query that already contains `LIMIT/OFFSET` is likewise applied before those clauses.

Nested relation scopes use generated alias namespaces (`t0`, `t0_rel`, `t0_rel_rel`, ...) so inner subqueries cannot shadow an outer correlation alias. Outside correlated scopes the stable historic SQL aliases (`t0`, `t1`, `p0`) are preserved.

For N:N correlation, the child target receives an `EXISTS` over the reflected pivot table. This keeps relation-key metadata in one correlation engine without injecting an unrelated JOIN into the child callback query.

`match_relation` uses the same correlation engine. TypeScript currently renders `match()` as `INNER JOIN + DISTINCT`; C++ renders EXISTS because keeping one reflected correlation compiler avoids two semantically overlapping implementations. Root-filtering behavior is equivalent for the supported predicates; byte-for-byte SQL-shape parity is not a project requirement.

## Pagination parity — 0.0.13

### Offset pagination

Two execution levels mirror the TypeScript architecture:

- `execute_paged(query, executor, dialect, options)` returns raw `Row` results and counts physical result rows;
- `execute_paged(query, session, options)` returns tracked unique root entities.

Pagination helpers first remove any previous query `LIMIT/OFFSET`, because `executePaged` owns the requested page just as the TypeScript builder overwrites those clauses.

The Session overload is root-aware even for an explicit 1:N/N:N JOIN that physically multiplies rows: it deduplicates by the reflected root PK while preserving the query result order, counts unique roots, then slices the requested page. It refuses partial root projections rather than creating incomplete managed entities, and hydrated pages reuse the Identity Map.

The current implementation chooses semantic correctness over a premature SQL-only optimization: tracked root pagination materializes the unpaged matching row stream before deduplication. This is a performance optimization opportunity, not a behavioral parity gap.

### Cursor pagination

Implemented:

- `first` / `after` forward pagination;
- `last` / `before` backward pagination;
- mode-driven keyset direction (`first` => after semantics, `last` => before semantics), matching TS even for unusual `first + before` / `last + after` combinations;
- `limit + 1` next/previous-page detection;
- reflected `cursor_order(field<^^T::member>, direction)` terms;
- lexicographic multi-column keyset predicates;
- ASC/DESC-aware break operators;
- non-null cursor keys;
- order signature validation so cursors cannot be reused with a different ordering;
- Session overload returning Identity-Map-managed entities;
- root-PK deduplication for row-multiplying explicit joins before page-size/has-extra evaluation.

Cursor payloads are opaque. The C++ encoder preserves the same semantic payload — version, ordered values and ordering signature — but does not promise TypeScript/C++ wire-format interchange.

As with tracked offset pagination, the root-deduplicating cursor path may inspect more candidate rows than an eventual optimized SQL plan. The observable pagination semantics are closed; SQL-plan optimization can evolve independently.

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

`select.hpp` exposes an internal extra-predicate compilation hook used by relation correlation and `without_pagination()` snapshots used by execution helpers. Session-specific tracked pagination remains isolated in `runtime_pagination.hpp` rather than growing the SELECT builder into a runtime monolith.

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

The two semantic edges tracked after 0.0.12 are closed in 0.0.13. The next releases should return to runtime parity:

1. **0.0.14:** nested transactions/savepoints plus rollback-safe in-memory Unit of Work / relation state.
2. Then: interceptors/hooks and domain events.
3. Then: `saveGraph` / `updateGraph` / `patchGraph`.
4. Then: schema/tooling/ecosystem modules such as introspection/diff, bulk operations, DTO/OpenAPI, cache, Tree/MPTT, pooling and code generation.

A later query-performance pass may replace in-memory root deduplication with a root-aware SQL page plan, provided it preserves the now-tested 0.0.13 semantics.