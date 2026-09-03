#pragma once

#include "metal/schema_introspection.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace metal {

namespace postgres_schema_detail {

inline bool row_bool(const Row& row, std::string_view key, bool fallback = false) {
    const auto found = row.find(std::string(key));
    if (found == row.end() || std::holds_alternative<std::nullptr_t>(found->second)) {
        return fallback;
    }
    return from_value<bool>(found->second);
}

inline std::optional<std::string> trim_nonempty(std::optional<std::string> value) {
    if (!value) return std::nullopt;
    const auto first = value->find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::nullopt;
    const auto last = value->find_last_not_of(" \t\r\n");
    return value->substr(first, last - first + 1);
}

inline std::optional<std::string> implicit_constraint_name(
    std::string_view table,
    std::string_view column,
    const std::optional<std::string>& name,
    std::string_view suffix) {
    if (!name) return std::nullopt;
    if (*name == std::string(table) + "_" + std::string(column) + "_" + std::string(suffix)) {
        return std::nullopt;
    }
    return name;
}

inline std::optional<std::string> implicit_table_constraint_name(
    std::string_view table,
    const std::optional<std::string>& name,
    std::string_view suffix) {
    if (!name) return std::nullopt;
    if (*name == std::string(table) + "_" + std::string(suffix)) return std::nullopt;
    return name;
}

inline DatabaseColumn* find_column(DatabaseTable& table, std::string_view name) {
    const auto found = std::find_if(
        table.columns.begin(), table.columns.end(),
        [&](const DatabaseColumn& column) { return column.name == name; });
    return found == table.columns.end() ? nullptr : &*found;
}

inline std::optional<std::string> scoped_reference_schema(
    std::string_view current_schema,
    std::string_view target_schema) {
    if (current_schema == target_schema) return std::nullopt;
    return std::string(target_schema);
}

struct IndexAccumulator {
    DatabaseIndex index;
    bool representable{true};
};

} // namespace postgres_schema_detail

