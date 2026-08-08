# MetalORM C++ parity matrix

The TypeScript `celsowm/metal-orm` repository is the behavioral and architectural reference for this port.

C++26 reflection may replace metadata plumbing, improve compile-time safety, and make APIs more strongly typed. It must not silently redefine MetalORM behavior.

SQLite is intentionally the only backend while semantic parity is being built.

## Runtime and mapping

| MetalORM capability | C++ status | Notes |
| --- | --- | --- |
| Entity/table metadata | ✅ | C++26 annotations + static reflection replace TS table/decorator metadata |
| Primary/generated columns | ✅ | `consteval` validated |
| Identity Map | ✅ | Separate runtime component |
| Unit of Work | ✅ | Separate component; shared DML AST |
| Session coordinator | ✅ | Coordinates UoW, Identity Map and relation processor |
| Dirty snapshots | ✅ | Reflection-generated |
| Persist/remove lifecycle | ✅ | Aligned with TS runtime semantics |
| Nested transactions/savepoints | ❌ | Not ported yet |
| Interceptors/hooks | ❌ | Not ported yet |
| Domain events | ❌ | Not ported yet |
| saveGraph/updateGraph/patchGraph | ❌ | Not ported yet |

## Relations

| MetalORM capability | C++ status | Notes |
| --- | --- | --- |
| belongsTo | ✅ | Reflected metadata + eager load |
| hasOne | ✅ | Reflected metadata + eager load |
| hasMany | ✅/🟡 | Dedicated collection, lazy/eager load and mutation; broader edge-case parity still evolving |
| belongsToMany | ✅ | Dedicated collection, lazy/eager load, IDs, sync, partial typed pivots and alternate `targetKey` behavior |
| morphTo | ✅ | Typed target set, lazy resolution, target switching, reset and cascade persist |
| morphOne | ✅ | Dedicated reference, lazy/eager loading, mutation and cascade behavior |
| morphMany | ✅ | Dedicated collection, lazy/eager loading, mutation and cascade behavior |
| cascade none/all/persist/remove/link | ✅ | Vocabulary and relation remove/persist semantics aligned |

### Has-many collection

Implemented:

- `load()`
- `get_items()`
- `add()`
- `attach()`
- `remove()`
- `clear()`
- Session-bound lazy loading
- reflected FK assignment for tracked roots
- UoW/cascade integration

### Many-to-many collection

Implemented:

- `load()`
- `get_items()`
- `attach(entity)`
- `attach(id)`
- `detach(entity)`
- `detach(id)`
- `sync_by_ids()`
- typed pivot hydration
- typed partial `pivot_patch<Pivot>` INSERT/UPDATE semantics
- compile-time pivot member ownership and patch-value compatibility
- relation-FK filtering from pivot DML payloads
- alternate non-primary `targetKey` for attach/detach/sync/pivot DML/cascade
- normal primary-key Identity Map integration after hydrated targets are materialized

`pivot_patch<Pivot>` is the C++ adaptation of TypeScript `Partial<TPivot>`: only explicitly reflected members become DML assignments, and repeated patches merge without resetting omitted fields.

The TypeScript `RelationChangeProcessor` now also resolves belongs-to-many mutation keys through the declared `targetKey`, so the previous implementation mismatch is closed in both repositories.

### Polymorphic relations

`morph_one` and `morph_many` keep the MetalORM parent-side semantics: the target row stores the reflected id/type pair, `set()` / `attach()` writes the pair in memory, lazy/eager hydration filters by both fields, and removal either clears them or cascades according to the relation mode.

`morph_to` keeps the child-side semantics with a compile-time target registry:

```cpp
[[=metal::mapping::morph_to<
    ^^Activity::subject_type,
    ^^Activity::subject_id,
    metal::mapping::cascade_mode::persist,
    metal::mapping::morph_target<"post", ^^Post>,
    metal::mapping::morph_target<"video", ^^Video>>{}]]
metal::morph_to_reference<Post, Video> subject;
```

The discriminator set is `consteval` validated for uniqueness. Every declared target/key must be compatible with the reflected id field. Lazy loading groups roots by discriminator and performs one query per concrete target type, then hydrates through the normal Identity Map.

The TypeScript query builder explicitly does not support JOIN-based `MorphTo` include and directs callers to lazy loading. The C++ typed SQL AST likewise does not model a polymorphic single-table JOIN; the parity path is `subject.load()`. `MorphOne` and `MorphMany` remain eager-loadable.

