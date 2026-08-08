# MetalORM C++

![CI](https://github.com/celsowm/metal-orm-cpp/actions/workflows/ci.yml/badge.svg?branch=main)

> A C++26-native port of MetalORM built around static reflection, annotations, splicing and expansion statements.

**Version:** `0.0.13`

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
  └── RelationChangeProcessor
          │
          ▼
      shared DML AST
          │
          ▼
        SQLite
```

The Unit of Work and relation mutation use the same INSERT/UPDATE/DELETE AST instead of maintaining a second hand-written SQL path.

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

`where_has_not` produces the inverse relation condition:

```cpp
auto users_without_posts =
    metal::where_has_not<^^User::posts>(
        metal::select<User>());
```

For a direct target predicate:

```cpp
auto admins = metal::where_relation<^^User::roles>(
    metal::select<User>(),
    metal::field<^^Role::name> == "admin");
```

Relation filters can be composed:

```cpp
auto admins_with_cpp_posts =
    metal::where_has<^^User::posts>(
        admins,
        [](auto& posts) {
            posts.where(
                metal::like(
                    metal::field<^^Post::title>,
                    "C++%"));
        });
```

`belongs_to`, `has_one`, `has_many`, N:N, `morph_one`, and `morph_many` correlations derive their keys from relation metadata. `morph_to` is rejected for `where_has` because one relation can resolve to different physical target tables, matching the TypeScript restriction.

0.0.13 moves relation correlation into the SELECT compiler's `WHERE` position. This matters for child pagination:

```cpp
auto roots = metal::where_has<^^User::posts>(
    metal::select<User>(),
    [](auto& posts) {
        posts
            .order_by(metal::field<^^Post::id>)
            .limit(10)
            .offset(1);
    });
```

The generated semantic order is now:

```text
child predicates
AND relation correlation
ORDER BY
LIMIT / OFFSET
```

rather than applying correlation around an already-paginated child query. A relation filter attached to a root query with an existing `LIMIT/OFFSET` is likewise evaluated before that root pagination.

Nested correlated scopes use internal aliases such as `t0`, `t0_rel`, `t0_rel_rel`, preventing child subqueries from shadowing their outer correlation alias. Normal non-correlated SQL preserves the established `t0/t1/p0` alias shape.

`match_relation<^^Relation>(...)` exposes the relation-matching contract with the same typed correlation engine. TypeScript currently renders `match()` as INNER JOIN + DISTINCT; the C++ implementation uses EXISTS so relation correlation has one source of truth while preserving root-filtering behavior.

## Offset pagination — 0.0.13

Level 1 / row pagination stays independent of Session:

```cpp
auto result = metal::execute_paged(
    query,
    executor,
    dialect,
    metal::PageOptions{
        .page = 2,
        .page_size = 25
    });
```

It returns `Row` values and counts physical result rows.

For tracked root entities, use the Session overload:

```cpp
auto result = metal::execute_paged(
    query,
    session,
    metal::PageOptions{
        .page = 1,
        .page_size = 25
    });
```

Pagination helpers own the requested page and therefore strip earlier query `LIMIT/OFFSET` before applying `PageOptions`, matching TypeScript `executePaged` behavior.

The Session path is **root-aware**. If an explicit 1:N/N:N JOIN physically returns the same root multiple times, MetalORM deduplicates by the reflected root PK while preserving result order, counts unique roots, slices the requested page, and materializes each root through the Identity Map.

Tracked pagination requires a complete root-entity projection. DTO/partial projections should use the row overload rather than create partially managed entities.

The current root-aware implementation materializes the unpaged matching row stream before deduplication. That is a performance optimization target, not a semantic parity gap.

## Cursor pagination — 0.0.13

Cursor ordering is expressed using reflected fields:

```cpp
std::vector order{
    metal::cursor_order(
        metal::field<^^User::score>,
        false),
    metal::cursor_order(
        metal::field<^^User::id>)
};
```

Forward page:

```cpp
auto page = metal::execute_cursor(
    metal::select<User>(),
    session,
    order,
    metal::CursorPageOptions{.first = 25});
```

Next page:

```cpp
auto next = metal::execute_cursor(
    metal::select<User>(),
    session,
    order,
    metal::CursorPageOptions{
        .first = 25,
        .after = page.page_info.end_cursor
    });
```

Backward pagination uses `last` / `before`. The implementation follows the TypeScript keyset semantics:

- lexicographic predicates for multiple ORDER BY columns;
- direction-aware ASC/DESC comparisons;
- mode-driven keyset direction (`first` means after semantics; `last` means before semantics);
- `limit + 1` page detection;
- forward and backward pagination;
- non-null cursor values;
- an ORDER BY signature embedded in the opaque cursor so a cursor cannot be silently reused with a different ordering;
- root-PK deduplication before page-size detection for tracked queries containing row-multiplying explicit joins.

The cursor encoding is intentionally opaque and internal to the C++ API; semantic compatibility does not imply a cross-language TypeScript/C++ wire-format guarantee.

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
