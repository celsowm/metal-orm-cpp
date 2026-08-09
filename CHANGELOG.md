# Changelog

All releases currently target GCC 16+ C++26 static reflection and intentionally use SQLite as the only executor/dialect.

## 0.0.25 - 2026-08-09

Query-cache core parity release.

- Added segregated `CacheReader`, `CacheWriter`, `CacheInvalidator` and `CacheProvider` contracts plus optional tag registration, clear and statistics capabilities.
- Added cache capability discovery for tags, prefix invalidation and TTL.
- Added human-readable cache durations (`s`, `m`, `h`, `d`, `w`) plus millisecond values, validation and formatting.
- Added a thread-safe bidirectional `TagIndex` for tag-to-key and key-to-tag membership.
- Added a thread-safe `MemoryCacheAdapter` with lazy TTL expiry, tags, prefix invalidation, clear, statistics and disposal.
- Added `CacheStrategy` / `DefaultCacheStrategy` with conditional caching and the TypeScript-compatible `tenant:<tenantId>:<queryKey>` key shape.
- Added `QueryCacheManager` with execute-around hit/miss behavior, default TTL, conditional storage, key/tag/prefix invalidation, clear, stats and disposal.
- Added generic `CachedQuery`, `cache_query()` and concise `cache()` wrappers without injecting cache state into the SELECT AST.
- Added `CacheSession` composition for Session + cache manager + optional tenant context and tenant-aware invalidation helpers.
- Cache entity results as typed `QueryResult` rows and re-hydrate through Session/Identity Map, preventing cache hits from creating detached duplicate entity identities.
- Added support for both ordinary `SelectQuery` and correlated `RelationFilteredQuery` caching through the same execution wrapper.
- Added real SQLite coverage with a counting executor proving cache hits skip SELECT execution, stale values persist until explicit invalidation, tag invalidation refreshes rows, tenant keys isolate entries and relation queries are cacheable.
- Preserved `auto_invalidate` as configuration without inventing runtime behavior that the current TypeScript cache manager does not implement.
- Marked cache 🟡 only at the adapter/ecosystem boundary: the core and memory provider are implemented, while first-party Keyv/ioredis equivalents await a deliberate C++ remote-cache dependency choice.

## 0.0.24 - 2026-08-09

Shared reflected database defaults and DTO/OpenAPI parity closure.

- Added C++26 annotation metadata for typed numeric/boolean defaults, quoted text defaults, explicit NULL defaults and raw SQL default expressions.
- Added compile-time validation for duplicate defaults and type/default mismatches, including nullable-only `default_null`.
- Added one shared reflection API (`has_column_default`, `column_default_sql`, literal extraction and entity-wide validation) instead of DDL- or API-specific metadata.
- Wired reflected defaults into SQLite `CREATE TABLE` generation with SQL string escaping and raw-expression preservation.
- Added reflected defaults to `expected_table()` / `DatabaseColumn::default_value`, allowing schema introspection/diff to compare declared defaults.
- Added a real SQLite E2E proving literal text, `0`, `false`, floating, `CURRENT_TIMESTAMP` and NULL defaults are applied when INSERT omits the fields.
- Added E2E convergence for `expected -> create -> introspect -> diff` with reflected defaults.
- Added `DtoField::has_default` and made create DTO requiredness depend on nullability plus default presence.
- Made create OpenAPI requiredness use the same reflected default metadata; falsy defaults such as `0` and `false` are correctly optional rather than tested by truthiness.
- Added compile-fail coverage for multiple default annotations on one column.
- Marked DTO/OpenAPI ✅ for the supported SQLite execution model.
- Split the remaining schema metadata gap: reflected defaults are now ✅, while physical FK/check declaration annotations remain future schema-layer work.

## 0.0.23 - 2026-08-09

Relation-aware DTO/REST/OpenAPI expansion.

