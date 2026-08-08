# MetalORM C++

![CI](https://github.com/celsowm/metal-orm-cpp/actions/workflows/ci.yml/badge.svg?branch=main)

> A C++26-native ORM built around static reflection, annotations, splicing and expansion statements.

**Version:** `0.0.3`

MetalORM C++ is deliberately not a C++20 ORM with a reflection adapter. Reflection is the architecture: no `entity_traits<T>`, no registration macros, no compatibility metadata layer, and no pre-C++26 fallback.

For now, **SQLite is intentionally the only executor/dialect**. The project is using SQLite as the proving ground while the C++26 object model, Unit of Work and relation semantics mature.

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

There is no duplicated relation schema. The pivot type and both pivot keys are reflections. The root and target keys default to their primary keys and can also be supplied explicitly as reflections.

The pivot is a normal mapped C++ type. A composite-key pivot satisfies `Mapped<T>` but not `Entity<T>`; tracked session entities intentionally require exactly one primary key.

## Mutable relation collections

To-many relationships use `metal::collection<T>`, not `std::vector<std::shared_ptr<T>>`.

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

With `cascade_mode::persist`, the two new roles are inserted first and the reflected pivot rows are written in the same transaction after generated IDs exist.

A loaded collection exposes normal read operations plus tracked mutation:

```cpp
user->roles.attach(auditor);
user->roles.detach(admin);
user->roles.sync({developer, auditor});

assert(user->roles.dirty());
session.commit();
assert(!user->roles.dirty());
```

`collection<T>` stores the current relation state and the last accepted baseline. The Unit of Work derives `added` and `removed` items from that diff, so an attach followed by a detach before commit naturally cancels out.

## 1:N mutation and cascades

```cpp
struct Post {
    std::int64_t id{};
    std::int64_t user_id{};
    std::string title;
};

struct User {
    // ...

    [[=metal::mapping::has_many<
        ^^Post::user_id,
        metal::mapping::cascade_mode::all>{}]]
    metal::collection<Post> posts;
};
```

`cascade_mode::all` means newly attached children can be persisted automatically, and detached children are removed. The Unit of Work updates the reflected child FK only after generated root IDs are available.

Without cascade remove, detaching from a `has_many` is allowed only when the reflected FK is optional; MetalORM then writes `NULL`.

For N:N, cascade remove/all is rejected at compile time because removing a link must not delete a target that may be shared by other roots.

## Relationship loading

All four relationship kinds remain reflection-native:

```cpp
[[=metal::mapping::has_many<^^Post::user_id>{}]]
metal::collection<Post> posts;

[[=metal::mapping::has_one<^^Profile::user_id>{}]]
std::shared_ptr<Profile> profile;

[[=metal::mapping::belongs_to<^^Comment::user_id>{}]]
std::shared_ptr<User> author;
```

And queries use the reflected member directly:

```cpp
auto users = session.query<User>()
    .where(metal::field<^^User::active> == true)
    .include<^^User::posts>()
    .include<^^User::profile>()
    .include<^^User::roles>()
    .all();
```

`include<^^...>()` dispatches at compile time to batched `belongs_to`, `has_one`, `has_many`, or `many_to_many` loading. Every hydrated row passes through the same Identity Map, so shared database rows are shared C++ instances within a session.

## Compile-time model validation

MetalORM validates mappings with `consteval` reflection. Invalid programs fail to compile for cases including:

- wrong relation member shape;
- FK reflected from the wrong owner type;
- incompatible FK/key C++ types;
- conflicting annotations;
- duplicate mapped column names;
- invalid generated-key declarations;
- destructive N:N cascade remove.

The test suite includes expected compile-failure tests and verifies the MetalORM diagnostic text.

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

## What 0.0.3 contains

- C++26 annotations and static reflection as the only metadata model
- `Mapped<T>` / `Entity<T>` concepts
- compile-time mapping validation
- typed expression AST and reflected `SELECT`
- reflected SQLite DDL, including composite primary keys
- SQLite executor
- `Session`, Unit of Work and Identity Map
- reflected dirty checking and generated keys
- batched `belongs_to`, `has_one`, `has_many`, `many_to_many`
- `metal::collection<T>` with `attach`, `detach`, `sync`, `loaded`, `dirty`
- relation diff flushing inside the UoW transaction
- cascade persist for 1:N and N:N
- cascade remove for 1:N
- shared target identity across relationships

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The CMake project intentionally fails on GCC < 16 and on non-GNU compilers today. That restriction should disappear only when another mainstream compiler implements the same C++26 reflection model, not through a compatibility metadata implementation.

## Direction

```text
C++ declarations + annotations
           │
           │ ^^
           ▼
   consteval model validation
           │
   ┌───────┼──────────────┐
   ▼       ▼              ▼
  DDL   Query AST     Relations
   │       │              │
   └───────┴──────┬───────┘
                  ▼
               Session
                  │
        Unit of Work / Identity Map
                  │
        entity diff + relation diff
                  │
                  ▼
                SQLite
```

The next major layer is a richer SQL AST/query builder while SQLite remains the only backend.
