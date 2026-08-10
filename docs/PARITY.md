# MetalORM C++ parity matrix

The TypeScript `celsowm/metal-orm` repository is the behavioral and architectural reference for this port.

MetalORM C++ intentionally targets C++26 static reflection rather than providing a compatibility layer for older C++ standards. Reflection may replace TypeScript metadata plumbing and stringly-typed APIs, but it must not silently redefine ORM behavior.

SQLite is intentionally the only concrete backend while semantic parity is built out.

Current release: **0.0.31**.

Legend:

- ✅ parity for the supported SQLite execution model
- 🟡 implemented, with an explicit remaining edge/integration gap
- ❌ not ported
- N/A not a distinct subsystem in the TypeScript reference

## Runtime and mapping

| MetalORM capability | C++ status | Notes |
| --- | --- | --- |
| Entity/table metadata | ✅ | C++26 annotations + static reflection |
| Column mapping / physical names | ✅ | reflected member annotations |
| Primary/generated columns | ✅ | `consteval` validated |
| Declared database types | ✅ | `database_type` preserved through schema/codegen |
| Reflected database defaults | ✅ | typed literal/text/null + raw SQL annotations |
| Identity Map | ✅ | separate runtime component |
| Unit of Work | ✅ | separate component; shared DML AST |
| Session coordinator | ✅ | coordinates UoW, Identity Map and relation processor |
| Dirty snapshots | ✅ | reflection-generated |
| Persist/remove lifecycle | ✅ | aligned with TS semantics |
| Transactional `commit()` | ✅ | executor capabilities + rollback restoration |
| Nested transactions/savepoints | ✅ | BEGIN outer; SAVEPOINT/RELEASE inner |
| rollback-only nested failure | ✅ | failed inner scope poisons outer scope |
| rollback-safe in-memory UoW state | ✅ | scalar/runtime checkpoints |
| rollback-safe generated IDs | ✅ | generated PK returns to checkpoint value |
| rollback-safe relation state | ✅ | reflected relation-wrapper snapshots |
| Table lifecycle hooks | ✅ | Session-bound |
| Session interceptors | ✅ | `before_flush` / `after_flush` |
| Domain events | ✅ | dispatched only after successful outermost commit |
| saveGraph/updateGraph/patchGraph | ✅ | typed nested graph payloads, pivots and pruning |

## Relations

| MetalORM capability | C++ status | Notes |
| --- | --- | --- |
| belongsTo | ✅ | dedicated reference, lazy/eager, target-key FK sync, rollback |
| hasOne | ✅ | dedicated reference, replacement/detach, cascade and rollback |
| hasMany | ✅/🟡 | dedicated collection; JS object conveniences are language-specific |
| belongsToMany | ✅ | lazy/eager, IDs, sync, typed pivot patches, alternate `targetKey` |
| morphTo | ✅ | typed target set, lazy resolution, switching/reset |
| morphOne | ✅ | dedicated reference, lazy/eager, mutation/cascade |
| morphMany | ✅ | dedicated collection, lazy/eager, mutation/cascade |
| cascade none/all/persist/remove/link | ✅ | aligned vocabulary/runtime behavior |

Raw `std::shared_ptr<T>` is not accepted as the reflected shape for `belongsTo`/`hasOne`; dedicated wrappers carry loading, dirty tracking, Identity Map integration and rollback checkpoints.

## Query builder and DML

