# Roadmap

MetalORM C++ follows one rule: new capabilities should deepen the C++26-native design rather than preserve compatibility with earlier metadata mechanisms.

## 0.0.2 — relation model

- `has_one`
- `has_many`
- `belongs_to`
- N:N `attach`, `detach`, `sync`
- typed pivot payloads
- cascade policy annotations
- graph persistence

## 0.0.3 — SQL AST parity foundation

- INSERT/UPDATE/DELETE builders independent of Session
- joins and aliases
- `IN`, `BETWEEN`, `IS NULL`, `EXISTS`
- aggregates and grouping
- subqueries
- CTEs
- expression visitor/strategy surface for dialects

## 0.0.4 — dialects

- PostgreSQL
- MySQL/MariaDB
- SQL Server
- RETURNING/OUTPUT strategies
- dialect-specific placeholders and generated-key behavior

## Later

- schema introspection and diff
- migrations
- hooks/interceptors
- domain events
- cache and pooling
- bulk operations
- polymorphic relations
- DTO/schema generation driven directly from reflection metadata
