# Changelog

All releases currently target GCC 16+ C++26 static reflection and intentionally use SQLite as the only executor/dialect.

## 0.0.13 - 2026-08-08

Relation-correlation and root-pagination hardening release.

- Moved relation correlation into the SELECT compiler's WHERE position instead of wrapping already-compiled root/child queries.
- Made callback-local relation `ORDER BY / LIMIT / OFFSET` run after the reflected correlation predicate, matching TypeScript `applyRelationCorrelation()` ordering.
- Made relation predicates apply before an existing root `LIMIT/OFFSET`.
- Added hierarchical aliases for nested correlated scopes (`t0`, `t0_rel`, `t0_rel_rel`, ...) so child subqueries cannot shadow outer aliases.
- Preserved the established `t0/t1/p0` SQL alias shape for ordinary non-correlated joins.
- Reworked N:N relation correlation as a reflected pivot `EXISTS` predicate inside the child WHERE pipeline.
- Added `without_pagination()` query snapshots and made paging helpers own/replace previous LIMIT/OFFSET state.
- Made Session offset pagination root-aware for explicit row-multiplying joins by deduplicating on the reflected root PK while preserving result order.
- Made Session cursor pagination root-aware before page-size and `hasExtra` evaluation.
- Kept raw row pagination row-oriented while tracked pagination remains root-oriented, matching the two abstraction levels of the reference runtime.
- Hardened cursor keyset mode so `first` always uses after semantics and `last` always uses before semantics, independently of which cursor field carries the token.
- Extended the SQLite E2E suite with root-limit correlation, child OFFSET correlation, pagination override, explicit 1:N JOIN paging, joined cursor paging, and unusual mode/cursor combinations.
- Closed the two explicit semantic edges left by 0.0.12; remaining work returns to runtime parity (transactions/savepoints and rollback-safe state).

## 0.0.12 - 2026-08-08

Relation-query and pagination parity release.

- Added reflection-driven `where_has<^^Relation>()`, `where_has_not<^^Relation>()`, `where_relation<^^Relation>()`, and `match_relation<^^Relation>()`.
- Added reflected correlation for belongs-to, has-one, has-many, N:N, MorphOne, and MorphMany; MorphTo relation filtering is intentionally rejected because its target table is discriminator-dependent.
- Added composable relation filters and callback-configured child queries.
- Added raw row/DTO offset pagination and Session-level tracked pagination with distinct root-PK counting and Identity Map reuse.
- Added cursor pagination with `first`/`after`, `last`/`before`, `limit + 1`, forward/backward traversal, reflected ordering, multi-column keysets, non-null key enforcement, and order-signature validation.
- Added `runtime_pagination.hpp` to keep Session materialization separate from the pure query AST.
- Added SQLite E2E coverage for relation predicates, chained relation filters, raw/tracked paging, forward/backward cursor pages, multi-column ordering, and invalid cursor reuse.
- Documented callback-local relation pagination and explicit-join root extraction as the two hardening edges subsequently closed in 0.0.13.

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