| MetalORM capability | C++ status | Notes |
| --- | --- | --- |
| Typed SELECT AST | ✅ | compile-time entity scope |
| comparisons/logical predicates | ✅ | typed scalar operands |
| scalar arithmetic `+ - * / %` | ✅ | recursive scalar AST |
| IN / NULL / LIKE | ✅ | values and subqueries |
| BETWEEN / NOT BETWEEN | ✅ | first-class expression AST |
| EXISTS / NOT EXISTS | ✅ | typed SELECT subqueries |
| reflected JOINs | ✅ | N:1 / 1:1 / 1:N / N:N |
| projections/aliases | ✅ | columns, aggregates, functions, CASE, windows, arithmetic |
| aggregates/GROUP BY/HAVING | ✅/🟡 | optional SQLite extensions vary by build |
| CTE / recursive CTE | ✅ | recursive traversal tested on SQLite |
| UNION / UNION ALL / INTERSECT / EXCEPT | ✅ | projection arity validated |
| window functions | ✅ | ranking, NTILE, LAG/LEAD, FIRST/LAST VALUE |
| derived tables / fromSubquery | ✅ | SQLite alias-list restriction diagnosed |
| CASE | ✅ | searched CASE in projection/predicates |
| SQL function AST | ✅ | recursive typed scalar node |
| text/control/date/JSON functions | ✅/🟡 | backend-specific extension edges remain |
| numeric function catalog | ✅/🟡 | optional SQLite math functions depend on build |
| INSERT/UPDATE/DELETE AST | ✅ | public builders + runtime share it |
| multi-row INSERT | ✅ | accumulated VALUES rows |
| INSERT ... SELECT | ✅ | typed SELECT source |
| RETURNING | ✅ | INSERT/UPDATE/DELETE |
| SQLite UPSERT | ✅ | conflict target, DO NOTHING/UPDATE, `excluded()` |
| DML typed predicate reuse | ✅ | UPDATE/DELETE reuse `Expression<T>` |

## Relation queries and pagination

| Capability | C++ status | Notes |
| --- | --- | --- |
| `where_has` / `where_has_not` | ✅ | reflected correlated EXISTS |
| `where_relation` | ✅ | shared relation-query compiler |
| `match_relation` | ✅ | behavioral relation matching |
| nested relation scopes | ✅ | hierarchical aliases avoid shadowing |
| offset pagination | ✅ | row-oriented raw executor; root-aware Session path |
| keyset/cursor pagination | ✅ | forward/backward, mixed ASC/DESC, ordering signatures |
| root deduplication | ✅ | reflected root PK |

## Bulk operations

| TypeScript behavior | C++ binding | Status |
| --- | --- | --- |
| `bulkInsert` | `bulk_insert<T>` | ✅ |
| `bulkUpdate` | `bulk_update<T>` | ✅ |
| `bulkUpdateWhere` | `bulk_update_where<T>` | ✅ |
| `bulkDelete` | `bulk_delete<T>` | ✅ |
| `bulkDeleteWhere` | `bulk_delete_where<T>` | ✅ |
| `bulkUpsert` | `bulk_upsert<T>` | ✅ |
| default chunk size 500 | `BulkBaseOptions::chunk_size = 500` | ✅ |
| sequential default / bounded concurrency | worker execution | ✅ |
| transactional by default | Session transaction | ✅ |
| non-transactional partial progress | direct chunk execution | ✅ |
| timing / per-chunk callback | `ChunkCompleteInfo` | ✅ |
| RETURNING | reflected selections | ✅ |

## Tree / MPTT

Tree/MPTT is ✅ for the supported SQLite model. The binding includes reflected tree metadata, ancestor/descendant/root/child/sibling/leaf queries, threaded descendants, insertion, sibling/subtree movement, cycle rejection, child promotion on detach, subtree deletion, recovery, validation and reflected multi-tree scopes.

Boundary-changing mutations are scope-aware. Moving subtrees are isolated below zero while old/new gaps are changed so the moving temporary range cannot be shifted by its own destination-gap update. `remove_from_tree()` promotes children and retains the removed row as a valid detached root.

## DTO / REST / OpenAPI

| Capability | C++ status | Notes |
| --- | --- | --- |
| response/create/update DTO descriptors | ✅ | derived from reflection |
| generated-field create/update exclusion | ✅ | reflection-derived |
| public member names vs SQL column names | ✅ | separated |
| defaults / nullability / requiredness | ✅ | shared metadata |
| runtime DTO transforms | ✅ | response merge/default/exclude/pick/map |
| scalar REST filters | ✅ | reflected allowlists |
| relation-aware recursive filters | ✅ | some/every/none/isEmpty/isNotEmpty |
| safe dynamic sorting | ✅ | reflected allowlist + PK tie-breaker |
| paged DTO execution | ✅ | reuses Session root pagination |
| DTO/OpenAPI schemas | ✅ | response/create/update/filter/pagination |
| nested relation OpenAPI | ✅ | single/collection shapes + components |
| Tree OpenAPI | ✅ | reflected Tree schemas |

