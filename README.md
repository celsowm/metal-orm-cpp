# MetalORM C++

![CI](https://github.com/celsowm/metal-orm-cpp/actions/workflows/ci.yml/badge.svg?branch=main)

> A C++26-native port of MetalORM built around static reflection, annotations, splicing and expansion statements.

**Version:** `0.0.7`

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
    std::string label;
    std::int64_t weight{};
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
    metal::many_to_many_collection<Role, UserRole> roles;
};
```

There is no duplicated relation schema. The pivot type and pivot keys are reflections, and root/target keys default to the reflected primary keys. A composite-key pivot satisfies `Mapped<T>` but not `Entity<T>`; tracked `Session` entities intentionally require exactly one primary key.

The N:N wrapper is checked at compile time: its `Pivot` template parameter must be exactly the same type referenced by the relation annotation.

## Relation collections

MetalORM TS has distinct has-many and many-to-many collection contracts. MetalORM C++ mirrors those roles instead of exposing one generic collection for both jobs.

### Has many

```cpp
struct User {
    [[=metal::mapping::has_many<
        ^^Post::user_id,
        metal::mapping::cascade_mode::all>{}]]
    metal::has_many_collection<Post> posts;
};
```

```cpp
auto post = user->posts.add();
post->title = "C++26 reflection";

user->posts.attach(existing_post);
user->posts.remove(existing_post);
user->posts.clear();

const auto& posts = user->posts.load();
const auto& same_posts = user->posts.get_items();
```

When the root is already tracked, `attach()` applies the reflected foreign key immediately. Cascades and persistence still flow through the Unit of Work during `commit()`.

### Many to many

```cpp
user->roles.load();

user->roles.attach(role);
user->roles.attach(role_id);

user->roles.detach(role);
user->roles.detach(role_id);

user->roles.sync_by_ids(
    std::vector<std::int64_t>{1, 2, 3});
```

ID-based operations use the relation's reflected target key. When that key is the target primary key they integrate directly with the Session Identity Map. When a different `targetKey` is declared, collection identity, pivot DML and cascade removal consistently use that alternate reflected member.

### Partial typed pivot patches

The TypeScript collection accepts `Partial<TPivot>`. The C++26-native equivalent is `pivot_patch<Pivot>`: only explicitly reflected members become mutation payloads.

```cpp
metal::pivot_patch<UserRole> patch;
patch
    .set<^^UserRole::label>(std::string{"owner"})
    .set<^^UserRole::weight>(std::int64_t{10});

user->roles.attach(role, patch);
session.commit();
```

The patch is checked at compile time: `^^Member` must belong to `UserRole`, and the supplied value type must be compatible with that reflected member.

Reattaching an existing target with a partial patch updates only those columns:

```cpp
metal::pivot_patch<UserRole> patch;
patch.set<^^UserRole::label>(std::string{"reviewer"});

user->roles.attach(role, patch);
session.commit();
```

If the existing pivot has `weight = 10`, the UPDATE changes only `label`; `weight` remains `10`. Repeated patches merge by column instead of reconstructing the pivot from C++ default values.

Pivot rows are hydrated back into the real reflected type:

```cpp
user->roles.load();

if (const auto* pivot = user->roles.pivot(role)) {
    std::cout << pivot->label;
}
```

The two relation FK columns are excluded from pivot DML payloads, matching MetalORM's pivot-payload filtering.

### Alternate target keys

A N:N relation is not required to use the target primary key. For example, the pivot can refer to a stable role code while the target still has a generated numeric PK:

```cpp
struct Role {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string code;
};

struct UserRoleByCode {
    std::int64_t user_id{};
    std::string role_code;
};

struct User {
    [[=metal::mapping::many_to_many<
        ^^UserRoleByCode,
        ^^UserRoleByCode::user_id,
        ^^UserRoleByCode::role_code,
        metal::mapping::cascade_mode::remove,
        std::meta::info{},
        ^^Role::code>{}]]
    metal::many_to_many_collection<Role, UserRoleByCode> roles;
};
```

Now these values are role codes, not role PKs:

```cpp
user->roles.attach(std::string{"DEV"});
user->roles.sync_by_ids(std::vector<std::string>{"DEV", "ADMIN"});
user->roles.detach(std::string{"ADMIN"});
```

Hydrated `Role` objects still enter the Session Identity Map under their real primary key. Relation identity and entity identity remain separate concepts.

## Lazy and eager relation loading

To-many wrappers are bound to the `Session` when their root entity becomes tracked. That makes lazy loading possible without building a separate runtime metadata registry:

```cpp
auto user = session.query<User>()
    .where(metal::field<^^User::id> == id)
    .first();

