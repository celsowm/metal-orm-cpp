# MetalORM C++

![CI](https://github.com/celsowm/metal-orm-cpp/actions/workflows/ci.yml/badge.svg?branch=main)

> A C++26-native port of MetalORM built around static reflection, annotations, splicing and expansion statements.

**Version:** `0.0.14`

MetalORM C++ deliberately has no compatibility metadata layer and no C++20/23 fallback. The TypeScript `metal-orm` repository is the behavioral and architectural reference; C++26 changes the mechanism, not the ORM semantics.

For now, **SQLite is intentionally the only executor/dialect**.

## Requirements

- GCC 16+
- `-std=c++26 -freflection`
- CMake 3.20+
- SQLite 3 development headers

## Reflection-native models

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

There are no registration macros or duplicated `entity_traits<T>` declarations. Columns, keys and relations are discovered from the C++ type itself.

## Runtime architecture

```text
Session
  ├── IdentityMap
  ├── UnitOfWork
  │     └── nested rollback checkpoints
  └── RelationChangeProcessor
          │
          ▼
      shared DML AST
          │
          ▼
        SQLite
```

The Unit of Work and relation mutation use the same INSERT/UPDATE/DELETE AST instead of maintaining a second hand-written SQL path.

## Transactions and savepoints — 0.0.14

Transaction capability is part of the executor contract. SQLite exposes both transactions and savepoints.

```cpp
session.transaction([](metal::Session& tx) {
    auto user = tx.find<User>(1);
    user->name = "outer";

    tx.transaction([](metal::Session& nested) {
        // SAVEPOINT metalorm_sp_1
        // changes are flushed before RELEASE SAVEPOINT
    });
});
```

The outer scope uses `BEGIN / COMMIT`. Nested scopes use `SAVEPOINT / RELEASE SAVEPOINT`. A nested failure executes `ROLLBACK TO SAVEPOINT` and marks the outer transaction rollback-only; catching the nested exception does not make the outer transaction committable.

Unlike a SQL-only wrapper, the C++ runtime checkpoints ORM state at every transaction/savepoint boundary. Rollback restores:

- reflected scalar values;
- dirty-check `original` snapshots and entity status;
- tracked entities removed during a failed DELETE;
- generated primary keys assigned by rolled-back INSERTs;
- Identity Map membership;
- reflected relation wrapper state, including collection baselines and pending pivot/morph changes.

A successful inner savepoint does not destroy the outer checkpoint. Therefore a later outer rollback can still undo an inner INSERT and restore relation state even after the inner scope was successfully flushed and released.

`Session::commit()` uses the same checkpoint mechanism. If database commit fails, ORM state is restored instead of leaving a generated ID or dirty snapshot falsely marked as committed.

## Relation collections

MetalORM C++ mirrors the distinct TypeScript relation roles:

```cpp
metal::has_many_collection<Post>
metal::many_to_many_collection<Role, UserRole>
metal::morph_one_reference<Cover>
metal::morph_many_collection<Attachment>
metal::morph_to_reference<Post, Video>
```

Many-to-many relations support entity or target-key attach/detach, `sync_by_ids()`, typed pivot hydration, alternate `targetKey`, and partial C++26 pivot patches:

```cpp
metal::pivot_patch<UserRole> patch;
patch
    .set<^^UserRole::label>(std::string{"owner"})
    .set<^^UserRole::weight>(std::int64_t{10});

user->roles.attach(role, patch);
```

The patch accepts only reflected members of the declared pivot type and validates value compatibility at compile time.

## Typed SELECT AST

Fields carry their entity owner in the C++ type:

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

`Post` fields do not satisfy the query constraints until a reflected join introduces `Post` into the typed query scope.

The SELECT AST includes:

- reflected INNER/LEFT joins, including N:N pivot expansion;
- predicates, NULL/LIKE/IN/BETWEEN and subqueries;
- aggregates, GROUP BY and HAVING;
- CTEs and recursive CTEs;
- UNION / UNION ALL / INTERSECT / EXCEPT;
- window functions;
- derived tables / `from_subquery`;
- searched CASE;
- recursive typed SQL function expressions.

## Computed expressions

Computed values share one recursive type:

```text
ScalarTerm<Result, Owners...>
  ├── reflected column
  ├── literal parameter
  ├── aggregate
  ├── SQL function
  ├── CASE
  └── window function
```

This preserves query ownership through nesting:

```cpp
auto normalized = metal::lower(
    metal::trim(metal::field<^^User::name>));

auto band = metal::case_when(
        metal::field<^^User::score> > 20,
        std::string{"high"})
    .otherwise(std::string{"normal"});
```

The SQLite function catalog includes broad text, numeric, control-flow, date/time and JSON families. Optional SQLite math/extension functions remain dependent on the linked SQLite build.

## Relation query predicates — 0.0.13

Relation filtering is reflection-driven; relation names and correlation keys are not supplied as strings.

```cpp
auto users = metal::where_has<^^User::posts>(
    metal::select<User>(),
    [](auto& posts) {
        posts.where(
            metal::like(
                metal::field<^^Post::title>,
                "C++%"));
    });
```

For a direct target predicate:

```cpp
auto admins = metal::where_relation<^^User::roles>(
    metal::select<User>(),
    metal::field<^^Role::name> == "admin");
```

`belongs_to`, `has_one`, `has_many`, N:N, `morph_one`, and `morph_many` correlations derive their keys from relation metadata. `morph_to` is rejected for `where_has` because one relation can resolve to different physical target tables, matching the TypeScript restriction.

0.0.13 moved relation correlation into the SELECT compiler's `WHERE` position. Callback-local `ORDER BY / LIMIT / OFFSET` therefore runs after correlation, while root relation predicates run before root pagination. Nested correlated scopes use internal aliases such as `t0`, `t0_rel`, `t0_rel_rel`, preventing child subqueries from shadowing outer aliases.

`match_relation<^^Relation>(...)` shares the same typed correlation engine. TypeScript renders `match()` as INNER JOIN + DISTINCT; C++ uses EXISTS to keep relation correlation in one implementation while preserving root-filtering behavior.

## Offset pagination — 0.0.13

Level 1 row pagination stays independent of Session:

```cpp
auto result = metal::execute_paged(
    query,
    executor,
    dialect,
    metal::PageOptions{.page = 2, .page_size = 25});
```

For tracked root entities:

```cpp
auto result = metal::execute_paged(
    query,
    session,
    metal::PageOptions{.page = 1, .page_size = 25});
```

Pagination helpers own the requested page and strip earlier query `LIMIT/OFFSET`. The Session path is root-aware: explicit 1:N/N:N joins are deduplicated by reflected root PK while preserving query order, and roots are materialized through the Identity Map.

Tracked pagination requires a complete root-entity projection. DTO/partial projections should use the row overload.

## Cursor pagination — 0.0.13

Cursor ordering uses reflected fields:

```cpp
std::vector order{
    metal::cursor_order(metal::field<^^User::score>, false),
    metal::cursor_order(metal::field<^^User::id>)
};

auto page = metal::execute_cursor(
    metal::select<User>(),
    session,
    order,
    metal::CursorPageOptions{.first = 25});
```

The keyset implementation supports lexicographic multi-column predicates, mixed ASC/DESC, `first/after`, `last/before`, `limit + 1`, non-null cursor values, ordering signatures, and tracked-root deduplication for row-multiplying joins.

Cursor encoding is intentionally opaque and internal to the C++ API; semantic parity does not imply TypeScript/C++ wire-format interchange.

## Shared DML AST

```cpp
auto insert = metal::InsertQueryBuilder{"users"}
    .values({
        {{"name", std::string{"Alice"}}},
        {{"name", std::string{"Bob"}}}
    })
    .returning({"id", "name"});
```

Supported DML includes multi-row INSERT, typed INSERT ... SELECT, RETURNING on INSERT/UPDATE/DELETE, and SQLite ON CONFLICT DO NOTHING / DO UPDATE with `excluded(column)`.

## Query module structure

```text
metal/query.hpp
  ├── query/core.hpp
  │    ├── core_types.hpp
  │    └── compiler.hpp -> sqlite_compiler.hpp
  ├── query/expressions.hpp
  ├── query/functions.hpp
  ├── query/select.hpp
  ├── query/relation_queries.hpp
  ├── query/relation_match.hpp
  └── query/pagination.hpp

metal/runtime_pagination.hpp
  └── Session / IdentityMap integration
```

## C++26 machinery used directly

```cpp
^^User
^^User::name
std::meta::info
std::meta::nonstatic_data_members_of(...)
std::meta::annotations_of(...)
std::meta::annotations_of_with_type(...)
std::meta::type_of(...)
std::meta::parent_of(...)
std::meta::identifier_of(...)
std::define_static_array(...)
template for (...)
entity.[:Member:]
```

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The project intentionally rejects GCC < 16 and non-GNU compilers today. Another compiler should be supported when it implements the same C++26 reflection model, not through a legacy metadata fallback.

See [`docs/PARITY.md`](docs/PARITY.md) for the explicit TypeScript parity matrix and ordered remaining gaps, and [`CHANGELOG.md`](CHANGELOG.md) for release history.
