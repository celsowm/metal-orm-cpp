# Database to C++26 entity generation

MetalORM C++ 0.0.28 can introspect SQLite and generate reflection-native entity declarations.

## CLI

```bash
metal-orm-gen \
  --db=app.sqlite \
  --out=include/app/entities.hpp \
  --namespace=app_model
```

Useful filters:

```bash
metal-orm-gen --db=app.sqlite --include=users,posts --out=entities.hpp
metal-orm-gen --db=app.sqlite --exclude=audit_log --no-relations --out=entities.hpp
```

`--out=-` writes the generated header to stdout.

## Library API

Generation is deliberately separated from introspection:

```cpp
metal::DatabaseSchema schema = metal::introspect_sqlite(executor);
auto generated = metal::generate_entity_header(schema, {
    .namespace_name = "app_model"
});
```

or, as a convenience:

```cpp
auto generated = metal::generate_sqlite_entity_header(executor);
```

`GeneratedEntityHeader::code` contains the header and `warnings` contains schema features that cannot currently be represented without changing runtime semantics.

## Generated mapping

A table such as:

```sql
CREATE TABLE posts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL REFERENCES users(id),
    title VARCHAR(200) NOT NULL DEFAULT 'untitled'
);
```

produces the same metadata in the C++ type itself:

```cpp
struct User;

struct [[=metal::mapping::table{"posts"}]] Post {
    [[=metal::mapping::primary_key,
      =metal::mapping::generated,
      =metal::mapping::database_type{"INTEGER"}]]
    std::int64_t id{};

    [[=metal::mapping::database_type{"INTEGER"}]]
    std::int64_t user_id{};

    [[=metal::mapping::database_type{"VARCHAR(200)"},
      =metal::mapping::default_text{"untitled"}]]
    std::string title{};

    [[=metal::mapping::belongs_to<^^Post::user_id>{}]]
    metal::belongs_to_reference<User> user;
};
```

There is no generated trait table, registration macro or JSON metadata file.

## Type mapping

For the current SQLite execution model:

- integer affinities -> `std::int64_t`;
- real/numeric affinities -> `double`;
- boolean declarations -> `bool`;
- text/date/time/json/uuid declarations -> `std::string`;
- nullable columns -> `std::optional<T>`.

The exact declared SQL type is additionally preserved with `mapping::database_type`. `create_table_sql()` and `expected_schema()` honor this annotation, so `VARCHAR(80)` is not silently degraded to `TEXT` in a schema round-trip.

Defaults are converted to the existing reflection-native annotations:

- numeric / boolean -> `mapping::default_value`;
- quoted text -> `mapping::default_text`;
- `NULL` -> `mapping::default_null` for nullable fields;
- SQL expressions -> `mapping::default_sql`.

## Foreign keys

A physical foreign key to the target entity's single primary key generates an owning-side `belongs_to_reference<T>`.

The generator deliberately does not invent inverse `has_many` metadata. In C++26 the inverse annotation requires a reflection such as `^^Child::foreign_key`; emitting both directions in ordinary struct declarations introduces a definition-order cycle. External relation registries would duplicate the metadata model, so they are not used.

Foreign keys to non-primary target keys remain represented by the scalar column and produce a warning until a reflection-native code-generation ordering mechanism can express the target-key dependency without a second metadata system.

## Current explicit edges

- SQLite BLOB values are not yet represented by `metal::Value`; BLOB-declared columns are generated as `std::string` with a warning while `database_type` preserves the SQL declaration.
- Views can be introspected with `--include-views`, but they are reported as warnings rather than emitted as writable entities. The runtime does not yet expose a read-only mapped-view contract, and generating a normal `mapping::table` would be semantically wrong.

These are explicit runtime/model gaps, not silent code-generator fallbacks.
