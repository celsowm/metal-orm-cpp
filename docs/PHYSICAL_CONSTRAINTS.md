# Physical schema constraints

MetalORM C++ keeps ORM relationships and physical database constraints separate.

A `belongs_to`, `has_one`, `has_many` or other relation annotation describes ORM behavior. It does **not** silently create a database foreign key. Physical foreign keys and CHECK constraints are declared explicitly as schema metadata.

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
        metal::mapping::referential_action::cascade>{}]]
    std::optional<std::int64_t> user_id;

    [[=metal::mapping::belongs_to<^^Post::user_id>{}]]
    metal::belongs_to_reference<User> user;
};
```

`mapping::reference<TargetColumn, OnDelete, OnUpdate>` is the direct member-reflection form. The referenced member must belong to a mapped type, both sides must be persistent scalar columns, and their underlying value types must be compatible. Invalid mappings fail at compile time.

Supported referential actions are `unspecified`, `no_action`, `restrict`, `cascade`, `set_null` and `set_default`.

The diff treats an omitted action and SQLite's introspected `NO ACTION` as semantically equivalent, avoiding false rebuild warnings.

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
        metal::mapping::referential_action::cascade>{}]]
    std::optional<std::int64_t> user_id;
};

struct [[=metal::mapping::table{"users"}]] User {
    [[=metal::mapping::primary_key]]
    std::int64_t id{};
};
```

`reference_to<TargetType, TargetPhysicalColumn, OnDelete, OnUpdate>` is still reflection-validated. Once the generated types are complete, MetalORM resolves the physical column name to exactly one persistent reflected target member and performs the same type-compatibility validation as `reference<^^Target::member>`. This makes generated self-references and cyclic schemas possible without replacing the C++26 metadata model with a runtime registry.

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

1. `CREATE TABLE` foreign-key and CHECK clauses;
2. `ALTER TABLE ... ADD` column rendering, including column CHECK and FK metadata;
3. reflection-derived expected schema metadata;
4. `pragma_foreign_key_list` introspection for foreign keys;
5. `sqlite_master.sql` parsing for column and table CHECK constraints;
6. schema diff comparison;
7. database-to-C++ entity generation, including FK actions, column checks and named/unnamed table checks.

SQLite has no dedicated CHECK-introspection PRAGMA. MetalORM therefore parses the stored `CREATE TABLE` statement with a small SQL-aware scanner. It splits only top-level table elements and tracks nested parentheses, quoted strings, quoted identifiers, line comments and block comments. Expressions such as function calls containing commas or literals containing parentheses are preserved rather than being split by a regular expression.

Existing SQLite tables cannot alter column or table CHECK constraints in place through the supported synchronization path. A mismatch is therefore reported as an explicit rebuild warning instead of emitting unsafe fake ALTER SQL.

Named foreign-key constraints and deferrability remain separate future extensions; they are not inferred from ORM relationship annotations.
