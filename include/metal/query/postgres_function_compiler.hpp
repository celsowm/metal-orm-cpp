#pragma once

#include "metal/query/core_types.hpp"

#include <cctype>

namespace metal {

inline std::string compile_scalar(const ScalarPtr& scalar, CompileContext& ctx);

inline std::string postgres_upper_ascii(std::string value) {
    for (char& c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}

inline std::string postgres_lower_ascii(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

inline std::string postgres_join_sql(
    const std::vector<std::string>& values,
    std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out += separator;
        out += values[i];
    }
    return out;
}

inline std::string compile_postgres_function(const FunctionRef& fn, CompileContext& ctx) {
    const auto name = postgres_upper_ascii(fn.name);
    std::vector<std::string> args;
    args.reserve(fn.args.size());
    for (const auto& arg : fn.args) args.push_back(compile_scalar(arg, ctx));

    const auto expect = [&](std::size_t count, std::string_view function_name) {
        if (args.size() != count) {
            throw std::logic_error(
                "MetalORM: " + std::string(function_name) + " expects " +
                std::to_string(count) + " argument" + (count == 1 ? "" : "s") +
                " on PostgreSQL");
        }
    };

    if (name == "NOW") {
        expect(0, "NOW");
        return "NOW()";
    }
    if (name == "CURRENT_DATE") {
        expect(0, "CURRENT_DATE");
        return "CURRENT_DATE";
    }
    if (name == "CURRENT_TIME") {
        expect(0, "CURRENT_TIME");
        return "CURRENT_TIME";
    }
    if (name == "UTC_NOW") {
        expect(0, "UTC_NOW");
        return "(NOW() AT TIME ZONE 'UTC')";
    }
    if (name == "LOCAL_TIME") {
        expect(0, "LOCAL_TIME");
        return "LOCALTIME";
    }
    if (name == "LOCAL_TIMESTAMP") {
        expect(0, "LOCAL_TIMESTAMP");
        return "LOCALTIMESTAMP";
    }

    if (name == "POSITION" || name == "LOCATE") {
        expect(2, name);
        return "POSITION(" + args[0] + " IN " + args[1] + ")";
    }
    if (name == "INSTR") {
        expect(2, "INSTR");
        return "STRPOS(" + args[0] + ", " + args[1] + ")";
    }
    if (name == "IF_NULL") {
        return "COALESCE(" + postgres_join_sql(args, ", ") + ")";
    }
    if (name == "GROUP_CONCAT") {
        if (args.size() == 1) return "STRING_AGG(" + args[0] + ", ',')";
        if (args.size() == 2) return "STRING_AGG(" + args[0] + ", " + args[1] + ")";
        throw std::logic_error("MetalORM: GROUP_CONCAT expects one or two arguments on PostgreSQL");
    }

    if (name == "RANDOM" || name == "RAND") {
        expect(0, name);
        return "RANDOM()";
    }
    if (name == "TRUNCATE") {
        expect(2, "TRUNCATE");
        return "TRUNC(" + args[0] + ", " + args[1] + ")";
    }

    if (name == "YEAR" || name == "MONTH" || name == "DAY" ||
        name == "HOUR" || name == "MINUTE" || name == "SECOND" ||
        name == "DAY_OF_WEEK" || name == "WEEK_OF_YEAR" || name == "QUARTER") {
        expect(1, name);
        std::string part = name;
        if (name == "DAY_OF_WEEK") part = "DOW";
        else if (name == "WEEK_OF_YEAR") part = "WEEK";
        return "EXTRACT(" + part + " FROM " + args[0] + ")";
    }

    if (name == "DATE_DIFF") {
        expect(2, "DATE_DIFF");
        return "(" + args[0] + "::DATE - " + args[1] + "::DATE)";
    }
    if (name == "DATE_FORMAT") {
        expect(2, "DATE_FORMAT");
        return "TO_CHAR(" + args[0] + ", " + args[1] + ")";
    }
    if (name == "UNIX_TIMESTAMP") {
        if (args.empty()) return "EXTRACT(EPOCH FROM NOW())::BIGINT";
        if (args.size() == 1) return "EXTRACT(EPOCH FROM " + args[0] + ")::BIGINT";
        throw std::logic_error("MetalORM: UNIX_TIMESTAMP expects zero or one argument on PostgreSQL");
    }
    if (name == "FROM_UNIXTIME") {
        expect(1, "FROM_UNIXTIME");
        return "TO_TIMESTAMP(" + args[0] + ")";
    }
    if (name == "END_OF_MONTH") {
        expect(1, "END_OF_MONTH");
        return "(DATE_TRUNC('month', " + args[0] + ") + INTERVAL '1 month' - INTERVAL '1 day')::DATE";
    }
    if (name.starts_with("DATE_ADD_") || name.starts_with("DATE_SUB_")) {
        expect(2, name);
        const bool add = name.starts_with("DATE_ADD_");
        const auto prefix_size = add ? std::string_view{"DATE_ADD_"}.size() : std::string_view{"DATE_SUB_"}.size();
        const auto unit = postgres_lower_ascii(name.substr(prefix_size));
        return "(" + args[0] + (add ? " + " : " - ") +
               "(" + args[1] + " || ' " + unit + "')::INTERVAL)";
    }
    if (name.starts_with("DATE_TRUNC_")) {
        expect(1, name);
        const auto part = postgres_lower_ascii(name.substr(std::string_view{"DATE_TRUNC_"}.size()));
        return "DATE_TRUNC('" + part + "', " + args[0] + ")";
    }
    if (name.starts_with("EXTRACT_")) {
        expect(1, name);
        const auto part = name.substr(std::string_view{"EXTRACT_"}.size());
        return "EXTRACT(" + part + " FROM " + args[0] + ")";
    }

    if (name == "JSON_PATH") {
        expect(2, "JSON_PATH");
        return "(JSONB_PATH_QUERY_FIRST((" + args[0] + ")::JSONB, (" + args[1] + ")::JSONPATH) #>> '{}')";
    }
    if (name == "JSON_LENGTH") {
        expect(1, "JSON_LENGTH");
        return "JSONB_ARRAY_LENGTH((" + args[0] + ")::JSONB)";
    }
    if (name == "JSON_ARRAYAGG") {
        expect(1, "JSON_ARRAYAGG");
        return "JSONB_AGG(" + args[0] + ")";
    }

    return fn.name + "(" + postgres_join_sql(args, ", ") + ")";
}

} // namespace metal
