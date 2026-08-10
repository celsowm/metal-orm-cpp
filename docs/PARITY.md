# MetalORM C++ parity matrix

The TypeScript `celsowm/metal-orm` repository is the behavioral and architectural reference for this port.

MetalORM C++ intentionally targets C++26 static reflection rather than carrying a compatibility layer for older C++ standards. Reflection may replace TypeScript metadata plumbing and stringly typed APIs, but it must not silently redefine ORM behavior.

SQLite remains intentionally the only concrete database backend while semantic parity is completed.

Current release: **0.0.37**.

Legend:

- ✅ parity for the supported SQLite execution model
- 🟡 implemented with an explicit remaining edge/integration gap
- ❌ not ported
- N/A language/runtime-specific rather than a distinct portable subsystem

## Runtime and mapping

| Capability | C++ status | Notes |
| --- | --- | --- |
| Entity/table metadata | ✅ | C++26 annotations + static reflection |
| Column mapping / physical names | ✅ | reflected member annotations |
| Primary/generated columns | ✅ | `consteval` validated |
| Declared database types | ✅ | `database_type` preserved through schema/codegen |
| Database defaults | ✅ | typed literal/text/null + raw SQL annotations |
| Native binary scalar values | ✅ | `metal::Blob` is a first-class `Value` alternative |
| Identity Map | ✅ | runtime component with stable keys including BLOB keys |
| Unit of Work | ✅ | independent runtime component sharing DML AST |
| Session coordinator | ✅ | UoW + Identity Map + relation processor |
| Dirty snapshots | ✅ | reflection-generated |
| Persist/remove lifecycle | ✅ | aligned with TypeScript semantics |
| Transactional `commit()` | ✅ | rollback restoration included |
| Nested transactions/savepoints | ✅ | outer transaction + inner SAVEPOINT |
| rollback-only nested failure | ✅ | failed inner scope poisons outer scope |
| rollback-safe generated IDs | ✅ | generated PK restored on rollback |
| rollback-safe relation state | ✅ | wrapper checkpoints |
| table lifecycle hooks | ✅ | Session-bound |
| Session interceptors | ✅ | before/after flush |
| Domain events | ✅ | dispatched after successful outermost commit |
| saveGraph/updateGraph/patchGraph | ✅ | typed nested graph payloads and pruning |

## Relations

| Capability | C++ status | Notes |
| --- | --- | --- |
| belongsTo | ✅ | lazy/eager, FK synchronization, rollback |
| belongsTo alternate target key | ✅ | direct reflected form + cycle-safe `belongs_to_key` |
| hasOne | ✅ | replacement/detach/cascade |
| hasMany | ✅/🟡 | semantic runtime parity; JS object conveniences are N/A |
| belongsToMany | ✅ | lazy/eager, IDs, sync, typed pivot patches, alternate target key |
| morphTo | ✅ | typed target set, lazy resolution, switching/reset |
| morphOne | ✅ | dedicated reference, lazy/eager, mutation/cascade |
| morphMany | ✅ | dedicated collection, lazy/eager, mutation/cascade |
| cascade none/all/persist/remove/link | ✅ | aligned vocabulary/runtime behavior |
| cyclic generated belongsTo relations | ✅ | forward-declared target + physical target-column resolution |
| generated single-reference target with composite PK | 🟡 | physical FK preserved; wrapper omitted because `belongs_to_reference<T>` requires one PK |

Raw `std::shared_ptr<T>` is intentionally not a reflected belongsTo/hasOne shape. Dedicated wrappers own load state, dirty state, Identity Map integration and rollback checkpoints.

`mapping::belongs_to<FK>` still means “target primary key” unless an alternate target key is explicitly declared. A physical foreign key never silently changes ORM relation semantics.

## Query builder and DML

