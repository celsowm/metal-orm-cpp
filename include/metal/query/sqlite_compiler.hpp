#pragma once

#include "metal/query/core_types.hpp"

#include <cctype>

namespace metal {

inline const std::string& alias_for(const CompileContext& ctx, std::type_index owner) {
    auto it = ctx.aliases.find(owner);
    if (it == ctx.aliases.end()) {
        throw std::logic_error("MetalORM: SQL expression references an entity outside the query scope");
    }
    return it->second;
}

inline std::string compile_column(const ColumnRef& column, const CompileContext& ctx) {
    const auto& alias = alias_for(ctx, column.owner);
    if (alias.empty()) return ctx.dialect.quote_identifier(column.column);
    return ctx.dialect.quote_identifier(alias) + "." + ctx.dialect.quote_identifier(column.column);
}

inline std::string compile_scalar(const ScalarPtr& scalar, CompileContext& ctx);
inline std::string compile_expression(const ExprPtr& expression, CompileContext& ctx);

inline std::string upper_ascii(std::string value) {
    for (char& c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}

inline std::string lower_ascii(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

inline void validate_function_identifier(std::string_view name) {
    if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_')) {
        throw std::invalid_argument("MetalORM: SQL function name must be a simple identifier");
    }
    for (char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
            throw std::invalid_argument("MetalORM: SQL function name must be a simple identifier");
        }
    }
}

inline std::string join_sql(const std::vector<std::string>& values, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out += separator;
        out += values[i];
    }
    return out;
}

