# MetalORM C++

![CI](https://github.com/celsowm/metal-orm-cpp/actions/workflows/ci.yml/badge.svg?branch=main)

> A C++26-native port of MetalORM built around static reflection, annotations, splicing and expansion statements.

**Version:** `0.0.6`

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

MetalORM TS has distinct has-many and many-to-many collection contracts. `0.0.6` mirrors that instead of exposing one generic collection for both jobs.

### Has many

```cpp
struct User {
    [[=metal::mapping::has_many<
        ^^Post::user_id,
        metal::mapping::cascade_mode::all>{}]]
    metal::has_many_collection<Post> posts;
};
```

The collection exposes the corresponding C++ API:

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

ID-based operations use the reflected target key. When that target key is the primary key they integrate with the Session Identity Map and materialize a tracked ID stub when necessary, matching the original collection's attach-by-ID behavior.

### Typed pivot payloads

The TypeScript runtime carries pivot values in an untyped `_pivot` object. C++ can preserve the same behavior more strongly by using the real reflected pivot type:

```cpp
user->roles.attach(
    role,
    UserRole{
        .label = "owner"
    });

session.commit();
```

The non-FK fields are written with the pivot INSERT. Reattaching an already-linked target with a new pivot value schedules an UPDATE rather than another link:

```cpp
user->roles.attach(
    role,
    UserRole{
        .label = "reviewer"
    });

session.commit();
```

Pivot rows are also hydrated back into `UserRole`:

```cpp
user->roles.load();

if (const auto* pivot = user->roles.pivot(role)) {
    std::cout << pivot->label;
}
```

**Known parity sub-gap:** MetalORM TS accepts `Partial<TPivot>` for pivot mutations. `0.0.6` currently accepts a complete typed `Pivot`, so unspecified non-FK members take their C++ default values. Partial-field pivot patching remains to be implemented.

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

`UnitOfWork` and `RelationChangeProcessor` compile these ASTs through the same SQLite dialect. N:N attach now emits a normal pivot INSERT, matching MetalORM rather than silently using `ON CONFLICT DO NOTHING`.

## Cascade semantics

The cascade vocabulary follows the original runtime:

```cpp
metal::mapping::cascade_mode::none
metal::mapping::cascade_mode::all
metal::mapping::cascade_mode::persist
metal::mapping::cascade_mode::remove
metal::mapping::cascade_mode::link
```

For N:N, `remove` and `all` are valid. Detaching deletes the pivot row first; when target removal is requested, the target is marked Removed and deleted by the second Unit of Work flush. `link` links already-persisted entities without cascading persist or remove.

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

Mappings are validated with `consteval` reflection. Invalid programs fail to compile for cases including:

- wrong relation member wrapper;
- N:N collection/pivot type mismatch;
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

Reflections are non-type template arguments in fields and relationship metadata; splicing is used for hydration, snapshots, key access and relationship mutation.

## What 0.0.6 contains

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
- `many_to_many_collection<T, Pivot>` with entity/ID attach and detach, `sync_by_ids`, typed pivot hydration and pivot updates
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

The immediate remaining relation gaps are partial pivot patches and edge cases around alternate non-primary `targetKey` identity. After those, parity work should move to Morph relations and then the missing query-builder surface such as CTEs, set operations and window functions.
