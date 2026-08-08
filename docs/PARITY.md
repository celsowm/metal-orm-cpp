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

## Query builder

| MetalORM capability | C++ status |
| --- | --- |
| Typed SELECT AST | ✅ |
| WHERE/comparisons/logical expressions | ✅ partial catalog |
| IN / NULL / LIKE | ✅ |
| Reflected JOINs | ✅ non-polymorphic + runtime MorphOne/MorphMany include |
| Projections/aliases | ✅ |
| Aggregates/GROUP BY/HAVING | ✅ |
| Scalar IN subqueries | ✅ |
| INSERT/UPDATE/DELETE AST | ✅ foundational |
| RETURNING | ❌ |
| UPSERT/conflict API parity | 🟡 minimal `do nothing` primitive only |
| CTE / recursive CTE | ❌ |
| derived tables | ❌ |
| UNION/UNION ALL/INTERSECT/EXCEPT | ❌ |
| EXISTS / NOT EXISTS | ❌ |
| BETWEEN | ❌ |
| window functions | ❌ |
| CASE | ❌ |
| JSON/function catalog | ❌ |
| relation query helpers (`whereHas`, etc.) | ❌ |
| pagination/cursor helpers | ❌ |

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

1. **0.0.9:** richer DML parity (`RETURNING`, multi-row insert, insert-select, SQLite conflict API).
2. **0.0.10:** CTEs, set operations, EXISTS/BETWEEN and window functions.
3. Then: saveGraph/runtime hooks/events/transactions and the remaining ecosystem modules.

This ordering may change when comparison with the TypeScript reference exposes a more fundamental dependency.
