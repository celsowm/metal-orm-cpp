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
| morphTo | ❌ | Next major relation family |
| morphOne | ❌ | Next major relation family |
| morphMany | ❌ | Next major relation family |
| cascade none/all/persist/remove/link | ✅ | Vocabulary and N:N remove semantics aligned |

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

### `targetKey` consistency note

The TypeScript schema and `DefaultManyToManyCollection` explicitly use `relation.targetKey` for ID-based relation identity. The current TypeScript `RelationChangeProcessor`, however, still resolves the target table primary key during N:N flush. The C++ port follows the declared relation contract end-to-end: when a custom `targetKey` is configured, that reflected member is used consistently by collection identity, pivot DML and cascade removal.

Remaining collection adaptation work is limited to JS-object-model conveniences such as `toJSON()`; these require an explicit C++ serialization design rather than literal API copying.

## Query builder

| MetalORM capability | C++ status |
| --- | --- |
| Typed SELECT AST | ✅ |
| WHERE/comparisons/logical expressions | ✅ partial catalog |
| IN / NULL / LIKE | ✅ |
| Reflected JOINs | ✅ |
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

1. **0.0.8:** `morphTo` / `morphOne` / `morphMany`.
2. **0.0.9:** richer DML parity (`RETURNING`, multi-row insert, insert-select, SQLite conflict API).
3. **0.0.10:** CTEs, set operations, EXISTS/BETWEEN and window functions.
4. Then: saveGraph/runtime hooks/events/transactions and the remaining ecosystem modules.

This ordering may change when comparison with the TypeScript reference exposes a more fundamental dependency.
