# Physical schema constraints

MetalORM C++ keeps ORM relationships and physical database constraints separate.

A `belongs_to`, `has_one`, `has_many` or other relation annotation describes ORM behavior. It does **not** silently create a database foreign key. Physical foreign keys are declared explicitly on persistent scalar members.

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

`mapping::reference<TargetColumn, OnDelete, OnUpdate>` is reflection-native. The referenced member must belong to a mapped type, both sides must be persistent scalar columns, and their underlying value types must be compatible. Invalid mappings fail at compile time.

Supported referential actions are:

- `unspecified`
- `no_action`
- `restrict`
- `cascade`
- `set_null`
- `set_default`

For SQLite, the same explicit metadata drives:

1. `CREATE TABLE` foreign-key clauses;
2. `ALTER TABLE ... ADD` column rendering;
3. reflection-derived expected schema metadata;
4. `pragma_foreign_key_list` introspection;
5. schema diff comparison.

The diff treats an omitted action and SQLite's introspected `NO ACTION` as semantically equivalent, avoiding false rebuild warnings.

Named foreign-key constraints, deferrability and CHECK constraints are intentionally not claimed as complete yet. CHECK introspection requires parsing SQLite table DDL because SQLite does not expose CHECK constraints through a dedicated PRAGMA; that is the next schema-constraint parity pass.