`morphTo` relation filtering remains discriminator-dependent and unsupported in the relation-query model itself; the DTO layer does not invent a different behavior.

## Cache

| Capability | C++ status | Notes |
| --- | --- | --- |
| cache reader/writer/invalidator/provider split | ✅ | ISP-style capabilities |
| human durations | ✅ | s/m/h/d/w + milliseconds |
| tenant-aware keys | ✅ | same `tenant:<id>:<key>` contract |
| TTL / tags / prefix invalidation | ✅ | MemoryCacheAdapter |
| statistics / clear / dispose | ✅ | capability-based |
| cached entity rehydration | ✅ | reuses Session + Identity Map |
| relation-query cache wrapper | ✅ | same execute-around path |
| first-party remote adapter | 🟡 | provider extension exists; no mandatory Redis client dependency yet |

`auto_invalidate` remains represented but intentionally inert because the TypeScript manager stores the option without consuming it during mutation execution.

## Procedure calls

Procedure-call API is ✅ for the supported SQLite execution model. `ProcedureCall`, ordered IN/OUT/INOUT parameters, optional schema/db-type metadata, compiled OUT metadata and multi-result execution capabilities exist independently from SELECT/DML.

SQLite correctly rejects stored procedures because SQLite has no stored-procedure facility and the TypeScript SQLite dialect also rejects them. Synthetic capability tests prove first-result-set and last-result-set OUT extraction without pretending SQLite supports procedures.

## Pooling — 0.0.27

| Capability | C++ status | Notes |
| --- | --- | --- |
| executor pool | ✅ | exported pooling API |
| leasing | ✅ | bounded/shared executor selection |
| concurrent selection safety | ✅ | serialized lease selection regression fixed |
| SQLite integration | ✅ | respects one-handle serialization semantics |

## DB-to-entity generation — 0.0.28 / 0.0.31

| Capability | C++ status | Notes |
| --- | --- | --- |
| schema -> C++26 entity header | ✅ | public generator + CLI |
| declared SQL types | ✅ | `database_type` |
| defaults | ✅ | typed/default_sql annotations |
| comments | ✅ | optional emitted docs |
| belongsTo wrappers from FKs | ✅/🟡 | generated when target can use current relation shape |
| physical FK preservation | ✅ | `reference_to<^^Target, "physical_column", ...>` |
| FK ON DELETE / ON UPDATE actions | ✅ | reflected `referential_action` |
| column CHECK preservation | ✅ | `mapping::check` |
| table CHECK preservation | ✅ | named and unnamed annotations |
| self/cyclic physical FK representation | ✅ | target type can be only forward-declared |
| excluded external target FK | 🟡 | warns because target type is unavailable to reflection |

`reference_to<^^Target, "physical_column", ...>` exists specifically so generated physical FKs do not require `^^Target::member` while the target is incomplete. Mapping validation later resolves the physical name to exactly one persistent reflected member and checks value-type compatibility.

## SQLite schema / DDL

| MetalORM capability | C++ status | Notes |
| --- | --- | --- |
| CREATE TABLE DDL | ✅ | reflection-derived |
| composite PK DDL | ✅ | validated |
| column defaults | ✅ | literal/text/null/raw |
| user indexes | ✅ | expected metadata + introspection |
| partial indexes | ✅ | `WHERE` round-trip |
| schema comments convention | ✅ | optional `schema_comments` metadata |
| schema introspection | ✅ | table/column/PK/index/view/FK/CHECK metadata |
| schema diff / plan execution | 🟡 | complete for represented SQLite metadata; FK name/deferrability remain unmodeled |
| synchronize / dry-run / destructive policy | ✅ | safe/destructive split |
| physical FK declaration | 🟡 | target + actions complete; named FK constraints/deferrability pending |
| column CHECK declaration | ✅ | `mapping::check` |
| table CHECK declaration | ✅ | named/unnamed |
| CHECK introspection | ✅ | SQL-aware `sqlite_master.sql` parser |

