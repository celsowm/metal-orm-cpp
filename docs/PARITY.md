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
| Reflected database defaults | ✅ | typed literal/text/null + raw SQL annotations |
| Identity Map | ✅ | Separate runtime component |
| Unit of Work | ✅ | Separate component; shared DML AST |
| Session coordinator | ✅ | Coordinates UoW, Identity Map and relation processor |
| Dirty snapshots | ✅ | Reflection-generated |
| Persist/remove lifecycle | ✅ | Aligned with TS semantics |
| Transactional `commit()` | ✅ | executor capabilities + rollback restoration |
| Nested transactions/savepoints | ✅ | BEGIN outer; SAVEPOINT/RELEASE inner |
| rollback-only nested failure | ✅ | inner failure poisons outer scope |
| rollback-safe in-memory UoW state | ✅ | scalar/runtime checkpoints |
| rollback-safe generated IDs | ✅ | generated PK returns to checkpoint value |
| rollback-safe relation state | ✅ | reflected relation-wrapper snapshots |
| Table lifecycle hooks | ✅ | Session-bound in both TS and C++ |
| Session interceptors | ✅ | `before_flush` / `after_flush` |
| Domain events | ✅ | dispatch only after successful outermost commit |
| saveGraph/updateGraph/patchGraph | ✅ | typed nested graph payloads, pivots, pruning and transaction integration |

## Transaction parity — 0.0.14

SQLite exposes transaction/savepoint capabilities explicitly. `Session::transaction()` mirrors the TypeScript nested-transaction contract: outer scope uses BEGIN/COMMIT, nested scope uses SAVEPOINT/RELEASE, and any failed nested scope marks the Session rollback-only. Checkpoints restore scalar values, generated IDs, tracking status, Identity Map membership, relations and event queues.

## Lifecycle and domain events — 0.0.15

Table hooks run inside the Unit of Work; Session interceptors wrap the full commit pipeline. Raw `Session::flush()` remains UoW-only. Domain events dispatch after the successful outermost COMMIT and never on SAVEPOINT release. Post-COMMIT handler failures propagate without pretending that the database commit was rolled back.

## Graph persistence — 0.0.16

C++ represents the TS DTO graph contract with reflected `graph<T>()` payloads:

```cpp
auto payload = metal::graph<User>()
    .set<^^User::name>(std::string{"Celso"})
    .relation<^^User::posts>([](auto& posts) {
        posts.add(
            metal::graph<Post>()
                .set<^^Post::title>(std::string{"Reflection"}));
    });

auto user = metal::save_graph(session, payload);
```

`save_graph`, `update_graph`, `patch_graph`, `prune_missing`, nested single/collection relations, IDs, N:N pivots, generated keys, lifecycle hooks and domain events share the same transactional runtime.

## Dedicated single references — 0.0.17

`belongsTo` and `hasOne` use dedicated wrappers exclusively:

```cpp
metal::belongs_to_reference<User> author;
metal::has_one_reference<Profile> profile;
```

Raw `std::shared_ptr<T>` annotated as either relation is rejected at compile time. The wrappers provide lazy/eager hydration, `load/get/set/reset`, dirty baselines, cascade-persist integration and rollback-safe state.

## Relations

| MetalORM capability | C++ status | Notes |
| --- | --- | --- |
| belongsTo | ✅ | dedicated reference, lazy/eager, target-key FK sync, rollback |
| hasOne | ✅ | dedicated reference, replacement/detach, cascade and rollback |
| hasMany | ✅/🟡 | dedicated collection; JS-object conveniences are language-specific |
| belongsToMany | ✅ | lazy/eager, IDs, sync, typed pivot patches, alternate `targetKey` |
| morphTo | ✅ | typed target set, lazy resolution, switching/reset |
| morphOne | ✅ | dedicated reference, lazy/eager, mutation/cascade |
| morphMany | ✅ | dedicated collection, lazy/eager, mutation/cascade |
| cascade none/all/persist/remove/link | ✅ | aligned vocabulary/runtime behavior |

## Query builder and DML

| MetalORM capability | C++ status | Notes |
| --- | --- | --- |
| Typed SELECT AST | ✅ | compile-time entity scope |
| comparisons/logical predicates | ✅ | typed scalar operands |
| scalar arithmetic `+ - * / %` | ✅ | recursive typed scalar AST; optionality propagated |
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
| DML typed predicate reuse | ✅ | UPDATE/DELETE can reuse `Expression<T>` plus chunked IN predicates |

