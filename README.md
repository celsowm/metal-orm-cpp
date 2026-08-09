# MetalORM C++

![CI](https://github.com/celsowm/metal-orm-cpp/actions/workflows/ci.yml/badge.svg?branch=main)

> A C++26-native port of MetalORM built around static reflection, annotations, splicing and expansion statements.

**Version:** `0.0.26`

MetalORM C++ deliberately has no C++20/23 compatibility layer. The TypeScript [`metal-orm`](https://github.com/celsowm/metal-orm) repository is the behavioral and architectural reference; C++26 changes the mechanism, not the ORM semantics.

For now, **SQLite is intentionally the only executor/dialect** while semantic parity is built out.

See [`docs/PARITY.md`](docs/PARITY.md) for the detailed parity matrix.

## Requirements

- GCC 16+
- `-std=c++26 -freflection`
- CMake 3.20+
- SQLite 3 development headers

## Reflection-native entities

```cpp
#include <metal/metal.hpp>

struct [[=metal::mapping::table{"posts"}]] Post {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::int64_t user_id{};
    std::string title;
};

struct [[=metal::mapping::table{"users"}]] User {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;

    [[=metal::mapping::has_many<^^Post::user_id>{}]]
    metal::has_many_collection<Post> posts;
};
```

There are no registration macros or duplicated `entity_traits<T>` declarations. Columns, keys and relations are discovered from the C++ type itself and validated with `consteval` reflection.

## Runtime

```text
Session
  ├── IdentityMap
  ├── UnitOfWork
  ├── RelationChangeProcessor
  ├── lifecycle hooks / interceptors
  ├── DomainEventBus
  └── nested transaction checkpoints
          │
          ▼
      shared DML AST
          │
          ▼
        SQLite
```

The runtime includes rollback-safe generated IDs and relation state, nested SAVEPOINTs, Session-bound lifecycle hooks, post-commit domain events, graph persistence and lazy/eager mutable relation wrappers.

## Querying

```cpp
auto query = metal::select<User>()
    .join<^^User::posts>()
    .project(metal::field<^^User::name>)
    .project(
        metal::count(metal::field<^^Post::id>)
            .as("post_count"))
    .where(
        metal::like(metal::field<^^User::name>, "C%") &&
        metal::between(metal::field<^^Post::id>, 1, 100))
    .group_by(metal::field<^^User::name>)
    .having(metal::count(metal::field<^^Post::id>) > 1);
```

The SELECT AST includes reflected JOINs, typed predicates/subqueries, first-class scalar arithmetic (`+ - * / %`), aggregates, CTEs/recursive CTEs, set operations, derived tables, CASE, windows, typed functions, relation predicates and offset/keyset pagination.

## Relation wrappers

```cpp
metal::belongs_to_reference<Author>
metal::has_one_reference<Profile>
metal::has_many_collection<Post>
metal::many_to_many_collection<Role, UserRole>
metal::morph_one_reference<Cover>
metal::morph_many_collection<Attachment>
metal::morph_to_reference<Post, Video>
```

Raw `std::shared_ptr<T>` is not a valid reflected `belongsTo`/`hasOne` shape. Dedicated references expose `load/get/set/reset/dirty`, use the Identity Map and participate in the normal relation processor and rollback checkpoints.

## Graph persistence

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

`save_graph`, `update_graph`, `patch_graph`, pruning and typed N:N pivot patches reuse the same transactional Session/UoW infrastructure.

## DTO / REST / OpenAPI — 0.0.24

DTO metadata is derived from the entity itself. Public API field names remain C++ member identifiers even when a member maps to a different physical SQL column. Database defaults are now reflection-native too:

```cpp
struct [[=metal::mapping::table{"users"}]] User {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::column{"display_name"}]]
    std::string displayName;

    [[=metal::mapping::default_text{"active"}]]
    std::string status;

    [[=metal::mapping::default_value{false}]]
    bool disabled{};

    [[=metal::mapping::default_sql{"CURRENT_TIMESTAMP"}]]
    std::string createdAt;

    [[=metal::mapping::default_null]]
    std::optional<std::string> bio;
};
```

`default_value{...}` handles numeric/bool literals, `default_text{...}` handles quoted text, `default_sql{...}` preserves raw SQL expressions and `default_null` represents an explicit nullable default. The same reflected declaration drives SQLite DDL, expected-schema metadata/diffing and create-DTO/OpenAPI requiredness. Defaults such as `0` and `false` are recognized by metadata presence rather than truthiness.

`displayName` is the DTO/OpenAPI key while `display_name` remains the SQL column. Generated members are excluded from create/update DTOs, update fields are optional, `std::optional<T>` drives nullability, and a non-null create field with a database default is correctly optional in the create contract.

Scalar REST filtering is allowlisted with reflected members rather than free-form SQL names:

```cpp
metal::FilterInput filters{{
    metal::filter_clause(
        "displayName",
        metal::FilterOperator::contains,
        metal::Value{std::string{"cel"}},
        metal::StringFilterMode::insensitive),
    metal::filter_clause(
        "age",
        metal::FilterOperator::gte,
        metal::Value{std::int64_t{18}})
}};

auto query = metal::apply_filter<
    ^^User::displayName,
    ^^User::age>(metal::select<User>(), filters);
```

The filter compiler resolves the public API key back to the reflected member and reuses the shared SELECT AST. It supports equality, `IN`/`NOT IN`, numeric ordering operators, string contains/starts/ends predicates, null checks and case-insensitive matching through `LOWER()`. Unknown fields, disallowed fields, invalid operator/type combinations and incompatible values fail before SQL execution.

0.0.23 added recursive relation-aware `WhereInput` on top of the existing relation-query compiler:

```cpp
metal::WhereInput postFilter = metal::where_input(metal::FilterInput{{
    metal::filter_clause(
        "title",
        metal::FilterOperator::contains,
        metal::Value{std::string{"C++"}})
}});

metal::WhereInput where;
where.relations.push_back(
    metal::relation_filter("posts").some(std::move(postFilter)));

auto query = metal::apply_where(
    metal::select<User>(),
    where,
    metal::DtoMemberPolicy<>{},
    metal::DtoRelationPolicy<^^User::posts>{});
```

`some`, `none`, `every`, `isEmpty` and `isNotEmpty` compile through the existing correlated `EXISTS` machinery for belongsTo, hasOne, hasMany, N:N, morphOne and morphMany. Nested relation filters are recursive. `every` intentionally preserves the TypeScript runtime's **non-vacuous** behavior: the relation must contain at least one row and no related row may fail the predicate. `morphTo` remains discriminator-dependent and unsupported by relation filtering in both the relation-query model and this REST binding.

Safe runtime sorting uses the same reflection model and appends the reflected primary key as a deterministic tie-breaker. The `WhereInput` overload of `execute_filtered_paged()` composes scalar/relation filters, allowlisted sorting, the existing Session-level root pagination engine and enhanced `PagedResponse` metadata.

OpenAPI schemas are framework-independent C++ objects derived from the same reflection metadata. Response/create/update DTO schemas, recursive relation filter schemas, nested DTOs, update-with-single-relations schemas, pagination, OpenAPI 3.0/3.1 nullability, route documents, Tree/MPTT components and relation component maps are available. Component helpers add deep cloning, stable canonical hashing, deterministic names, reusable-schema extraction and `$ref` replacement without introducing a JSON or web-framework dependency.

DTO/OpenAPI is now **✅ parity for the supported SQLite execution model**. `morphTo` filtering remains the same discriminator-dependent relation-query limitation rather than a DTO-specific gap.

## Query cache — 0.0.25

The TypeScript query-cache contract is available without pushing cache state into the SQL AST:

```cpp
auto provider = std::make_shared<metal::MemoryCacheAdapter>();
metal::QueryCacheManager cacheManager{provider};
metal::CacheSession cached{
    session,
    cacheManager,
    metal::CacheTenantId{std::int64_t{42}}
};

auto query = metal::cache(
    metal::select<User>(),
    "active-users",
    metal::Duration{"30m"},
    {"users", "dashboard"});

auto users = cached.execute(query);
cached.invalidate_cache_tags({"users"});
```

The cache core keeps the TypeScript ISP split with `CacheReader`, `CacheWriter`, `CacheInvalidator` and `CacheProvider`, plus optional tag-registration, clear and statistics capabilities. `MemoryCacheAdapter` provides TTL, tags, prefix invalidation, clear/statistics and thread-safe storage; `TagIndex` maintains bidirectional tag/key membership.

`QueryCacheManager` implements the same execute-around contract: tenant-aware key generation (`tenant:<id>:<key>`), cache hit before execution, conditional caching, TTL, tag registration and key/tag/prefix invalidation. Human-readable durations support `s`, `m`, `h`, `d` and `w` just like the reference API.

C++ caches typed `QueryResult` rows rather than forcing a generic JSON serializer into the core. Entity cache hits are therefore re-hydrated through the existing `Session` path and Identity Map. A cached result does not create a second instance of an already tracked entity. Both ordinary `SelectQuery` and correlated `RelationFilteredQuery` are supported by the same wrapper.

`auto_invalidate` is preserved as cache configuration but is intentionally not given invented behavior: the current TypeScript manager/facet stores that flag without consuming it during mutation execution. Explicit key/tag/prefix invalidation therefore remains the behavioral contract in both bindings.

The **cache core is complete for the supported SQLite model**. Overall cache parity remains 🟡 only at the adapter/ecosystem boundary: the TypeScript package bundles Keyv and ioredis adapters, while the C++ library currently exposes the provider extension point and the in-memory provider without choosing a mandatory Redis client dependency.

## Procedure calls — 0.0.26

Procedure calls have a dedicated AST and execution capability surface:

```cpp
auto procedure = metal::call_procedure("refresh_user", std::string{"admin"})
    .in("user_id", std::int64_t{7})
    .out("total", std::string{"INTEGER"})
    .in_out("state", std::string{"pending"}, std::string{"TEXT"});
```

`ProcedureCall`, `ProcedureRef` and ordered `ProcedureParam` nodes preserve `IN`, `OUT` and `INOUT` directions, optional schema and optional database type metadata. `CompiledProcedureCall` carries the SQL/params plus the ordered OUT names and whether those values come from the first or last result set, matching the reference distinction between PostgreSQL and MySQL/MSSQL.

C++ keeps procedure support behind segregated `ProcedureCompiler` and `ProcedureExecutor` capabilities rather than bloating every `Dialect` and `DbExecutor` implementation. `ProcedureExecutionResult` returns all result sets plus a case-insensitive name-to-`Value` OUT map. Missing result sets, empty OUT sets and absent OUT columns fail explicitly.

SQLite intentionally does **not** implement those capabilities. The TypeScript `SqliteDialect` also throws for stored procedures, so C++ rejects the call at the same database-capability boundary rather than emitting fake `CALL` SQL. The parity suite uses a synthetic procedure-capable dialect/executor to prove compilation, multi-result execution and both first/last OUT extraction without pretending SQLite has stored procedures.

Procedure calls are **✅ parity for the supported SQLite execution model**: the complete public/vendor-independent contract exists, and SQLite's correct behavior is explicit unsupported rejection.

## Tree / MPTT — 0.0.21

Tree metadata is reflection-native rather than string-configured:

```cpp
struct [[=metal::mapping::table{"categories"}]] Category {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;

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

auto tree = metal::create_tree_manager<Category>(session)
    .with_scope<^^Category::tenant_id>(42);

auto root_id = tree.insert_as_child(
    metal::Value{nullptr},
    metal::tree_row<Category>()
        .set<^^Category::name>("Root")
        .build());
```

The 0.0.21 Tree/MPTT pass includes reflected mapping validation, `TreeQuery<T>`, `TreeManager<T>`, ancestor/descendant/path/root/child/sibling/depth/leaf queries, threaded descendants, insert-as-child/root, sibling and subtree movement, `remove_from_tree()`, subtree deletion, recovery and structural validation.

`TreeQuery::find_leaves()` is now a normal typed SELECT built from scalar arithmetic: `subtract(field<Right>, field<Left>) == 1`. The shared arithmetic AST supports `+`, `-`, `*`, `/` and integral `%`, so Tree no longer needs a special raw SELECT for leaf discovery.

Multi-tree scope is applied to both reads **and boundary-changing mutations**. Subtree moves isolate the moving range below zero while gaps are closed/opened so the temporary subtree cannot be shifted by its own destination-gap update; the E2E suite explicitly moves a width-4 subtree and checks exact boundaries. `remove_from_tree()` promotes direct children to the removed node's parent, compacts the descendant forest by one depth level and retains the detached node as a standalone root with valid boundaries.

Tree/MPTT is now marked **✅ parity for the supported SQLite execution model** in [`docs/PARITY.md`](docs/PARITY.md).

## Bulk operations — 0.0.19

Bulk rows and column selections use reflected members rather than duplicated string column names:

```cpp
std::vector<metal::BulkRow> rows{
    metal::bulk_row<User>()
        .set<^^User::name>("Ada")
        .build(),
    metal::bulk_row<User>()
        .set<^^User::name>("Grace")
        .build()
};

metal::BulkInsertOptions options;
options.chunk_size = 500;
options.transactional = true;
options.returning = metal::bulk_columns<^^User::id, ^^User::name>();

auto result = metal::bulk_insert<User>(session, rows, options);
```

The SQLite binding now mirrors the TypeScript bulk subsystem:

- `bulk_insert<T>()` uses multi-row INSERT chunks;
- `bulk_update<T>()` preserves the reference strategy of one identity-aware UPDATE per row;
- `bulk_update_where<T>()` updates ID chunks with `IN (...)`;
- `bulk_delete<T>()` deletes ID chunks with `IN (...)`;
- `bulk_delete_where<T>()` executes a single typed predicate delete;
- `bulk_upsert<T>()` uses multi-row `INSERT ... ON CONFLICT`, including DO NOTHING and `excluded(...)` updates;
- chunk size, bounded concurrency, transactional/non-transactional execution, chunk timings and completion callbacks are supported;
- INSERT/UPDATE/UPSERT support reflected RETURNING selections;
- `by`, conflict, update and RETURNING columns can be selected from `^^T::member` reflections instead of free-form strings.

Bulk operations reuse the existing DML AST and `Session::transaction()` implementation. The SQLite executor serializes access to its single connection so bounded bulk workers cannot race the underlying handle.

## SQLite schema introspection and synchronization — 0.0.18

Introspect the live database:

```cpp
auto actual = metal::introspect_sqlite(
    session.executor(),
    metal::IntrospectOptions{
        .exclude_tables = {"schema_comments"},
        .include_views = true
    });
```

Build expected schema from reflected entities:

```cpp
auto expected = metal::expected_schema<User, Post>(dialect);

metal::add_expected_index<
    User,
    ^^User::name>(
        expected,
        dialect,
        "users_name_idx");
```

Diff without touching the database:

```cpp
auto plan = metal::diff_schema(
    expected,
    actual,
    dialect);
```

Or synchronize directly:

```cpp
auto plan = metal::synchronize_schema(
    expected,
    session.executor(),
    dialect,
    metal::SynchronizeOptions{
        .allow_destructive = false,
        .dry_run = false
    });
```

The SQLite introspector reads:

- tables and columns;
- ordered primary keys;
- type/nullability/default values;
- AUTOINCREMENT;
- physical foreign keys and referential actions;
- user indexes;
- views;
- optional `schema_comments` table/column comments.

The diff/synchronizer follows the current TypeScript safety policy:

- create table / add column / add index are safe;
- drop table / drop index require `allow_destructive=true`;
- SQLite ALTER COLUMN emits a warning instead of fake SQL;
- SQLite DROP COLUMN emits a rebuild warning and no automatic rebuild;
- `dry_run=true` executes nothing.

Reflected defaults now feed the expected schema and diff engine directly. ORM relation metadata is still not silently converted into physical FK constraints; the TypeScript reference also keeps relation definitions separate from schema `references` metadata.

## Transactions

```cpp
session.transaction([](metal::Session& tx) {
    auto user = tx.find<User>(1);
    user->name = "outer";

    tx.transaction([](metal::Session&) {
        // SAVEPOINT metalorm_sp_1
    });
});
```

A failed nested scope rolls back to its SAVEPOINT and marks the outer transaction rollback-only. Rollback restores database and ORM memory state.

## Build

```bash
cmake -S . -B build \
  -DCMAKE_CXX_COMPILER=g++-16 \
  -DMETAL_ORM_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

MetalORM intentionally refuses older compilers instead of shipping a compatibility metadata system.

## Current roadmap

0.0.26 closes procedure-call parity for the supported SQLite execution model by matching the reference's explicit SQLite rejection while providing the vendor-independent AST and capability execution contract. The next parity pass moves to pooling, followed by DB-to-entity generation. A first-party remote-cache adapter remains an ecosystem decision rather than a reason to couple the core to a specific C++ Redis client; physical FK/check declaration metadata remains a separate schema-layer gap.

See [`CHANGELOG.md`](CHANGELOG.md) and [`docs/PARITY.md`](docs/PARITY.md) for release-by-release details and remaining gaps.