### Physical constraints — 0.0.30 / 0.0.31

ORM relation metadata never implies a physical database constraint. Physical FKs use either:

```cpp
[[=metal::mapping::reference<^^User::id,
    metal::mapping::referential_action::cascade>{}]]
std::optional<std::int64_t> user_id;
```

or the codegen/cycle-safe equivalent:

```cpp
[[=metal::mapping::reference_to<^^User, "id",
    metal::mapping::referential_action::cascade>{}]]
std::optional<std::int64_t> user_id;
```

Column and table checks use:

```cpp
[[=metal::mapping::check<"age >= 0">{}]]
std::int64_t age{};

struct [[
    =metal::mapping::table{"orders"},
    =metal::mapping::table_check<"quantity > 0">{},
    =metal::mapping::named_table_check<"price_guard", "price >= 0">{}
]] Order { /* ... */ };
```

SQLite exposes no dedicated CHECK-introspection PRAGMA. MetalORM parses the stored CREATE TABLE statement with a scanner that tracks nested parentheses, strings, quoted identifiers and comments, and only splits table elements on top-level commas. CHECK mismatches on existing tables produce explicit rebuild warnings rather than fake ALTER SQL.

## Schema/tooling/ecosystem summary

| MetalORM capability | C++ status |
| --- | --- |
| SQLite DDL generation | ✅ |
| composite PK DDL | ✅ |
| reflected column defaults | ✅ |
| partial indexes | ✅ |
| SQLite schema introspection | ✅ |
| schema diff / plan execution | 🟡 |
| schema synchronize / dry-run / destructive policy | ✅ |
| physical FK declaration metadata | 🟡 |
| physical CHECK declaration metadata | ✅ |
| migration history/versioned migration runner | N/A |
| bulk operations | ✅ |
| DTO/OpenAPI | ✅ |
| Tree/MPTT | ✅ |
| cache layer | 🟡 |
| procedure calls | ✅ |
| pooling | ✅ |
| DB-to-entity code generation | ✅/🟡 |

## Release progression

- **0.0.13** — relation-query and root-aware pagination parity.
- **0.0.14** — nested transactions/savepoints and rollback-safe state.
- **0.0.15** — lifecycle/interceptors/domain events.
- **0.0.16** — graph persistence.
- **0.0.17** — dedicated single-relation references.
- **0.0.18** — SQLite introspection/diff/synchronization baseline.
- **0.0.19** — bulk operations.
- **0.0.21** — Tree/MPTT parity and subtree-move hardening.
- **0.0.22–0.0.24** — DTO/REST/OpenAPI and reflected defaults.
- **0.0.25** — query cache core.
- **0.0.26** — procedure-call contract / correct SQLite rejection.
- **0.0.27** — pooling.
- **0.0.28** — DB-to-C++ entity generator and CLI.
- **0.0.29** — reflected SQLite partial indexes.
- **0.0.30** — explicit reflected physical foreign keys + actions.
- **0.0.31** — column/table CHECK round-trip, SQL-aware CHECK introspection, and constraint-preserving entity generation.

## Next parity targets

The highest-value remaining gaps are deliberately narrower now:

1. **Foreign-key constraint name and deferrability metadata** where the TypeScript schema contract exposes them and the backend can preserve them safely.
2. **Remote cache adapter policy** without forcing a Redis client into the core library.
3. **Generated relation wrappers for alternate target keys/cyclic relation shapes** beyond physical-FK preservation.
4. Future database backends should implement existing capability boundaries rather than expanding the SQLite core with vendor-specific switches.

A later performance pass may replace in-memory root pagination deduplication with a root-aware SQL page plan, provided it preserves the tested 0.0.13 semantics.
