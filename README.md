# MetalORM C++

![CI](https://github.com/celsowm/metal-orm-cpp/actions/workflows/ci.yml/badge.svg?branch=main)

> A C++26-native port of MetalORM built around static reflection, annotations, splicing and expansion statements.

**Version:** `0.0.16`

MetalORM C++ deliberately has no C++20/23 compatibility layer. The TypeScript [`metal-orm`](https://github.com/celsowm/metal-orm) repository is the behavioral and architectural reference; C++26 changes the mechanism, not the ORM semantics.

For now, **SQLite is intentionally the only executor/dialect** while semantic parity is built out.

See [`docs/PARITY.md`](docs/PARITY.md) for the detailed parity matrix and remaining gaps.

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

## Runtime architecture

```text
Session
  ├── IdentityMap
  ├── UnitOfWork
  │     ├── typed lifecycle hooks
  │     └── nested rollback checkpoints
  ├── RelationChangeProcessor
  ├── Session interceptors
  └── DomainEventBus
          │
          ▼
      shared DML AST
          │
          ▼
        SQLite
```

The Unit of Work, graph persistence and relation mutations reuse the same runtime/DML infrastructure rather than maintaining independent SQL paths.

## Querying

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

`Post` fields do not satisfy the query scope until a reflected join introduces `Post`.

The SELECT AST includes:

- reflected JOINs;
- typed predicates and subqueries;
- aggregates / GROUP BY / HAVING;
- CTEs and recursive CTEs;
- UNION / UNION ALL / INTERSECT / EXCEPT;
- derived tables;
- CASE;
- window functions;
- typed SQL functions;
- relation predicates (`where_has`, `where_has_not`, `where_relation`);
- offset and keyset/cursor pagination.

## DML

The same DML AST is public and used by the runtime:

```cpp
auto insert = metal::InsertQueryBuilder{"users"}
    .values({
        {"name", std::string{"Alice"}},
        {"score", std::int64_t{10}}
    })
    .returning({"id", "name"});
```

Supported SQLite-oriented DML includes:

- multi-row INSERT;
- INSERT ... SELECT;
- UPDATE / DELETE;
- RETURNING;
- ON CONFLICT DO NOTHING;
- ON CONFLICT DO UPDATE;
- `excluded(column)`.

## Relation wrappers

The runtime models relation roles explicitly:

```cpp
metal::belongs_to_reference<Author>
metal::has_one_reference<Profile>
metal::has_many_collection<Post>
metal::many_to_many_collection<Role, UserRole>
metal::morph_one_reference<Cover>
metal::morph_many_collection<Attachment>
metal::morph_to_reference<Post, Video>
```

The new `belongs_to_reference<T>` / `has_one_reference<T>` wrappers are the intended canonical single-reference shapes. Their full generic lazy/mutation integration and removal of legacy raw-`shared_ptr` relation shape are the focused 0.0.17 task; graph persistence already uses the typed wrappers.

### Typed N:N pivot patches

```cpp
metal::pivot_patch<UserRole> patch;
patch
    .set<^^UserRole::label>(std::string{"owner"})
    .set<^^UserRole::weight>(std::int64_t{10});

user->roles.attach(role, patch);
```

The patch accepts only reflected members of the declared pivot type and validates value compatibility at compile time. Alternate relation `targetKey` values are respected end-to-end.

## Graph persistence — 0.0.16

The TypeScript implementation accepts DTO-like nested objects. C++ expresses the same graph semantics through a reflection-typed payload:

```cpp
auto payload = metal::graph<User>()
    .set<^^User::name>(std::string{"Celso"})
    .relation<^^User::profile>(
        metal::graph<Profile>()
            .set<^^Profile::bio>(std::string{"C++26"}))
    .relation<^^User::posts>([](auto& posts) {
        posts.add(
            metal::graph<Post>()
                .set<^^Post::title>(std::string{"Reflection"}));
        posts.add_id(42);
    });

auto user = metal::save_graph(session, payload);
```

Scalar fields are reflected template arguments. Invalid owners, relation members or incompatible values fail during compilation rather than becoming runtime string-key errors.

### Save, update and patch

```cpp
metal::save_graph(session, payload);
metal::update_graph(session, payload_with_pk);
metal::patch_graph(session, partial_payload_with_pk);
```

`update_graph` and `patch_graph` require the root primary key in the graph payload and return an empty `shared_ptr` when the root does not exist.

Omitted scalar fields and omitted relations remain untouched.

### Prune collection members

```cpp
metal::GraphOptions options{
    .prune_missing = true
};

auto updated = metal::update_graph(
    session,
    payload,
    options);
```

For has-many/morph-many this removes missing members according to relation cascade semantics. For N:N it detaches missing links.

### Pivot data inside a graph

```cpp
metal::pivot_patch<UserRole> owner;
owner.set<^^UserRole::label>(std::string{"owner"});

auto payload = metal::graph<User>()
    .relation<^^User::roles>([&](auto& roles) {
        roles.add(
            metal::graph<Role>()
                .set<^^Role::name>(std::string{"admin"}),
            owner);
    });
```

No second pivot-payload representation is introduced: graph persistence reuses `pivot_patch<Pivot>`.

Graph operations are transactional by default and compose with the existing UnitOfWork checkpoint system. Generated IDs, relation wrapper state and queued domain events therefore participate in rollback.

## Transactions and savepoints

```cpp
session.transaction([](metal::Session& tx) {
    auto user = tx.find<User>(1);
    user->name = "outer";

    tx.transaction([](metal::Session& nested) {
        // SAVEPOINT metalorm_sp_1
    });
});
```

The outer scope uses `BEGIN / COMMIT`. Nested scopes use `SAVEPOINT / RELEASE SAVEPOINT`. A failed nested scope executes `ROLLBACK TO SAVEPOINT` and marks the outer transaction rollback-only.

Rollback restores both SQL state and ORM memory state:

- reflected scalar values;
- dirty snapshots/status;
- generated IDs;
- Identity Map membership;
- deleted tracking;
- relation wrapper state;
- queued domain events.

## Lifecycle hooks and interceptors

Lifecycle hooks are Session-bound in both the C++ and current TypeScript implementations:

```cpp
metal::TableHooks<User> hooks;

hooks.before_insert = [](metal::Session&, User& user) {
    user.name = normalize(user.name);
};

hooks.after_insert = [](metal::Session&, User& user) {
    user.domain_events.raise(UserCreated{user.id});
};

session.register_table_hooks<User>(std::move(hooks));
```

Lifecycle ordering is:

```text
INSERT: beforeInsert -> INSERT/generated id -> snapshot/identity -> afterInsert
UPDATE: dirty diff -> beforeUpdate -> UPDATE -> refreshed snapshot -> afterUpdate
DELETE: beforeDelete -> DELETE/remove tracking -> afterDelete
```

Session-wide interceptors are separate:

```cpp
session.register_interceptor({
    .before_flush = [](metal::Session&) {},
    .after_flush = [](metal::Session&) {}
});
```

Raw `session.flush()` is UoW-only. Interceptors, relation processing and event dispatch belong to `commit()` / `transaction()`.

## Typed domain events

```cpp
struct UserCreated {
    std::int64_t id{};
};

using UserEvents = metal::domain_event_queue<UserCreated>;

struct [[=metal::mapping::table{"users"}]] EventUser {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::ignore]]
    UserEvents domain_events;
};
```

```cpp
user.domain_events.raise(UserCreated{user.id});

session.register_domain_event_handler<UserCreated>(
    [](const UserCreated& event, metal::Session&) {
        // outermost COMMIT already succeeded
    });
```

Nested SAVEPOINT release never dispatches events. Failed transaction scopes restore event queues. Handler failures after COMMIT are propagated as post-commit failures and never cause a fake rollback.

## SQLite DDL

```cpp
const auto sql = metal::create_table_sql<User>(dialect);
```

The current DDL layer covers the SQLite foundation and composite primary keys used by relation pivots. Introspection/diff/migration tooling remains a later parity area.

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

The immediate next release is deliberately narrow:

### 0.0.17

- finish Session-bound lazy loading for `belongs_to_reference<T>` and `has_one_reference<T>`;
- process generic `set/reset` mutations and cascade semantics through `RelationChangeProcessor`;
- accept/rollback single-reference baselines consistently;
- remove raw `std::shared_ptr<T>` as a valid reflected relation shape.

After that, parity moves to larger ecosystem modules such as schema introspection/diff, bulk operations, DTO/OpenAPI, cache, Tree/MPTT, pooling and code generation.

## Status

The project is intentionally experimental and tracks bleeding-edge C++26 reflection. The TypeScript implementation remains the behavioral reference; deviations are recorded in [`docs/PARITY.md`](docs/PARITY.md), not hidden behind compatibility shims.

See [`CHANGELOG.md`](CHANGELOG.md) for release-by-release changes.
