#pragma once

#include "metal/column_defaults.hpp"
#include "metal/reference_traits.hpp"
#include "metal/query.hpp"

#include <string>
#include <type_traits>
#include <vector>

namespace metal {

template <typename T>
std::string sqlite_type_name() {
    using U = optional_value_t<T>;
    if constexpr (std::is_integral_v<U> || std::is_same_v<U, bool>) return "INTEGER";
    else if constexpr (std::is_floating_point_v<U>) return "REAL";
    else if constexpr (std::is_same_v<U, std::string>) return "TEXT";
    else static_assert(!sizeof(U), "MetalORM: unsupported SQLite column type");
}

template <reflect::Mapped T>
std::string create_table_sql(const Dialect& dialect) {
    static_assert(reflect::validate_mapping<T>());
    static_assert(reflect::validate_column_defaults<T>());

    std::string sql = "CREATE TABLE IF NOT EXISTS " + dialect.quote_identifier(reflect::table_name<T>()) + " (";
    bool first = true;
    constexpr auto pk_count = reflect::primary_key_count<T>();
    std::vector<std::string> primary_keys;

    reflect::for_each_column<T>([&]<std::meta::info Member>() {
        using M = reflect::member_type_t<Member>;
        if (!first) sql += ", ";
        first = false;

        const auto name = reflect::column_name<Member>();
        sql += dialect.quote_identifier(name) + " " + sqlite_type_name<M>();

        if constexpr (reflect::has<mapping::primary_key_t>(Member)) {
            primary_keys.push_back(name);
            if constexpr (pk_count == 1) sql += " PRIMARY KEY";
        }
        if constexpr (reflect::has<mapping::generated_t>(Member)) {
            static_assert(pk_count == 1,
                          "MetalORM: SQLite AUTOINCREMENT cannot be used with a composite primary key");
            static_assert(std::is_integral_v<optional_value_t<M>>,
                          "MetalORM: generated SQLite columns must be integral");
            sql += " AUTOINCREMENT";
        }
        if constexpr (!is_optional_v<M>) {
            if constexpr (!reflect::has<mapping::primary_key_t>(Member) || pk_count > 1) {
                sql += " NOT NULL";
            }
        }
        if constexpr (reflect::has_column_default<Member>()) {
            sql += " DEFAULT " + reflect::column_default_sql<Member>();
        }
    });

    if constexpr (pk_count > 1) {
        sql += ", PRIMARY KEY (";
        for (std::size_t i = 0; i < primary_keys.size(); ++i) {
            if (i) sql += ", ";
            sql += dialect.quote_identifier(primary_keys[i]);
        }
        sql += ")";
    }

    sql += ");";
    return sql;
}

} // namespace metal
