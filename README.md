# MetalORM C++

![CI](https://github.com/celsowm/metal-orm-cpp/actions/workflows/ci.yml/badge.svg?branch=main)

> A C++26-native port of MetalORM built around static reflection, annotations, splicing and expansion statements.

**Version:** `0.0.10`

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

The patch is checked at compile time: `^^Member` must belong to `UserRole`, and the supplied value type must be compatible with that reflected member. Reattaching an existing target with a partial patch updates only the supplied columns and preserves the others.

### Alternate target keys

A N:N relation is not required to use the target primary key. Relation identity can use a reflected alternate member while hydrated entities continue to use their real primary key in the Session Identity Map.

```cpp
[[=metal::mapping::many_to_many<
    ^^UserRoleByCode,
    ^^UserRoleByCode::user_id,
    ^^UserRoleByCode::role_code,
    metal::mapping::cascade_mode::remove,
    std::meta::info{},
    ^^Role::code>{}]]
metal::many_to_many_collection<Role, UserRoleByCode> roles;
```

```cpp
user->roles.attach(std::string{"DEV"});
user->roles.sync_by_ids(std::vector<std::string>{"DEV", "ADMIN"});
user->roles.detach(std::string{"ADMIN"});
```

## Polymorphic relations

`0.0.8` ports the MetalORM `morphTo`, `morphOne`, and `morphMany` family without introducing a runtime metadata registry.

### Morph one

```cpp
struct Cover {
    std::optional<std::int64_t> imageable_id;
    std::optional<std::string> imageable_type;
};

struct Post {
    [[=metal::mapping::morph_one<
        ^^Cover::imageable_type,
        ^^Cover::imageable_id,
        "post",
        metal::mapping::cascade_mode::all>{}]]
    metal::morph_one_reference<Cover> cover;
};
```

```cpp
post->cover.set(cover);
session.commit();

assert(cover->imageable_id == post->id);
assert(cover->imageable_type == "post");
```

`load()` is lazy and `.include<^^Post::cover>()` eagerly hydrates the relation through the same Identity Map.

### Morph many

```cpp
struct Post {
    [[=metal::mapping::morph_many<
        ^^Attachment::attachable_type,
        ^^Attachment::attachable_id,
        "post",
        metal::mapping::cascade_mode::all>{}]]
    metal::morph_many_collection<Attachment> attachments;
};
```

```cpp
auto attachment = post->attachments.add();
attachment->name = "spec.pdf";

post->attachments.attach(existing);
post->attachments.remove(existing);
post->attachments.clear();
```

When parent and child are both new, the first Unit of Work flush generates the parent PK, the relation processor reapplies the final id/type pair, and the second flush persists those final values.

### Morph to

```cpp
struct Activity {
    std::optional<std::int64_t> subject_id;
    std::optional<std::string> subject_type;

    [[=metal::mapping::morph_to<
        ^^Activity::subject_type,
        ^^Activity::subject_id,
        metal::mapping::cascade_mode::persist,
        metal::mapping::morph_target<"post", ^^Post>,
        metal::mapping::morph_target<"video", ^^Video>>{}]]
    metal::morph_to_reference<Post, Video> subject;
};
```

```cpp
activity->subject.set(post);
session.commit();

activity->subject.load();
auto loaded_post = activity->subject.get_as<Post>();

activity->subject.set(video);
session.commit();

activity->subject.reset();
session.commit();
```

The target set and discriminator values are compile-time data. MorphTo lazy loading groups roots by discriminator and performs one query per concrete target type, then hydrates targets through the normal Identity Map. MetalORM TS explicitly has no JOIN-based MorphTo include; `subject.load()` is the parity path.

## Lazy and eager relation loading

Relation wrappers are bound to the `Session` when their root entity becomes tracked. Lazy and eager hydration reuse the same Identity Map.

```cpp
auto user = session.query<User>()
    .where(metal::field<^^User::id> == id)
    .first();

assert(!user->roles.loaded());
user->roles.load();
assert(user->roles.loaded());
```

```cpp
auto users = session.query<User>()
    .include<^^User::posts>()
    .include<^^User::roles>()
    .all();
```

The current SQLite executor is synchronous, so `load()` is synchronous; this is an execution-model adaptation, not a different relation semantic.

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
    .having(metal::count(metal::field<^^Post::id>) > 1)
    .order_by(metal::field<^^User::name>, false)
    .limit(5);
```

`select<User>()` initially has only `User` in its C++ query scope. A predicate using `Post` does not satisfy the query constraints until `join<^^User::posts>()` introduces `Post` through reflected relationship metadata.

For N:N, one reflected relation expands into both joins:

```text
User -> UserRole pivot -> Role
```

## Advanced SELECT AST

`0.0.10` ports the next MetalORM query-builder family without creating a raw-SQL side channel.

### BETWEEN and EXISTS

```cpp
auto subquery = metal::select<User>()
    .clear_projection()
    .project(metal::field<^^User::id>)
    .where(metal::field<^^User::active> == true);

auto query = metal::select<User>()
    .where(
        metal::between(metal::field<^^User::score>, 10, 100) &&
        metal::exists(subquery));
```

`not_between(...)` and `not_exists(...)` use the same expression AST and compose with `&&`, `||`, and `!`.

### CTEs and recursive CTEs

```cpp
auto active = metal::select<User>()
    .where(metal::field<^^User::active> == true);

auto query = metal::select<User>()
    .with("active_users", active)
    .from("active_users");
```

A real recursive traversal keeps table members reflected while the CTE name remains a SQL identifier:

```cpp
auto tree = metal::select<Node>()
    .where(metal::is_null(metal::field<^^Node::parent_id>));

auto step = metal::select<Node>();
step.join_cte<^^Node::parent_id>("tree", "id");
tree.union_all(step);

auto query = metal::select<Node>()
    .with_recursive(
        "tree",
        tree,
        {"id", "parent_id", "name"})
    .from("tree");
```

CTE column lists are checked against projection arity before compilation.

### Set operations

```cpp
auto current = metal::select<User>()
    .clear_projection()
    .project(metal::field<^^User::id>);

auto archived = metal::select<ArchivedUser>()
    .clear_projection()
    .project(metal::field<^^ArchivedUser::id>);

current.union_all(archived);
```

The AST supports `union_with`, `union_all`, `intersect`, and `except_with`. Both sides must project the same number of expressions. Compound `ORDER BY`, `LIMIT`, and `OFFSET` are emitted after the set-operation chain.

### Window functions

```cpp
auto query = metal::select<Employee>()
    .clear_projection()
    .project(metal::field<^^Employee::id>)
    .project(
        metal::row_number()
            .partition_by(metal::field<^^Employee::department>)
            .order_by(metal::field<^^Employee::salary>, false)
            .as("rank_in_department"))
    .project(
        metal::lag(metal::field<^^Employee::salary>, 1, 0)
            .partition_by(metal::field<^^Employee::department>)
            .order_by(metal::field<^^Employee::id>)
            .as("previous_salary"));
```

The current catalog includes:

```text
ROW_NUMBER
RANK
DENSE_RANK
NTILE
LAG
LEAD
FIRST_VALUE
LAST_VALUE
```

Partition/order fields remain constrained by the typed query scope and literal arguments are bound as parameters.

## Shared DML AST

Like the original MetalORM, persistence and relation mutation share DML builders instead of maintaining a second hand-written SQL path. `0.0.9` expanded that shared layer to the richer SQLite DML surface.

### Multi-row INSERT + RETURNING

```cpp
auto query = metal::InsertQueryBuilder{"users"}
    .values({
        {{"name", std::string{"Alice"}}, {"score", std::int64_t{10}}},
        {{"name", std::string{"Bob"}},   {"score", std::int64_t{20}}}
    })
    .returning({"id", "name"})
    .compile(dialect);

auto result = db->execute(query.sql, query.params);
```

`RETURNING` is supported on INSERT, UPDATE and DELETE and flows through the normal `QueryResult.rows` path.

### INSERT ... SELECT

```cpp
auto source = metal::select<Source>();
source
    .clear_projection()
    .project(metal::field<^^Source::name>)
    .project(metal::field<^^Source::score>)
    .where(metal::field<^^Source::score> >= 10);

metal::insert_into<Target>()
    .from_select(source, {"name", "score"})
    .returning({"id"});
```

`VALUES` and `SELECT` are mutually exclusive sources, matching the MetalORM TypeScript insert-state model.

### SQLite UPSERT / ON CONFLICT

```cpp
auto query = metal::InsertQueryBuilder{"users"}
    .values({
        {"email", std::string{"alice@example.com"}},
        {"name", std::string{"Alice"}}
    })
    .on_conflict({"email"})
    .do_update({
        {"name", metal::excluded("name")}
    })
    .returning({"id", "name"})
    .compile(dialect);
```

`on_conflict(columns)` requires explicit SQLite conflict-target columns. Both `do_nothing()` and `do_update(...)` are supported. `excluded(column)` is accepted only in the conflict-update assignment branch.

Normal UoW persistence and relation mutation continue compiling through these same builders.

## Cascade semantics

The cascade vocabulary follows the original runtime:

```cpp
metal::mapping::cascade_mode::none
metal::mapping::cascade_mode::all
metal::mapping::cascade_mode::persist
metal::mapping::cascade_mode::remove
metal::mapping::cascade_mode::link
```

The same two-phase commit architecture handles normal and polymorphic relation mutations:

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

## Compile-time model validation

Mappings and typed relation payloads fail at compile time for cases including:

- wrong relation member wrapper;
- N:N collection/pivot type mismatch;
- pivot patch member from the wrong pivot type;
- pivot patch value incompatible with its reflected member;
- incompatible FK/key C++ types;
- invalid Morph field ownership or key types;
- duplicate/empty MorphTo discriminators;
- MorphTo target not represented by the typed reference;
- conflicting annotations;
- duplicate mapped column names;
- invalid generated-key declarations.

Query AST validation additionally constrains reflected fields to the current query scope and rejects scalar-subquery, CTE-column-list, and set-operation projection arity mismatches before emitting SQL.

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

Reflections are non-type template arguments throughout mapping, queries, relation metadata and mutation. Structural class NTTPs carry Morph discriminator values at compile time.

## What 0.0.10 contains

- C++26 static reflection and annotations as the only metadata model
- `Mapped<T>` / `Entity<T>` concepts and `consteval` validation
- typed SELECT SQL AST with compile-time query scope
- reflected joins, projections, predicates, aggregates, grouping and scalar subqueries
- `BETWEEN` / `NOT BETWEEN` and typed `EXISTS` / `NOT EXISTS`
- CTEs and genuinely recursive CTE traversal
- UNION / UNION ALL / INTERSECT / EXCEPT with projection-arity validation
- typed window-function projections and partition/order specs
- shared INSERT / UPDATE / DELETE AST builders
- multi-row INSERT and typed INSERT ... SELECT
- INSERT/UPDATE/DELETE RETURNING, including aliases
- SQLite ON CONFLICT DO NOTHING / DO UPDATE with conflict targets and update predicates
- `excluded(column)` conflict-update operands
- reflected SQLite DDL, including composite primary keys
- SQLite executor
- `Session` coordinating `IdentityMap`, `UnitOfWork`, and `RelationChangeProcessor`
- reflected dirty checking and generated keys
- dedicated has-many / N:N relation collections and partial typed pivots
- alternate non-primary N:N `targetKey` behavior
- MorphTo/MorphOne/MorphMany runtime parity
- MetalORM-compatible cascade vocabulary including `link`

See `docs/PARITY.md` for the explicit reference matrix and ordered parity roadmap.

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The CMake project intentionally fails on GCC < 16 and on non-GNU compilers today. That restriction should disappear only when another mainstream compiler implements the same C++26 reflection model, not through a compatibility metadata implementation.

## Direction

The TypeScript MetalORM remains the feature and behavior reference. With the 0.0.10 advanced SELECT family closed, the next parity target is derived-table / `fromSubquery` support, CASE expressions, and the broader SQL function catalog; relation-query helpers and pagination follow after that.
