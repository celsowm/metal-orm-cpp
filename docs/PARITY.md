# MetalORM C++ parity matrix

The TypeScript `celsowm/metal-orm` repository is the behavioral and architectural reference for this port.

MetalORM C++ intentionally targets C++26 static reflection rather than providing a compatibility layer for older C++ standards. Reflection may replace TypeScript metadata plumbing and stringly-typed APIs, but it must not silently redefine ORM behavior.

SQLite is intentionally the only concrete backend while semantic parity is built out.

Current release: **0.0.34**.

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
| Native binary scalar values | ✅ | `metal::Blob` (`std::vector<std::byte>`) is a first-class `Value` alternative |
| Identity Map | ✅ | separate runtime component; binary keys have stable hex identity keys |
| Unit of Work | ✅ | separate component; shared DML AST |
| Session coordinator | ✅ | coordinates UoW, Identity Map and relation processor |
| Dirty snapshots | ✅ | reflection-generated, including binary members |
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
| comparisons/logical predicates | ✅ | typed scalar operands, including BLOB equality/range operands where SQLite supports them |
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
| SQLite binary parameter/result transport | ✅ | bind/read uses SQLite BLOB APIs; empty BLOB remains distinct from NULL |

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
| binary OpenAPI schema | ✅ | `metal::Blob` -> `type: string`, `format: byte`, matching TypeScript |
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
| cached entity rehydration | ✅ | reuses Session + Identity Map; `QueryResult` can contain BLOB values |
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

## DB-to-entity generation — 0.0.28 / 0.0.31–0.0.34

| Capability | C++ status | Notes |
| --- | --- | --- |
| schema -> C++26 entity header | ✅ | public generator + CLI |
| declared SQL types | ✅ | `database_type` |
| defaults | ✅ | typed/default_sql annotations |
| comments | ✅ | optional emitted docs |
| BLOB/BINARY/BYTEA members | ✅ | generated as `metal::Blob`, no text fallback |
| belongsTo wrappers from FKs | ✅/🟡 | generated when target can use current relation shape |
| physical FK preservation | ✅ | `reference_to<^^Target, "physical_column", ...>` |
| FK ON DELETE / ON UPDATE actions | ✅ | reflected `referential_action` |
| FK constraint names | ✅ | preserved in `reference_to` |
| FK deferrability | ✅ | `DEFERRABLE INITIALLY DEFERRED` round-trip |
| column UNIQUE preservation | ✅ | `unique` / `named_unique` |
| column CHECK preservation | ✅ | `mapping::check` |
| table CHECK preservation | ✅ | named and unnamed annotations |
| self/cyclic physical FK representation | ✅ | target type can be only forward-declared |
| excluded external target FK | 🟡 | warns because target type is unavailable to reflection |

`reference_to<^^Target, "physical_column", ...>` exists specifically so generated physical FKs do not require `^^Target::member` while the target is incomplete. Mapping validation later resolves the physical name to exactly one persistent reflected member and checks value-type compatibility. Constraint names, actions and deferred behavior travel through the same generated annotation.

## SQLite schema / DDL

| MetalORM capability | C++ status | Notes |
| --- | --- | --- |
| CREATE TABLE DDL | ✅ | reflection-derived |
| composite PK DDL | ✅ | validated |
| column defaults | ✅ | literal/text/null/raw |
| column UNIQUE declaration | ✅ | unnamed and named reflected annotations |
| native BLOB affinity | ✅ | `metal::Blob` maps to `BLOB` without custom `database_type` |
| user indexes | ✅ | expected metadata + introspection |
| partial indexes | ✅ | `WHERE` round-trip |
| schema comments convention | ✅ | optional `schema_comments` metadata |
| schema introspection | ✅ | table/column/PK/index/view/FK/UNIQUE/CHECK metadata |
| schema diff / plan execution | ✅ | represented SQLite metadata converges; rebuild-only changes warn instead of emitting fake ALTER SQL |
| synchronize / dry-run / destructive policy | ✅ | safe/destructive split |
| physical FK declaration | ✅ | target, name, actions and deferred behavior |
| column CHECK declaration | ✅ | `mapping::check` |
| table CHECK declaration | ✅ | named/unnamed |
| CHECK introspection | ✅ | SQL-aware `sqlite_master.sql` parser |
| FK name/deferrability introspection | ✅ | PRAGMA target/actions enriched from stored DDL |
| column UNIQUE introspection | ✅ | inline and single-column table constraints; SQLite autoindexes stay out of user-index metadata |

### Physical constraints — 0.0.30–0.0.33

