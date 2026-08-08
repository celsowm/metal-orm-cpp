#pragma once

#include "metal/query/core_types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace metal {

struct ForeignKeyReference {
    std::string table;
    std::string column;
    std::optional<std::string> on_delete;
    std::optional<std::string> on_update;
};

struct DatabaseColumn {
    std::string name;
    std::string type;
    bool not_null{false};
    std::optional<std::string> default_value;
    bool auto_increment{false};
    std::optional<ForeignKeyReference> references;
    std::optional<std::string> comment;
};

struct DatabaseIndexColumn {
    std::string column;
};

struct DatabaseIndex {
    std::string name;
    std::vector<DatabaseIndexColumn> columns;
    bool unique{false};
    std::optional<std::string> where;
};

struct DatabaseTable {
    std::string name;
    std::vector<DatabaseColumn> columns;
    std::vector<std::string> primary_key;
    std::vector<DatabaseIndex> indexes;
    std::optional<std::string> comment;
};

struct DatabaseView {
    std::string name;
    std::vector<DatabaseColumn> columns;
    std::optional<std::string> definition;
    std::optional<std::string> comment;
};

struct DatabaseSchema {
    std::vector<DatabaseTable> tables;
    std::vector<DatabaseView> views;
};

struct ExpectedTable {
    DatabaseTable table;
    std::string create_table_sql;
    std::vector<std::string> create_index_sql;
};

struct ExpectedSchema {
    std::vector<ExpectedTable> tables;
};

enum class SchemaChangeKind {
    CreateTable,
    DropTable,
    AddColumn,
    DropColumn,
    AlterColumn,
    AddIndex,
    DropIndex
};

struct SchemaChange {
    SchemaChangeKind kind{};
    std::string table;
    std::string description;
    std::vector<std::string> statements;
    bool safe{true};
};

struct SchemaPlan {
    std::vector<SchemaChange> changes;
    std::vector<std::string> warnings;
};

struct IntrospectOptions {
    std::vector<std::string> include_tables;
    std::vector<std::string> exclude_tables;
    bool include_views{false};
    std::vector<std::string> exclude_views;
};

struct SchemaDiffOptions {
    bool allow_destructive{false};
};

struct SynchronizeOptions {
    bool allow_destructive{false};
    bool dry_run{false};
};

} // namespace metal
