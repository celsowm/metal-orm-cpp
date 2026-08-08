# MetalORM C++

![CI](https://github.com/celsowm/metal-orm-cpp/actions/workflows/ci.yml/badge.svg?branch=main)

> A C++26-native ORM built around static reflection, annotations, splicing and expansion statements.

**Version:** `0.0.1`

MetalORM C++ is not a C++20 port with a reflection adapter bolted on. Reflection is the architecture.
There is intentionally no `entity_traits<T>`, no registration macro, no compatibility metadata layer and no support for pre-C++26 compilers.

## Requirements

- GCC 16+
- `-std=c++26 -freflection`
- CMake 3.20+
- SQLite 3 development headers for the 0.0.1 executor

GCC 16 implements the C++26 reflection proposal P2996R13 together with annotations (P3394R4), `define_static_*` (P3491R3) and expansion statements (P1306R5).

## Model

```cpp
#include <metal/metal.hpp>

struct [[=metal::mapping::table{"roles"}]] Role {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    std::string name;
};

struct [[=metal::mapping::table{"users"}]] User {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    std::string name;
    int age{};
    bool active{true};

    [[=metal::mapping::many_to_many{"user_roles", "user_id", "role_id"}]]
    std::vector<std::shared_ptr<Role>> roles;
};
```

There is no duplicated schema declaration. `User` is the schema source of truth.

MetalORM reflects it with:

```cpp
^^User
^^User::name
std::meta::nonstatic_data_members_of(...)
std::meta::annotations_of_with_type(...)
std::meta::identifier_of(...)
```

and accesses discovered members with splicing:

```cpp
entity.[:member:]
```

## Query

Reflections are used directly as non-type template arguments:

```cpp
auto users = session.query<User>()
    .where(
        (metal::field<^^User::age> >= 18) &&
        (metal::field<^^User::active> == true)
    )
    .order_by(metal::field<^^User::name>)
    .include<^^User::roles>()
    .all();
```

The field type is derived from `std::meta::type_of(^^User::age)`, so comparisons remain strongly typed.
Cross-entity `where` expressions are different C++ types and cannot be passed to the wrong query.

## What 0.0.1 proves

- C++26 annotations as ORM mapping metadata
- zero handwritten member lists
- compile-time member enumeration with expansion statements
- splice-based hydration and snapshots
- typed expression AST for `=`, `<>`, `>`, `>=`, `<`, `<=`, `AND`, `OR`, `NOT`
- reflected `SELECT`
- reflected SQLite DDL generation
- SQLite executor
- `Session`
- Unit of Work
- dirty checking by reflected snapshots
- generated primary keys
- Identity Map
- batched many-to-many hydration
- identity preservation across N:N relationships

## Identity Map + N:N

If two users have the same role, the role is the same entity instance inside a session:

```cpp
auto users = session.query<User>()
    .include<^^User::roles>()
    .all();

assert(users[0]->roles[0] == users[1]->roles[0]);
```

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The CMake project deliberately fails on GCC < 16 and on non-GNU compilers for this first release.
That restriction should disappear only when another mainstream compiler implements the same C++26 reflection model — not through a compatibility implementation.

## Architecture

The TypeScript MetalORM has three useful levels: SQL/query AST, ORM runtime, and entity metadata.
The C++ port keeps that separation, but its metadata level is native C++26 reflection rather than decorators plus a runtime registry.

```text
C++ entity declarations
        │
        │ ^^ + annotations
        ▼
 compile-time metadata
        │
   ┌────┼───────────┐
   ▼    ▼           ▼
  DDL  Query AST   Hydration
   │    │           │
   └────┴─────┬─────┘
              ▼
          Orm Session
              │
       Unit of Work / Identity Map
              │
              ▼
           Executor
```

## Non-goals of 0.0.1

The TypeScript project already has substantially more surface area. This first C++ version deliberately does not fake parity.
Upcoming work can add PostgreSQL/MySQL/SQL Server dialects, one-to-one/one-to-many relations, pivot mutation (`attach`, `detach`, `sync`), richer SQL AST nodes, schema diff/introspection, hooks, events, pooling, cache and bulk operations on top of the same reflection-first foundation.
