# Belongs-to target keys

MetalORM C++ 0.0.35 closes the DB-to-C++ relation-generation gap for foreign keys that reference a non-primary target column.

The TypeScript MetalORM `belongsTo()` contract allows its target-side key (`localKey` in the TypeScript API) to replace the target primary key. C++ already supported the same runtime behavior through the reflected `TargetKey` parameter of `mapping::belongs_to`; 0.0.35 adds a cycle-safe representation for generated code.

## Direct reflected form

When both mapped types are complete and a reflected target member is available, the existing form remains valid:

```cpp
[[=metal::mapping::belongs_to<
    ^^Order::customer_code,
    metal::mapping::cascade_mode::none,
    ^^Customer::code>{}]]
metal::belongs_to_reference<Customer> customer;
```

Omitting the third template argument continues to mean the target primary key. Physical foreign-key metadata does not silently change that ORM meaning.

## Cycle-safe form

Generated entities can contain self references or mutually cyclic references. In those cases `^^Target::member` may not be formable at the point where the source type is declared because `Target` is still incomplete.

Use `belongs_to_key` with the target type reflection and physical target-column name:

```cpp
struct Customer;

struct [[=metal::mapping::table{"orders"}]] Order {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    std::string customer_code;

    [[=metal::mapping::belongs_to_key<
        ^^Order::customer_code,
        ^^Customer,
        "code">{}]]
    metal::belongs_to_reference<Customer> customer;
};
```

`belongs_to_key` resolves `"code"` through C++26 reflection when relation metadata is evaluated, after the target mapped type is complete. Resolution uses the physical column name, including `mapping::column{"..."}` aliases, and requires exactly one matching member. Normal mapping validation then verifies that the resolved target member is persistent and type-compatible with the source foreign key.

## Runtime behavior

Both relation forms feed the same `relation_annotation_traits::target_key()` contract. Therefore alternate target keys automatically participate in the existing relation machinery:

- lazy and eager belongs-to loading;
- Identity Map hydration by the entity primary key;
- reflected JOIN generation;
- `where_has`, `where_relation` and `match_relation` correlation;
- `belongs_to_reference::set()` foreign-key synchronization;
- Unit of Work relation flushing and rollback checkpoints.

There is no alternate runtime path for generated relations.

## DB-to-C++ generation

For an introspected foreign key:

```sql
FOREIGN KEY (customer_code) REFERENCES customers(code)
```

MetalORM now generates a relation wrapper when `customers` has exactly one primary key, even though `code` is the relationship target key:

```cpp
[[=metal::mapping::belongs_to_key<
    ^^Order::customer_code,
    ^^Customer,
    "code">{}]]
metal::belongs_to_reference<Customer> customer;
```

The physical FK remains independently represented by `mapping::reference_to`. ORM relation metadata never replaces or implies the database constraint.

## Composite-primary-key targets

`belongs_to_reference<T>` currently requires `T` to satisfy `reflect::Entity`, which means exactly one primary key. If an introspected FK points to a table with a composite primary key, the generator preserves the physical FK but does not emit a single-reference wrapper. It returns an explicit warning instead of producing invalid C++.

This is separate from alternate target keys: a target with one primary key may use any compatible persistent column as its belongs-to target key.
