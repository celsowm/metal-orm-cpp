# Physical schema constraints

MetalORM C++ keeps ORM relationships and physical database constraints separate.

A `belongs_to`, `has_one`, `has_many` or other relation annotation describes ORM behavior. It does **not** silently create a database foreign key. Physical foreign keys, UNIQUE constraints and CHECK constraints are declared explicitly as schema metadata.

## Foreign keys

```cpp
struct [[=metal::mapping::table{"users"}]] User {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
};

struct [[=metal::mapping::table{"posts"}]] Post {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::reference<
        ^^User::id,
        metal::mapping::referential_action::cascade,
        metal::mapping::referential_action::restrict,
        "fk_posts_user",
        true>{}]]
    std::optional<std::int64_t> user_id;

    [[=metal::mapping::belongs_to<^^Post::user_id>{}]]
    metal::belongs_to_reference<User> user;
};
```

`mapping::reference<TargetColumn, OnDelete, OnUpdate, ConstraintName, Deferrable>` is the direct member-reflection form. The referenced member must belong to a mapped type, both sides must be persistent scalar columns, and their underlying value types must be compatible. Invalid mappings fail at compile time.

`ConstraintName` defaults to the empty string. `Deferrable` defaults to `false`; when true the SQLite DDL is emitted as `DEFERRABLE INITIALLY DEFERRED`, matching the boolean `ForeignKeyReference.deferrable` contract in the TypeScript reference.

Supported referential actions are `unspecified`, `no_action`, `restrict`, `cascade`, `set_null` and `set_default`.

The diff treats an omitted action and SQLite's introspected `NO ACTION` as semantically equivalent, avoiding false rebuild warnings. Constraint name and deferred/immediate behavior are compared as physical schema metadata too.

### Generated and cyclic foreign keys

Database-to-entity generation uses an equivalent form that does not require the target member to be nameable while the target type is still only forward-declared:

```cpp
struct User;

struct [[=metal::mapping::table{"posts"}]] Post {
    [[=metal::mapping::primary_key]]
    std::int64_t id{};

    [[=metal::mapping::reference_to<
        ^^User,
        "id",
        metal::mapping::referential_action::cascade,
        metal::mapping::referential_action::restrict,
        "fk_posts_user",
        true>{}]]
    std::optional<std::int64_t> user_id;
};

struct [[=metal::mapping::table{"users"}]] User {
    [[=metal::mapping::primary_key]]
    std::int64_t id{};
};
```

`reference_to<TargetType, TargetPhysicalColumn, OnDelete, OnUpdate, ConstraintName, Deferrable>` is still reflection-validated. Once the generated types are complete, MetalORM resolves the physical column name to exactly one persistent reflected target member and performs the same type-compatibility validation as `reference<^^Target::member>`. This makes generated self-references and cyclic schemas possible without replacing the C++26 metadata model with a runtime registry.

## Column UNIQUE constraints

The TypeScript `ColumnDef.unique?: boolean | string` contract maps directly to two reflected forms:

```cpp
struct [[=metal::mapping::table{"users"}]] User {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::unique]]
    std::optional<std::string> handle;

    [[=metal::mapping::named_unique<"uq_users_email">{}]]
    std::string email;
};
```

`mapping::unique` is the equivalent of `unique: true`. `mapping::named_unique<"name">{}` is the equivalent of the TypeScript string form. A column can declare at most one physical UNIQUE annotation, named constraints require a non-empty name, and the annotation is valid only on persistent scalar members.

Column UNIQUE metadata is intentionally separate from `DatabaseIndex`. SQLite implements UNIQUE constraints with internal autoindexes, but an ORM-level column constraint is not treated as if the user explicitly created a `CREATE UNIQUE INDEX` statement.

For externally created SQLite schemas, the stored-DDL parser recognizes both inline `UNIQUE` / `CONSTRAINT name UNIQUE` and single-column table-level `UNIQUE(column)` forms. Composite or expression-based UNIQUE constraints are not collapsed into per-column metadata; explicit composite uniqueness remains represented by the existing unique-index surface.

SQLite cannot add a UNIQUE column through `ALTER TABLE ... ADD COLUMN`. Schema synchronization therefore reports a rebuild requirement rather than emitting SQL that SQLite would reject.

## Column CHECK constraints

The TypeScript `col.check(...)` contract maps to a reflected member annotation:

```cpp
struct [[=metal::mapping::table{"people"}]] Person {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::check<"age >= 0">{}]]
    std::int64_t age{};
};
```

A column can declare at most one physical check, matching the single `ColumnDef.check` field in the TypeScript reference. Empty expressions and checks attached to non-persistent members fail during compile-time mapping validation.

## Table CHECK constraints

Table checks are annotations on the mapped type. Both unnamed and named constraints are supported:

```cpp
struct [[
    =metal::mapping::table{"orders"},
    =metal::mapping::table_check<"quantity > 0">{},
    =metal::mapping::named_table_check<
        "price_guard",
        "price >= 0">{}
]] Order {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::int64_t quantity{};
    double price{};
};
```

This corresponds to the TypeScript table-level `{ name?, expression }` CHECK metadata without introducing a runtime schema registry.

## SQLite round-trip

For SQLite, the same explicit metadata now drives:

1. `CREATE TABLE` foreign-key, UNIQUE and CHECK clauses;
2. named inline foreign-key and UNIQUE constraints;
3. `DEFERRABLE INITIALLY DEFERRED` foreign-key behavior;
4. reflection-derived expected schema metadata;
5. `pragma_foreign_key_list` introspection for FK target and referential actions;
6. `sqlite_master.sql` parsing for FK names/deferrability, UNIQUE metadata and column/table CHECK constraints;
7. schema diff comparison;
8. database-to-C++ entity generation, including FK name/actions/deferrability, UNIQUE metadata, column checks and named/unnamed table checks.

SQLite's FK PRAGMA does not expose the physical constraint name or whether the declaration is deferred, so MetalORM complements PRAGMA data by parsing the stored `CREATE TABLE` statement. The same SQL-aware scanner tracks nested parentheses, quoted strings, quoted identifiers, line comments and block comments. It also recovers column UNIQUE names that SQLite otherwise hides behind `sqlite_autoindex_*` objects.

Only `DEFERRABLE INITIALLY DEFERRED` maps to `deferrable=true`. `NOT DEFERRABLE`, `DEFERRABLE INITIALLY IMMEDIATE`, bare `DEFERRABLE` and the other immediate SQLite forms remain `false`.

Existing SQLite tables cannot alter FK modifiers, column UNIQUE constraints or CHECK constraints in place through the supported synchronization path. A mismatch is therefore reported as an explicit rebuild warning instead of emitting unsafe fake ALTER SQL.
