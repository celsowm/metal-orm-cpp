# MetalORM C++

![CI](https://github.com/celsowm/metal-orm-cpp/actions/workflows/ci.yml/badge.svg?branch=main)

> A C++26-native port of MetalORM built around static reflection, annotations, splicing and expansion statements.

**Version:** `0.0.20`

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

The SELECT AST includes reflected JOINs, typed predicates/subqueries, aggregates, CTEs/recursive CTEs, set operations, derived tables, CASE, windows, typed functions, relation predicates and offset/keyset pagination.

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

## Tree / MPTT — 0.0.20

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

The 0.0.20 baseline ports the TypeScript Nested Set/MPTT model with reflected mapping validation, `TreeQuery<T>`, `TreeManager<T>`, ancestor/descendant/path/root/child/sibling/depth queries, leaf discovery, threaded descendants, insert-as-child/root, sibling movement, subtree movement, subtree deletion, recovery and structural validation.

Multi-tree scope is applied to both reads **and boundary-changing mutations**. This intentionally hardens an edge in the current TypeScript manager where raw `lft/rght` shift SQL does not consistently append scope conditions. Subtree moves also isolate the moving range below zero while gaps are closed/opened so the temporary subtree cannot be shifted by its own destination-gap update.

The remaining Tree edge is narrow: the generic SELECT AST still lacks a first-class arithmetic scalar node, so `TreeManager::get_leaves()` currently emits the tiny `(rght - lft) = 1` predicate directly instead of exposing `TreeQuery::find_leaves()` as a normal `SelectQuery<T>`. The questionable TS `removeFromTree()` semantics are also intentionally not copied until that source behavior is clarified/fixed.

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

ORM relation metadata is not silently converted into physical FK constraints; the TypeScript reference also keeps relation definitions separate from schema `references` metadata.

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

Tree/MPTT is now the 0.0.20 baseline. The immediate next pass should close the two explicit Tree edges — arithmetic-expression support so leaves stay inside the generic SELECT AST, and a source-level decision/fix for the current TypeScript `removeFromTree()` behavior — before moving on to DTO/OpenAPI, cache, procedure-call, pooling and DB-to-entity generation gaps.

See [`CHANGELOG.md`](CHANGELOG.md) and [`docs/PARITY.md`](docs/PARITY.md) for release-by-release details and remaining gaps.
