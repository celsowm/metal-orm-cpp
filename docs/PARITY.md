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
| IN / NULL / LIKE | ✅ | values and subqueries |
| BETWEEN / NOT BETWEEN | ✅ | first-class expression AST |
| EXISTS / NOT EXISTS | ✅ | typed SELECT subqueries |
| reflected JOINs | ✅ | N:1 / 1:1 / 1:N / N:N |
| projections/aliases | ✅ | columns, aggregates, functions, CASE, windows |
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

## Relation-query parity — 0.0.13

`where_has`, `where_has_not`, `where_relation` and behavioral `match_relation` use reflected correlation. Correlation is compiled in the real WHERE position before child/root pagination. Nested scopes use hierarchical aliases to avoid shadowing.

## Pagination parity — 0.0.13

Raw executor pagination is row-oriented. Session pagination is root-oriented and deduplicates row-multiplying joins by reflected root PK. Cursor pagination supports forward/backward mode, `limit + 1`, multi-column lexicographic keys, mixed ASC/DESC, ordering signatures and root deduplication.

## SQLite schema parity — 0.0.18

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

`diff_schema` / `synchronize_schema` follow the TS safety contract:

- missing table -> CREATE TABLE + expected indexes, safe;
- missing column -> ALTER TABLE ADD, safe;
- missing index -> CREATE INDEX, safe;
- extra table/index -> destructive and SQL is emitted only with `allow_destructive=true`;
- SQLite ALTER COLUMN -> warning only;
- SQLite DROP COLUMN -> warning + no rebuild SQL;
- `dry_run=true` never executes the plan.

ORM relation metadata is deliberately not treated as a physical FK constraint declaration. The TypeScript model also keeps relation metadata separate from column `references`. Introspection reports real FK constraints, while reflected FK/check/default declaration remains a later DDL-metadata extension rather than hidden inference.

The 0.0.18 E2E requires the complete cycle `expected -> synchronize -> introspect -> diff` to converge to an empty plan with no warnings.

## Schema/tooling/ecosystem

| MetalORM capability | C++ status |
| --- | --- |
| SQLite DDL generation | ✅ foundational |
| composite PK DDL | ✅ |
| SQLite schema introspection | ✅ |
| schema diff / plan execution | ✅/🟡 |
| schema synchronize / dry-run / destructive policy | ✅ |
| physical FK/check/default declaration metadata | ❌ |
| migration history/versioned migration runner | not present as a distinct TS subsystem |
| bulk operations | ❌ |
| DTO/OpenAPI | ❌ |
| Tree/MPTT | ❌ |
| cache layer | ❌ |
| procedure calls | ❌ |
| pooling | ❌ |
| DB-to-entity code generation | ❌ |

The 🟡 on schema diff reflects the expected-metadata surface, not the diff engine: reflected tables currently expose scalar type/nullability/generated state and explicitly-added indexes, but do not yet have C++ annotations for physical FK/default/check constraints.

## Ordered parity roadmap

A fresh audit of the current TypeScript repository shows a concrete bulk subsystem (`bulk insert/update/delete/upsert`, chunking, transaction controls, returning and dialect strategy metadata). Therefore the next focused release is:

1. **0.0.19:** bulk insert/update/delete/upsert for SQLite, reusing the existing DML AST and Session transaction semantics; reflected `by`/returning columns instead of string column APIs where C++ can make them static.
2. Then re-audit DTO/OpenAPI, cache, Tree/MPTT, pooling and DB-to-entity generation against the current TS tree.

A later performance pass may replace in-memory root pagination deduplication with a root-aware SQL page plan, provided it preserves the tested 0.0.13 semantics.
