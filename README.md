# MetalORM C++

![CI](https://github.com/celsowm/metal-orm-cpp/actions/workflows/ci.yml/badge.svg?branch=main)

> A C++26-native ORM built around static reflection, annotations, splicing and expansion statements.

**Version:** `0.0.2`

MetalORM C++ is not a C++20 port with a reflection adapter bolted on. Reflection is the architecture.
There is intentionally no `entity_traits<T>`, no registration macro, no compatibility metadata layer and no support for pre-C++26 compilers.

For now, **SQLite is intentionally the only executor/dialect**. The project is prioritizing the C++26 object/metadata model before multiplying database backends.

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
        ^^UserRole::role_id>{}]]
    std::vector<std::shared_ptr<Role>> roles;
};
```

There is no duplicated relation schema. The N:N annotation carries reflections, not strings. MetalORM derives the pivot table name, pivot column names, root primary key and target primary key from the reflected declarations.

The pivot is a normal mapped C++ type. Because it has a composite primary key, it satisfies `Mapped<UserRole>` but intentionally not `Entity<UserRole>`: `Session`/Identity Map entities require exactly one primary key.

## Relationship annotations

All relationship metadata is strongly tied to reflected members:

```cpp
[[=metal::mapping::has_many<^^Post::user_id>{}]]
std::vector<std::shared_ptr<Post>> posts;

[[=metal::mapping::has_one<^^Profile::user_id>{}]]
std::shared_ptr<Profile> profile;

[[=metal::mapping::belongs_to<^^Comment::user_id>{}]]
std::shared_ptr<User> author;
```

The omitted local/target key defaults to that side's primary key. Non-default keys can also be supplied as reflections.

MetalORM validates relation shape and keys at compile time. A `has_many` on a `shared_ptr`, a FK reflected from the wrong class, incompatible key types, conflicting annotations, duplicate columns, or an invalid generated key is a compile error rather than a runtime ORM error.

## Query + include

```cpp
auto users = session.query<User>()
    .where(
        (metal::field<^^User::age> >= 18) &&
        (metal::field<^^User::active> == true)
    )
    .order_by(metal::field<^^User::name>)
    .include<^^User::posts>()
    .include<^^User::profile>()
    .include<^^User::roles>()
    .all();
```

`include<^^...>()` dispatches from the annotation type at compile time to batched `belongs_to`, `has_one`, `has_many`, or `many_to_many` loading.

All hydrated targets pass through the same session Identity Map. If multiple roots reference the same row, they share the same C++ entity instance.

## C++26 machinery used directly

MetalORM intentionally exposes and builds around the adopted language facilities:

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

Reflections are also used as non-type template arguments in fields and relationship metadata.

## What 0.0.2 contains

- C++26 annotations as ORM mapping metadata
- zero handwritten member lists
- `Mapped<T>` and `Entity<T>` concepts
- compile-time model validation
- typed expression AST for `=`, `<>`, `>`, `>=`, `<`, `<=`, `AND`, `OR`, `NOT`
- reflected `SELECT`
- reflected SQLite DDL
- single and composite primary-key DDL
- SQLite executor
- `Session`
- Unit of Work
- reflected snapshots + dirty checking
- generated primary keys
- Identity Map
- batched `belongs_to`, `has_one`, `has_many`, `many_to_many`
- shared target identity across relations

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The CMake project deliberately fails on GCC < 16 and on non-GNU compilers for this release. That restriction should disappear only when another mainstream compiler implements the same C++26 reflection model, not through a compatibility metadata implementation.

## Direction

The TypeScript MetalORM separates SQL/query AST, ORM runtime, and entity metadata. MetalORM C++ keeps those architectural layers, but the metadata layer is native C++26 reflection rather than decorators plus a runtime registry.

```text
C++ declarations + annotations
           │
           │ ^^
           ▼
   compile-time model
           │
   ┌───────┼───────────┐
   ▼       ▼           ▼
  DDL   Query AST   Hydration
   │       │           │
   └───────┴─────┬─────┘
                 ▼
              Session
                 │
       Unit of Work / Identity Map
                 │
                 ▼
              SQLite
```

The next layers can add relation mutation (`attach`, `detach`, `sync`), cascades and a richer SQL AST while keeping SQLite as the proving ground.