## Relation-query parity — 0.0.13

`where_has`, `where_has_not`, `where_relation` and behavioral `match_relation` use reflected correlation. Correlation is compiled in the real WHERE position before child/root pagination. Nested scopes use hierarchical aliases to avoid shadowing.

## Pagination parity — 0.0.13

Raw executor pagination is row-oriented. Session pagination is root-oriented and deduplicates row-multiplying joins by reflected root PK. Cursor pagination supports forward/backward mode, `limit + 1`, multi-column lexicographic keys, mixed ASC/DESC, ordering signatures and root deduplication.

## SQLite schema parity — 0.0.24

Schema state is represented independently from ORM mapping:

```cpp
auto actual = metal::introspect_sqlite(executor, {
    .exclude_tables = {"schema_comments"},
    .include_views = true
});

auto expected = metal::expected_schema<User, Post>(dialect);
metal::add_expected_index<User, ^^User::email>(
    expected,
    dialect,
    "users_email_idx",
    true);

auto plan = metal::diff_schema(expected, actual, dialect);
```

The SQLite introspector reads tables, ordered PK columns, column type/nullability/default, AUTOINCREMENT, foreign-key metadata/actions, user indexes, views and optional `schema_comments` table/column comments. Include/exclude table and view filters mirror the TS surface.

Two SQLite introspection normalizations intentionally avoid false self-diffs that exist in the current TS implementation: a PRAGMA primary-key column is treated as non-null even when SQLite reports `notnull=0`, and AUTOINCREMENT is detected from the table DDL instead of being hard-coded false. A schema created by MetalORM must not immediately diff against itself.

Expected schema is reflection-derived. Index columns are declared with reflected members rather than strings:

```cpp
metal::add_expected_index<User, ^^User::email, ^^User::tenant_id>(
    expected, dialect, "users_email_tenant_idx", true);
```

0.0.24 adds shared reflected defaults:

```cpp
[[=metal::mapping::default_text{"active"}]]
std::string status;

[[=metal::mapping::default_value{0}]]
std::int64_t retries{};

[[=metal::mapping::default_sql{"CURRENT_TIMESTAMP"}]]
std::string created_at;

[[=metal::mapping::default_null]]
std::optional<std::string> note;
```

The same default metadata is rendered into `CREATE TABLE`, stored in `DatabaseColumn::default_value`, compared by schema diff and consumed by create DTO/OpenAPI requiredness. Text literals are SQL-escaped, numeric/bool defaults are typed, raw SQL is preserved, and `default_null` is restricted to nullable members. The E2E requires `expected -> create -> introspect -> diff` to converge with literal, falsy, raw and null defaults.

`diff_schema` / `synchronize_schema` follow the TS safety contract:

- missing table -> CREATE TABLE + expected indexes, safe;
- missing column -> ALTER TABLE ADD, safe;
- missing index -> CREATE INDEX, safe;
- extra table/index -> destructive and SQL is emitted only with `allow_destructive=true`;
- SQLite ALTER COLUMN -> warning only;
- SQLite DROP COLUMN -> warning + no rebuild SQL;
- `dry_run=true` never executes the plan.

ORM relation metadata is deliberately not treated as a physical FK constraint declaration. The TypeScript model also keeps relation metadata separate from column `references`. Introspection reports real FK constraints; reflected FK/check declaration remains a later DDL-metadata extension rather than hidden inference.

## Bulk parity — 0.0.19

The C++ binding mirrors the concrete TypeScript bulk subsystem rather than replacing it with a different batching model:

| TypeScript operation/behavior | C++ binding | Strategy |
| --- | --- | --- |
| `bulkInsert` | `bulk_insert<T>` | multi-row INSERT per chunk |
| `bulkUpdate` | `bulk_update<T>` | individual identity-aware UPDATE per row |
| `bulkUpdateWhere` | `bulk_update_where<T>` | UPDATE with `IN (...)` per ID chunk |
| `bulkDelete` | `bulk_delete<T>` | DELETE with `IN (...)` per ID chunk |
| `bulkDeleteWhere` | `bulk_delete_where<T>` | one typed predicate DELETE |
| `bulkUpsert` | `bulk_upsert<T>` | multi-row INSERT/ON CONFLICT per chunk |
| default chunk size 500 | `BulkBaseOptions::chunk_size = 500` | ✅ |
| sequential by default | `concurrency = 1` | ✅ |
| bounded numeric concurrency | worker pool | ✅ |
| transactional by default | existing `Session::transaction()` | ✅ |
| non-transactional partial progress | direct chunk execution | ✅ |
| timing / per-chunk callback | timings + `ChunkCompleteInfo` | ✅ |
| RETURNING on insert/update/upsert | reflected selections | ✅ |

