# PostgreSQL backend

PostgreSQL support is being introduced behind the same `DbExecutor` boundary used by SQLite, without making libpq a mandatory dependency of the core library.

## Current slice

When PostgreSQL development files are available, CMake exposes an optional target:

```cmake
find_package(metal-orm CONFIG REQUIRED)
target_link_libraries(app PRIVATE metal::orm-postgres)
```

The executor API is declared in:

```cpp
#include <metal/postgres_execution.hpp>
```

`PostgresExecutor` currently provides:

- libpq connection management through `PQconnectdb`
- parameterized raw SQL through `PQexecParams`
- `Value` transport for null, integers, doubles, strings, booleans and binary `bytea`
- typed result decoding for booleans, integer types, floating-point types and `bytea`
- affected-row reporting
- transactions
- savepoints with the same identifier validation used by the SQLite executor

The PostgreSQL target is intentionally separate from `metal::orm`; consumers that only use SQLite do not acquire a libpq dependency.

## Not yet parity

This executor slice is not a declaration of full PostgreSQL parity. The following work remains before PostgreSQL can be considered a complete backend:

1. a PostgreSQL query compiler/dialect rather than reusing SQLite-specific function rendering
2. globally correct numbered-placeholder allocation across nested subqueries, CTEs, derived tables and set operations
3. PostgreSQL DDL and schema introspection
4. generated-key integration using `RETURNING` rather than SQLite `last_insert_rowid` semantics
5. PostgreSQL procedure compilation/execution
6. end-to-end integration tests against a real PostgreSQL service

The numbered-placeholder item is deliberately treated as a compiler ownership problem. Rewriting `$1`/`$2` strings after a subquery has already compiled would make parameter identity depend on SQL text and is therefore not an acceptable implementation strategy.
