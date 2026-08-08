# Changelog

## 0.0.10 - 2026-08-08

Advanced SELECT parity release. SQLite remains the only executor/dialect intentionally.

- Add `BETWEEN` / `NOT BETWEEN` as first-class expression AST nodes that compose with existing logical predicates.
- Add `EXISTS` / `NOT EXISTS` over typed `BasicSelectQuery` subqueries.
- Add `WITH` CTEs with optional column lists and validate CTE column-list arity against the reflected query projection.
- Add `WITH RECURSIVE` and a typed `join_cte<^^Member>(name, column)` bridge for genuine recursive graph/tree queries without raw SQL predicates.
- Add `UNION`, `UNION ALL`, `INTERSECT`, and `EXCEPT` set-operation AST state; reject operands with different projection arity before SQL compilation.
- Apply compound-query `ORDER BY`, `LIMIT`, and `OFFSET` after the set-operation chain, matching the MetalORM TypeScript compiler model.
- Add typed window projection terms for `ROW_NUMBER`, `RANK`, `DENSE_RANK`, `NTILE`, `LAG`, `LEAD`, `FIRST_VALUE`, and `LAST_VALUE`.
- Add composable `partition_by()` and `order_by()` window specifications with reflected fields constrained by the query scope.
- Compile window literal arguments as normal bound SQLite parameters rather than embedding values into SQL.
- Reuse the existing SELECT compiler, parameter vector, reflection-based field references, and projection aliases for CTEs, set operations, subqueries, and windows.
- Add an end-to-end SQLite suite covering BETWEEN, EXISTS, all four set operations, ordinary CTEs, a genuinely recursive parent/child CTE, row numbering, partition/order windows, and LAG state.
- Validate the generated recursive and window SQL independently against SQLite during development.

## 0.0.9 - 2026-08-08

SQLite DML-parity release. SQLite remains the only executor/dialect intentionally.

- Expand `InsertQueryBuilder` from single-row inserts to accumulated multi-row `VALUES` sources.
- Add `INSERT ... SELECT` using the existing typed `BasicSelectQuery` AST as the source instead of embedding raw SQL.
- Reject mixing `VALUES` and `SELECT` insert sources, matching the TypeScript builder state model.
- Add `RETURNING` to INSERT, UPDATE and DELETE, including returned-column aliases.
- Reuse the existing SQLite executor result-row path for DML `RETURNING`; no second execution API is introduced.
- Replace the minimal conflict flag with `on_conflict(columns).do_nothing()` / `.do_update(...)` conflict state.
- Require explicit SQLite conflict-target columns, matching the TypeScript SQLite dialect contract.
- Add `excluded(column)` as a typed DML operand for `DO UPDATE SET target = excluded.target`.
- Restrict `excluded()` compilation to the conflict-update branch so it cannot silently produce invalid normal INSERT/UPDATE SQL.
- Add optional predicates to `DO UPDATE`, compiled after conflict-update assignments.
- Keep Unit of Work and relation mutation on the same shared DML builders after the AST expansion.
- Add an end-to-end in-memory SQLite suite covering multi-row RETURNING, INSERT SELECT, DO UPDATE, DO UPDATE WHERE, DO NOTHING, UPDATE RETURNING, DELETE RETURNING and invalid mixed-source/conflict configurations.

## 0.0.8 - 2026-08-08

Polymorphic-relation parity release. SQLite remains the only executor/dialect intentionally.

- Add C++26-native `morph_to`, `morph_one`, and `morph_many` relation annotations.
- Represent MorphTo discriminator maps at compile time with `morph_target<"type", ^^Target, ^^OptionalKey>` instead of a runtime string-to-table registry.
- Add dedicated `morph_to_reference<T...>`, `morph_one_reference<T>`, and `morph_many_collection<T>` wrappers mirroring the distinct TypeScript runtime contracts.
- Add lazy MorphTo resolution and lazy/eager MorphOne/MorphMany hydration through the normal Session Identity Map.
- Batch MorphOne/MorphMany loading by reflected id/type columns and batch MorphTo loading by discriminator, issuing one query per concrete target type instead of one query per root.
- Add MorphTo target switching and reset semantics, including nullable discriminator/id clearing on the root entity.
- Add MorphOne/MorphMany mutation semantics: attach writes the reflected discriminator/id pair, detach clears it or cascades removal according to the configured mode.
- Integrate polymorphic cascade persist/remove with the existing two-phase Unit of Work / RelationChangeProcessor commit sequence.
- Rebind MorphOne/MorphMany id/type fields after the first UoW flush so a newly generated parent key is persisted correctly when parent and children are all new in the same commit.
- Resolve MorphTo target keys only after cascaded target persistence, so generated target IDs are available before the root discriminator/id pair is written.
- Add `consteval` validation for wrapper shape, field ownership, discriminator uniqueness, target membership, target-key compatibility and mapped target types.
- Add compile-fail coverage for wrong Morph wrappers and duplicate MorphTo discriminators.
- Add an end-to-end SQLite suite covering generated parent keys, lazy/eager MorphOne/MorphMany, cascade removal, MorphTo cascade persist, concrete-target switching, lazy resolution, Identity Map reuse and reset.
- Confirm the TypeScript relation-change processor now honors declared belongs-to-many `targetKey`, closing the temporary cross-repository consistency note introduced during 0.0.7.
- Preserve the TypeScript rule that MorphTo has no single-table JOIN representation; lazy polymorphic resolution remains the parity path for the typed SQL model.