- Added recursive `WhereInput` relation filters with `some`, `every`, `none`, `isEmpty` and `isNotEmpty` operators.
- Added reflection-native `DtoRelationPolicy<^^T::relation...>` allowlists with compile-time owner/relation validation and runtime rejection of unknown/disallowed API relation names.
- Reused the existing correlated relation-query compiler for belongsTo, hasOne, hasMany, N:N, morphOne and morphMany instead of adding a REST-specific SQL compiler.
- Added recursive relation predicates, including multi-level REST filters such as user -> posts -> comments.
- Matched the TypeScript runtime's non-vacuous `every()` behavior: the relation must be non-empty and no related row may fail the predicate.
- Kept morphTo relation filtering explicitly unsupported because the target table is discriminator-dependent, matching the existing relation-query limitation.
- Added a relation-aware `execute_filtered_paged()` overload that combines recursive filters, reflected safe sorting, existing root-aware Session pagination and enhanced page metadata.
- Added nested relation DTO OpenAPI generation with single-object and collection shapes, bounded compile-time recursion and explicit morphTo treatment.
- Added recursive relation-filter OpenAPI schemas using the same `some/every/none/isEmpty/isNotEmpty` contract as runtime filtering.
- Added nested single-relation update schemas and relation component maps.
- Added OpenAPI component utilities for deep cloning, canonical structural hashing, deterministic names, `$ref` creation/replacement, reusable-schema extraction and heterogeneous reflected component generation.
- Added real SQLite coverage for 1:N, N:N, recursive relation filtering, non-vacuous `every`, relation allowlists and relation-aware paged execution.
- Added OpenAPI coverage for nested DTOs, recursive relation filters, update-with-relations, component maps, deterministic naming, deduplication, deep cloning and `$ref` replacement.
- Kept DTO/OpenAPI 🟡 only for default-aware create requiredness: reflected database-default metadata still belongs in the shared mapping/DDL layer rather than an API-only declaration.

## 0.0.22 - 2026-08-09

Reflection-native DTO/REST/OpenAPI baseline.

- Added response/create/update DTO descriptors derived from C++26 reflection instead of duplicated API metadata.
- Kept public API member names distinct from physical SQL column names, including explicit coverage for a `displayName` member mapped to `display_name`.
- Added reflected DTO transforms for response projection, generated-field exclusion, member picking, defaults, exclusion and field mapping.
- Added enhanced `PagedResponse` metadata on top of the existing Session/root-aware pagination runtime.
- Added reflection-allowlisted scalar REST filters with equality, IN/NOT IN, numeric ordering, string contains/starts/ends, null checks and case-insensitive string matching through the shared SELECT AST.
- Added operator/type validation so boolean, numeric and string REST filter surfaces match the TypeScript contract instead of accepting arbitrary SQL comparisons.
- Added reflection-allowlisted dynamic sorting with public-name resolution and reflected primary-key tie-breaking for deterministic pagination.
- Added `execute_filtered_paged()` to compose REST filtering, safe sorting, `execute_paged()` and enhanced page metadata without a second executor.
- Added framework-independent OpenAPI 3.0/3.1 schema models plus response/create/update DTO schema generation.
- Added OpenAPI REST filter schemas driven by the same reflected allowlists used by runtime filtering.
- Added pagination parameter/response schemas and route-document structures.
- Added reflected Tree/MPTT OpenAPI schemas for node results, threaded nodes, flat tree-list entries and reusable components.
- Added real SQLite coverage for filters, physical/public-name resolution, sorting and filtered paged execution, plus dedicated DTO/OpenAPI and Tree-schema suites.
- Added compile-fail coverage rejecting DTO policies that reference a member from another entity.
- Marked DTO/OpenAPI 🟡: relation-aware REST filters and nested relation/component OpenAPI remain, and create-time default-aware requiredness waits for shared reflected default metadata in the mapping/DDL layer.

## 0.0.21 - 2026-08-09

Tree/MPTT parity closure and reusable scalar arithmetic.

- Added first-class scalar arithmetic to the shared SELECT AST with `+`, `-`, `*`, `/` and integral `%` operations.
- Added typed arithmetic result promotion with `std::optional` propagation and compile-time rejection of boolean arithmetic.
- Added SQLite compilation for nested arithmetic expressions while preserving lexical parameter order.
- Added real SQLite coverage for arithmetic in projections and predicates.
- Added `TreeQuery<T>::find_leaves()` as a normal typed query using `(rght - lft) = 1`; `TreeManager::get_leaves()` no longer emits a special raw SELECT.
- Added width-4 subtree-move regression coverage so the negative temporary-range isolation is proven for real subtrees rather than only leaves.
- Added `TreeManager<T>::remove_from_tree()` with child promotion, descendant boundary/depth compaction, and retained-node detachment as a valid root.
- Added dedicated SQLite coverage for `remove_from_tree()` with exact MPTT bounds, parent/depth promotion, retained root placement and cross-scope isolation.
- Registered the remove-from-tree E2E in the CMake test suite and bumped the C++ release to 0.0.21.
- Closed the Tree/MPTT parity row for the supported SQLite execution model; the next parity pass moves to DTO/OpenAPI and the remaining tooling/runtime gaps.

