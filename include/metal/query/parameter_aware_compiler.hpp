#pragma once

// Keep the mature SQLite-oriented scalar/function renderers as implementation
// helpers while replacing only the expression layer that owns nested queries.
#define compile_scalar compile_scalar_legacy
#define compile_expression compile_expression_legacy
#define compile_case compile_case_legacy
#include "metal/query/sqlite_compiler.hpp"
#undef compile_case
#undef compile_expression
#undef compile_scalar

namespace metal {

inline std::string compile_scalar(const ScalarPtr& scalar, CompileContext& ctx);
inline std::string compile_expression(const ExprPtr& expression, CompileContext& ctx);

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
            const auto nested_dialect = offset_placeholders(ctx.dialect, ctx.params.size());
            auto sub = node.compile_subquery(nested_dialect);
            ctx.params.insert(ctx.params.end(), sub.params.begin(), sub.params.end());
            return left + (node.negated ? " NOT IN (" : " IN (") + sub.sql + ")";
        } else if constexpr (std::same_as<N, ExistsNode>) {
            const auto nested_dialect = offset_placeholders(ctx.dialect, ctx.params.size());
            auto sub = node.compile_subquery(nested_dialect);
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
