#pragma once

#include "metal/execution.hpp"
#include "metal/schema_types.hpp"
#include "metal/sqlite_ddl_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace metal {

namespace schema_detail {

inline std::optional<std::string> row_string(const Row& row, std::string_view key) {
    const auto found = row.find(std::string(key));
    if (found == row.end() || std::holds_alternative<std::nullptr_t>(found->second)) return std::nullopt;
    return from_value<std::string>(found->second);
}

inline std::int64_t row_int(const Row& row, std::string_view key, std::int64_t fallback = 0) {
    const auto found = row.find(std::string(key));
    if (found == row.end() || std::holds_alternative<std::nullptr_t>(found->second)) return fallback;
    return from_value<std::int64_t>(found->second);
}

inline bool contains_name(const std::vector<std::string>& values, std::string_view name) {
    return std::find(values.begin(), values.end(), name) != values.end();
}

inline bool include_table(std::string_view name, const IntrospectOptions& options) {
    if (!options.include_tables.empty() && !contains_name(options.include_tables, name)) return false;
    return !contains_name(options.exclude_tables, name);
}

inline bool include_view(std::string_view name, const IntrospectOptions& options) {
    return !contains_name(options.exclude_views, name);
}

inline std::string uppercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

inline std::optional<std::string> normalize_action(const std::optional<std::string>& action) {
    if (!action) return std::nullopt;
    const auto normalized = uppercase(*action);
    if (normalized == "NO ACTION" || normalized == "RESTRICT" ||
        normalized == "CASCADE" || normalized == "SET NULL" ||
        normalized == "SET DEFAULT") {
        return normalized;
    }
    return std::nullopt;
}

struct SchemaComments {
    std::unordered_map<std::string, std::string> tables;
    std::unordered_map<std::string, std::string> columns;
};

inline SchemaComments load_comments(DbExecutor& executor) {
    SchemaComments result;
    const auto exists = executor.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name='schema_comments' LIMIT 1;");
    if (exists.rows.empty()) return result;

    const auto rows = executor.execute(
        "SELECT object_type, table_name, column_name, comment FROM schema_comments;");
    for (const auto& row : rows.rows) {
        const auto object_type = row_string(row, "object_type");
        const auto table_name = row_string(row, "table_name");
        const auto comment = row_string(row, "comment");
        if (!object_type || !table_name || !comment || comment->empty()) continue;
        const auto kind = uppercase(*object_type);
        if (kind == "TABLE") {
            result.tables[*table_name] = *comment;
        } else if (kind == "COLUMN") {
            const auto column_name = row_string(row, "column_name");
            if (column_name) result.columns[*table_name + "." + *column_name] = *comment;
        }
    }
    return result;
}

inline std::optional<std::string> partial_index_where(std::string_view create_sql) {
    const auto upper = uppercase(std::string(create_sql));
    const auto pos = upper.find(" WHERE ");
    if (pos == std::string::npos) return std::nullopt;
    return std::string(create_sql.substr(pos + 7));
}

} // namespace schema_detail