| Capability | C++ status | Notes |
| --- | --- | --- |
| typed SELECT AST | ✅ | compile-time owner scope |
| comparisons/logical predicates | ✅ | typed scalar operands |
| BLOB predicates | ✅ | first-class binary values |
| scalar arithmetic `+ - * / %` | ✅ | recursive scalar AST |
| IN / NULL / LIKE | ✅ | values and subqueries |
| BETWEEN / NOT BETWEEN | ✅ | first-class expression AST |
| EXISTS / NOT EXISTS | ✅ | typed SELECT subqueries |
| reflected JOINs | ✅ | N:1 / 1:1 / 1:N / N:N |
| projections/aliases | ✅ | columns, aggregates, functions, CASE, windows |
| aggregates/GROUP BY/HAVING | ✅/🟡 | optional SQLite extensions vary by build |
| CTE / recursive CTE | ✅ | recursive traversal tested on SQLite |
| UNION / UNION ALL / INTERSECT / EXCEPT | ✅ | projection arity checked |
| window functions | ✅ | ranking, NTILE, LAG/LEAD, FIRST/LAST VALUE |
| derived tables / fromSubquery | ✅ | SQLite alias-list restriction diagnosed |
| CASE | ✅ | searched CASE |
| SQL function AST | ✅ | recursive typed scalar node |
| text/control/date/JSON functions | ✅/🟡 | backend extension edges remain |
| numeric function catalog | ✅/🟡 | optional SQLite math functions depend on build |
| INSERT/UPDATE/DELETE AST | ✅ | public builders + runtime share it |
| multi-row INSERT | ✅ | accumulated VALUES rows |
| INSERT ... SELECT | ✅ | typed source SELECT |
| RETURNING | ✅ | INSERT/UPDATE/DELETE |
| SQLite UPSERT | ✅ | conflict target, DO NOTHING/UPDATE, `excluded()` |
| typed DML predicate reuse | ✅ | UPDATE/DELETE reuse `Expression<T>` |
| SQLite binary parameter/result transport | ✅ | native bind/read BLOB APIs |

## Relation queries and pagination

| Capability | C++ status | Notes |
| --- | --- | --- |
| `where_has` / `where_has_not` | ✅ | correlated EXISTS |
| `where_relation` | ✅ | shared relation-query compiler |
| `match_relation` | ✅ | behavioral relation matching |
| alternate-key relation correlation | ✅ | same target-key resolver used by JOIN/load/filter paths |
| nested relation scopes | ✅ | hierarchical aliases |
| offset pagination | ✅ | root-aware Session path |
| keyset/cursor pagination | ✅ | forward/backward, mixed ASC/DESC |
| root deduplication | ✅ | reflected root PK |
| morphTo relation filtering | 🟡 | discriminator-dependent; not invented at DTO layer |

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
| per-chunk callback/timing | `ChunkCompleteInfo` | ✅ |
| RETURNING | reflected selections | ✅ |

## Tree / MPTT

Tree/MPTT is ✅ for the supported SQLite model.

The binding includes reflected tree metadata, ancestors/descendants/root/child/sibling/leaf queries, threaded descendants, insertion, sibling/subtree movement, cycle rejection, child promotion on detach, subtree deletion, recovery, validation and reflected multi-tree scopes.

Boundary-changing moves isolate the moving range below zero while source/destination gaps are changed, preventing the temporary subtree from being shifted by its own destination-gap update. `remove_from_tree()` promotes children and keeps the removed row as a valid detached root.

## DTO / REST / OpenAPI

| Capability | C++ status | Notes |
| --- | --- | --- |
| response/create/update DTO descriptors | ✅ | reflection-derived |
| generated-field create/update exclusion | ✅ | reflection-derived |
| public member vs physical SQL names | ✅ | separated |
| defaults/nullability/requiredness | ✅ | shared metadata |
| runtime DTO transforms | ✅ | merge/default/exclude/pick/map |
| scalar REST filters | ✅ | reflected allowlists |
| recursive relation filters | ✅ | some/every/none/isEmpty/isNotEmpty |
| safe dynamic sorting | ✅ | allowlist + PK tie-breaker |
| paged DTO execution | ✅ | Session pagination reused |
| DTO/OpenAPI schemas | ✅ | response/create/update/filter/pagination |
| binary OpenAPI schema | ✅ | `metal::Blob` -> `string/byte`, matching TypeScript |
| nested relation OpenAPI | ✅ | single/collection shapes + components |
| Tree OpenAPI | ✅ | reflected Tree schemas |

