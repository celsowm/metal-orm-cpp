#pragma once

#include "metal/ddl.hpp"
#include "metal/reference_traits.hpp"
#include "metal/schema_types.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace metal {

template <reflect::Entity T>
ExpectedTable expected_table(const Dialect& dialect) {
    static_assert(reflect::validate_mapping<T>());
    static_assert(reflect::validate_column_defaults<T>());
    static_assert(reflect::validate_physical_references<T>());

    ExpectedTable expected;
    expected.table.name = reflect::table_name<T>();
    expected.create_table_sql = create_table_sql<T>(dialect);

    reflect::for_each_column<T>([&]<std::meta::info Member>() {
        using M = reflect::member_type_t<Member>;
        DatabaseColumn column;
        column.name = reflect::column_name<Member>();
        column.type = sqlite_column_type_name<Member>();
        column.not_null = !is_optional_v<M>;
        if constexpr (reflect::has_column_default<Member>()) {
            column.default_value = reflect::column_default_sql<Member>();
        }
        column.auto_increment = reflect::has<mapping::generated_t>(Member);

        if constexpr (reflect::has<mapping::reference>(Member)) {
            constexpr auto reference = reflect::annotation<mapping::reference>(Member);
            constexpr auto target = reference.target_column;
            using Target = reflect::owner_type_t<target>;

            ForeignKeyReference foreign_key{
                .table = reflect::table_name<Target>(),
                .column = reflect::column_name<target>()
            };

            constexpr auto on_delete = mapping::referential_action_sql(reference.on_delete);
            if constexpr (!on_delete.empty()) {
                foreign_key.on_delete = std::string(on_delete);
            }

            constexpr auto on_update = mapping::referential_action_sql(reference.on_update);
            if constexpr (!on_update.empty()) {
                foreign_key.on_update = std::string(on_update);
            }

            column.references = std::move(foreign_key);
        }

        expected.table.columns.push_back(std::move(column));
        if constexpr (reflect::has<mapping::primary_key_t>(Member)) {
            expected.table.primary_key.push_back(reflect::column_name<Member>());
        }
    });

    return expected;
}

template <reflect::Entity... Ts>
ExpectedSchema expected_schema(const Dialect& dialect) {
    ExpectedSchema result;
    result.tables.reserve(sizeof...(Ts));
    (result.tables.push_back(expected_table<Ts>(dialect)), ...);
    return result;
}

template <reflect::Entity T, std::meta::info... Members>
void add_expected_index(
    ExpectedSchema& schema,
    const Dialect& dialect,
    std::string name,
    bool unique = false,
    std::optional<std::string> where = std::nullopt) {
    static_assert(sizeof...(Members) > 0,
                  "MetalORM: schema index requires at least one reflected column");
    static_assert((std::same_as<reflect::owner_type_t<Members>, T> && ...),
                  "MetalORM: schema index members must belong to the declared entity");
    static_assert((reflect::is_persistent_member<Members>() && ...),
                  "MetalORM: schema index members must be persistent scalar columns");

    const auto table_name = reflect::table_name<T>();
    auto table = std::find_if(
        schema.tables.begin(), schema.tables.end(),
        [&](const ExpectedTable& entry) { return entry.table.name == table_name; });
    if (table == schema.tables.end()) {
        throw std::invalid_argument("MetalORM: expected schema does not contain the indexed entity table");
    }

    DatabaseIndex index;
    index.name = std::move(name);
    index.unique = unique;
    index.where = std::move(where);
    (index.columns.push_back(DatabaseIndexColumn{reflect::column_name<Members>()}), ...);

    std::string sql = "CREATE ";
    if (unique) sql += "UNIQUE ";
    sql += "INDEX IF NOT EXISTS " + dialect.quote_identifier(index.name) +
           " ON " + dialect.quote_identifier(table_name) + " (";
    for (std::size_t i = 0; i < index.columns.size(); ++i) {
        if (i) sql += ", ";
        sql += dialect.quote_identifier(index.columns[i].column);
    }
    sql += ")";
    if (index.where) sql += " WHERE " + *index.where;
    sql += ";";

    table->table.indexes.push_back(index);
    table->create_index_sql.push_back(std::move(sql));
}

} // namespace metal
