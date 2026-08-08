# MetalORM C++ parity matrix

The TypeScript `celsowm/metal-orm` repository is the behavioral and architectural reference for this port.

C++26 reflection may replace metadata plumbing, improve compile-time safety, and make APIs more strongly typed. It must not silently redefine MetalORM behavior.

SQLite is intentionally the only backend while semantic parity is being built.

## Runtime and mapping

| MetalORM capability | C++ status | Notes |
| --- | --- | --- |
| Entity/table metadata | ✅ | C++26 annotations + static reflection replace TS table/decorator metadata |
| Primary/generated columns | ✅ | `consteval` validated |
| Identity Map | ✅ | Separate runtime component |
| Unit of Work | ✅ | Separate component; shared DML AST |
| Session coordinator | ✅ | Coordinates UoW, Identity Map and relation processor |
| Dirty snapshots | ✅ | Reflection-generated |
| Persist/remove lifecycle | ✅ | Aligned with TS runtime semantics |
| Nested transactions/savepoints | ❌ | Not ported yet |
| Interceptors/hooks | ❌ | Not ported yet |
| Domain events | ❌ | Not ported yet |
| saveGraph/updateGraph/patchGraph | ❌ | Not ported yet |

## Relations

| MetalORM capability | C++ status | Notes |
| --- | --- | --- |
| belongsTo | ✅ | Reflected metadata + eager load |
| hasOne | ✅ | Reflected metadata + eager load |
| hasMany | ✅/🟡 | Dedicated collection, lazy/eager load and mutation; broader edge-case parity still evolving |
| belongsToMany | ✅ | Dedicated collection, lazy/eager load, IDs, sync, partial typed pivots and alternate `targetKey` behavior |
| morphTo | ✅ | Typed target set, lazy resolution, target switching, reset and cascade persist |
| morphOne | ✅ | Dedicated reference, lazy/eager loading, mutation and cascade behavior |
| morphMany | ✅ | Dedicated collection, lazy/eager loading, mutation and cascade behavior |
| cascade none/all/persist/remove/link | ✅ | Vocabulary and relation remove/persist semantics aligned |

### Has-many collection

Implemented:

- `load()`
- `get_items()`
- `add()`
- `attach()`
- `remove()`
- `clear()`
- Session-bound lazy loading
- reflected FK assignment for tracked roots
- UoW/cascade integration

### Many-to-many collection

Implemented:

- `load()`
- `get_items()`
- `attach(entity)`
- `attach(id)`
- `detach(entity)`
- `detach(id)`
- `sync_by_ids()`
- typed pivot hydration
- typed partial `pivot_patch<Pivot>` INSERT/UPDATE semantics
- compile-time pivot member ownership and patch-value compatibility
- relation-FK filtering from pivot DML payloads
- alternate non-primary `targetKey` for attach/detach/sync/pivot DML/cascade
- normal primary-key Identity Map integration after hydrated targets are materialized

`pivot_patch<Pivot>` is the C++ adaptation of TypeScript `Partial<TPivot>`: only explicitly reflected members become DML assignments, and repeated patches merge without resetting omitted fields.

The TypeScript `RelationChangeProcessor` now also resolves belongs-to-many mutation keys through the declared `targetKey`, so the previous implementation mismatch is closed in both repositories.

### Polymorphic relations

`morph_one` and `morph_many` keep the MetalORM parent-side semantics: the target row stores the reflected id/type pair, `set()` / `attach()` writes the pair in memory, lazy/eager hydration filters by both fields, and removal either clears them or cascades according to the relation mode.

`morph_to` keeps the child-side semantics with a compile-time target registry:

```cpp
[[=metal::mapping::morph_to<
    ^^Activity::subject_type,
    ^^Activity::subject_id,
    metal::mapping::cascade_mode::persist,
    metal::mapping::morph_target<"post", ^^Post>,
    metal::mapping::morph_target<"video", ^^Video>>{}]]
metal::morph_to_reference<Post, Video> subject;
```