Rows are constructed through `bulk_row<T>().set<^^T::member>(value)`. `by`, conflict, update and RETURNING selections use reflected members through `bulk_columns<^^T::member...>()`; this is an intentional C++26 binding improvement over the string-column portions of the TypeScript surface.

`bulk_update` deliberately remains one UPDATE per row because that is the reference implementation's current strategy. `bulk_delete` and `bulk_update_where` deliberately use `IN (...)` chunks, and `bulk_upsert` deliberately infers default update columns from the first row excluding conflict columns. These are parity choices, not accidental implementation limitations.

The shared DML AST gained reusable typed `Expression<T>` predicates and explicit IN predicates for UPDATE/DELETE, so bulk execution does not create a second SQL compiler. SQLite connection access is serialized inside `SQLiteExecutor`; bounded workers therefore preserve the TypeScript concurrency control contract without data-racing one SQLite handle.

## Tree / MPTT parity — 0.0.21

The TypeScript tree subsystem is based on the Nested Set/MPTT model. The C++ binding keeps the same behavior while replacing free-form tree column strings with reflected annotations:

```cpp
struct [[=metal::mapping::table{"categories"}]] Category {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::tree_parent]]
    std::optional<std::int64_t> parent_id;
    [[=metal::mapping::tree_left]]
    std::int64_t lft{};
    [[=metal::mapping::tree_right]]
    std::int64_t rght{};
    [[=metal::mapping::tree_depth]]
    std::optional<std::int64_t> depth;
    [[=metal::mapping::tree_scope]]
    std::int64_t tenant_id{};
};
```

`validate_tree_mapping<T>()` requires exactly one parent/left/right member, at most one depth member, a nullable parent key compatible with the reflected PK, integral boundaries/depth and persistent scalar scope members. Invalid nullable-parent shape has dedicated compile-fail coverage.

The completed SQLite binding includes:

- pure `NestedSetStrategy` calculations and parent-link recovery;
- reflected `TreeQuery<T>` for ancestors, descendants, direct children, parent lookup, siblings, roots, subtree, tree list, **leaves**, depth and ID lookup;
- Session-bound `TreeManager<T>` for node/root/child/descendant/path/sibling/parent reads;
- threaded descendants, leaf discovery, descendant count and level calculation;
- root/child insertion with `INSERT ... RETURNING`;
- move up/down, move to another parent/root and descendant-cycle rejection;
- `remove_from_tree()` with child promotion and retained-node detachment as a new root;
- subtree deletion;
- tree recovery and overlap/boundary validation;
- multiple trees in one table through reflected `tree_scope` values.

The scalar AST now has first-class binary arithmetic. `TreeQuery::find_leaves()` is therefore a normal typed `SelectQuery<T>` using `(rght - lft) = 1`; `TreeManager::get_leaves()` no longer owns a special raw SELECT path. Arithmetic `+`, `-`, `*`, `/` and integral `%` are independently covered against SQLite.

Three hardenings preserve the documented Tree contract instead of copying source-level traps. Scope conditions are appended to raw boundary-shift mutations, so changing tenant A cannot rewrite tenant B. Moving subtrees are isolated below zero before old/new gaps are closed/opened, and regression coverage includes a width-4 subtree rather than only leaves. Finally, `remove_from_tree()` promotes children, compacts the former subtree and retains the detached row as a valid root; it does not reproduce the current TypeScript stale-overlapping-boundary behavior.

The Tree/MPTT row is now ✅ for the supported SQLite execution model.

## DTO / REST / OpenAPI parity — 0.0.24

The TypeScript DTO subsystem includes response/create/update DTOs, transforms, scalar and relation-aware REST filters, safe sorting/paging helpers and OpenAPI generators. The C++ binding derives these surfaces directly from C++26 reflection instead of maintaining a second metadata model.

The 0.0.22 scalar baseline includes:

