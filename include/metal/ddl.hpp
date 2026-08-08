#pragma once

#include "metal/query.hpp"

#include <string>
#include <type_traits>

namespace metal {

template <typename T>
std::string sqlite_type_name() {
    using U = optional_value_t<T>;
    if constexpr (std::is_integral_v<U> || std::is_same_v<U, bool>) return "INTEGER";
    else if constexpr (std::is_floating_point_v<U>) return "REAL";
    else if constexpr (std::is_same_v<U, std::string>) return "TEXT";
    else static_assert(!sizeof(U), "MetalORM: unsupported SQLite column type");
}

template <reflect::Entity T>
std::string create_table_sql(const Dialect& dialect) {
    std::string sql = "CREATE TABLE IF NOT EXISTS " + dialect.quote_identifier(reflect::table_name<T>()) + " (";
    bool first = true;

    reflect::for_each_column<T>([&]<std::meta::info Member>() {
        using M = reflect::member_type_t<Member>;
        if (!first) sql += ", ";
        first = false;
        sql += dialect.quote_identifier(reflect::column_name<Member>()) + " " + sqlite_type_name<M>();

        if constexpr (reflect::has<mapping::primary_key_t>(Member)) {
            sql += " PRIMARY KEY";
        }
        if constexpr (reflect::has<mapping::generated_t>(Member)) {
            static_assert(std::is_integral_v<optional_value_t<M>>, "MetalORM: generated columns must be integral in 0.0.1");
            sql += " AUTOINCREMENT";
        }
        if constexpr (!is_optional_v<M>) {
            if constexpr (!reflect::has<mapping::primary_key_t>(Member)) sql += " NOT NULL";
        }
    });

    sql += ");";
    return sql;
}

} // namespace metal