Remaining relation adaptation work is limited to JS-object-model conveniences such as `toJSON()` and broader has-many edge cases; those require explicit C++ serialization/API decisions rather than literal copying.

## Query builder and DML

| MetalORM capability | C++ status | Notes |
| --- | --- | --- |
| Typed SELECT AST | ✅ | Compile-time entity scope |
| WHERE/comparisons/logical expressions | ✅ partial catalog | Computed scalar operands now share the same predicate AST |
| IN / NULL / LIKE | ✅ | Typed values/subqueries |
| BETWEEN / NOT BETWEEN | ✅ | First-class expression AST |
| EXISTS / NOT EXISTS | ✅ | Typed SELECT subqueries |
| Reflected JOINs | ✅ | Non-polymorphic typed joins + runtime MorphOne/MorphMany include |
| Projections/aliases | ✅ | Columns, aggregates, functions, CASE, and window terms |
| Aggregates/GROUP BY/HAVING | ✅/🟡 | Core set plus STDDEV/VARIANCE AST; SQLite runtime availability can vary for extension functions |
| Scalar IN subqueries | ✅ | Exactly-one projection validation |
| CTE | ✅ | Multiple CTEs and optional column lists |
| recursive CTE | ✅ | `WITH RECURSIVE` plus typed CTE join bridge; exercised on a real parent/child traversal |
| UNION / UNION ALL | ✅ | Compound SELECT AST |
| INTERSECT / EXCEPT | ✅ | Compound SELECT AST |
| window functions | ✅ | ROW_NUMBER/RANK/DENSE_RANK/NTILE/LAG/LEAD/FIRST_VALUE/LAST_VALUE with partition/order specs |
| derived tables / fromSubquery | ✅ | Typed subquery source + alias; SQLite column-alias-list syntax is explicitly rejected |
| CASE | ✅ | Typed searched CASE usable in projections and predicates |
| SQL function AST | ✅ | Recursive typed scalar node + validated custom function identifier |
| SQLite text/control/date/JSON function families | ✅/🟡 | Broad executable catalog; TS-only/cross-dialect helpers continue to be filled as SQLite semantics are defined |
| numeric function catalog | ✅/🟡 | Broad AST surface; execution of optional math functions depends on SQLite build capabilities |
| INSERT/UPDATE/DELETE AST | ✅ | Shared by public builders, UoW and relation processor |
| Multi-row INSERT | ✅ | Multiple `VALUES` rows in one statement |
| INSERT ... SELECT | ✅ | Typed `BasicSelectQuery` source |
| RETURNING | ✅ | INSERT/UPDATE/DELETE, including aliases |
| SQLite UPSERT/conflict API | ✅ | target columns, DO NOTHING, DO UPDATE, update predicate, `excluded()` |
| relation query helpers (`whereHas`, etc.) | ❌ | Planned next |
| pagination/cursor helpers | ❌ | Planned next |

### Computed scalar AST

0.0.11 removes the old projection-only split between columns, aggregates, and windows. These are now recursive scalar expressions:

```text
ScalarTerm<Result, Owners...>
  ├── reflected column
  ├── literal parameter
  ├── aggregate
  ├── function call
  ├── CASE
  └── window function
```

The owner pack propagates through nested expressions. This means, for example, `lower(field<^^Post::title>)` cannot be projected by a `select<User>()` until `Post` is actually part of the typed query scope.

### Derived tables

```cpp
auto source = metal::select<User>()
    .clear_projection()
    .project(metal::field<^^User::id>)
    .project(metal::field<^^User::name>)
    .where(metal::field<^^User::score> >= 10);

auto query = metal::select<User>()
    .from_subquery(source, "high_scores")
    .where(metal::field<^^User::score> <= 20);
```

The subquery remains a normal `BasicSelectQuery`, and projection/subquery/outer-predicate parameters are appended in SQL lexical order. SQLite does not support a derived-table column alias list of the form `AS alias(col1, col2)`, so the SQLite-only port rejects a non-empty alias list and requires aliases to be applied to source projections instead.

### CASE and function expressions