## 0.0.20 - 2026-08-09

Tree/MPTT parity baseline.

- Added reflection-native tree mapping annotations: `tree_parent`, `tree_left`, `tree_right`, optional `tree_depth`, and repeatable `tree_scope` members.
- Added `validate_tree_mapping<T>()` with compile-time checks for required columns, nullable parent keys, PK compatibility and integral nested-set boundaries/depth.
- Added pure `NestedSetStrategy` helpers for descendant counts, leaf/root detection, ancestor/descendant checks, subtree width, insertion positions, recovery, threading and structural validation.
- Added reflected `TreeQuery<T>` for ancestors, descendants, direct children, parent lookup, siblings, roots, subtree/tree-list queries, depth lookup, ID lookup and typed multi-tree scope values.
- Added Session-bound `TreeManager<T>` for node/root/child/descendant/path/sibling/parent reads, threaded descendants, leaf discovery, depth calculation, root/child insertion, sibling movement, subtree movement/deletion, recovery and validation.
- Reused the existing SELECT/DML AST wherever it can express the operation; tree boundary shifts remain explicit arithmetic UPDATEs instead of introducing a second query compiler.
- Applied scope predicates to raw boundary-changing mutations, hardening the current TypeScript edge where scoped trees can otherwise rewrite another scope's `lft/rght` values.
- Hardened subtree moves by isolating the moving range below zero before closing/opening gaps; a positive temporary range can be shifted by the destination-gap update and corrupt the restore delta.
- Corrected adjacent sibling swapping so both `move_up` and `move_down` use one symmetric subtree-swap algorithm instead of routing downward movement through an upward-only calculation.
- Added cycle protection against moving a node beneath itself or one of its descendants.
- Added a real SQLite E2E suite covering scoped insertion, roots/children/descendants/path/depth/leaves/threading, move up/down/to, subtree deletion, recovery, validation and scope isolation.
- Added compile-fail coverage rejecting non-nullable tree parent columns.
- Left two explicit Tree edges for the next pass instead of copying questionable behavior: first-class arithmetic scalar AST support for `TreeQuery::find_leaves()`, and clarification/fix of the TypeScript `removeFromTree()` implementation that currently leaves the removed row with stale overlapping boundaries.

## 0.0.19 - 2026-08-08

Bulk-operation parity release.

- Added `bulk_insert`, `bulk_update`, `bulk_update_where`, `bulk_delete`, `bulk_delete_where` and `bulk_upsert` for the SQLite execution model.
- Preserved the TypeScript reference strategies instead of inventing different SQL: multi-row INSERT/UPSERT per chunk, individual identity-aware UPDATEs for `bulk_update`, and `IN (...)` chunks for update/delete-by-ID operations.
- Added reflection-native `bulk_row<T>().set<^^T::member>(value)` payload construction with compile-time owner/member/value validation.
- Added reflected `bulk_columns<^^T::member...>()`, `bulk_all_columns<T>()` and `bulk_no_columns()` selections for `by`, conflict, update and RETURNING columns where the TypeScript API uses strings/column objects.
- Added default chunk size 500, bounded worker concurrency, transactional execution by default, non-transactional partial progress, chunk timings and completion callbacks.
- Added `BulkResult`/metadata with processed-row count, chunks executed, RETURNING rows and optional timings.
- Extended the shared UPDATE/DELETE DML AST with `IN` predicates and reusable typed `Expression<T>` filters so the bulk subsystem does not introduce a second SQL compiler.
- Reused `Session::transaction()` for rollback semantics across chunks; a later chunk failure rolls back earlier chunks when transactional execution is enabled.
- Serialized access to the single SQLite connection inside `SQLiteExecutor`, allowing bounded bulk workers without racing the underlying handle.
- Added a dedicated SQLite E2E suite covering insert, update, update-where, delete, delete-where, upsert/update, upsert/do-nothing, RETURNING, callbacks/timing, concurrency, transaction rollback, non-transactional partial progress and invalid chunk sizes.

