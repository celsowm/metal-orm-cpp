#pragma once

#include "metal/column_defaults.hpp"
#include "metal/reference_traits.hpp"
#include "metal/query.hpp"
#include "metal/schema_constraints.hpp"

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

template <std::meta::info Member>
std::string sqlite_column_type_name() {
    if constexpr (reflect::has<mapping::database_type>(Member)) {
        constexpr auto declared = reflect::annotation<mapping::database_type>(Member);
        return std::string(declared.name.view());
    } else {
        return sqlite_type_name<reflect::member_type_t<Member>>();
    }
}

template <std::meta::info Member>
std::string sqlite_reference_sql(const Dialect& dialect) {
    static_assert(reflect::validate_physical_reference<Member>());
    static_assert(reflect::has_physical_reference<Member>(),
                  "MetalORM: sqlite_reference_sql requires a physical reference annotation");

    using Reference = reflect::physical_reference_annotation_t<Member>;
    using Traits = mapping::reference_annotation_traits<Reference>;
    constexpr auto target = reflect::physical_reference_target<Member>();
    using Target = reflect::owner_type_t<target>;

    std::string sql = " REFERENCES " + dialect.quote_identifier(reflect::table_name<Target>()) +
                      " (" + dialect.quote_identifier(reflect::column_name<target>()) + ")";

    constexpr auto on_delete = mapping::referential_action_sql(Traits::on_delete);
    if constexpr (!on_delete.empty()) {
        sql += " ON DELETE ";
        sql += on_delete;
    }

    constexpr auto on_update = mapping::referential_action_sql(Traits::on_update);
    if constexpr (!on_update.empty()) {
        sql += " ON UPDATE ";
        sql += on_update;
    }

    return sql;
}

template <std::meta::info Member>
std::string sqlite_column_check_sql() {
    static_assert(reflect::validate_column_check<Member>());
    static_assert(reflect::has_column_check<Member>(),
                  "MetalORM: sqlite_column_check_sql requires a physical check annotation");
    using Check = reflect::column_check_annotation_t<Member>;
    using Traits = mapping::check_annotation_traits<Check>;
    return " CHECK (" + std::string(Traits::expression.view()) + ")";
}

template <reflect::Mapped T>
std::string create_table_sql(const Dialect& dialect) {
    static_assert(reflect::validate_mapping<T>());
    static_assert(reflect::validate_column_defaults<T>());
    static_assert(reflect::validate_physical_references<T>());
    static_assert(reflect::validate_physical_checks<T>());

    std::string sql = "CREATE TABLE IF NOT EXISTS " + dialect.quote_identifier(reflect::table_name<T>()) + " (";
    bool first = true;
    constexpr auto pk_count = reflect::primary_key_count<T>();
    std::vector<std::string> primary_keys;

    reflect::for_each_column<T>([&]<std::meta::info Member>() {
        using M = reflect::member_type_t<Member>;
        if (!first) sql += ", ";
        first = false;

        const auto name = reflect::column_name<Member>();
        sql += dialect.quote_identifier(name) + " " + sqlite_column_type_name<Member>();

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
        if constexpr (reflect::has_column_check<Member>()) {
            sql += sqlite_column_check_sql<Member>();
        }
        if constexpr (reflect::has_physical_reference<Member>()) {
            sql += sqlite_reference_sql<Member>(dialect);
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

    reflect::for_each_table_check<T>([&]<typename Check>() {
        using Traits = mapping::table_check_annotation_traits<Check>;
        sql += ", ";
        if constexpr (Traits::named) {
            sql += "CONSTRAINT " + dialect.quote_identifier(std::string(Traits::name.view())) + " ";
        }
        sql += "CHECK (" + std::string(Traits::expression.view()) + ")";
    });

    sql += ");";
    return sql;
}

} // namespace metal
