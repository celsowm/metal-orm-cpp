# Changelog

All releases currently target GCC 16+ C++26 static reflection and intentionally use SQLite as the only executor/dialect.

## 0.0.12 - 2026-08-08

Relation-query and pagination parity release.

- Added reflection-driven `where_has<^^Relation>()`, `where_has_not<^^Relation>()`, `where_relation<^^Relation>()`, and `match_relation<^^Relation>()`.
- Added reflected correlation for belongs-to, has-one, has-many, N:N, MorphOne, and MorphMany; MorphTo relation filtering is intentionally rejected because its target table is discriminator-dependent.
- Added composable relation filters and callback-configured child queries.
- Added raw row/DTO offset pagination and Session-level tracked pagination with distinct root-PK counting and Identity Map reuse.
- Added cursor pagination with `first`/`after`, `last`/`before`, `limit + 1`, forward/backward traversal, reflected ordering, multi-column keysets, non-null key enforcement, and order-signature validation.
- Added `runtime_pagination.hpp` to keep Session materialization separate from the pure query AST.
- Added SQLite E2E coverage for relation predicates, chained relation filters, raw/tracked paging, forward/backward cursor pages, multi-column ordering, and invalid cursor reuse.
- Fixed cursor comparison direction to follow the TypeScript execution mode: forward pages use the after predicate and backward pages use the before predicate.
- Documented two remaining edges instead of claiming false parity: callback-local relation `LIMIT/OFFSET`, and root-aware page extraction for explicitly joined queries that physically duplicate root rows.

## 0.0.11 - 2026-08-08

Computed expressions and derived tables: modular query AST/compiler, recursive `ScalarTerm`, `from_subquery`, searched CASE, and broad typed SQLite function helpers.

## 0.0.10 - 2026-08-08

Advanced SELECT: BETWEEN/EXISTS, CTEs and recursive CTEs, set operations, and window functions.

## 0.0.9 - 2026-08-08

DML parity: multi-row INSERT, INSERT ... SELECT, RETURNING, SQLite ON CONFLICT, and `excluded()`.

## 0.0.8 - 2026-08-08

Polymorphic relations: MorphTo, MorphOne, MorphMany, typed discriminator targets, lazy/eager hydration where applicable, and cascade semantics.

## 0.0.7 - 2026-08-08

Typed `pivot_patch<Pivot>` and alternate N:N `targetKey` parity.

## 0.0.6 - 2026-08-08

Dedicated has-many and many-to-many collection roles, lazy loading, attach/detach/sync, and typed pivot hydration.

## 0.0.5 - 2026-08-08

Runtime architectural parity: Session coordinator, separate IdentityMap/UoW/RelationChangeProcessor, shared DML AST, and corrected cascade/lifecycle semantics.

## 0.0.4 - 2026-08-08

Typed SQL AST with reflected joins, predicates, projections, aggregates, grouping, and scalar subqueries.

## 0.0.3 - 2026-08-08

Mutable reflected relation collections and transactional relation diffs.

## 0.0.2 - 2026-08-08

Reflection-native relationship metadata and compile-time mapping validation.

## 0.0.1 - 2026-08-08

Initial C++26-native SQLite release using static reflection, annotations, splicing, expansion statements, Session/UoW/Identity Map foundations, and the first typed query AST.
