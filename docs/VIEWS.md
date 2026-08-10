# Read-only mapped views

MetalORM C++ 0.0.36 adds a dedicated runtime and code-generation surface for SQLite views without pretending that a view is a mutable ORM entity.

## Mapping

Views use `mapping::view` instead of `mapping::table`:

```cpp
struct [[=metal::mapping::view{"active_users"}]] ActiveUsersView {
    std::int64_t id{};
    std::string name;
};
```

A mapped view satisfies `reflect::ViewMapped<T>`, but it deliberately does **not** satisfy `reflect::Mapped<T>` or `reflect::Entity<T>`.

That distinction is the read-only contract:

- no ORM primary key is required or invented;
- no Identity Map entry is created;
- no Unit of Work snapshot is created;
- `Session::persist()` / `Session::remove()` do not accept the type;
- relation annotations and generated-column annotations are rejected by view validation.

Returned values are detached C++ objects. Mutating one changes only the local object.

## Typed querying

Known scalar columns reuse the existing expression DSL:

```cpp
auto rows = metal::view_query<ActiveUsersView>(session)
    .where(metal::field<^^ActiveUsersView::name> == std::string{"Ada"})
    .order_by<^^ActiveUsersView::id>()
    .limit(20)
    .all();
```

`view_query<T>()` can receive a `Session`-like context exposing `executor()` and `dialect()`, or an explicit `DbExecutor` + `Dialect` pair.

The view query surface supports:

- typed `Expression<T>` predicates;
- reflected ordering;
- limit/offset;
- `all()` materialization;
- `first()` materialization;
- `compile()` for inspection or custom execution.

It intentionally does not expose INSERT, UPDATE, DELETE, persistence, relation mutation, eager relation loading, or Identity Map semantics.

## Untyped SQLite view expressions

SQLite frequently reports no declared type for computed view columns. For example, a view column produced by:

```sql
COUNT(o.id) AS order_count
```

normally appears through `PRAGMA table_info(view_name)` with an empty declared type.

The generator therefore uses `metal::Value` for an introspected view column whose declared type is empty:

```cpp
metal::Value order_count{nullptr};
```

This preserves the real runtime SQLite value (`int64`, `double`, text, blob, bool/null representation) rather than incorrectly forcing the column to `std::string`.

Columns with declared SQLite types remain strongly typed:

- integer affinity -> `std::int64_t`;
- real/numeric affinity -> `double`;
- text affinity -> `std::string`;
- blob/binary affinity -> `metal::Blob`;
- boolean declarations -> `bool`.

When SQLite reports a known column as nullable, generated code uses `std::optional<T>` conservatively.

## Code generation

There are four view-aware generator entry points:

```cpp
metal::generate_view_header(schema);
metal::generate_sqlite_view_header(executor, options, introspection);
metal::generate_model_header(schema);
metal::generate_sqlite_model_header(executor, options, introspection);
```

`generate_view_header()` emits only read-only view models.

`generate_model_header()` emits ordinary table entities and read-only views into one self-contained header. Internally it keeps table generation on the established entity-generator path and adds the view declarations before the generated namespace closes.

The CLI now uses the combined model generator. With:

```text
metal-orm-gen --db=app.sqlite --include-views
```

introspected views are emitted as `mapping::view` models instead of being reduced to warnings.

Without `--include-views`, CLI behavior remains table-only because SQLite view introspection is opt-in.

## Why views are not entities

SQLite can expose a view whose rows have no stable unique key, whose columns are derived expressions, and whose apparent mutability depends on triggers or backend-specific rules. Inventing an ORM primary key or silently routing view objects through the Unit of Work would make identity, updates, deletes, rollback and relation behavior ambiguous.

MetalORM therefore models the actual invariant: a database view is a typed read source. If a future backend exposes explicitly updatable views, that capability should be modeled independently rather than weakening the read-only contract of ordinary mapped views.