ORM relation metadata never implies a physical database constraint. Physical FKs use either:

```cpp
[[=metal::mapping::reference<
    ^^User::id,
    metal::mapping::referential_action::cascade,
    metal::mapping::referential_action::restrict,
    "fk_posts_user",
    true>{}]]
std::optional<std::int64_t> user_id;
```

or the codegen/cycle-safe equivalent:

```cpp
[[=metal::mapping::reference_to<
    ^^User,
    "id",
    metal::mapping::referential_action::cascade,
    metal::mapping::referential_action::restrict,
    "fk_posts_user",
    true>{}]]
std::optional<std::int64_t> user_id;
```

The last boolean maps to `DEFERRABLE INITIALLY DEFERRED`. SQLite's FK PRAGMA supplies target/actions; the SQL-aware stored-DDL parser complements it with the physical constraint name and deferred/immediate mode. Only the actually deferred SQLite spelling is treated as `deferrable=true`.

Column UNIQUE metadata mirrors TypeScript's `unique?: boolean | string`:

```cpp
[[=metal::mapping::unique]]
std::optional<std::string> handle;

[[=metal::mapping::named_unique<"uq_users_email">{}]]
std::string email;
```

SQLite hides UNIQUE constraints behind internal `sqlite_autoindex_*` objects. MetalORM therefore reads the stored table DDL to recover unnamed/named column UNIQUE constraints without misclassifying those autoindexes as explicit user indexes. Single-column table-level `UNIQUE(column)` is also recognized; composite/expression uniqueness remains on the explicit index surface.

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

The stored-DDL scanner tracks nested parentheses, strings, quoted identifiers and comments, and only splits table elements on top-level commas. FK/UNIQUE/CHECK mismatches on existing tables produce explicit rebuild warnings rather than fake ALTER SQL. A missing UNIQUE column also requires rebuild because SQLite rejects `ALTER TABLE ADD COLUMN ... UNIQUE`.

## Native binary values — 0.0.34

Binary columns use a distinct public scalar instead of overloading `std::string`:

```cpp
struct [[=metal::mapping::table{"files"}]] File {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    metal::Blob contents;
    std::optional<metal::Blob> thumbnail;
};
```

`metal::Blob` is `std::vector<std::byte>` and participates in `Value`, reflection persistence, snapshots, typed query operands, DTO rows and cached `QueryResult`s. SQLite uses the BLOB binding/result APIs rather than text conversion, so embedded zero bytes and arbitrary octets are preserved. Empty BLOB is intentionally bound with a non-null pointer and zero length, keeping it distinct from SQL `NULL`.

DB-to-C++ generation maps `BLOB`, `BINARY`, `VARBINARY` and `BYTEA` declarations to `metal::Blob`. OpenAPI matches the TypeScript binary contract as `type: string`, `format: byte`. Raw SQL defaults such as SQLite `X'...'` remain representable through the existing `default_sql` annotation rather than pretending binary defaults are text literals.

## Schema/tooling/ecosystem summary

| MetalORM capability | C++ status |
| --- | --- |
| SQLite DDL generation | ✅ |
| composite PK DDL | ✅ |
| reflected column defaults | ✅ |
| column UNIQUE metadata | ✅ |
| native BLOB/binary values | ✅ |
| partial indexes | ✅ |
| SQLite schema introspection | ✅ |
| schema diff / plan execution | ✅ |
| schema synchronize / dry-run / destructive policy | ✅ |
| physical FK declaration metadata | ✅ |
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
- **0.0.32** — named physical FKs, deferred-FK semantics, DDL introspection/diff and codegen preservation.
- **0.0.33** — unnamed/named column UNIQUE metadata, DDL/introspection/diff/codegen round-trip, and safe SQLite rebuild diagnostics.
- **0.0.34** — native `metal::Blob` values, SQLite binary bind/read, BLOB mapping/codegen, binary predicates and OpenAPI metadata.

## Next parity targets

The highest-value remaining gaps are now outside the basic SQLite scalar/physical-column contract:

1. **Generated relation wrappers for alternate target keys/cyclic relation shapes** beyond physical-FK preservation.
2. **Read-only mapped-view runtime support**, allowing introspected SQLite views to become usable generated entities rather than warnings.
3. **First-party remote cache adapter policy** without forcing a Redis client into the core library.
4. Future database backends should implement existing capability boundaries rather than expanding the SQLite core with vendor-specific switches.

A later performance pass may replace in-memory root pagination deduplication with a root-aware SQL page plan, provided it preserves the tested 0.0.13 semantics.
