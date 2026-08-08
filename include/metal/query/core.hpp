#pragma once

#include "metal/reflection.hpp"

#include <algorithm>
#include <cctype>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace metal {

class Dialect {
public:
    virtual ~Dialect() = default;
    virtual std::string quote_identifier(std::string_view id) const = 0;
    virtual std::string placeholder(std::size_t index) const = 0;
};

class SQLiteDialect final : public Dialect {
public:
    std::string quote_identifier(std::string_view id) const override {
        std::string out = "\"";
        for (char c : id) {
            if (c == '"') out += "\"\"";
            else out += c;
        }
        out += '"';
        return out;
    }

    std::string placeholder(std::size_t) const override { return "?"; }
};

struct CompiledQuery {
    std::string sql;
    std::vector<Value> params;
};

template <typename T, typename... Ts>
inline constexpr bool type_in_pack_v = (std::same_as<T, Ts> || ...);

template <typename... Ts>
struct type_list {};

template <typename Left, typename Right>
struct type_list_concat;

template <typename... Left, typename... Right>
struct type_list_concat<type_list<Left...>, type_list<Right...>> {
    using type = type_list<Left..., Right...>;
};

template <typename... Lists>
struct type_list_concat_many;

template <typename List>
struct type_list_concat_many<List> { using type = List; };

template <typename First, typename Second, typename... Rest>
struct type_list_concat_many<First, Second, Rest...> {
    using type = typename type_list_concat_many<
        typename type_list_concat<First, Second>::type,
        Rest...>::type;
};

template <typename... Lists>
using type_list_concat_many_t = typename type_list_concat_many<Lists...>::type;

enum class CompareOp { Eq, Ne, Gt, Ge, Lt, Le };
enum class LogicOp { And, Or };
enum class AggregateKind { Count, Sum, Avg, Min, Max, Stddev, Variance };
enum class JoinKind { Inner, Left };
enum class SetOperationKind { Union, UnionAll, Intersect, Except };

inline std::string compare_token(CompareOp op) {
    switch (op) {
        case CompareOp::Eq: return "=";
        case CompareOp::Ne: return "<>";
        case CompareOp::Gt: return ">";
        case CompareOp::Ge: return ">=";
        case CompareOp::Lt: return "<";
        case CompareOp::Le: return "<=";
    }
    return "=";
}

inline std::string aggregate_token(AggregateKind kind) {
    switch (kind) {
        case AggregateKind::Count: return "COUNT";
        case AggregateKind::Sum: return "SUM";
        case AggregateKind::Avg: return "AVG";
        case AggregateKind::Min: return "MIN";
        case AggregateKind::Max: return "MAX";
        case AggregateKind::Stddev: return "STDDEV";
        case AggregateKind::Variance: return "VARIANCE";
    }
    return "COUNT";
}

inline std::string set_operation_token(SetOperationKind kind) {
    switch (kind) {
        case SetOperationKind::Union: return "UNION";
        case SetOperationKind::UnionAll: return "UNION ALL";
        case SetOperationKind::Intersect: return "INTERSECT";
        case SetOperationKind::Except: return "EXCEPT";
    }
    return "UNION";
}

struct ColumnRef {
    std::type_index owner{typeid(void)};
    std::string column;
};

struct AggregateRef {
    AggregateKind kind{AggregateKind::Count};
    std::optional<ColumnRef> column;
    bool distinct{false};
};

struct ScalarNode;
struct ExprNode;
using ScalarPtr = std::shared_ptr<const ScalarNode>;
using ExprPtr = std::shared_ptr<const ExprNode>;

struct FunctionRef {
    std::string name;
    std::vector<ScalarPtr> args;
};

struct CaseBranch {
    ExprPtr when;
    ScalarPtr then_value;
};

struct CaseRef {
    std::vector<CaseBranch> branches;
    std::optional<ScalarPtr> else_value;
};

struct WindowOrderRef {
    ColumnRef column;
    bool ascending{true};
};

struct WindowRef {
    std::string name;
    std::vector<ScalarPtr> args;
    std::vector<ColumnRef> partition_by;
    std::vector<WindowOrderRef> order_by;
};