## 0.0.18 - 2026-08-08

SQLite schema introspection/diff/synchronization release.

- Added dialect-independent `DatabaseSchema`/table/column/index/view metadata plus expected-schema and schema-plan models.
- Added `introspect_sqlite()` with include/exclude table filters, optional views, ordered primary keys, column metadata/defaults, physical FK metadata/actions, user indexes and optional `schema_comments` comments.
- Normalize SQLite primary-key nullability and detect AUTOINCREMENT from `sqlite_master` DDL so a schema created by MetalORM does not immediately self-diff because of PRAGMA reporting quirks.
- Added reflection-derived `expected_schema<T...>()` and typed `add_expected_index<T, ^^members...>()` so expected index columns remain compile-time entity members instead of free-form column strings.
- Kept ORM relation metadata separate from physical DDL FK declarations, matching the TypeScript separation between relations and column `references`.
- Added `diff_schema()` with safe CREATE TABLE / ADD COLUMN / CREATE INDEX planning and destructive DROP TABLE / DROP INDEX gating.
- Match the SQLite TypeScript dialect policy for unsupported structural changes: ALTER COLUMN and DROP COLUMN produce explicit rebuild warnings rather than fake SQL.
- Added `execute_schema_plan()` and `synchronize_schema()` with `allow_destructive` and `dry_run` controls.
- Added SQLite E2E coverage for introspection of PK/FK/index/view/comments, dry-run, safe synchronization, destructive gating, alter/drop-column warnings, and convergence of `expected -> synchronize -> introspect -> diff` to an empty plan.
- Selected the existing TypeScript bulk subsystem as the next concrete parity target rather than inventing a migration-history framework that the reference does not expose as a distinct subsystem.

## 0.0.17 - 2026-08-08

Dedicated single-reference parity release.

- Made `belongs_to_reference<T>` and `has_one_reference<T>` the only valid reflected shapes for `belongsTo` and `hasOne` mappings.
- Removed raw `std::shared_ptr<T>` relation compatibility from reflection validation instead of retaining a legacy fallback.
- Added Session-bound lazy loaders for both single-reference wrappers and made eager/lazy hydration establish a clean wrapper baseline through `_metal_hydrate()`.
- Added generic `set()` / `reset()` mutation tracking with baseline acceptance after successful commit.
- Synchronize `belongsTo` root foreign keys from the reflected target key; newly generated relation keys are resolved after the first UoW flush and persisted by the second flush.
- Added `hasOne` attach/detach processing through `RelationChangeProcessor`, including nullable child FK detach and cascade-remove behavior.
- Added cascade-persist preparation for newly attached single-reference targets where configured.
- Included single-reference baselines in the existing rollback-safe reflected runtime checkpoint path, so failed transactions restore both current pointer and dirty baseline.
- Migrated the foundational runtime test models away from raw `shared_ptr` relations.
- Added SQLite E2E coverage for lazy belongs-to/has-one loading, Identity Map reuse, set/reset, generated IDs, replacement/detach, cascade persist and transaction rollback.
- Added compile-fail coverage proving raw `shared_ptr` is rejected separately for both belongs-to and has-one mappings.

## 0.0.16 - 2026-08-08

Typed graph-persistence release.

- Added reflection-driven `graph<T>()` payloads as the C++ binding of the TypeScript DTO graph contract.
- Added compile-time checked scalar assignment through `.set<^^T::member>(value)`; foreign members, relation members and incompatible scalar types are rejected before SQL generation.
- Added nested graph payloads for single references and callback-configured collection payloads.
- Added graph collection inputs for nested entities, existing `shared_ptr` targets and relation IDs.
- Reused typed `pivot_patch<Pivot>` for N:N graph entries instead of introducing a second pivot payload representation.
- Added `save_graph`, `update_graph` and `patch_graph`; update/patch require a reflected root PK and return an empty pointer when the root does not exist.
- Added `GraphOptions::prune_missing` for has-many/morph-many removal and N:N detach semantics.
- Made graph operations transactional by default through the existing Session transaction/checkpoint pipeline, preserving rollback of generated IDs, relation state and domain-event queues.
- Added dedicated `belongs_to_reference<T>` and `has_one_reference<T>` wrappers as the future canonical single-reference shapes; their generic runtime integration was completed in 0.0.17.
- Added SQLite E2E coverage for root + has-one + has-many + N:N/pivot creation, generated keys, hooks/events, pruning, partial patch behavior, nested belongs-to creation and missing-root updates.
- Added compile-fail coverage for incompatible reflected graph scalar values.
- Updated lifecycle parity after the TypeScript reference moved table hooks to Session-bound registration as well; the old TableDef-vs-Session divergence no longer exists.

