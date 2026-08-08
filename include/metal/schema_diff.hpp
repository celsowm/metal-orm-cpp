#pragma once

#include "metal/execution.hpp"
#include "metal/schema_introspection.hpp"
#include "metal/schema_types.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace metal {

namespace schema_detail {

inline std::string normalize_schema_type(std::string value) {
    std::string result;
    result.reserve(value.size());
    bool pending_space = false;
    for (unsigned char ch : value) {
        if (std::isspace(ch)) {
            pending_space = !result.empty();
            continue;
        }
        if (pending_space) {
            result.push_back(' ');
            pending_space = false;
        }
        result.push_back(static_cast<char>(std::tolower(ch)));
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

inline std::optional<std::string> normalize_schema_default(
    const std::optional<std::string>& value) {
    if (!value) return std::nullopt;
    auto text = *value;
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string{};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

inline const DatabaseColumn* find_column(const DatabaseTable& table, std::string_view name) {
    const auto found = std::find_if(
        table.columns.begin(), table.columns.end(),
        [&](const DatabaseColumn& column) { return column.name == name; });
    return found == table.columns.end() ? nullptr : &*found;
}

inline const DatabaseIndex* find_index(const DatabaseTable& table, std::string_view name) {
    const auto found = std::find_if(
        table.indexes.begin(), table.indexes.end(),
        [&](const DatabaseIndex& index) { return index.name == name; });
    return found == table.indexes.end() ? nullptr : &*found;
}

inline std::string render_column_definition(const DatabaseColumn& column, const Dialect& dialect) {
    std::string sql = dialect.quote_identifier(column.name) + " " + column.type;
    if (column.not_null) sql += " NOT NULL";
    if (column.default_value) sql += " DEFAULT " + *column.default_value;
    return sql;
}

inline std::string render_index(
    const DatabaseTable& table,
    const DatabaseIndex& index,
    const Dialect& dialect) {
    std::string sql = "CREATE ";
    if (index.unique) sql += "UNIQUE ";
    sql += "INDEX IF NOT EXISTS " + dialect.quote_identifier(index.name) +
           " ON " + dialect.quote_identifier(table.name) + " (";
    for (std::size_t i = 0; i < index.columns.size(); ++i) {
        if (i) sql += ", ";
        sql += dialect.quote_identifier(index.columns[i].column);
    }
    sql += ")";
    if (index.where) sql += " WHERE " + *index.where;
    sql += ";";
    return sql;
}

inline bool same_index(const DatabaseIndex& expected, const DatabaseIndex& actual) {
    if (expected.unique != actual.unique || expected.columns.size() != actual.columns.size()) return false;
    for (std::size_t i = 0; i < expected.columns.size(); ++i) {
        if (expected.columns[i].column != actual.columns[i].column) return false;
    }
    return expected.where == actual.where;
}

} // namespace schema_detail

inline SchemaPlan diff_schema(
    const ExpectedSchema& expected,
    const DatabaseSchema& actual,
    const Dialect& dialect,
    const SchemaDiffOptions& options = {}) {
    using namespace schema_detail;

    SchemaPlan plan;
    std::unordered_map<std::string, const DatabaseTable*> actual_tables;
    for (const auto& table : actual.tables) actual_tables.emplace(table.name, &table);

    for (const auto& wanted : expected.tables) {
        const auto& expected_table = wanted.table;
        const auto actual_it = actual_tables.find(expected_table.name);
        if (actual_it == actual_tables.end()) {
            std::vector<std::string> statements;
            statements.push_back(wanted.create_table_sql);
            statements.insert(
                statements.end(), wanted.create_index_sql.begin(), wanted.create_index_sql.end());
            plan.changes.push_back(SchemaChange{
                .kind = SchemaChangeKind::CreateTable,
                .table = expected_table.name,
                .description = "Create table " + expected_table.name,
                .statements = std::move(statements),
                .safe = true
            });
            continue;
        }

        const auto& actual_table = *actual_it->second;
        for (const auto& column : expected_table.columns) {
            const auto* actual_column = find_column(actual_table, column.name);
            if (!actual_column) {
                plan.changes.push_back(SchemaChange{
                    .kind = SchemaChangeKind::AddColumn,
                    .table = expected_table.name,
                    .description = "Add column " + column.name + " to " + expected_table.name,
                    .statements = {
                        "ALTER TABLE " + dialect.quote_identifier(expected_table.name) +
                        " ADD " + render_column_definition(column, dialect) + ";"
                    },
                    .safe = true
                });
                continue;
            }

            const bool changed =
                normalize_schema_type(column.type) != normalize_schema_type(actual_column->type) ||
                column.not_null != actual_column->not_null ||
                normalize_schema_default(column.default_value) !=
                    normalize_schema_default(actual_column->default_value) ||
                column.auto_increment != actual_column->auto_increment;
            if (changed) {
                plan.warnings.push_back(
                    "SQLite ALTER COLUMN is not supported; rebuild table " +
                    expected_table.name + " to change column " + column.name + ".");
            }
        }

        for (const auto& column : actual_table.columns) {
            if (find_column(expected_table, column.name)) continue;
            plan.changes.push_back(SchemaChange{
                .kind = SchemaChangeKind::DropColumn,
                .table = expected_table.name,
                .description = "Drop column " + column.name + " from " + expected_table.name,
                .statements = {},
                .safe = false
            });
            plan.warnings.push_back(
                "Dropping columns on SQLite requires table rebuild (column " +
                column.name + " on " + expected_table.name + ").");
        }

        for (const auto& index : expected_table.indexes) {
            const auto* actual_index = find_index(actual_table, index.name);
            if (!actual_index) {
                plan.changes.push_back(SchemaChange{
                    .kind = SchemaChangeKind::AddIndex,
                    .table = expected_table.name,
                    .description = "Create index " + index.name + " on " + expected_table.name,
                    .statements = {render_index(expected_table, index, dialect)},
                    .safe = true
                });
            } else if (!same_index(index, *actual_index)) {
                plan.warnings.push_back(
                    "SQLite index " + index.name + " on " + expected_table.name +
                    " differs from the expected definition; drop/recreate it explicitly.");
            }
        }

        for (const auto& index : actual_table.indexes) {
            if (find_index(expected_table, index.name)) continue;
            plan.changes.push_back(SchemaChange{
                .kind = SchemaChangeKind::DropIndex,
                .table = expected_table.name,
                .description = "Drop index " + index.name + " on " + expected_table.name,
                .statements = options.allow_destructive
                    ? std::vector<std::string>{
                        "DROP INDEX IF EXISTS " + dialect.quote_identifier(index.name) + ";"}
                    : std::vector<std::string>{},
                .safe = false
            });
        }
    }

    for (const auto& actual_table : actual.tables) {
        const bool exists = std::any_of(
            expected.tables.begin(), expected.tables.end(),
            [&](const ExpectedTable& table) { return table.table.name == actual_table.name; });
        if (exists) continue;
        plan.changes.push_back(SchemaChange{
            .kind = SchemaChangeKind::DropTable,
            .table = actual_table.name,
            .description = "Drop table " + actual_table.name,
            .statements = options.allow_destructive
                ? std::vector<std::string>{
                    "DROP TABLE IF EXISTS " + dialect.quote_identifier(actual_table.name) + ";"}
                : std::vector<std::string>{},
            .safe = false
        });
    }

    return plan;
}

inline void execute_schema_plan(
    const SchemaPlan& plan,
    DbExecutor& executor,
    const SynchronizeOptions& options = {}) {
    if (options.dry_run) return;
    for (const auto& change : plan.changes) {
        if (!change.safe && !options.allow_destructive) continue;
        for (const auto& statement : change.statements) {
            if (!statement.empty()) executor.execute(statement);
        }
    }
}

inline SchemaPlan synchronize_schema(
    const ExpectedSchema& expected,
    const DatabaseSchema& actual,
    DbExecutor& executor,
    const Dialect& dialect,
    const SynchronizeOptions& options = {}) {
    const auto plan = diff_schema(expected, actual, dialect, options);
    execute_schema_plan(plan, executor, options);
    return plan;
}

inline SchemaPlan synchronize_schema(
    const ExpectedSchema& expected,
    DbExecutor& executor,
    const Dialect& dialect,
    const SynchronizeOptions& options = {},
    const IntrospectOptions& introspect_options = {}) {
    const auto actual = introspect_sqlite(executor, introspect_options);
    return synchronize_schema(expected, actual, executor, dialect, options);
}

} // namespace metal
