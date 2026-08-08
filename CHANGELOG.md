# Changelog

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
