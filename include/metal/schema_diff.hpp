#pragma once

#include "metal/execution.hpp"
#include "metal/query/core_types.hpp"
#include "metal/schema_introspection_dispatch.hpp"
#include "metal/schema_types.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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

inline std::optional<std::string> normalize_check_expression(
    const std::optional<std::string>& value) {
    return normalize_schema_default(value);
}

inline std::string normalize_check_expression(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

inline std::string normalize_reference_action(const std::optional<std::string>& value) {
    if (!value || value->empty()) return "NO ACTION";
    std::string result = *value;
    std::transform(
        result.begin(), result.end(), result.begin(),
        [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return result;
}

inline bool same_reference(
    const std::optional<ForeignKeyReference>& expected,
    const std::optional<ForeignKeyReference>& actual) {
    if (!expected || !actual) return !expected && !actual;
    return expected->table == actual->table &&
           expected->column == actual->column &&
           expected->name == actual->name &&
           expected->deferrable == actual->deferrable &&
           expected->schema == actual->schema &&
           normalize_reference_action(expected->on_delete) ==
               normalize_reference_action(actual->on_delete) &&
           normalize_reference_action(expected->on_update) ==
               normalize_reference_action(actual->on_update);
}

inline bool same_check(const DatabaseCheck& expected, const DatabaseCheck& actual) {
    return expected.name == actual.name &&
           normalize_check_expression(expected.expression) ==
               normalize_check_expression(actual.expression);
}

inline bool same_checks(
    const std::vector<DatabaseCheck>& expected,
    const std::vector<DatabaseCheck>& actual) {
    if (expected.size() != actual.size()) return false;
    std::vector<bool> matched(actual.size(), false);
    for (const auto& check : expected) {
        bool found = false;
        for (std::size_t i = 0; i < actual.size(); ++i) {
            if (!matched[i] && same_check(check, actual[i])) {
                matched[i] = true;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
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

inline std::string render_reference_table(
    const ForeignKeyReference& reference,
    const Dialect& dialect) {
    std::string sql;
    if (reference.schema) {
        sql += dialect.quote_identifier(*reference.schema) + ".";
    }
    sql += dialect.quote_identifier(reference.table);
    return sql;
}

inline std::string render_reference_definition(
    const ForeignKeyReference& reference,
    const Dialect& dialect) {
    std::string sql;
    if (reference.name) {
        sql += " CONSTRAINT " + dialect.quote_identifier(*reference.name);
    }
    sql += " REFERENCES " + render_reference_table(reference, dialect) +
           " (" + dialect.quote_identifier(reference.column) + ")";
    if (reference.on_delete) sql += " ON DELETE " + *reference.on_delete;
    if (reference.on_update) sql += " ON UPDATE " + *reference.on_update;
    if (reference.deferrable) sql += " DEFERRABLE INITIALLY DEFERRED";
    return sql;
}

inline std::string render_column_definition(const DatabaseColumn& column, const Dialect& dialect) {
    std::string sql = dialect.quote_identifier(column.name) + " " + column.type;
    if (column.not_null) sql += " NOT NULL";
    if (column.unique) {
        if (column.unique_name) {
            sql += " CONSTRAINT " + dialect.quote_identifier(*column.unique_name);
        }
        sql += " UNIQUE";
    }
    if (column.default_value) sql += " DEFAULT " + *column.default_value;
    if (column.check) sql += " CHECK (" + *column.check + ")";
    if (column.references) sql += render_reference_definition(*column.references, dialect);
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

struct ColumnDiff {
    bool type_changed{false};
    bool nullability_changed{false};
    bool unique_changed{false};
    bool unique_name_changed{false};
    bool default_changed{false};
    bool auto_increment_changed{false};
    bool check_changed{false};
    bool reference_changed{false};

    [[nodiscard]] bool any() const noexcept {
        return type_changed || nullability_changed || unique_changed ||
               unique_name_changed || default_changed || auto_increment_changed ||
               check_changed || reference_changed;
    }

    [[nodiscard]] bool postgres_scalar_mutation() const noexcept {
        return type_changed || nullability_changed || default_changed || auto_increment_changed;
    }
};

inline ColumnDiff diff_column(
    const DatabaseColumn& expected,
    const DatabaseColumn& actual) {
    return ColumnDiff{
        .type_changed = normalize_schema_type(expected.type) != normalize_schema_type(actual.type),
        .nullability_changed = expected.not_null != actual.not_null,
        .unique_changed = expected.unique != actual.unique,
        .unique_name_changed = expected.unique_name != actual.unique_name,
        .default_changed = normalize_schema_default(expected.default_value) !=
            normalize_schema_default(actual.default_value),
        .auto_increment_changed = expected.auto_increment != actual.auto_increment,
        .check_changed = normalize_check_expression(expected.check) !=
            normalize_check_expression(actual.check),
        .reference_changed = !same_reference(expected.references, actual.references)
    };
}

inline void diff_sqlite_table(
    const DatabaseTable& expected_table,
    const DatabaseTable& actual_table,
    const Dialect& dialect,
    const SchemaDiffOptions& options,
    SchemaPlan& plan) {
    for (const auto& column : expected_table.columns) {
        const auto* actual_column = find_column(actual_table, column.name);
        if (!actual_column) {
            if (column.unique) {
                plan.changes.push_back(SchemaChange{
                    .kind = SchemaChangeKind::AddColumn,
                    .table = expected_table.name,
                    .description = "Add unique column " + column.name + " to " + expected_table.name,
                    .statements = {},
                    .safe = false
                });
                plan.warnings.push_back(
                    "SQLite ADD COLUMN does not permit UNIQUE constraints; rebuild table " +
                    expected_table.name + " to add unique column " + column.name + ".");
            } else {
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
            }
            continue;
        }

        if (diff_column(column, *actual_column).any()) {
            plan.warnings.push_back(
                "SQLite ALTER COLUMN is not supported; rebuild table " +
                expected_table.name + " to change column " + column.name + ".");
        }
    }

    if (!same_checks(expected_table.checks, actual_table.checks)) {
        plan.warnings.push_back(
            "SQLite table CHECK constraints on " + expected_table.name +
            " differ from the expected definition; rebuild the table to change them.");
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

inline std::vector<std::string> render_postgres_scalar_alterations(
    const DatabaseTable& table,
    const DatabaseColumn& expected,
    const ColumnDiff& diff,
    const Dialect& dialect) {
    std::vector<std::string> statements;
    const auto table_name = dialect.quote_identifier(table.name);
    const auto column_name = dialect.quote_identifier(expected.name);
    const auto prefix = "ALTER TABLE " + table_name + " ALTER COLUMN " + column_name + " ";

    if (diff.type_changed) {
        statements.push_back(prefix + "TYPE " + expected.type + ";");
    }
    if (diff.default_changed) {
        statements.push_back(
            expected.default_value
                ? prefix + "SET DEFAULT " + *expected.default_value + ";"
                : prefix + "DROP DEFAULT;");
    }
    if (diff.nullability_changed) {
        statements.push_back(prefix + (expected.not_null ? "SET" : "DROP") + " NOT NULL;");
    }
    if (diff.auto_increment_changed) {
        statements.push_back(
            expected.auto_increment
                ? prefix + "ADD GENERATED BY DEFAULT AS IDENTITY;"
                : prefix + "DROP IDENTITY IF EXISTS;");
    }
    return statements;
}

inline void diff_postgres_table(
    const DatabaseTable& expected_table,
    const DatabaseTable& actual_table,
    const Dialect& dialect,
    const SchemaDiffOptions& options,
    SchemaPlan& plan) {
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

        const auto diff = diff_column(column, *actual_column);
        if (diff.postgres_scalar_mutation()) {
            auto statements = render_postgres_scalar_alterations(
                expected_table, column, diff, dialect);
            if (!statements.empty()) {
                plan.changes.push_back(SchemaChange{
                    .kind = SchemaChangeKind::AlterColumn,
                    .table = expected_table.name,
                    .description = "Alter column " + column.name + " on " + expected_table.name,
                    .statements = std::move(statements),
                    .safe = true
                });
            }
            if (diff.auto_increment_changed) {
                plan.warnings.push_back(
                    "Altering PostgreSQL identity properties on " + expected_table.name + "." +
                    column.name + " may fail if an existing sequence is attached; verify generated column state.");
            }
        }

        if (diff.unique_changed || diff.unique_name_changed) {
            plan.warnings.push_back(
                "PostgreSQL UNIQUE constraint on " + expected_table.name + "." + column.name +
                " differs from the expected schema; constraint migration is not yet automatic.");
        }
        if (diff.check_changed) {
            plan.warnings.push_back(
                "PostgreSQL CHECK constraint on " + expected_table.name + "." + column.name +
                " differs from the expected schema; constraint migration is not yet automatic.");
        }
        if (diff.reference_changed) {
            plan.warnings.push_back(
                "PostgreSQL foreign key on " + expected_table.name + "." + column.name +
                " differs from the expected schema; constraint migration is not yet automatic.");
        }
    }

    if (!same_checks(expected_table.checks, actual_table.checks)) {
        plan.warnings.push_back(
            "PostgreSQL table CHECK constraints on " + expected_table.name +
            " differ from the expected schema; constraint migration is not yet automatic.");
    }

    for (const auto& column : actual_table.columns) {
        if (find_column(expected_table, column.name)) continue;
        plan.changes.push_back(SchemaChange{
            .kind = SchemaChangeKind::DropColumn,
            .table = expected_table.name,
            .description = "Drop column " + column.name + " from " + expected_table.name,
            .statements = options.allow_destructive
                ? std::vector<std::string>{
                    "ALTER TABLE " + dialect.quote_identifier(expected_table.name) +
                    " DROP COLUMN " + dialect.quote_identifier(column.name) + ";"}
                : std::vector<std::string>{},
            .safe = false
        });
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
            plan.changes.push_back(SchemaChange{
                .kind = SchemaChangeKind::AddIndex,
                .table = expected_table.name,
                .description = "Recreate index " + index.name + " on " + expected_table.name,
                .statements = {
                    "DROP INDEX IF EXISTS " + dialect.quote_identifier(index.name) + ";",
                    render_index(expected_table, index, dialect)
                },
                .safe = true
            });
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

inline void diff_existing_table(
    const DatabaseTable& expected_table,
    const DatabaseTable& actual_table,
    const Dialect& dialect,
    const SchemaDiffOptions& options,
    SchemaPlan& plan) {
    switch (dialect.family()) {
        case DialectFamily::PostgreSQL:
            diff_postgres_table(expected_table, actual_table, dialect, options, plan);
            return;
        case DialectFamily::SQLite:
        case DialectFamily::Generic:
            diff_sqlite_table(expected_table, actual_table, dialect, options, plan);
            return;
    }
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

        diff_existing_table(
            expected_table,
            *actual_it->second,
            dialect,
            options,
            plan);
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
    const auto plan = diff_schema(
        expected,
        actual,
        dialect,
        SchemaDiffOptions{.allow_destructive = options.allow_destructive});
    execute_schema_plan(plan, executor, options);
    return plan;
}

inline SchemaPlan synchronize_schema(
    const ExpectedSchema& expected,
    DbExecutor& executor,
    const Dialect& dialect,
    const SynchronizeOptions& options = {},
    const IntrospectOptions& introspect_options = {}) {
    const auto actual = introspect_schema(executor, dialect, introspect_options);
    return synchronize_schema(expected, actual, executor, dialect, options);
}

} // namespace metal