assert(!user->roles.loaded());
user->roles.load();
assert(user->roles.loaded());
```

Eager loading remains available:

```cpp
auto users = session.query<User>()
    .include<^^User::posts>()
    .include<^^User::roles>()
    .all();
```

Both paths hydrate through the same Identity Map. The current SQLite executor is synchronous, so `load()` is synchronous; this is an execution-model adaptation, not a different relation semantic.

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
    .limit(5);
```

`select<User>()` initially has only `User` in its C++ query scope. A predicate using `Post` does not satisfy the query constraints until `join<^^User::posts>()` introduces `Post` through reflected relationship metadata.

For N:N, one reflected relation expands into both joins:

```text
User -> UserRole pivot -> Role
```

## Shared DML AST

Like the original MetalORM, persistence and relation mutation share DML builders instead of maintaining a second hand-written SQL path:

```cpp
auto insert = metal::InsertQueryBuilder{"users"}
    .values({metal::DmlAssignment{"name", std::string{"Celso"}}});

auto update = metal::UpdateQueryBuilder{"users"}
    .set({metal::DmlAssignment{"name", std::string{"Updated"}}})
    .where_eq("id", std::int64_t{1});

auto erase = metal::DeleteQueryBuilder{"users"}
    .where_eq("id", std::int64_t{1});
```

`UnitOfWork` and `RelationChangeProcessor` compile these ASTs through the same SQLite dialect. N:N attach emits a normal pivot INSERT, matching MetalORM rather than silently using `ON CONFLICT DO NOTHING`.

## Cascade semantics

The cascade vocabulary follows the original runtime:

```cpp
metal::mapping::cascade_mode::none
metal::mapping::cascade_mode::all
metal::mapping::cascade_mode::persist
metal::mapping::cascade_mode::remove
metal::mapping::cascade_mode::link
```

For N:N, `remove` and `all` are valid. Detaching deletes the pivot row first; when target removal is requested, the target is deleted afterward. For normal hydrated/tracked targets this is scheduled through the second Unit of Work flush. Alternate-target-key stubs that are not tracked are deleted through shared DML using that declared target key. `link` links already-persisted entities without cascading persist or remove.

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

Commit order mirrors the MetalORM runtime model:

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

## Compile-time model validation

Mappings and typed pivot patches fail at compile time for cases including:

- wrong relation member wrapper;
- N:N collection/pivot type mismatch;
- pivot patch member from the wrong pivot type;
- pivot patch value incompatible with its reflected member;
- FK reflected from the wrong owner type;
- incompatible FK/key C++ types;
- conflicting annotations;
- duplicate mapped column names;
- invalid generated-key declarations.

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

Reflections are non-type template arguments in fields, relationship metadata and pivot patches; splicing is used for hydration, snapshots, key access, relationship mutation and application of typed partial pivot values.

## What 0.0.7 contains

- C++26 static reflection and annotations as the only metadata model
- `Mapped<T>` / `Entity<T>` concepts and `consteval` validation
- typed SELECT SQL AST with compile-time query scope
- reflected joins, projections, predicates, aggregates, grouping and scalar subqueries
- shared INSERT / UPDATE / DELETE AST builders
- reflected SQLite DDL, including composite primary keys
- SQLite executor
- `Session` coordinating `IdentityMap`, `UnitOfWork`, and `RelationChangeProcessor`
- reflected dirty checking and generated keys
- batched eager relations plus Session-bound lazy to-many loading
- `has_many_collection<T>` with `load`, `get_items`, `add`, `attach`, `remove`, `clear`
- `many_to_many_collection<T, Pivot>` with entity/ID attach and detach, `sync_by_ids`, typed pivot hydration and partial pivot updates
- alternate non-primary N:N `targetKey` behavior
- MetalORM-compatible cascade vocabulary including `link`
- N:N `remove/all` semantics aligned with the relation contract

See `docs/PARITY.md` for the explicit reference matrix and ordered parity roadmap.

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The CMake project intentionally fails on GCC < 16 and on non-GNU compilers today. That restriction should disappear only when another mainstream compiler implements the same C++26 reflection model, not through a compatibility metadata implementation.

## Direction

The TypeScript MetalORM is the feature reference. The C++ port should preserve its layers and semantics while replacing runtime metadata/string configuration with C++26 reflection where possible.

With the core N:N collection gaps closed in 0.0.7, the next parity family is `morphTo`, `morphOne`, and `morphMany`, followed by richer DML and the remaining query-builder surface such as CTEs, set operations and window functions.