struct ScalarNode {
    std::variant<ColumnRef, Value, AggregateRef, FunctionRef, CaseRef, WindowRef> node;
};

struct ComparisonNode {
    ScalarPtr left;
    CompareOp op{CompareOp::Eq};
    ScalarPtr right;
};

struct NullCheckNode {
    ScalarPtr operand;
    bool negated{false};
};

struct LikeNode {
    ScalarPtr operand;
    ScalarPtr pattern;
    bool negated{false};
};

struct BetweenNode {
    ScalarPtr operand;
    ScalarPtr lower;
    ScalarPtr upper;
    bool negated{false};
};

struct InValuesNode {
    ScalarPtr operand;
    std::vector<Value> values;
    bool negated{false};
};

struct InSubqueryNode {
    ScalarPtr operand;
    std::function<CompiledQuery(const Dialect&)> compile_subquery;
    bool negated{false};
};

struct ExistsNode {
    std::function<CompiledQuery(const Dialect&)> compile_subquery;
    bool negated{false};
};

struct LogicalNode {
    LogicOp op{LogicOp::And};
    ExprPtr left;
    ExprPtr right;
};

struct NotNode {
    ExprPtr operand;
};

struct ExprNode {
    std::variant<
        ComparisonNode,
        NullCheckNode,
        LikeNode,
        BetweenNode,
        InValuesNode,
        InSubqueryNode,
        ExistsNode,
        LogicalNode,
        NotNode> node;
};

template <typename Result, typename... Owners>
class ScalarTerm {
public:
    using result_type = Result;
    using owners = type_list<Owners...>;

    explicit ScalarTerm(ScalarPtr node) : node_(std::move(node)) {}

    template <typename Node>
    explicit ScalarTerm(Node node)
        : node_(std::make_shared<ScalarNode>(ScalarNode{std::move(node)})) {}

    ScalarTerm as(std::string alias) const {
        ScalarTerm copy = *this;
        copy.alias_ = std::move(alias);
        return copy;
    }

    const ScalarPtr& node() const noexcept { return node_; }
    const std::optional<std::string>& alias() const noexcept { return alias_; }

private:
    ScalarPtr node_;
    std::optional<std::string> alias_;
};

template <typename... Owners>
class Expression {
public:
    using owners = type_list<Owners...>;

    explicit Expression(ExprPtr node) : node_(std::move(node)) {}

    template <typename Node>
    explicit Expression(Node node)
        : node_(std::make_shared<ExprNode>(ExprNode{std::move(node)})) {}

    const ExprPtr& node() const noexcept { return node_; }

    template <typename Owner>
    requires (sizeof...(Owners) > 1 && (std::same_as<Owner, Owners> && ...))
    operator Expression<Owner>() const {
        return Expression<Owner>{node_};
    }

private:
    ExprPtr node_;
};

struct CompileContext {
    const Dialect& dialect;
    std::unordered_map<std::type_index, std::string> aliases;
    std::vector<Value>& params;
};

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
    if (name == "CHR") return "char(" + join_sql(args, ", ") + ")";

    if (name == "RANDOM" || name == "RAND") {
        return "(abs(random()) / 9223372036854775808.0)";
    }
    if (name == "COT") {
        if (args.size() != 1) throw std::logic_error("MetalORM: COT expects one argument");
        return "(1.0 / tan(" + args[0] + "))";
    }
    if (name == "CBRT") {
        if (args.size() != 1) throw std::logic_error("MetalORM: CBRT expects one argument");
        return "(sign(" + args[0] + ") * pow(abs(" + args[0] + "), 1.0 / 3.0))";
    }
    if (name == "TRUNCATE") return "trunc(" + join_sql(args, ", ") + ")";

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
    if (value.else_value) out += " ELSE " + compile_scalar(**value.else_value, ctx);
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
            auto sub = node.compile_subquery(ctx.dialect);
            ctx.params.insert(ctx.params.end(), sub.params.begin(), sub.params.end());
            return compile_scalar(node.operand, ctx) + (node.negated ? " NOT IN (" : " IN (") + sub.sql + ")";
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