The discriminator set is `consteval` validated for uniqueness. Every declared target/key must be compatible with the reflected id field. Lazy loading groups roots by discriminator and performs one query per concrete target type, then hydrates through the normal Identity Map.

The TypeScript query builder explicitly does not support JOIN-based `MorphTo` include and directs callers to lazy loading. The C++ typed SQL AST likewise does not model a polymorphic single-table JOIN; the parity path is `subject.load()`. `MorphOne` and `MorphMany` remain eager-loadable.

Remaining relation adaptation work is limited to JS-object-model conveniences such as `toJSON()` and broader has-many edge cases; those require explicit C++ serialization/API decisions rather than literal copying.

## Query builder and DML

| MetalORM capability | C++ status | Notes |
| --- | --- | --- |
| Typed SELECT AST | ✅ | Compile-time entity scope |
| WHERE/comparisons/logical expressions | ✅ partial catalog | More operators remain in 0.0.10 |
| IN / NULL / LIKE | ✅ | Typed values/subqueries |
| Reflected JOINs | ✅ | Non-polymorphic typed joins + runtime MorphOne/MorphMany include |
| Projections/aliases | ✅ | Columns + aggregates |
| Aggregates/GROUP BY/HAVING | ✅ | Foundational aggregate set |
| Scalar IN subqueries | ✅ | Exactly-one projection validation |
| INSERT/UPDATE/DELETE AST | ✅ | Shared by public builders, UoW and relation processor |
| Multi-row INSERT | ✅ | Multiple `VALUES` rows in one statement |
| INSERT ... SELECT | ✅ | Typed `BasicSelectQuery` source |
| RETURNING | ✅ | INSERT/UPDATE/DELETE, including aliases |
| SQLite UPSERT/conflict API | ✅ | target columns, DO NOTHING, DO UPDATE, update predicate, `excluded()` |
| CTE / recursive CTE | ❌ | Planned for 0.0.10 |
| derived tables | ❌ | Not ported yet |
| UNION/UNION ALL/INTERSECT/EXCEPT | ❌ | Planned for 0.0.10 |
| EXISTS / NOT EXISTS | ❌ | Planned for 0.0.10 |
| BETWEEN | ❌ | Planned for 0.0.10 |
| window functions | ❌ | Planned for 0.0.10 |
| CASE | ❌ | Not ported yet |
| JSON/function catalog | ❌ | Not ported yet |
| relation query helpers (`whereHas`, etc.) | ❌ | Not ported yet |
| pagination/cursor helpers | ❌ | Not ported yet |

### DML parity details

The TypeScript insert builder treats `VALUES` and `SELECT` as mutually exclusive insert sources. C++ 0.0.9 keeps the same state rule:

```cpp
auto insert = metal::InsertQueryBuilder{"users"}
    .values({
        {{"name", std::string{"A"}}, {"score", std::int64_t{10}}},
        {{"name", std::string{"B"}}, {"score", std::int64_t{20}}}
    })
    .returning({"id", "name"});
```

Typed INSERT SELECT reuses the normal select AST:

```cpp
auto source = metal::select<Source>();
source
    .clear_projection()
    .project(metal::field<^^Source::name>)
    .project(metal::field<^^Source::score>);

metal::insert_into<Target>()
    .from_select(source, {"name", "score"})
    .returning({"id"});
```

SQLite conflict handling follows the TypeScript SQLite dialect contract and requires explicit conflict columns:

```cpp
metal::InsertQueryBuilder{"users"}
    .values({{"email", email}, {"name", name}})
    .on_conflict({"email"})
    .do_update({{"name", metal::excluded("name")}})
    .returning({"id", "name"});
```

`excluded()` is a dedicated DML operand and is accepted only in the `DO UPDATE SET` branch.

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

1. **0.0.10:** CTEs/recursive CTEs, set operations, EXISTS/NOT EXISTS, BETWEEN and window functions.
2. Then: derived tables/CASE/function catalog and relation query helpers.
3. Then: saveGraph/runtime hooks/events/transactions and the remaining ecosystem modules.

This ordering may change when comparison with the TypeScript reference exposes a more fundamental dependency.