## Cache — 0.0.25 / 0.0.37

| Capability | C++ status | Notes |
| --- | --- | --- |
| reader/writer/invalidator/provider split | ✅ | ISP-style capabilities |
| human durations | ✅ | s/m/h/d/w + milliseconds |
| tenant-aware keys | ✅ | `tenant:<id>:<key>` |
| Memory TTL/tags/prefix invalidation | ✅ | `MemoryCacheAdapter` |
| statistics / clear / dispose capabilities | ✅ | optional segregated interfaces |
| cached entity rehydration | ✅ | Session + Identity Map reused |
| cached BLOB values | ✅ | `QueryResult` supports `metal::Blob` |
| relation-query cache wrapper | ✅ | same execute-around path |
| portable remote cache codec | ✅ | versioned binary `QueryResult` codec |
| Keyv-style remote adapter | ✅ | `KeyValueCacheAdapter`; optional prefix capability, no tags |
| Redis-style remote adapter | ✅ | TTL + prefix scan boundary + tag sets |
| remote backend ownership policy | ✅ | external by default; opt-in adapter ownership |
| mandatory Redis client dependency | N/A | deliberately injected behind `RedisBackend` |

The C++ remote policy mirrors the TypeScript adapter semantics without requiring hiredis, redis-plus-plus, Boost.Redis or another specific network client in the ORM core.

`KeyValueCacheAdapter::capabilities().prefix` reflects the actual backend capability. This intentionally avoids the current TypeScript Keyv adapter false-positive where `prefix: true` is advertised even when the selected store has no iterator and prefix invalidation later throws.

Remote cache payloads preserve all `Value` alternatives plus affected-row/insert-id metadata. Corrupt or unsupported-version payloads are evicted and treated as misses; transport/backend errors are not silently swallowed.

`auto_invalidate` remains represented but intentionally inert because the TypeScript manager stores the option without consuming it during mutation execution.

## Procedure calls

Procedure-call API is ✅ for the supported SQLite execution model.

`ProcedureCall`, ordered IN/OUT/INOUT parameters, optional schema/database-type metadata, compiled OUT metadata and multi-result execution capability boundaries are implemented independently from SELECT/DML.

SQLite correctly rejects stored procedures because SQLite has no stored-procedure facility and the TypeScript SQLite dialect also rejects them. Synthetic capability tests prove first/last-result-set OUT extraction without pretending SQLite supports procedures.

## Pooling — 0.0.27

| Capability | C++ status | Notes |
| --- | --- | --- |
| executor pool | ✅ | exported pooling API |
| leasing | ✅ | bounded/shared executor selection |
| concurrent selection safety | ✅ | serialized lease selection |
| SQLite integration | ✅ | one-handle serialization semantics preserved |

## DB-to-C++ generation — 0.0.28 / 0.0.31–0.0.36

| Capability | C++ status | Notes |
| --- | --- | --- |
| schema -> C++26 entity header | ✅ | generator + CLI |
| declared SQL types | ✅ | `database_type` |
| defaults | ✅ | typed/default_sql annotations |
| comments | ✅ | optional emitted docs |
| BLOB/BINARY/BYTEA members | ✅ | generated as `metal::Blob` |
| belongsTo wrappers from PK FKs | ✅ | generated automatically |
| belongsTo wrappers from alternate target keys | ✅ | `belongs_to_key` |
| cyclic alternate-key wrappers | ✅ | target may be forward-declared |
| physical FK preservation | ✅ | `reference_to<^^Target, "physical_column", ...>` |
| FK actions/name/deferrability | ✅ | round-tripped |
| column UNIQUE preservation | ✅ | unnamed/named |
| column CHECK preservation | ✅ | reflected column checks |
| table CHECK preservation | ✅ | named/unnamed |
| read-only SQLite views | ✅ | generated as `mapping::view` read models |
| computed view column without declared SQLite type | ✅ | generated as `metal::Value` |
| composite-PK target relation wrapper | 🟡 | physical FK preserved; single-reference wrapper omitted |
| excluded external target FK | 🟡 | warns because target type is unavailable to reflection |

