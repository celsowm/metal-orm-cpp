# PostgreSQL backend

PostgreSQL support lives behind the same `DbExecutor` and `Dialect` boundaries used by SQLite, without making libpq a mandatory dependency of the core library.

## Build and link

When PostgreSQL development files are available, CMake exposes the optional target:

```cmake
find_package(metal-orm CONFIG REQUIRED)
target_link_libraries(app PRIVATE metal::orm-postgres)
```

The executor API is declared in:

```cpp
#include <metal/postgres_execution.hpp>
```

The PostgreSQL target is intentionally separate from `metal::orm`; consumers that only use SQLite do not acquire a libpq dependency.

## Implemented

`PostgresDialect` and `PostgresExecutor` currently cover:

- ANSI identifier quoting and native `$1`, `$2`, ... placeholders
- globally offset numbered placeholders across nested subqueries, CTEs, derived tables and set operations
- PostgreSQL-specific scalar function rendering
- PostgreSQL DDL types, generated identity columns and `RETURNING` generated-key retrieval
- parameterized SQL through `PQexecParams`
- `Value` transport for null, integers, doubles, strings, booleans and binary `bytea`
- typed result decoding for booleans, integer types, floating-point types and `bytea`
- affected-row reporting
- transactions and savepoints
- PostgreSQL schema introspection
- stored procedure compilation with `CALL`, including IN, OUT and INOUT parameters
- stored procedure execution through the common `ProcedureExecutor` capability
- live PostgreSQL E2E coverage in CI

For PostgreSQL procedures, OUT parameters occupy their PostgreSQL-required argument position as `NULL`; IN and INOUT values keep normal numbered parameter binding. PostgreSQL's returned OUT/INOUT row is exposed through the existing `ProcedureExecutionResult::out` API.

## Remaining backend-parity work

PostgreSQL is now a real executable backend, but it is not yet declared fully equivalent to the SQLite coverage matrix. Remaining work is primarily breadth rather than the original backend bootstrap:

1. exercise more schema-diff/synchronization operations against a live PostgreSQL service
2. expand PostgreSQL-specific DDL/introspection edge coverage (constraints, views, indexes and destructive migrations)
3. broaden live query/function coverage beyond the current representative E2E
4. validate procedure overload/type-resolution edge cases and richer PostgreSQL procedure signatures
5. audit SQLite-specific assumptions in higher-level bulk/tree/cache paths under PostgreSQL

Backend-specific behavior should continue to be implemented at compiler/executor/schema boundaries rather than by rewriting generated SQL strings after compilation.