- response/create/update DTO descriptors from persistent reflected members;
- generated-field exclusion for create/update DTOs;
- public DTO keys based on C++ member identifiers while physical SQL names remain mapping concerns;
- reflected compile-time exclusion/pick policies with foreign-member rejection;
- runtime row transforms equivalent to response merge, defaults, exclude, pick and field mapping;
- enhanced `PagedResponse` metadata layered over the existing root-aware pagination runtime;
- allowlisted scalar REST filters resolved from public API names back to reflected members;
- equality, `IN`/`NOT IN`, numeric ordering, string contains/starts/ends, null checks and case-insensitive matching through the shared query AST;
- allowlisted dynamic sorting with reflected primary-key tie-breaking for deterministic pages;
- `execute_filtered_paged()` reusing `execute_paged()` rather than introducing a DTO-specific executor;
- response/create/update, scalar filter, pagination and Tree/MPTT OpenAPI schemas.

0.0.23 closes the relation/nested sub-pass:

- recursive `WhereInput` adds `some`, `every`, `none`, `isEmpty` and `isNotEmpty` relation clauses;
- root relation access is allowlisted with reflected `DtoRelationPolicy<^^T::relation...>` members;
- belongsTo, hasOne, hasMany, N:N, morphOne and morphMany reuse the existing correlated `EXISTS` relation-query compiler;
- nested relation predicates recurse through the same machinery, including multi-level filters such as user -> posts -> comments;
- `every(P)` intentionally matches the TypeScript runtime's non-vacuous semantics: at least one related row must exist and no related row may fail `P`;
- relation-aware `execute_filtered_paged()` composes filters, safe sort, tracked root pagination and enhanced page metadata;
- nested DTO OpenAPI schemas distinguish single-object and collection relation shapes;
- recursive relation-filter OpenAPI schemas expose the same `some/every/none/isEmpty/isNotEmpty` contract used by the runtime;
- update-with-relations schemas cover nested single relations without pretending collection update semantics that are not represented by that TS helper;
- component maps, deep schema cloning, stable canonical hashing, deterministic component naming, reusable-schema extraction and `$ref` replacement are framework-independent C++ utilities;
- real SQLite coverage exercises 1:N, N:N, recursive relation filtering, non-vacuous `every`, relation allowlists and relation-aware paged execution.

0.0.24 closes the final create-contract gap. Reflected database defaults now make non-null columns optional in create DTO/OpenAPI when SQLite can supply the value. `DtoField::has_default` exposes that fact without duplicating SQL metadata. Literal `0` and `false` are handled by annotation presence rather than JavaScript-style truthiness; raw SQL defaults also make create fields optional while remaining opaque expressions.

`morphTo` filtering remains explicitly unsupported because its target table depends on a runtime discriminator; this is already the relation-query limitation and is not papered over by the DTO layer. The TypeScript OpenAPI source has a single-relation schema path that does not fully mirror its runtime `RelationFilter` operator shape; the C++ generator deliberately follows the runtime/type contract uniformly instead of reproducing that inconsistency.

DTO/OpenAPI is now ✅ for the supported SQLite execution model.

## Query cache parity — 0.0.25

The TypeScript cache subsystem is an execute-around query-result cache with provider interfaces, duration helpers, tenant-aware keys, conditional caching, TTL and explicit invalidation. The C++ binding keeps that contract while using typed rows instead of forcing JavaScript serialization semantics into the core.

Implemented in 0.0.25:

- segregated `CacheReader`, `CacheWriter`, `CacheInvalidator` and `CacheProvider` interfaces, plus optional tag-registration, clear and statistics capabilities;
- `CacheCapabilities` for tag/prefix/TTL feature discovery;
- human-readable duration parsing/formatting for `s`, `m`, `h`, `d` and `w` plus millisecond values;
- thread-safe bidirectional `TagIndex`;
- thread-safe `MemoryCacheAdapter` with lazy TTL expiry, tag invalidation, prefix invalidation, clear, statistics and disposal;
- `DefaultCacheStrategy` with the reference `tenant:<tenantId>:<queryKey>` key shape and condition predicate;
- `QueryCacheManager` execute-around behavior, default TTL, key/tag/prefix invalidation, clear/stats/dispose and tag registration;
- generic `CachedQuery` / `cache()` wrappers that do not alter SQL AST compilation and work for ordinary SELECTs and `RelationFilteredQuery`;
- `CacheSession` composition for Session + cache-manager + tenant context without making the core `Session` own a cache dependency;
- entity hits cache `QueryResult` rows and re-hydrate through Session/Identity Map, preserving one entity identity rather than returning detached deserialized objects;
- real SQLite coverage with a counting executor proving hits skip new SELECTs, explicit tag invalidation refreshes stale rows, tenant keys isolate entries and correlated relation queries are cacheable.