## Read-only mapped views — 0.0.36

Views use `mapping::view` and satisfy `reflect::ViewMapped<T>`, deliberately not `reflect::Entity<T>`.

Consequences:

- no invented ORM primary key;
- no Identity Map participation;
- no Unit of Work tracking;
- no `Session::persist/remove` surface;
- no mutable ORM relation annotations;
- detached read values;
- typed predicates/order/limit/offset through `view_query<T>()`.

SQLite computed view columns often have an empty declared type in `PRAGMA table_info`. The generator uses `metal::Value` for those columns instead of incorrectly coercing runtime integers/reals/blobs to text.

`metal-orm-gen --include-views` emits table entities and read-only view models in one header.

## SQLite schema / DDL

| Capability | C++ status | Notes |
| --- | --- | --- |
| CREATE TABLE DDL | ✅ | reflection-derived |
| composite PK DDL | ✅ | validated |
| column defaults | ✅ | literal/text/null/raw |
| column UNIQUE declaration | ✅ | unnamed/named |
| native BLOB affinity | ✅ | `metal::Blob` -> BLOB |
| user indexes | ✅ | expected metadata + introspection |
| partial indexes | ✅ | WHERE round-trip |
| schema comments convention | ✅ | optional metadata table |
| schema introspection | ✅ | table/column/PK/index/view/FK/UNIQUE/CHECK |
| schema diff / plan execution | ✅ | represented SQLite metadata converges |
| synchronize/dry-run/destructive policy | ✅ | safe/destructive split |
| physical FK declaration | ✅ | target/name/actions/deferred behavior |
| column/table CHECK | ✅ | declaration + SQL-aware introspection |
| FK name/deferrability introspection | ✅ | PRAGMA enriched from stored DDL |
| column UNIQUE introspection | ✅ | stored DDL + autoindex separation |

SQLite rebuild-only changes produce explicit warnings rather than fake ALTER statements. A missing UNIQUE column also requires rebuild because SQLite rejects `ALTER TABLE ADD COLUMN ... UNIQUE`.

## Release progression

| Release | Main parity slice |
| --- | --- |
| 0.0.27 | executor pooling |
| 0.0.28 | DB-to-C++ entity generation |
| 0.0.29 | partial indexes |
| 0.0.30 | physical foreign keys separated from ORM relations |
| 0.0.31 | CHECK constraints + SQL-aware SQLite DDL parser |
| 0.0.32 | named/deferred physical FKs |
| 0.0.33 | column UNIQUE round-trip |
| 0.0.34 | native BLOB/`metal::Blob` runtime + codegen |
| 0.0.35 | generated alternate target-key and cyclic belongsTo wrappers |
| 0.0.36 | read-only mapped SQLite views + view codegen |
| 0.0.37 | first-party remote cache codec/adapters |

## Remaining high-value gaps

The broad SQLite semantic surface is now substantially converged. The clearest next work is no longer another small SQLite metadata patch:

1. **Second concrete database backend** over the existing executor/dialect/capability boundaries, to prove that the architecture is genuinely backend-agnostic rather than merely abstract-looking.
2. **Composite-key relation identity/runtime design** if full single-reference semantics for composite-PK targets are required; this cannot be solved honestly by codegen alone.
3. **morphTo relation filtering** only if a discriminator-aware query contract can be added without inventing behavior absent from the TypeScript relation-query model.
4. Optional concrete Redis client bridge packages may be added, but the ORM remote-cache abstraction itself is complete and no mandatory Redis dependency is desired.

The port continues to prefer explicit unsupported boundaries over fake compatibility layers or silently lossy behavior.