```cpp
auto band = metal::case_when(
        metal::field<^^User::score> > 20,
        std::string{"high"})
    .when(
        metal::field<^^User::score> > 10,
        std::string{"medium"})
    .otherwise(std::string{"low"});

auto query = metal::select<User>()
    .clear_projection()
    .project(band.as("band"))
    .where(
        metal::lower(
            metal::trim(metal::field<^^User::name>)) == "alice");
```

Representative SQLite helper families now include text operations (`lower`, `upper`, trims, concat, substring, replace, length, left/right, character/byte helpers), numeric operations, control flow (`coalesce`, `if_null`, `nullif`, `greatest`, `least`), date/time helpers, and JSON path/length/aggregation helpers. `sql_function<Result>(name, ...)` is the extensibility path and accepts only a simple SQL identifier; literal arguments still become bound parameters.

### Advanced SELECT parity details

CTEs reuse the same typed SELECT source rather than accepting raw SQL:

```cpp
auto active = metal::select<User>()
    .where(metal::field<^^User::active> == true);

auto query = metal::select<User>()
    .with("active_users", active)
    .from("active_users");
```

Recursive CTEs can join a reflected table member to a CTE column without introducing a string predicate:

```cpp
auto tree = metal::select<Node>()
    .where(metal::is_null(metal::field<^^Node::parent_id>));

auto step = metal::select<Node>();
step.join_cte<^^Node::parent_id>("tree", "id");
tree.union_all(step);

auto query = metal::select<Node>()
    .with_recursive("tree", tree, {"id", "parent_id", "name"})
    .from("tree");
```

Set operations validate projection arity before producing SQL and compound-level ordering/pagination is emitted after the set-operation chain, matching the TypeScript compiler structure.

Window terms remain typed projections:

```cpp
auto query = metal::select<Employee>()
    .clear_projection()
    .project(metal::field<^^Employee::id>)
    .project(
        metal::row_number()
            .partition_by(metal::field<^^Employee::department>)
            .order_by(metal::field<^^Employee::salary>, false)
            .as("rank_in_department"));
```

Window field references remain constrained by the compile-time query scope and literal window arguments are parameterized.

### DML parity details

The TypeScript insert builder treats `VALUES` and `SELECT` as mutually exclusive insert sources. C++ 0.0.9 keeps the same state rule:

```cpp
auto insert = metal::InsertQueryBuilder{"users"}
    .values({
        {{"name", std::string{"A"}}, {"score", std::int64_t{10}}},
        {{"name", std::string{"B"}}, {"score", std::int64_t{20}}}
    })
    .returning({"id", "name"});
```

Typed INSERT SELECT reuses the normal select AST:

```cpp
auto source = metal::select<Source>();
source
    .clear_projection()
    .project(metal::field<^^Source::name>)
    .project(metal::field<^^Source::score>);

metal::insert_into<Target>()
    .from_select(source, {"name", "score"})
    .returning({"id"});
```

SQLite conflict handling follows the TypeScript SQLite dialect contract and requires explicit conflict columns:

```cpp
metal::InsertQueryBuilder{"users"}
    .values({{"email", email}, {"name", name}})
    .on_conflict({"email"})
    .do_update({{"name", metal::excluded("name")}})
    .returning({"id", "name"});
```

`excluded()` is a dedicated DML operand and is accepted only in the `DO UPDATE SET` branch.

## Schema/tooling/ecosystem

| MetalORM capability | C++ status |
| --- | --- |
| SQLite DDL generation | ✅ foundational |
| composite PK DDL | ✅ |
| schema introspection | ❌ |
| schema diff | ❌ |
| migrations/tooling | ❌ |
| DTO/OpenAPI | ❌ |
| Tree/MPTT | ❌ |
| cache layer | ❌ |
| bulk operations | ❌ |
| procedure calls | ❌ |
| pooling | ❌ |
| DB-to-entity code generation | ❌ |

## Ordered parity roadmap

The next releases should close reference gaps rather than add unrelated capabilities:

1. **0.0.12:** relation query helpers (`whereHas`, `whereHasNot`, relation conditions) and pagination/cursor helpers.
2. Then: nested transactions/savepoints, interceptors/hooks, domain events, and graph persistence/update helpers.
3. Then: schema/tooling/ecosystem modules such as introspection/diff, bulk operations, DTO/OpenAPI, cache, Tree/MPTT, pooling, and code generation.

This ordering may change when comparison with the TypeScript reference exposes a more fundamental dependency.