## 0.0.7 - 2026-08-08

Typed pivot-patch and alternate-target-key parity release. SQLite remains the only executor/dialect intentionally.

- Replace full-pivot mutation payloads with `metal::pivot_patch<Pivot>`, the C++26-native equivalent of MetalORM TS `Partial<TPivot>` semantics.
- Address pivot members by reflection with `patch.set<^^Pivot::member>(value)`; reject members from another pivot type at compile time.
- Validate pivot-patch value compatibility at compile time instead of deferring obvious type mismatches to runtime conversion.
- Merge repeated pivot patches by column and update the hydrated pivot object in memory without resetting untouched fields.
- Compile pivot INSERT/UPDATE DML from only the fields explicitly present in the patch, preserving existing database values for omitted members.
- Filter pivot root/target FK columns from generated pivot DML, matching the original `filterPivotPayload` behavior.
- Make N:N attach/detach/sync consistently use the relation's reflected `targetKey`, including when it is not the target primary key.
- Preserve normal Session Identity Map semantics by primary key after alternate-key relation hydration.
- Make cascade remove work for alternate-key attach stubs by deleting the target through the declared relation target key when no tracked entity exists.
- Preserve integral zero as a valid relation key unless the key is specifically a generated primary key.
- Add an end-to-end alternate-target-key suite using `Role::code` while `Role::id` remains the real primary key.
- Add compile-fail coverage for foreign pivot members and incompatible pivot-patch value types.
- Follow the declared `targetKey` relation contract end-to-end; at the time of this release the TypeScript collection/schema already used `targetKey` while its mutation processor still fell back to the target primary key. That TypeScript processor was corrected before 0.0.8.

## 0.0.6 - 2026-08-08

Relation-collection parity release. SQLite remains the only executor/dialect intentionally.

- Replace the generic `metal::collection<T>` surface with dedicated `metal::has_many_collection<T>` and `metal::many_to_many_collection<T, Pivot>` wrappers, matching the distinct collection roles in MetalORM TS.
- Make reflected mapping validation require the correct wrapper shape for each to-many relation and validate that the N:N wrapper's `Pivot` type matches the reflected pivot annotation.
- Add Session-bound lazy `load()` and `get_items()` semantics so a tracked relation can be loaded without `.include<...>()`.
- Add has-many `add()`, `attach()`, `remove()`, and `clear()` semantics; attaching to an already tracked root applies the reflected foreign key immediately.
- Add N:N attach/detach by entity or reflected target-key value and `sync_by_ids()`.
- Bind ID-based N:N operations to reflected target identity and the Session Identity Map when the target key is the primary key.
- Hydrate N:N pivot rows into the real reflected C++ pivot type rather than an untyped side object.
- Accept typed pivot payloads on N:N attach, persist their non-FK fields on pivot INSERT, and update an existing pivot when an already-linked target is reattached with new pivot data.
- Restore MetalORM pivot-insert semantics by using a normal INSERT instead of SQLite `ON CONFLICT DO NOTHING` for relation attach.
- Extend runtime-parity coverage for lazy loading, `get_items()`, attach by ID, `sync_by_ids()`, typed pivot hydration, pivot INSERT, and pivot UPDATE on real in-memory SQLite.
- Keep synchronous `load()` as the C++ adaptation while the only executor is synchronous SQLite.
- Leave partial-field pivot patching as a known remaining sub-gap: this release accepts a full typed `Pivot` payload, whereas MetalORM TS accepts `Partial<TPivot>`.

## 0.0.5 - 2026-08-08

MetalORM architectural-parity release. SQLite remains the only executor/dialect intentionally.

- Extract `IdentityMap`, `UnitOfWork`, runtime tracking types, and `RelationChangeProcessor` from `Session` so the session coordinates the runtime instead of owning every responsibility directly.
- Move concrete has-many and many-to-many mutation processing into `RelationChangeProcessor`, leaving `Session` as the runtime coordinator.
- Add shared `InsertQueryBuilder`, `UpdateQueryBuilder`, and `DeleteQueryBuilder` DML ASTs.
- Route Unit of Work INSERT/UPDATE/DELETE through those DML builders instead of manually concatenating SQL.
- Route has-many FK updates and many-to-many pivot INSERT/DELETE through the same DML AST layer.
- Align commit ordering with MetalORM TS: prepare cascaded persistence, flush the UoW, process relation changes, then flush the UoW again for changes scheduled by relations.
- Add `cascade_mode::link` to match the original MetalORM cascade vocabulary.
- Restore MetalORM many-to-many semantics by allowing `cascade_mode::remove` and `cascade_mode::all` instead of rejecting them at compile time.
- Implement N:N cascade remove by deleting the pivot relation first and scheduling the target for the second UoW flush.
- Align `persist()` lifecycle semantics: entities carrying an existing identity are tracked as Managed rather than blindly inserted.
- Align `remove()` semantics: detached/untracked instances are not implicitly attached just to be deleted.
- Preserve non-generated integral primary key `0` as a valid Identity Map key; zero remains an empty-key sentinel only for generated integral keys.
- Remove the obsolete compile-fail test that treated N:N cascade remove as invalid.
- Add a dedicated runtime-parity test covering DML AST compilation, `link` metadata, N:N cascade-remove execution, managed persist, detached remove, and manual zero-key identity on real in-memory SQLite.