inline std::string compile_function(const FunctionRef& fn, CompileContext& ctx) {
    validate_function_identifier(fn.name);
    const auto name = upper_ascii(fn.name);

    // These renderers need to compile one logical argument more than once. Compile
    // them before the generic argument pass so each SQL placeholder gets its own binding.
    if (name == "TRUNCATE") {
        if (fn.args.size() != 2) throw std::logic_error("MetalORM: TRUNCATE expects value and digits");
        const auto value = compile_scalar(fn.args[0], ctx);
        const auto scale_left = compile_scalar(fn.args[1], ctx);
        const auto scale_right = compile_scalar(fn.args[1], ctx);
        return "(CAST((" + value + ") * pow(10.0, " + scale_left + ") AS INTEGER) / pow(10.0, " + scale_right + "))";
    }

    std::vector<std::string> args;
    args.reserve(fn.args.size());
    for (const auto& arg : fn.args) args.push_back(compile_scalar(arg, ctx));

    if (name == "NOW") return "datetime('now', 'localtime')";
    if (name == "CURRENT_DATE") return "date('now', 'localtime')";
    if (name == "CURRENT_TIME") return "time('now', 'localtime')";
    if (name == "UTC_NOW") return "datetime('now')";
    if (name == "LOCAL_TIME") return "time('now', 'localtime')";
    if (name == "LOCAL_TIMESTAMP") return "datetime('now', 'localtime')";

    if (name == "CONCAT") {
        if (args.empty()) return "''";
        return "(" + join_sql(args, " || ") + ")";
    }
    if (name == "LEFT") {
        if (args.size() != 2) throw std::logic_error("MetalORM: LEFT expects two arguments");
        return "substr(" + args[0] + ", 1, " + args[1] + ")";
    }
    if (name == "RIGHT") {
        if (args.size() != 2) throw std::logic_error("MetalORM: RIGHT expects two arguments");
        return "substr(" + args[0] + ", -(" + args[1] + "))";
    }
    if (name == "ASCII") {
        if (args.size() != 1) throw std::logic_error("MetalORM: ASCII expects one argument");
        return "unicode(substr(" + args[0] + ", 1, 1))";
    }
    if (name == "CHR") {
        if (args.size() != 1) throw std::logic_error("MetalORM: CHR expects one argument");
        return "char(" + args[0] + ")";
    }
    if (name == "BIT_LENGTH") {
        if (args.size() != 1) throw std::logic_error("MetalORM: BIT_LENGTH expects one argument");
        return "(length(CAST(" + args[0] + " AS BLOB)) * 8)";
    }
    if (name == "OCTET_LENGTH") {
        if (args.size() != 1) throw std::logic_error("MetalORM: OCTET_LENGTH expects one argument");
        return "length(CAST(" + args[0] + " AS BLOB))";
    }
    if (name == "POSITION" || name == "LOCATE") {
        if (args.size() != 2) throw std::logic_error("MetalORM: POSITION/LOCATE expects two arguments on SQLite");
        return "instr(" + args[1] + ", " + args[0] + ")";
    }
    if (name == "IF_NULL") return "COALESCE(" + join_sql(args, ", ") + ")";
    if (name == "GREATEST") return "max(" + join_sql(args, ", ") + ")";
    if (name == "LEAST") return "min(" + join_sql(args, ", ") + ")";

    if (name == "RANDOM" || name == "RAND") {
        return "(abs(random()) / 9223372036854775808.0)";
    }
    if (name == "MOD") {
        if (args.size() != 2) throw std::logic_error("MetalORM: MOD expects two arguments");
        return "((" + args[0] + ") % (" + args[1] + "))";
    }
    if (name == "COT") {
        if (args.size() != 1) throw std::logic_error("MetalORM: COT expects one argument");
        return "(1.0 / tan(" + args[0] + "))";
    }
    if (name == "CBRT") {
        if (args.size() != 1) throw std::logic_error("MetalORM: CBRT expects one argument");
        return "pow(" + args[0] + ", 1.0 / 3.0)";
    }

    if (name == "YEAR" || name == "MONTH" || name == "DAY" ||
        name == "HOUR" || name == "MINUTE" || name == "SECOND" ||
        name == "DAY_OF_WEEK" || name == "WEEK_OF_YEAR") {
        if (args.size() != 1) throw std::logic_error("MetalORM: date-part function expects one argument");
        std::string format;
        if (name == "YEAR") format = "%Y";
        else if (name == "MONTH") format = "%m";
        else if (name == "DAY") format = "%d";
        else if (name == "HOUR") format = "%H";
        else if (name == "MINUTE") format = "%M";
        else if (name == "SECOND") format = "%S";
        else if (name == "DAY_OF_WEEK") format = "%w";
        else format = "%W";
        return "CAST(strftime('" + format + "', " + args[0] + ") AS INTEGER)";
    }
    if (name == "QUARTER") {
        if (args.size() != 1) throw std::logic_error("MetalORM: QUARTER expects one argument");
        return "((CAST(strftime('%m', " + args[0] + ") AS INTEGER) + 2) / 3)";
    }
    if (name == "DATE_DIFF") {
        if (args.size() != 2) throw std::logic_error("MetalORM: DATE_DIFF expects two arguments");
        return "CAST(julianday(" + args[0] + ") - julianday(" + args[1] + ") AS INTEGER)";
    }
    if (name == "DATE_FORMAT") {
        if (args.size() != 2) throw std::logic_error("MetalORM: DATE_FORMAT expects two arguments");
        return "strftime(" + args[1] + ", " + args[0] + ")";
    }
    if (name == "UNIX_TIMESTAMP") {
        if (args.empty()) return "CAST(strftime('%s', 'now') AS INTEGER)";
        if (args.size() == 1) return "CAST(strftime('%s', " + args[0] + ") AS INTEGER)";
        throw std::logic_error("MetalORM: UNIX_TIMESTAMP expects zero or one argument");
    }
    if (name == "FROM_UNIXTIME") {
        if (args.size() != 1) throw std::logic_error("MetalORM: FROM_UNIXTIME expects one argument");
        return "datetime(" + args[0] + ", 'unixepoch')";
    }
    if (name == "END_OF_MONTH") {
        if (args.size() != 1) throw std::logic_error("MetalORM: END_OF_MONTH expects one argument");
        return "date(" + args[0] + ", 'start of month', '+1 month', '-1 day')";
    }
    if (name.starts_with("DATE_ADD_") || name.starts_with("DATE_SUB_")) {
        if (args.size() != 2) throw std::logic_error("MetalORM: DATE_ADD/DATE_SUB expects date and interval");
        const bool add = name.starts_with("DATE_ADD_");
        const auto prefix_size = add ? std::string_view{"DATE_ADD_"}.size() : std::string_view{"DATE_SUB_"}.size();
        const auto unit = lower_ascii(name.substr(prefix_size));
        return "datetime(" + args[0] + ", '" + (add ? "+" : "-") + "' || " + args[1] + " || ' " + unit + "')";
    }
    if (name.starts_with("DATE_TRUNC_")) {
        if (args.size() != 1) throw std::logic_error("MetalORM: DATE_TRUNC expects one date argument");
        const auto part = lower_ascii(name.substr(std::string_view{"DATE_TRUNC_"}.size()));
        if (part == "day") return "date(" + args[0] + ")";
        return "date(" + args[0] + ", 'start of " + part + "')";
    }
    if (name.starts_with("EXTRACT_")) {
        if (args.size() != 1) throw std::logic_error("MetalORM: EXTRACT expects one date argument");
        const auto part = name.substr(std::string_view{"EXTRACT_"}.size());
        std::string format;
        if (part == "YEAR") format = "%Y";
        else if (part == "MONTH") format = "%m";
        else if (part == "DAY") format = "%d";
        else if (part == "HOUR") format = "%H";
        else if (part == "MINUTE") format = "%M";
        else if (part == "SECOND") format = "%S";
        else if (part == "DOW") format = "%w";
        else if (part == "WEEK") format = "%W";
        else throw std::logic_error("MetalORM: unsupported SQLite EXTRACT part");
        return "CAST(strftime('" + format + "', " + args[0] + ") AS INTEGER)";
    }

    if (name == "JSON_PATH") {
        if (args.size() != 2) throw std::logic_error("MetalORM: JSON_PATH expects value and path");
        return "json_extract(" + args[0] + ", " + args[1] + ")";
    }
    if (name == "JSON_LENGTH") {
        if (args.empty() || args.size() > 2) throw std::logic_error("MetalORM: JSON_LENGTH expects one or two arguments");
        return "json_array_length(" + join_sql(args, ", ") + ")";
    }
    if (name == "JSON_ARRAYAGG") {
        if (args.size() != 1) throw std::logic_error("MetalORM: JSON_ARRAYAGG expects one argument");
        return "json_group_array(" + args[0] + ")";
    }

    if (name == "TRUNCATE") {
        throw std::logic_error("MetalORM: internal TRUNCATE renderer ordering failure");
    }

    return fn.name + "(" + join_sql(args, ", ") + ")";
}

