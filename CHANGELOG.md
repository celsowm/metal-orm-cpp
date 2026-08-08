# Changelog

## 0.0.3 - 2026-08-08

Mutable reflected relation collections. SQLite remains the only executor/dialect intentionally.

- Add `metal::collection<T>` with `attach`, `detach`, `sync`, `loaded`, `dirty`, iteration and indexed access.
- Replace `std::vector<std::shared_ptr<T>>` as the supported shape for `has_many` and `many_to_many` relations.
- Track relation state as current items versus an accepted baseline, so opposite mutations before `commit()` cancel naturally.
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