inline DatabaseSchema introspect_postgres(
    DbExecutor& executor,
    const IntrospectOptions& options = {}) {
    using namespace postgres_schema_detail;
    using schema_detail::include_table;
    using schema_detail::include_view;
    using schema_detail::normalize_action;
    using schema_detail::row_int;
    using schema_detail::row_string;

    const std::string schema_name = options.schema.value_or("public");
    const std::vector<Value> schema_param{schema_name};

    DatabaseSchema schema;
    std::unordered_map<std::string, std::size_t> table_positions;

    const auto column_rows = executor.execute(
        R"SQL(
SELECT
    cls.relname AS table_name,
    att.attname AS column_name,
    pg_catalog.format_type(att.atttypid, att.atttypmod) AS data_type,
    att.attnotnull AS not_null,
    pg_catalog.pg_get_expr(def.adbin, def.adrelid) AS column_default,
    att.attnum AS ordinal_position,
    (att.attidentity <> '') AS is_identity,
    pg_catalog.col_description(cls.oid, att.attnum) AS comment
FROM pg_catalog.pg_attribute AS att
JOIN pg_catalog.pg_class AS cls ON cls.oid = att.attrelid
JOIN pg_catalog.pg_namespace AS ns ON ns.oid = cls.relnamespace
LEFT JOIN pg_catalog.pg_attrdef AS def
    ON def.adrelid = att.attrelid AND def.adnum = att.attnum
WHERE ns.nspname = $1
  AND cls.relkind IN ('r', 'p')
  AND att.attnum > 0
  AND NOT att.attisdropped
ORDER BY cls.relname, att.attnum;
)SQL",
        schema_param);

    for (const auto& row : column_rows.rows) {
        const auto table_name = row_string(row, "table_name");
        const auto column_name = row_string(row, "column_name");
        if (!table_name || !column_name || !include_table(*table_name, options)) continue;

        auto [position, inserted] = table_positions.emplace(*table_name, schema.tables.size());
        if (inserted) {
            DatabaseTable table;
            table.name = *table_name;
            schema.tables.push_back(std::move(table));
        }

        DatabaseColumn column;
        column.name = *column_name;
        column.type = row_string(row, "data_type").value_or("");
        column.not_null = row_bool(row, "not_null");
        column.default_value = row_string(row, "column_default");
        column.auto_increment = row_bool(row, "is_identity") ||
            (column.default_value && column.default_value->starts_with("nextval("));
        column.comment = trim_nonempty(row_string(row, "comment"));
        schema.tables[position->second].columns.push_back(std::move(column));
    }

    const auto table_comment_rows = executor.execute(
        R"SQL(
SELECT
    cls.relname AS table_name,
    pg_catalog.obj_description(cls.oid, 'pg_class') AS comment
FROM pg_catalog.pg_class AS cls
JOIN pg_catalog.pg_namespace AS ns ON ns.oid = cls.relnamespace
WHERE ns.nspname = $1
  AND cls.relkind IN ('r', 'p')
ORDER BY cls.relname;
)SQL",
        schema_param);
    for (const auto& row : table_comment_rows.rows) {
        const auto table_name = row_string(row, "table_name");
        if (!table_name) continue;
        const auto position = table_positions.find(*table_name);
        if (position == table_positions.end()) continue;
        schema.tables[position->second].comment = trim_nonempty(row_string(row, "comment"));
    }

    const auto primary_key_rows = executor.execute(
        R"SQL(
SELECT
    cls.relname AS table_name,
    att.attname AS column_name,
    keys.ord AS ordinal_position
FROM pg_catalog.pg_constraint AS con
JOIN pg_catalog.pg_class AS cls ON cls.oid = con.conrelid
JOIN pg_catalog.pg_namespace AS ns ON ns.oid = cls.relnamespace
JOIN LATERAL unnest(con.conkey) WITH ORDINALITY AS keys(attnum, ord) ON TRUE
JOIN pg_catalog.pg_attribute AS att
    ON att.attrelid = cls.oid AND att.attnum = keys.attnum
WHERE ns.nspname = $1
  AND con.contype = 'p'
ORDER BY cls.relname, keys.ord;
)SQL",
        schema_param);
    for (const auto& row : primary_key_rows.rows) {
        const auto table_name = row_string(row, "table_name");
        const auto column_name = row_string(row, "column_name");
        if (!table_name || !column_name) continue;
        const auto position = table_positions.find(*table_name);
        if (position == table_positions.end()) continue;
        schema.tables[position->second].primary_key.push_back(*column_name);
    }

    const auto unique_rows = executor.execute(
        R"SQL(
SELECT
    cls.relname AS table_name,
    con.conname AS constraint_name,
    att.attname AS column_name,
    cardinality(con.conkey) AS column_count
FROM pg_catalog.pg_constraint AS con
JOIN pg_catalog.pg_class AS cls ON cls.oid = con.conrelid
JOIN pg_catalog.pg_namespace AS ns ON ns.oid = cls.relnamespace
JOIN LATERAL unnest(con.conkey) WITH ORDINALITY AS keys(attnum, ord) ON TRUE
JOIN pg_catalog.pg_attribute AS att
    ON att.attrelid = cls.oid AND att.attnum = keys.attnum
WHERE ns.nspname = $1
  AND con.contype = 'u'
ORDER BY cls.relname, con.conname, keys.ord;
)SQL",
        schema_param);
    for (const auto& row : unique_rows.rows) {
        if (row_int(row, "column_count") != 1) continue;
        const auto table_name = row_string(row, "table_name");
        const auto column_name = row_string(row, "column_name");
        if (!table_name || !column_name) continue;
        const auto position = table_positions.find(*table_name);
        if (position == table_positions.end()) continue;
        auto* column = find_column(schema.tables[position->second], *column_name);
        if (!column) continue;
        column->unique = true;
        column->unique_name = implicit_constraint_name(
            *table_name,
            *column_name,
            row_string(row, "constraint_name"),
            "key");
    }

    const auto foreign_key_rows = executor.execute(
        R"SQL(
SELECT
    src.relname AS table_name,
    con.conname AS constraint_name,
    src_att.attname AS column_name,
    target_ns.nspname AS target_schema,
    target.relname AS target_table,
    target_att.attname AS target_column,
    CASE con.confdeltype
        WHEN 'a' THEN 'NO ACTION'
        WHEN 'r' THEN 'RESTRICT'
        WHEN 'c' THEN 'CASCADE'
        WHEN 'n' THEN 'SET NULL'
        WHEN 'd' THEN 'SET DEFAULT'
    END AS on_delete,
    CASE con.confupdtype
        WHEN 'a' THEN 'NO ACTION'
        WHEN 'r' THEN 'RESTRICT'
        WHEN 'c' THEN 'CASCADE'
        WHEN 'n' THEN 'SET NULL'
        WHEN 'd' THEN 'SET DEFAULT'
    END AS on_update,
    con.condeferrable AS deferrable
FROM pg_catalog.pg_constraint AS con
JOIN pg_catalog.pg_class AS src ON src.oid = con.conrelid
JOIN pg_catalog.pg_namespace AS src_ns ON src_ns.oid = src.relnamespace
JOIN pg_catalog.pg_class AS target ON target.oid = con.confrelid
JOIN pg_catalog.pg_namespace AS target_ns ON target_ns.oid = target.relnamespace
JOIN LATERAL unnest(con.conkey) WITH ORDINALITY AS src_keys(attnum, ord) ON TRUE
JOIN LATERAL unnest(con.confkey) WITH ORDINALITY AS target_keys(attnum, ord)
    ON target_keys.ord = src_keys.ord
JOIN pg_catalog.pg_attribute AS src_att
    ON src_att.attrelid = src.oid AND src_att.attnum = src_keys.attnum
JOIN pg_catalog.pg_attribute AS target_att
    ON target_att.attrelid = target.oid AND target_att.attnum = target_keys.attnum
WHERE src_ns.nspname = $1
  AND con.contype = 'f'
ORDER BY src.relname, con.conname, src_keys.ord;
)SQL",
        schema_param);
    for (const auto& row : foreign_key_rows.rows) {
        const auto table_name = row_string(row, "table_name");
        const auto column_name = row_string(row, "column_name");
        const auto target_schema = row_string(row, "target_schema");
        const auto target_table = row_string(row, "target_table");
        const auto target_column = row_string(row, "target_column");
        if (!table_name || !column_name || !target_schema || !target_table || !target_column) {
            continue;
        }
        const auto position = table_positions.find(*table_name);
        if (position == table_positions.end()) continue;
        auto* column = find_column(schema.tables[position->second], *column_name);
        if (!column || column->references) continue;
        column->references = ForeignKeyReference{
            .table = *target_table,
            .column = *target_column,
            .name = implicit_constraint_name(
                *table_name,
                *column_name,
                row_string(row, "constraint_name"),
                "fkey"),
            .on_delete = normalize_action(row_string(row, "on_delete")),
            .on_update = normalize_action(row_string(row, "on_update")),
            .deferrable = row_bool(row, "deferrable"),
            .schema = scoped_reference_schema(schema_name, *target_schema)
        };
    }

    const auto check_rows = executor.execute(
        R"SQL(
SELECT
    cls.relname AS table_name,
    con.conname AS constraint_name,
    pg_catalog.pg_get_expr(con.conbin, con.conrelid) AS expression,
    cardinality(con.conkey) AS column_count,
    att.attname AS column_name
FROM pg_catalog.pg_constraint AS con
JOIN pg_catalog.pg_class AS cls ON cls.oid = con.conrelid
JOIN pg_catalog.pg_namespace AS ns ON ns.oid = cls.relnamespace
LEFT JOIN pg_catalog.pg_attribute AS att
    ON cardinality(con.conkey) = 1
   AND att.attrelid = cls.oid
   AND att.attnum = con.conkey[1]
WHERE ns.nspname = $1
  AND con.contype = 'c'
ORDER BY cls.relname, con.conname;
)SQL",
        schema_param);
    for (const auto& row : check_rows.rows) {
        const auto table_name = row_string(row, "table_name");
        const auto expression = trim_nonempty(row_string(row, "expression"));
        if (!table_name || !expression) continue;
        const auto position = table_positions.find(*table_name);
        if (position == table_positions.end()) continue;

        const auto column_name = row_string(row, "column_name");
        const auto constraint_name = row_string(row, "constraint_name");
        if (row_int(row, "column_count") == 1 && column_name &&
            constraint_name == std::optional<std::string>{
                *table_name + "_" + *column_name + "_check"}) {
            if (auto* column = find_column(schema.tables[position->second], *column_name)) {
                column->check = *expression;
                continue;
            }
        }

        schema.tables[position->second].checks.push_back(DatabaseCheck{
            .name = implicit_table_constraint_name(*table_name, constraint_name, "check"),
            .expression = *expression
        });
    }

    const auto index_rows = executor.execute(
        R"SQL(
SELECT
    tbl.relname AS table_name,
    idx.relname AS index_name,
    i.indisunique AS is_unique,
    pg_catalog.pg_get_expr(i.indpred, i.indrelid) AS predicate,
    att.attname AS column_name,
    keys.ord AS ordinal_position
FROM pg_catalog.pg_index AS i
JOIN pg_catalog.pg_class AS tbl ON tbl.oid = i.indrelid
JOIN pg_catalog.pg_namespace AS ns ON ns.oid = tbl.relnamespace
JOIN pg_catalog.pg_class AS idx ON idx.oid = i.indexrelid
JOIN LATERAL unnest(i.indkey) WITH ORDINALITY AS keys(attnum, ord) ON TRUE
LEFT JOIN pg_catalog.pg_attribute AS att
    ON att.attrelid = tbl.oid AND att.attnum = keys.attnum
LEFT JOIN pg_catalog.pg_constraint AS con ON con.conindid = i.indexrelid
WHERE ns.nspname = $1
  AND NOT i.indisprimary
  AND con.oid IS NULL
  AND i.indexprs IS NULL
  AND i.indnatts = i.indnkeyatts
ORDER BY tbl.relname, idx.relname, keys.ord;
)SQL",
        schema_param);

    std::unordered_map<std::string, IndexAccumulator> indexes;
    std::vector<std::string> index_order;
    for (const auto& row : index_rows.rows) {
        const auto table_name = row_string(row, "table_name");
        const auto index_name = row_string(row, "index_name");
        if (!table_name || !index_name) continue;
        const auto position = table_positions.find(*table_name);
        if (position == table_positions.end()) continue;

        const std::string key = *table_name + "\n" + *index_name;
        auto [found, inserted] = indexes.emplace(key, IndexAccumulator{});
        if (inserted) {
            found->second.index.name = *index_name;
            found->second.index.unique = row_bool(row, "is_unique");
            found->second.index.where = trim_nonempty(row_string(row, "predicate"));
            index_order.push_back(key);
        }
        const auto column_name = row_string(row, "column_name");
        if (!column_name) {
            found->second.representable = false;
        } else {
            found->second.index.columns.push_back(DatabaseIndexColumn{*column_name});
        }
    }
    for (const auto& key : index_order) {
        auto found = indexes.find(key);
        if (found == indexes.end() || !found->second.representable ||
            found->second.index.columns.empty()) {
            continue;
        }
        const auto separator = key.find('\n');
        const auto table_name = key.substr(0, separator);
        const auto position = table_positions.find(table_name);
        if (position == table_positions.end()) continue;
        schema.tables[position->second].indexes.push_back(std::move(found->second.index));
    }

    if (options.include_views) {
        const auto view_rows = executor.execute(
            R"SQL(
SELECT
    cls.relname AS view_name,
    pg_catalog.pg_get_viewdef(cls.oid, true) AS definition,
    pg_catalog.obj_description(cls.oid, 'pg_class') AS comment
FROM pg_catalog.pg_class AS cls
JOIN pg_catalog.pg_namespace AS ns ON ns.oid = cls.relnamespace
WHERE ns.nspname = $1
  AND cls.relkind = 'v'
ORDER BY cls.relname;
)SQL",
            schema_param);

        std::unordered_map<std::string, std::size_t> view_positions;
        for (const auto& row : view_rows.rows) {
            const auto view_name = row_string(row, "view_name");
            if (!view_name || !include_view(*view_name, options)) continue;
            DatabaseView view;
            view.name = *view_name;
            view.definition = trim_nonempty(row_string(row, "definition"));
            view.comment = trim_nonempty(row_string(row, "comment"));
            view_positions.emplace(*view_name, schema.views.size());
            schema.views.push_back(std::move(view));
        }

        const auto view_column_rows = executor.execute(
            R"SQL(
SELECT
    cls.relname AS view_name,
    att.attname AS column_name,
    pg_catalog.format_type(att.atttypid, att.atttypmod) AS data_type,
    att.attnotnull AS not_null,
    pg_catalog.col_description(cls.oid, att.attnum) AS comment
FROM pg_catalog.pg_attribute AS att
JOIN pg_catalog.pg_class AS cls ON cls.oid = att.attrelid
JOIN pg_catalog.pg_namespace AS ns ON ns.oid = cls.relnamespace
WHERE ns.nspname = $1
  AND cls.relkind = 'v'
  AND att.attnum > 0
  AND NOT att.attisdropped
ORDER BY cls.relname, att.attnum;
)SQL",
            schema_param);
        for (const auto& row : view_column_rows.rows) {
            const auto view_name = row_string(row, "view_name");
            const auto column_name = row_string(row, "column_name");
            if (!view_name || !column_name) continue;
            const auto position = view_positions.find(*view_name);
            if (position == view_positions.end()) continue;
            DatabaseColumn column;
            column.name = *column_name;
            column.type = row_string(row, "data_type").value_or("");
            column.not_null = row_bool(row, "not_null");
            column.comment = trim_nonempty(row_string(row, "comment"));
            schema.views[position->second].columns.push_back(std::move(column));
        }
    }

    return schema;
}

} // namespace metal