inline std::string compile_aggregate(const AggregateRef& aggregate, const CompileContext& ctx) {
    std::string out = aggregate_token(aggregate.kind) + "(";
    if (aggregate.distinct) out += "DISTINCT ";
    out += aggregate.column ? compile_column(*aggregate.column, ctx) : "*";
    out += ")";
    return out;
}

inline std::string compile_window(const WindowRef& window, CompileContext& ctx) {
    validate_function_identifier(window.name);
    std::string out = window.name + "(";
    for (std::size_t i = 0; i < window.args.size(); ++i) {
        if (i) out += ", ";
        out += compile_scalar(window.args[i], ctx);
    }
    out += ") OVER (";
    if (!window.partition_by.empty()) {
        out += "PARTITION BY ";
        for (std::size_t i = 0; i < window.partition_by.size(); ++i) {
            if (i) out += ", ";
            out += compile_column(window.partition_by[i], ctx);
        }
    }
    if (!window.order_by.empty()) {
        if (!window.partition_by.empty()) out += " ";
        out += "ORDER BY ";
        for (std::size_t i = 0; i < window.order_by.size(); ++i) {
            if (i) out += ", ";
            out += compile_column(window.order_by[i].column, ctx) +
                   (window.order_by[i].ascending ? " ASC" : " DESC");
        }
    }
    out += ")";
    return out;
}

inline std::string compile_case(const CaseRef& value, CompileContext& ctx) {
    if (value.branches.empty()) throw std::logic_error("MetalORM: CASE requires at least one WHEN branch");
    std::string out = "CASE";
    for (const auto& branch : value.branches) {
        out += " WHEN " + compile_expression(branch.when, ctx) +
               " THEN " + compile_scalar(branch.then_value, ctx);
    }
    if (value.else_value) out += " ELSE " + compile_scalar(*value.else_value, ctx);
    out += " END";
    return out;
}