## 0.0.15 - 2026-08-08

Lifecycle hooks, Session interceptors and domain-event parity release.

- Added typed `TableHooks<T>` with `before/after` INSERT, UPDATE and DELETE lifecycle callbacks wired through the Unit of Work.
- Preserved the TypeScript hook ordering, including dirty-diff-before-`beforeUpdate` behavior and `afterDelete` after tracking removal while retaining the entity alive for the callback.
- Added `SessionInterceptor` with `before_flush` / `after_flush` around relation prepare, both UoW flushes and relation processing.
- Kept raw `Session::flush()` UoW-only: table hooks run, while Session interceptors, relation processing and event dispatch remain commit/transaction concerns.
- Added typed `domain_event_queue<Events...>` and `DomainEventBus`, with compile-time event membership and handler-signature checks and internal type erasure only.
- Added reflected discovery of ignored domain-event queue members on tracked entities and transaction-checkpoint restoration of queued events.
- Dispatch domain events only after the successful outermost database COMMIT; nested SAVEPOINT releases never dispatch events.
- Roll back events raised inside failed transactions/savepoints together with scalar/relation runtime state so stale events cannot leak into a later commit.
- Treat event-handler exceptions as post-commit failures: the exception propagates, but MetalORM does not pretend an already successful database COMMIT was rolled back.
- Match the TS bus clearing rule: an entity event queue is cleared only after all handlers complete; a handler failure leaves the queue intact for caller-defined recovery/retry policy.
- Added E2E coverage for hook/interceptor ordering, raw flush boundaries, nested event timing, rollback of events, hook failure, `afterFlush` failure, DELETE lifecycle, and post-commit handler failure.
- Hook registration is Session-bound in C++; the TypeScript reference was subsequently aligned to the same ownership model on 2026-08-08.

## 0.0.14 - 2026-08-08

Nested-transaction and rollback-safe runtime parity release.

- Added transaction/savepoint capabilities to `DbExecutor`; SQLite advertises transactions and savepoints explicitly instead of requiring Session to concatenate transaction SQL.
- Added validated SQLite `BEGIN`, `COMMIT`, `ROLLBACK`, `SAVEPOINT`, `RELEASE SAVEPOINT`, and `ROLLBACK TO SAVEPOINT` operations.
- Added `Session::transaction(fn)` with transaction depth, deterministic `metalorm_sp_N` savepoints, nested release, and rollback-to-savepoint behavior.
- Added rollback-only semantics: a failed inner transaction poisons the outer scope even when the inner exception is caught, matching the TypeScript runtime contract.
- Reject nested transactions on executors without savepoint capability instead of issuing a second `BEGIN`.
- Added nested UnitOfWork checkpoints that retain status, original dirty snapshot, current reflected scalar values, and reflected relation-wrapper state.
- Restore UPDATE state after rollback, resurrect DELETE-tracked entities, rebuild the Identity Map, and remove entities introduced only inside a rolled-back scope.
- Reset generated primary keys to their pre-transaction values when an INSERT is rolled back.
- Preserve an outer checkpoint after a successful inner savepoint so a later outer rollback can undo inner inserts and accepted relation baselines.
- Restore relation wrapper state through reflection-generated restore closures instead of resetting only SQL state.
- Made `commit()` transactional through executor capabilities and checkpoint restoration; a failed database COMMIT restores ORM state as well as rolling back SQLite.
- Added a dedicated SQLite E2E suite covering rollback-safe UPDATE/DELETE/INSERT, generated IDs, nested success, rollback-only nested failure, relation state across released savepoints, commit failure, and missing-savepoint capability.

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
- Closed the two explicit semantic edges left by 0.0.12; remaining work returns to runtime parity.

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