## 0.0.4 - 2026-08-08

Typed SQL AST release. SQLite remains the only executor/dialect intentionally.

- Replace the value-only predicate representation with a real SQL expression AST.
- Track query scope in the C++ query type, so fields from joined entities become valid only after their reflected relation is joined.
- Add reflected `INNER JOIN` and `LEFT JOIN` generation for `belongs_to`, `has_one`, `has_many`, and `many_to_many` relations.
- Generate N:N pivot joins from reflected pivot/table/key metadata with no string relation configuration.
- Add field-to-field comparisons with compile-time compatible-type checking.
- Add `IN` / `NOT IN` for typed ranges, including empty-range semantics.
- Add `IS NULL`, `IS NOT NULL`, `LIKE`, and `NOT LIKE` predicates.
- Add typed projections and projection aliases.
- Add `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`, and `COUNT(*)` aggregate terms.
- Add `GROUP BY` and aggregate-aware `HAVING`.
- Add scalar `IN (subquery)` / `NOT IN (subquery)` with exactly-one-projection validation.
- Add multiple `ORDER BY` terms, `DISTINCT`, `LIMIT`, and `OFFSET` support in the AST.
- Qualify columns only when joins require aliases, keeping simple SQLite queries compact.
- Add a dedicated typed-query test executable covering joins, N:N pivots, aggregates, grouping, nulls, LIKE, IN, subqueries and compile-time query-scope constraints.

## 0.0.3 - 2026-08-08

Mutable reflected relation collections. SQLite remains the only executor/dialect intentionally.

- Add `metal::collection<T>` with `attach`, `detach`, `sync`, `loaded`, `dirty`, iteration and indexed access.
- Replace `std::vector<std::shared_ptr<T>>` as the supported shape for `has_many` and `many_to_many` relations.
- Track relation state as current items versus an accepted baseline, so opposite mutations before a commit naturally cancel.
- Flush relation diffs through the same Unit of Work transaction as entity inserts/updates/deletes.
- Add relation cascade metadata with concise C++26 annotation syntax.
- Support cascade persist for `has_many` and `many_to_many` attached targets.
- Support cascade remove for `has_many` detached targets.
- Reject cascade remove/all for N:N at compile time because targets may be shared by multiple roots.
- Make `has_many` relation flush update reflected foreign keys after generated IDs exist.
- Make N:N relation flush generate SQLite `INSERT OR IGNORE` / `DELETE` against the reflected pivot type.
- Keep collection hydration batched and Identity-Map-aware.
- Add end-to-end coverage for attach/detach/sync, cascaded inserts, cascaded child removal, pivot diffing and reload verification.
- Add a compile-fail test for destructive N:N cascade configuration.

## 0.0.2 - 2026-08-08

Reflection-native relationship release. SQLite remains the only executor/dialect intentionally.

- Replace string-based relationship metadata with `std::meta::info` template arguments.
- Add `Mapped<T>` for reflected tables and reserve `Entity<T>` for mapped types with exactly one primary key.
- Add reflected composite-primary-key DDL, allowing pivot tables to be modeled directly in C++.
- Add `belongs_to`, `has_one`, `has_many`, and redesigned `many_to_many` annotations.
- Make `.include<^^Relation>()` dispatch all four relation kinds at compile time.
- Keep relationship loading batched and integrated with the Identity Map.
- Add `consteval` model validation for relationship member shape, key ownership, key type compatibility, conflicting annotations, generated-key rules, unsupported members, and duplicate mapped column names.
- Remove pivot table/key strings from N:N metadata; table and key names now come from reflected C++ declarations.
- Extend the end-to-end SQLite test graph to cover 1:1, 1:N, N:1, N:N, composite pivot keys, dirty checking, and shared entity identity.

## 0.0.1 - 2026-08-08

Initial C++26-native release.

- Require GCC 16+ with `-std=c++26 -freflection`.
- Use P2996 reflection as the entity introspection mechanism.
- Use P3394 annotations for table, column, primary-key, generated and many-to-many metadata.
- Use expansion statements to generate column traversal at compile time.
- Use splicing for hydration, snapshotting and primary-key access.
- Add typed query expression AST and reflected SELECT generation.
- Add SQLite DDL/executor, Session, Unit of Work, dirty checking and Identity Map.
- Add batched many-to-many hydration with shared target identity.
- Explicitly provide no C++20/23 fallback metadata path.