inline std::string compile_arithmetic(const ArithmeticRef& value, CompileContext& ctx) {
    return "(" + compile_scalar(value.left, ctx) + " " + arithmetic_token(value.op) + " " +
           compile_scalar(value.right, ctx) + ")";
}

inline std::string compile_scalar(const ScalarPtr& scalar, CompileContext& ctx) {
    return std::visit([&](const auto& node) -> std::string {
        using N = std::remove_cvref_t<decltype(node)>;
        if constexpr (std::same_as<N, ColumnRef>) {
            return compile_column(node, ctx);
        } else if constexpr (std::same_as<N, Value>) {
            ctx.params.push_back(node);
            return ctx.dialect.placeholder(ctx.params.size());
        } else if constexpr (std::same_as<N, ArithmeticRef>) {
            return compile_arithmetic(node, ctx);
        } else if constexpr (std::same_as<N, AggregateRef>) {
            return compile_aggregate(node, ctx);
        } else if constexpr (std::same_as<N, FunctionRef>) {
            return compile_function(node, ctx);
        } else if constexpr (std::same_as<N, CaseRef>) {
            return compile_case(node, ctx);
        } else {
            return compile_window(node, ctx);
        }
    }, scalar->node);
}

inline std::string compile_expression(const ExprPtr& expression, CompileContext& ctx) {
    return std::visit([&](const auto& node) -> std::string {
        using N = std::remove_cvref_t<decltype(node)>;
        if constexpr (std::same_as<N, ComparisonNode>) {
            return compile_scalar(node.left, ctx) + " " + compare_token(node.op) + " " +
                   compile_scalar(node.right, ctx);
        } else if constexpr (std::same_as<N, NullCheckNode>) {
            return compile_scalar(node.operand, ctx) + (node.negated ? " IS NOT NULL" : " IS NULL");
        } else if constexpr (std::same_as<N, LikeNode>) {
            return compile_scalar(node.operand, ctx) + (node.negated ? " NOT LIKE " : " LIKE ") +
                   compile_scalar(node.pattern, ctx);
        } else if constexpr (std::same_as<N, BetweenNode>) {
            return compile_scalar(node.operand, ctx) + (node.negated ? " NOT BETWEEN " : " BETWEEN ") +
                   compile_scalar(node.lower, ctx) + " AND " + compile_scalar(node.upper, ctx);
        } else if constexpr (std::same_as<N, InValuesNode>) {
            if (node.values.empty()) return node.negated ? "1 = 1" : "0 = 1";
            std::string out = compile_scalar(node.operand, ctx) + (node.negated ? " NOT IN (" : " IN (");
            for (std::size_t i = 0; i < node.values.size(); ++i) {
                if (i) out += ", ";
                ctx.params.push_back(node.values[i]);
                out += ctx.dialect.placeholder(ctx.params.size());
            }
            return out + ")";
        } else if constexpr (std::same_as<N, InSubqueryNode>) {
            const auto left = compile_scalar(node.operand, ctx);
            auto sub = node.compile_subquery(ctx.dialect);
            ctx.params.insert(ctx.params.end(), sub.params.begin(), sub.params.end());
            return left + (node.negated ? " NOT IN (" : " IN (") + sub.sql + ")";
        } else if constexpr (std::same_as<N, ExistsNode>) {
            auto sub = node.compile_subquery(ctx.dialect);
            ctx.params.insert(ctx.params.end(), sub.params.begin(), sub.params.end());
            return std::string(node.negated ? "NOT EXISTS (" : "EXISTS (") + sub.sql + ")";
        } else if constexpr (std::same_as<N, LogicalNode>) {
            return "(" + compile_expression(node.left, ctx) +
                   (node.op == LogicOp::And ? " AND " : " OR ") +
                   compile_expression(node.right, ctx) + ")";
        } else {
            return "NOT (" + compile_expression(node.operand, ctx) + ")";
        }
    }, expression->node);
}

} // namespace metal