`auto_invalidate` remains represented but intentionally inert. That matches the current TypeScript implementation: the option is stored by the cache facet, but `QueryCacheManager` and mutation execution do not consume it. C++ does not invent automatic mutation invalidation and then label the divergence parity.

The cache core is complete for the supported SQLite model. The overall cache row remains 🟡 only because the TypeScript package also ships ecosystem-specific Keyv and ioredis adapters. C++ exposes the same provider extension boundary plus the in-memory provider, but does not yet mandate a particular Redis client dependency.

## Procedure-call parity — 0.0.26

The TypeScript API models procedure calls independently from SELECT/DML with a procedure reference, ordered `IN`/`OUT`/`INOUT` parameters, optional schema, optional database type metadata, compiled OUT-result metadata and a multi-result execution contract. The C++ binding keeps those semantics while splitting optional database capabilities instead of forcing every dialect/executor to implement procedure methods.

0.0.26 adds:

- `ProcedureCall`, `ProcedureRef`, `ProcedureParam` and `ProcedureDirection` as first-class procedure AST types;
- immutable-style `call_procedure(...).in(...).out(...).in_out(...)` construction, including schema and `db_type` metadata;
- scalar procedure inputs backed by the same typed `ScalarPtr` representation used by the query AST;
- `CompiledProcedureCall` with `ProcedureOutSource::{None,FirstResultSet,LastResultSet}` and ordered OUT names;
- segregated `ProcedureCompiler` and `ProcedureExecutor` capability interfaces rather than extending the already-minimal `Dialect`/`DbExecutor` base contracts with methods unsupported by SQLite;
- `ProcedureExecutionResult` containing all result sets plus a name-to-`Value` OUT map;
- case-insensitive OUT-column matching and explicit errors for missing result sets, empty OUT result sets and absent OUT columns;
- regression coverage proving both first-result-set and last-result-set OUT extraction, preserving the semantic difference used by PostgreSQL versus MySQL/MSSQL in the reference implementation;
- explicit rejection when either the active dialect lacks procedure compilation or the executor lacks multi-result procedure execution.

SQLite stored procedures are **not** implemented because SQLite has no stored-procedure facility and the TypeScript `SqliteDialect.compileProcedureCall()` also throws `Stored procedures are not supported by the SQLite dialect.` The supported SQLite behavior is therefore an explicit rejection, not emulated `CALL` SQL. A synthetic capability dialect/executor in the C++ suite proves the vendor-independent AST/execution contract without pretending SQLite can execute procedures.

Procedure calls are ✅ for the supported SQLite execution model: the public contract is present and SQLite rejects exactly at the unsupported database capability boundary. Future PostgreSQL/MySQL/MSSQL dialects can implement `ProcedureCompiler`/`ProcedureExecutor` without modifying the core Session or query AST.

## Schema/tooling/ecosystem

| MetalORM capability | C++ status |
| --- | --- |
| SQLite DDL generation | ✅ foundational |
| composite PK DDL | ✅ |
| reflected column defaults | ✅ |
| SQLite schema introspection | ✅ |
| schema diff / plan execution | ✅/🟡 |
| schema synchronize / dry-run / destructive policy | ✅ |
| physical FK/check declaration metadata | ❌ |
| migration history/versioned migration runner | not present as a distinct TS subsystem |
| bulk operations | ✅ |
| DTO/OpenAPI | ✅ |
| Tree/MPTT | ✅ |
| cache layer | 🟡 |
| procedure calls | ✅ |
| pooling | ❌ |
| DB-to-entity code generation | ❌ |

The 🟡 on schema diff reflects the remaining expected-metadata surface, not the diff engine: reflected defaults are now first-class, while physical FK/check declarations still do not have C++ annotations. The 🟡 on cache is an adapter/ecosystem boundary: the cache core and in-memory implementation are present, while a first-party remote adapter awaits a deliberate C++ Redis dependency policy.

## Ordered parity roadmap

0.0.26 closes the procedure-call surface for the supported SQLite model by matching the reference's explicit SQLite rejection while providing the vendor-independent AST and capability-based execution contract. The next parity pass moves to pooling, followed by DB-to-entity generation. A first-party remote-cache adapter and physical FK/check declaration metadata remain separate integration/schema decisions and should continue to follow the reflection-native, no-compatibility approach.

A later performance pass may replace in-memory root pagination deduplication with a root-aware SQL page plan, provided it preserves the tested 0.0.13 semantics.
