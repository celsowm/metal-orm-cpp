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

inline std::optional<std::string> normalize_optional_check_expression(
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

inline bool same_reference_definition(
    const ForeignKeyReference& expected,
    const ForeignKeyReference& actual) {
    return expected.table == actual.table &&
           expected.column == actual.column &&
           expected.deferrable == actual.deferrable &&
           expected.schema == actual.schema &&
           normalize_reference_action(expected.on_delete) ==
               normalize_reference_action(actual.on_delete) &&
           normalize_reference_action(expected.on_update) ==
               normalize_reference_action(actual.on_update);
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
        .check_changed = normalize_optional_check_expression(expected.check) !=
            normalize_optional_check_expression(actual.check),
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

inline std::string postgres_column_constraint_name(
    const DatabaseTable& table,
    const DatabaseColumn& column,
    std::string_view suffix,
    const std::optional<std::string>& explicit_name) {
    if (explicit_name) return *explicit_name;
    return table.name + "_" + column.name + "_" + std::string(suffix);
}

inline std::string postgres_drop_constraint(
    const DatabaseTable& table,
    std::string_view constraint,
    const Dialect& dialect) {
    return "ALTER TABLE " + dialect.quote_identifier(table.name) +
           " DROP CONSTRAINT " + dialect.quote_identifier(constraint) + ";";
}

inline std::string postgres_add_unique_constraint(
    const DatabaseTable& table,
    const DatabaseColumn& column,
    const Dialect& dialect) {
    const auto name = postgres_column_constraint_name(
        table, column, "key", column.unique_name);
    return "ALTER TABLE " + dialect.quote_identifier(table.name) +
           " ADD CONSTRAINT " + dialect.quote_identifier(name) +
           " UNIQUE (" + dialect.quote_identifier(column.name) + ");";
}

inline std::string postgres_add_check_constraint(
    const DatabaseTable& table,
    const DatabaseColumn& column,
    const Dialect& dialect) {
    const auto name = postgres_column_constraint_name(
        table, column, "check", std::nullopt);
    return "ALTER TABLE " + dialect.quote_identifier(table.name) +
           " ADD CONSTRAINT " + dialect.quote_identifier(name) +
           " CHECK (" + *column.check + ");";
}

inline std::string postgres_add_reference_constraint(
    const DatabaseTable& table,
    const DatabaseColumn& column,
    const ForeignKeyReference& reference,
    const Dialect& dialect) {
    const auto name = postgres_column_constraint_name(
        table, column, "fkey", reference.name);
    std::string sql = "ALTER TABLE " + dialect.quote_identifier(table.name) +
        " ADD CONSTRAINT " + dialect.quote_identifier(name) +
        " FOREIGN KEY (" + dialect.quote_identifier(column.name) + ")" +
        " REFERENCES " + render_reference_table(reference, dialect) +
        " (" + dialect.quote_identifier(reference.column) + ")";
    if (reference.on_delete) sql += " ON DELETE " + *reference.on_delete;
    if (reference.on_update) sql += " ON UPDATE " + *reference.on_update;
    if (reference.deferrable) sql += " DEFERRABLE INITIALLY DEFERRED";
    sql += ";";
    return sql;
}

inline void diff_postgres_unique_constraint(
    const DatabaseTable& table,
    const DatabaseColumn& expected,
    const DatabaseColumn& actual,
    const ColumnDiff& diff,
    const Dialect& dialect,
    const SchemaDiffOptions& options,
    SchemaPlan& plan) {
    if (!diff.unique_changed && !diff.unique_name_changed) return;

    if (expected.unique && !actual.unique) {
        plan.changes.push_back(SchemaChange{
            .kind = SchemaChangeKind::AlterColumn,
            .table = table.name,
            .description = "Add UNIQUE constraint on " + table.name + "." + expected.name,
            .statements = {postgres_add_unique_constraint(table, expected, dialect)},
            .safe = true
        });
        return;
    }

    const auto actual_name = postgres_column_constraint_name(
        table, actual, "key", actual.unique_name);
    if (!expected.unique && actual.unique) {
        plan.changes.push_back(SchemaChange{
            .kind = SchemaChangeKind::AlterColumn,
            .table = table.name,
            .description = "Drop UNIQUE constraint on " + table.name + "." + expected.name,
            .statements = options.allow_destructive
                ? std::vector<std::string>{postgres_drop_constraint(table, actual_name, dialect)}
                : std::vector<std::string>{},
            .safe = false
        });
        return;
    }

    if (expected.unique && actual.unique) {
        const auto expected_name = postgres_column_constraint_name(
            table, expected, "key", expected.unique_name);
        if (actual_name != expected_name) {
            plan.changes.push_back(SchemaChange{
                .kind = SchemaChangeKind::AlterColumn,
                .table = table.name,
                .description = "Rename UNIQUE constraint on " + table.name + "." + expected.name,
                .statements = {
                    "ALTER TABLE " + dialect.quote_identifier(table.name) +
                    " RENAME CONSTRAINT " + dialect.quote_identifier(actual_name) +
                    " TO " + dialect.quote_identifier(expected_name) + ";"
                },
                .safe = true
            });
        }
    }
}

inline void diff_postgres_check_constraint(
    const DatabaseTable& table,
    const DatabaseColumn& expected,
    const DatabaseColumn& actual,
    const ColumnDiff& diff,
    const Dialect& dialect,
    const SchemaDiffOptions& options,
    SchemaPlan& plan) {
    if (!diff.check_changed) return;
    const auto name = postgres_column_constraint_name(
        table, expected, "check", std::nullopt);

    if (expected.check && !actual.check) {
        plan.changes.push_back(SchemaChange{
            .kind = SchemaChangeKind::AlterColumn,
            .table = table.name,
            .description = "Add CHECK constraint on " + table.name + "." + expected.name,
            .statements = {postgres_add_check_constraint(table, expected, dialect)},
            .safe = true
        });
        return;
    }

    if (!expected.check && actual.check) {
        plan.changes.push_back(SchemaChange{
            .kind = SchemaChangeKind::AlterColumn,
            .table = table.name,
            .description = "Drop CHECK constraint on " + table.name + "." + expected.name,
            .statements = options.allow_destructive
                ? std::vector<std::string>{postgres_drop_constraint(table, name, dialect)}
                : std::vector<std::string>{},
            .safe = false
        });
        return;
    }

    if (expected.check && actual.check) {
        plan.changes.push_back(SchemaChange{
            .kind = SchemaChangeKind::AlterColumn,
            .table = table.name,
            .description = "Replace CHECK constraint on " + table.name + "." + expected.name,
            .statements = {
                postgres_drop_constraint(table, name, dialect),
                postgres_add_check_constraint(table, expected, dialect)
            },
            .safe = true
        });
    }
}

inline void diff_postgres_reference_constraint(
    const DatabaseTable& table,
    const DatabaseColumn& expected,
    const DatabaseColumn& actual,
    const ColumnDiff& diff,
    const Dialect& dialect,
    const SchemaDiffOptions& options,
    SchemaPlan& plan) {
    if (!diff.reference_changed) return;

    if (expected.references && !actual.references) {
        plan.changes.push_back(SchemaChange{
            .kind = SchemaChangeKind::AlterColumn,
            .table = table.name,
            .description = "Add foreign key on " + table.name + "." + expected.name,
            .statements = {
                postgres_add_reference_constraint(
                    table, expected, *expected.references, dialect)
            },
            .safe = true
        });
        return;
    }

    if (!expected.references && actual.references) {
        const auto actual_name = postgres_column_constraint_name(
            table, actual, "fkey", actual.references->name);
        plan.changes.push_back(SchemaChange{
            .kind = SchemaChangeKind::AlterColumn,
            .table = table.name,
            .description = "Drop foreign key on " + table.name + "." + expected.name,
            .statements = options.allow_destructive
                ? std::vector<std::string>{postgres_drop_constraint(table, actual_name, dialect)}
                : std::vector<std::string>{},
            .safe = false
        });
        return;
    }

    if (!expected.references || !actual.references) return;

    const auto actual_name = postgres_column_constraint_name(
        table, actual, "fkey", actual.references->name);
    const auto expected_name = postgres_column_constraint_name(
        table, expected, "fkey", expected.references->name);

    if (same_reference_definition(*expected.references, *actual.references)) {
        if (actual_name != expected_name) {
            plan.changes.push_back(SchemaChange{
                .kind = SchemaChangeKind::AlterColumn,
                .table = table.name,
                .description = "Rename foreign key on " + table.name + "." + expected.name,
                .statements = {
                    "ALTER TABLE " + dialect.quote_identifier(table.name) +
                    " RENAME CONSTRAINT " + dialect.quote_identifier(actual_name) +
                    " TO " + dialect.quote_identifier(expected_name) + ";"
                },
                .safe = true
            });
        }
        return;
    }

    plan.changes.push_back(SchemaChange{
        .kind = SchemaChangeKind::AlterColumn,
        .table = table.name,
        .description = "Replace foreign key on " + table.name + "." + expected.name,
        .statements = {
            postgres_drop_constraint(table, actual_name, dialect),
            postgres_add_reference_constraint(
                table, expected, *expected.references, dialect)
        },
        .safe = true
    });
}

inline std::string postgres_primary_key_constraint_name(const DatabaseTable& table) {
    return table.primary_key_name.value_or(table.name + "_pkey");
}

inline std::string postgres_add_primary_key_constraint(
    const DatabaseTable& table,
    const Dialect& dialect) {
    const auto name = postgres_primary_key_constraint_name(table);
    std::string sql = "ALTER TABLE " + dialect.quote_identifier(table.name) +
        " ADD CONSTRAINT " + dialect.quote_identifier(name) + " PRIMARY KEY (";
    for (std::size_t i = 0; i < table.primary_key.size(); ++i) {
        if (i) sql += ", ";
        sql += dialect.quote_identifier(table.primary_key[i]);
    }
    sql += ");";
    return sql;
}

inline void diff_postgres_primary_key(
    const DatabaseTable& expected,
    const DatabaseTable& actual,
    const Dialect& dialect,
    const SchemaDiffOptions& options,
    SchemaPlan& plan) {
    if (expected.primary_key == actual.primary_key) return;

    if (actual.primary_key.empty()) {
        if (expected.primary_key.empty()) return;
        plan.changes.push_back(SchemaChange{
            .kind = SchemaChangeKind::AlterTable,
            .table = expected.name,
            .description = "Add primary key on " + expected.name,
            .statements = {postgres_add_primary_key_constraint(expected, dialect)},
            .safe = true
        });
        return;
    }

    const auto drop = postgres_drop_constraint(
        actual, postgres_primary_key_constraint_name(actual), dialect);
    if (expected.primary_key.empty()) {
        plan.changes.push_back(SchemaChange{
            .kind = SchemaChangeKind::AlterTable,
            .table = expected.name,
            .description = "Drop primary key on " + expected.name,
            .statements = options.allow_destructive
                ? std::vector<std::string>{drop}
                : std::vector<std::string>{},
            .safe = false
        });
        return;
    }

    plan.changes.push_back(SchemaChange{
        .kind = SchemaChangeKind::AlterTable,
        .table = expected.name,
        .description = "Replace primary key on " + expected.name,
        .statements = options.allow_destructive
            ? std::vector<std::string>{
                drop,
                postgres_add_primary_key_constraint(expected, dialect)}
            : std::vector<std::string>{},
        .safe = false
    });
}

inline std::string postgres_table_check_constraint_name(
    const DatabaseTable& table,
    const DatabaseCheck& check) {
    return check.name.value_or(table.name + "_check");
}

inline std::string postgres_add_table_check_constraint(
    const DatabaseTable& table,
    const DatabaseCheck& check,
    const Dialect& dialect) {
    std::string sql = "ALTER TABLE " + dialect.quote_identifier(table.name) + " ADD ";
    if (check.name) {
        sql += "CONSTRAINT " + dialect.quote_identifier(*check.name) + " ";
    }
    sql += "CHECK (" + check.expression + ");";
    return sql;
}

inline void diff_postgres_table_checks(
    const DatabaseTable& expected,
    const DatabaseTable& actual,
    const Dialect& dialect,
    const SchemaDiffOptions& options,
    SchemaPlan& plan) {
    std::vector<bool> matched(actual.checks.size(), false);

    for (const auto& wanted : expected.checks) {
        const auto wanted_expression = normalize_check_expression(wanted.expression);
        std::optional<std::size_t> same_name;
        std::optional<std::size_t> same_expression;

        for (std::size_t i = 0; i < actual.checks.size(); ++i) {
            if (matched[i]) continue;
            const auto& candidate = actual.checks[i];
            if (wanted.name && candidate.name == wanted.name) {
                same_name = i;
                break;
            }
            if (!same_expression &&
                normalize_check_expression(candidate.expression) == wanted_expression) {
                same_expression = i;
            }
        }

        if (same_name) {
            const auto& candidate = actual.checks[*same_name];
            matched[*same_name] = true;
            if (normalize_check_expression(candidate.expression) == wanted_expression) continue;

            plan.changes.push_back(SchemaChange{
                .kind = SchemaChangeKind::AlterTable,
                .table = expected.name,
                .description = "Replace table CHECK constraint " + *wanted.name +
                    " on " + expected.name,
                .statements = {
                    postgres_drop_constraint(
                        actual, postgres_table_check_constraint_name(actual, candidate), dialect),
                    postgres_add_table_check_constraint(expected, wanted, dialect)
                },
                .safe = true
            });
            continue;
        }

        if (same_expression) {
            const auto& candidate = actual.checks[*same_expression];
            matched[*same_expression] = true;
            if (!wanted.name) continue;

            const auto actual_name = postgres_table_check_constraint_name(actual, candidate);
            if (actual_name != *wanted.name) {
                plan.changes.push_back(SchemaChange{
                    .kind = SchemaChangeKind::AlterTable,
                    .table = expected.name,
                    .description = "Rename table CHECK constraint on " + expected.name,
                    .statements = {
                        "ALTER TABLE " + dialect.quote_identifier(expected.name) +
                        " RENAME CONSTRAINT " + dialect.quote_identifier(actual_name) +
                        " TO " + dialect.quote_identifier(*wanted.name) + ";"
                    },
                    .safe = true
                });
            }
            continue;
        }

        plan.changes.push_back(SchemaChange{
            .kind = SchemaChangeKind::AlterTable,
            .table = expected.name,
            .description = "Add table CHECK constraint on " + expected.name,
            .statements = {postgres_add_table_check_constraint(expected, wanted, dialect)},
            .safe = true
        });
    }

    for (std::size_t i = 0; i < actual.checks.size(); ++i) {
        if (matched[i]) continue;
        const auto& check = actual.checks[i];
        const auto name = postgres_table_check_constraint_name(actual, check);
        plan.changes.push_back(SchemaChange{
            .kind = SchemaChangeKind::AlterTable,
            .table = expected.name,
            .description = "Drop table CHECK constraint " + name + " from " + expected.name,
            .statements = options.allow_destructive
                ? std::vector<std::string>{postgres_drop_constraint(actual, name, dialect)}
                : std::vector<std::string>{},
            .safe = false
        });
    }
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

        diff_postgres_unique_constraint(
            expected_table, column, *actual_column, diff, dialect, options, plan);
        diff_postgres_check_constraint(
            expected_table, column, *actual_column, diff, dialect, options, plan);
        diff_postgres_reference_constraint(
            expected_table, column, *actual_column, diff, dialect, options, plan);
    }

    diff_postgres_primary_key(expected_table, actual_table, dialect, options, plan);
    diff_postgres_table_checks(expected_table, actual_table, dialect, options, plan);

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