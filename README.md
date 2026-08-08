# MetalORM C++

![CI](https://github.com/celsowm/metal-orm-cpp/actions/workflows/ci.yml/badge.svg?branch=main)

> A C++26-native port of MetalORM built around static reflection, annotations, splicing and expansion statements.

**Version:** `0.0.15`

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
  │     ├── table lifecycle hooks
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

Unlike a SQL-only wrapper, the C++ runtime checkpoints ORM state at every transaction/savepoint boundary. Rollback restores reflected scalar values, dirty snapshots/status, generated IDs, Identity Map membership, deleted tracking and relation wrapper state. A successful inner savepoint does not destroy the outer checkpoint, so an outer rollback can still undo inner work.

`Session::commit()` uses the same checkpoint mechanism. If database commit fails, ORM state is restored instead of leaving a generated ID or dirty snapshot falsely marked as committed.

## Lifecycle hooks, interceptors and events — 0.0.15

Table hooks are typed by entity:

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

The lifecycle matches the TypeScript Unit of Work:

```text
INSERT: beforeInsert -> INSERT/generated id -> snapshot/identity -> afterInsert
UPDATE: dirty diff -> beforeUpdate -> UPDATE -> refreshed snapshot -> afterUpdate
DELETE: beforeDelete -> DELETE/remove tracking -> afterDelete
```

The C++ registration surface is deliberately type-safe and Session-bound. TypeScript stores the same lifecycle callbacks on `TableDef`; this binding-scope difference is documented rather than hidden as fake 1:1 API syntax.

Session-wide flush interceptors remain a separate concept:

```cpp
session.register_interceptor({
    .before_flush = [](metal::Session&) {},
    .after_flush = [](metal::Session&) {}
});
```

They surround the complete commit/transaction flush pipeline. Raw `session.flush()` remains UoW-only, so table hooks run there but Session interceptors, relation processing and domain-event dispatch do not.

Domain events are typed C++ values rather than string-discriminated public payloads:

```cpp
struct UserCreated {
    std::int64_t id{};
};

struct UserRenamed {
    std::int64_t id{};
    std::string name;
};

using UserEvents = metal::domain_event_queue<UserCreated, UserRenamed>;

struct [[=metal::mapping::table{"users"}]] User {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;

    [[=metal::mapping::ignore]]
    UserEvents domain_events;
};
```

Raise and handle them with compile-time event types:

```cpp
user.domain_events.raise(UserCreated{user.id});

session.register_domain_event_handler<UserCreated>(
    [](const UserCreated& event, metal::Session& committed) {
        // the outermost database COMMIT is already successful here
    });
```

Event queues participate in transaction checkpoints. An event raised inside a failed transaction/savepoint disappears with that rollback rather than leaking into a later commit. Nested `RELEASE SAVEPOINT` never dispatches. Events dispatch only after successful outermost COMMIT.

If a handler itself throws, the error is a **post-commit** failure: it propagates, but MetalORM does not issue a fake rollback for database work that is already committed. Matching the TypeScript bus, the queue is cleared only after all handlers complete, so a failing handler leaves it available for caller-defined recovery or retry policy.

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

The SELECT AST includes reflected joins, predicates/subqueries, aggregates/GROUP BY/HAVING, CTEs/recursive CTEs, set operations, window functions, derived tables, searched CASE and recursive typed SQL function expressions.

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

0.0.13 moved relation correlation into the SELECT compiler's `WHERE` position. Callback-local `ORDER BY / LIMIT / OFFSET` runs after correlation, while root relation predicates run before root pagination. Nested correlated scopes use internal aliases such as `t0`, `t0_rel`, `t0_rel_rel`, preventing child subqueries from shadowing outer aliases.

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