inline DatabaseSchema introspect_sqlite(
    DbExecutor& executor,
    const IntrospectOptions& options = {}) {
    using namespace schema_detail;

    DatabaseSchema schema;
    const auto comments = load_comments(executor);
    const auto master = executor.execute(
        "SELECT name, sql FROM sqlite_master "
        "WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name;");

    for (const auto& row : master.rows) {
        const auto name = row_string(row, "name");
        if (!name || !include_table(*name, options)) continue;
        const auto create_sql = row_string(row, "sql").value_or("");
        const bool has_autoincrement = uppercase(create_sql).find("AUTOINCREMENT") != std::string::npos;

        DatabaseTable table;
        table.name = *name;
        if (auto found = comments.tables.find(*name); found != comments.tables.end()) {
            table.comment = found->second;
        }

        const auto columns = executor.execute(
            "SELECT cid, name, type, \"notnull\", dflt_value, pk "
            "FROM pragma_table_info(?) ORDER BY cid;",
            {std::string(*name)});
        for (const auto& info : columns.rows) {
            const auto column_name = row_string(info, "name");
            if (!column_name) continue;
            const auto pk_order = row_int(info, "pk");
            DatabaseColumn column;
            column.name = *column_name;
            column.type = row_string(info, "type").value_or("");
            column.not_null = row_int(info, "notnull") == 1 || pk_order > 0;
            column.default_value = row_string(info, "dflt_value");
            column.auto_increment = has_autoincrement && pk_order > 0;
            if (auto found = comments.columns.find(*name + "." + *column_name);
                found != comments.columns.end()) {
                column.comment = found->second;
            }
            table.columns.push_back(std::move(column));
        }

        parse_sqlite_unique_constraints(create_sql, table);
        parse_sqlite_check_constraints(create_sql, table);

        std::vector<std::pair<std::int64_t, std::string>> primary_key;
        for (const auto& info : columns.rows) {
            const auto order = row_int(info, "pk");
            const auto column_name = row_string(info, "name");
            if (order > 0 && column_name) primary_key.emplace_back(order, *column_name);
        }
        std::sort(primary_key.begin(), primary_key.end());
        for (const auto& [_, column] : primary_key) table.primary_key.push_back(column);

        const auto foreign_keys = executor.execute(
            "SELECT id, seq, \"table\", \"from\", \"to\", on_update, on_delete "
            "FROM pragma_foreign_key_list(?) ORDER BY id, seq;",
            {std::string(*name)});
        for (const auto& fk : foreign_keys.rows) {
            const auto from = row_string(fk, "from");
            const auto target_table = row_string(fk, "table");
            const auto target_column = row_string(fk, "to");
            if (!from || !target_table || !target_column) continue;
            auto column = std::find_if(
                table.columns.begin(), table.columns.end(),
                [&](const DatabaseColumn& candidate) { return candidate.name == *from; });
            if (column == table.columns.end()) continue;
            column->references = ForeignKeyReference{
                .table = *target_table,
                .column = *target_column,
                .on_delete = normalize_action(row_string(fk, "on_delete")),
                .on_update = normalize_action(row_string(fk, "on_update"))
            };
        }
        parse_sqlite_foreign_key_modifiers(create_sql, table);

        const auto index_list = executor.execute(
            "SELECT seq, name, \"unique\", origin, partial "
            "FROM pragma_index_list(?) ORDER BY seq;",
            {std::string(*name)});
        for (const auto& idx : index_list.rows) {
            const auto index_name = row_string(idx, "name");
            const auto origin = row_string(idx, "origin").value_or("");
            if (!index_name || origin == "pk" || index_name->starts_with("sqlite_autoindex_")) continue;

            DatabaseIndex index;
            index.name = *index_name;
            index.unique = row_int(idx, "unique") == 1;
            const auto index_columns = executor.execute(
                "SELECT seqno, cid, name FROM pragma_index_info(?) ORDER BY seqno;",
                {std::string(*index_name)});
            for (const auto& info : index_columns.rows) {
                if (const auto column = row_string(info, "name")) {
                    index.columns.push_back(DatabaseIndexColumn{*column});
                }
            }
            if (row_int(idx, "partial") == 1) {
                const auto sql = executor.execute(
                    "SELECT sql FROM sqlite_master WHERE type='index' AND name=? LIMIT 1;",
                    {std::string(*index_name)});
                if (!sql.rows.empty()) {
                    if (const auto definition = row_string(sql.rows.front(), "sql")) {
                        index.where = partial_index_where(*definition);
                    }
                }
            }
            table.indexes.push_back(std::move(index));
        }

        schema.tables.push_back(std::move(table));
    }

    if (options.include_views) {
        const auto views = executor.execute(
            "SELECT name, sql FROM sqlite_master WHERE type='view' ORDER BY name;");
        for (const auto& row : views.rows) {
            const auto name = row_string(row, "name");
            if (!name || !include_view(*name, options)) continue;
            DatabaseView view;
            view.name = *name;
            view.definition = row_string(row, "sql");
            if (auto found = comments.tables.find(*name); found != comments.tables.end()) {
                view.comment = found->second;
            }
            const auto columns = executor.execute(
                "SELECT cid, name, type, \"notnull\", dflt_value, pk "
                "FROM pragma_table_info(?) ORDER BY cid;",
                {std::string(*name)});
            for (const auto& info : columns.rows) {
                const auto column_name = row_string(info, "name");
                if (!column_name) continue;
                DatabaseColumn column;
                column.name = *column_name;
                column.type = row_string(info, "type").value_or("");
                column.not_null = row_int(info, "notnull") == 1;
                if (auto found = comments.columns.find(*name + "." + *column_name);
                    found != comments.columns.end()) {
                    column.comment = found->second;
                }
                view.columns.push_back(std::move(column));
            }
            schema.views.push_back(std::move(view));
        }
    }

    return schema;
}

} // namespace metal
