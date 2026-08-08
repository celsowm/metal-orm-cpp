# MetalORM C++

![CI](https://github.com/celsowm/metal-orm-cpp/actions/workflows/ci.yml/badge.svg?branch=main)

> A C++26-native ORM built around static reflection, annotations, splicing and expansion statements.

**Version:** `0.0.5`

MetalORM C++ is deliberately not a C++20 ORM with a reflection adapter. Reflection is the architecture: no `entity_traits<T>`, no registration macros, no compatibility metadata layer, and no pre-C++26 fallback.

For now, **SQLite is intentionally the only executor/dialect**. The TypeScript MetalORM is the behavioral and architectural reference; C++26 changes the mechanism, not the ORM semantics.

## Requirements

- GCC 16+
- `-std=c++26 -freflection`
- CMake 3.20+
- SQLite 3 development headers

## Model once, reflect everywhere

```cpp
#include <metal/metal.hpp>

struct [[=metal::mapping::table{"roles"}]] Role {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;
};

struct [[=metal::mapping::table{"user_roles"}]] UserRole {
    [[=metal::mapping::primary_key]] std::int64_t user_id{};
    [[=metal::mapping::primary_key]] std::int64_t role_id{};
};

struct [[=metal::mapping::table{"users"}]] User {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;

    [[=metal::mapping::many_to_many<
        ^^UserRole,
        ^^UserRole::user_id,
        ^^UserRole::role_id,
        metal::mapping::cascade_mode::persist>{}]]
    metal::collection<Role> roles;
};
```

There is no duplicated relation schema. The pivot type and pivot keys are reflections, and root/target keys default to the reflected primary keys.

A composite-key pivot satisfies `Mapped<T>` but not `Entity<T>`; tracked `Session` entities intentionally require exactly one primary key.

## Typed SQL AST

The query builder is a typed SQL AST rather than a bag of column-name strings.

```cpp
auto query = metal::select<User>()
    .join<^^User::posts>()
    .project(metal::field<^^User::name>)
    .project(
        metal::count(metal::field<^^Post::id>)
            .as("post_count"))
    .where(
        metal::like(metal::field<^^User::name>, "C%") &&
        metal::in(
            metal::field<^^Post::id>,
            std::vector<std::int64_t>{1, 2, 3}))
    .group_by(metal::field<^^User::name>)
    .having(
        metal::count(metal::field<^^Post::id>) > 1)
    .order_by(metal::field<^^User::name>, false)
    .limit(5)
    .offset(2);
```

`select<User>()` initially has only `User` in its C++ query scope. A predicate using `Post` does not satisfy the query's constraints until `join<^^User::posts>()` introduces `Post` through reflected relationship metadata.

The join itself is generated from the annotation. There are no table or FK strings at the call site.

### Reflected joins

Both inner and left joins are supported:

```cpp
auto q = metal::select<User>()
    .left_join<^^User::roles>()
    .project(metal::field<^^User::name>)
    .project_as(metal::field<^^Role::name>, "role_name");
```

For N:N, MetalORM expands the one reflected relation into both SQL joins:

```text
User -> UserRole pivot -> Role
```

The pivot table and all four key sides come from reflected C++ declarations.

### Predicates

```cpp
metal::field<^^User::age> >= 18
metal::field<^^User::id> == metal::field<^^Post::user_id>
metal::is_null(metal::field<^^User::nickname>)
metal::is_not_null(metal::field<^^User::nickname>)
metal::like(metal::field<^^User::name>, "C%")
metal::not_like(metal::field<^^User::name>, "%bot%")
metal::in(metal::field<^^User::id>, ids)
metal::not_in(metal::field<^^User::id>, ids)
```

Field-to-field comparisons require compatible reflected C++ member types. `IN` over an empty range compiles to a false predicate rather than invalid `IN ()` SQLite syntax.

### Aggregates and grouping

```cpp
metal::count(metal::field<^^Post::id>)
metal::count(metal::field<^^Post::id>, true) // DISTINCT
metal::count_all<User>()
metal::sum(metal::field<^^Invoice::amount>)
metal::avg(metal::field<^^Invoice::amount>)
metal::min(metal::field<^^Invoice::amount>)
metal::max(metal::field<^^Invoice::amount>)
```

Aggregate terms can be projections or predicates inside `HAVING`.

### Scalar subqueries

```cpp
auto post_users = metal::select<Post>()
    .project(metal::field<^^Post::user_id>)
    .where(metal::like(
        metal::field<^^Post::title>, "%C++%"));

auto users = metal::select<User>()
    .where(metal::in(
        metal::field<^^User::id>,
        post_users));
```

A scalar subquery used by `IN` must project exactly one expression. The nested parameters are merged into the outer compiled query.

## Shared DML AST

Like the original MetalORM, the runtime no longer has a separate hand-written SQL path for persistence. INSERT, UPDATE and DELETE go through shared DML builders:

```cpp
auto insert = metal::InsertQueryBuilder{"users"}
    .values({
        metal::DmlAssignment{"name", std::string{"Celso"}}
    });

auto update = metal::UpdateQueryBuilder{"users"}
    .set({metal::DmlAssignment{"name", std::string{"Updated"}}})
    .where_eq("id", std::int64_t{1});

auto erase = metal::DeleteQueryBuilder{"users"}
    .where_eq("id", std::int64_t{1});
```

`UnitOfWork` and relation mutation compile these ASTs through the same SQLite dialect.

## Mutable relation collections

To-many relationships use `metal::collection<T>` rather than `std::vector<std::shared_ptr<T>>`.

```cpp
auto developer = std::make_shared<Role>();
developer->name = "developer";

auto admin = std::make_shared<Role>();
admin->name = "admin";

auto user = std::make_shared<User>();
user->name = "Celso";
user->roles.attach(developer);
user->roles.attach(admin);

session.persist(user);
session.commit();
```

With `cascade_mode::persist`, new relation targets are inserted first and reflected pivot rows are written in the same transaction after generated IDs exist.

Collections track current state versus an accepted baseline:

```cpp
user->roles.attach(auditor);
user->roles.detach(admin);
user->roles.sync({developer, auditor});

assert(user->roles.dirty());
session.commit();
assert(!user->roles.dirty());
```

The Unit of Work derives added/removed relation items from this diff, so opposite mutations before a commit naturally cancel.

## Cascade semantics

MetalORM C++ follows the original cascade vocabulary:

```cpp
metal::mapping::cascade_mode::none
metal::mapping::cascade_mode::all
metal::mapping::cascade_mode::persist
metal::mapping::cascade_mode::remove
metal::mapping::cascade_mode::link
```

For N:N, `remove` and `all` are valid just like in MetalORM TS. Detaching deletes the pivot row first; when target removal is requested, the target is then marked Removed and deleted by a second Unit of Work flush.

`link` links already-persisted entities without cascading persist or remove.

## 1:N mutation and cascades

```cpp
struct User {
    // ...

    [[=metal::mapping::has_many<
        ^^Post::user_id,
        metal::mapping::cascade_mode::all>{}]]
    metal::collection<Post> posts;
};
```

`cascade_mode::all` persists newly attached children and removes detached children. The reflected child FK is assigned only after generated root IDs exist.

## Relationship loading

All four relation kinds remain reflection-native:

```cpp
[[=metal::mapping::has_many<^^Post::user_id>{}]]
metal::collection<Post> posts;

[[=metal::mapping::has_one<^^Profile::user_id>{}]]
std::shared_ptr<Profile> profile;

[[=metal::mapping::belongs_to<^^Comment::user_id>{}]]
std::shared_ptr<User> author;
```

And ORM loading uses reflected members directly:

```cpp
auto users = session.query<User>()
    .where(metal::field<^^User::active> == true)
    .include<^^User::posts>()
    .include<^^User::profile>()
    .include<^^User::roles>()
    .all();
```

`include<^^...>()` dispatches at compile time to batched `belongs_to`, `has_one`, `has_many`, or `many_to_many` loading. Every hydrated row passes through the same Identity Map.

## Runtime architecture

`Session` coordinates dedicated runtime components rather than implementing all responsibilities itself:

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

Commit order follows the MetalORM runtime model:

```text
prepare cascaded persistence
        ↓
UnitOfWork.flush()
        ↓
RelationChangeProcessor.process()
        ↓
UnitOfWork.flush()
        ↓
COMMIT
```

The second flush is important because relation processing can schedule entity removals or updates.

## Compile-time model validation

MetalORM validates mappings with `consteval` reflection. Invalid programs fail to compile for cases including:

- wrong relation member shape;
- FK reflected from the wrong owner type;
- incompatible FK/key C++ types;
- conflicting annotations;
- duplicate mapped column names;
- invalid generated-key declarations.

The test suite also checks typed query scope: a joined entity's fields are not accepted by a query before that reflected join enters the query type.

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

Reflections are non-type template arguments in fields and relationship metadata; splicing is used for hydration, snapshots, key access and relationship mutation.

## What 0.0.5 contains

- C++26 static reflection and annotations as the only metadata model
- `Mapped<T>` / `Entity<T>` concepts and `consteval` model validation
- typed SQL expression AST with compile-time query scope
- reflected `INNER JOIN` / `LEFT JOIN`, including N:N pivots
- typed projections and aliases
- comparisons, `IN`, null predicates and `LIKE`
- aggregates, `GROUP BY`, `HAVING`
- scalar subqueries
- shared INSERT / UPDATE / DELETE AST builders
- reflected SQLite DDL, including composite primary keys
- SQLite executor
- `Session` coordinating separate `IdentityMap`, `UnitOfWork`, and `RelationChangeProcessor`
- reflected dirty checking and generated keys
- batched `belongs_to`, `has_one`, `has_many`, `many_to_many`
- `metal::collection<T>` with `attach`, `detach`, `sync`, `loaded`, `dirty`
- MetalORM-compatible cascade vocabulary including `link`
- N:N `remove/all` semantics aligned with the TypeScript runtime

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The CMake project intentionally fails on GCC < 16 and on non-GNU compilers today. That restriction should disappear only when another mainstream compiler implements the same C++26 reflection model, not through a compatibility metadata implementation.

## Direction

The TypeScript MetalORM is the feature reference. The C++ port should preserve its layers and semantics while replacing runtime metadata/string configuration with C++26 reflection where possible.

```text
C++ declarations + annotations
           │
           │ ^^
           ▼
   consteval model validation
           │
   ┌───────┼──────────────┐
   ▼       ▼              ▼
  DDL   typed SQL AST  Relations
           │              │
           └──────┬───────┘
                  ▼
               Session
         ┌────────┼───────────┐
         ▼        ▼           ▼
   IdentityMap  UnitOfWork  RelationChangeProcessor
                  │           │
                  └─────┬─────┘
                        ▼
                  shared DML AST
                        │
                        ▼
                      SQLite
```

Next work should follow missing MetalORM parity rather than invent new APIs: typed DML values/results, richer relation collections and pivot payloads, Morph relations, then the remaining query-builder features such as CTEs, set operations and window functions.
