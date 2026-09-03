#pragma once

#include "metal/reflection.hpp"

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

enum class GeneratedKeyRetrieval {
    DriverLastInsertId,
    Returning
};

class Dialect {
public:
    virtual ~Dialect() = default;
    virtual std::string quote_identifier(std::string_view id) const = 0;
    virtual std::string placeholder(std::size_t index) const = 0;
    [[nodiscard]] virtual GeneratedKeyRetrieval generated_key_retrieval() const noexcept {
        return GeneratedKeyRetrieval::DriverLastInsertId;
    }
};

inline std::string quote_ansi_identifier(std::string_view id) {
    std::string out = "\"";
    for (char c : id) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += '"';
    return out;
}

class PlaceholderOffsetDialect final : public Dialect {
public:
    PlaceholderOffsetDialect(const Dialect& dialect, std::size_t offset)
        : dialect_(dialect), offset_(offset) {}

    std::string quote_identifier(std::string_view id) const override {
        return dialect_.quote_identifier(id);
    }

    std::string placeholder(std::size_t index) const override {
        return dialect_.placeholder(offset_ + index);
    }

    [[nodiscard]] GeneratedKeyRetrieval generated_key_retrieval() const noexcept override {
        return dialect_.generated_key_retrieval();
    }

private:
    const Dialect& dialect_;
    std::size_t offset_{};
};

inline PlaceholderOffsetDialect offset_placeholders(
    const Dialect& dialect,
    std::size_t offset) {
    return PlaceholderOffsetDialect{dialect, offset};
}

class SQLiteDialect final : public Dialect {
public:
    std::string quote_identifier(std::string_view id) const override {
        return quote_ansi_identifier(id);
    }

    std::string placeholder(std::size_t) const override { return "?"; }
};

class PostgresDialect final : public Dialect {
public:
    std::string quote_identifier(std::string_view id) const override {
        return quote_ansi_identifier(id);
    }

    std::string placeholder(std::size_t index) const override {
        if (index == 0) {
            throw std::invalid_argument("MetalORM: PostgreSQL placeholder indexes are one-based");
        }
        return "$" + std::to_string(index);
    }

    [[nodiscard]] GeneratedKeyRetrieval generated_key_retrieval() const noexcept override {
        return GeneratedKeyRetrieval::Returning;
    }
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

template <>
struct type_list_concat_many<> { using type = type_list<>; };

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
enum class ArithmeticOp { Add, Subtract, Multiply, Divide, Modulo };
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

inline std::string arithmetic_token(ArithmeticOp op) {
    switch (op) {
        case ArithmeticOp::Add: return "+";
        case ArithmeticOp::Subtract: return "-";
        case ArithmeticOp::Multiply: return "*";
        case ArithmeticOp::Divide: return "/";
        case ArithmeticOp::Modulo: return "%";
    }
    return "+";
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

struct ArithmeticRef {
    ScalarPtr left;
    ArithmeticOp op{ArithmeticOp::Add};
    ScalarPtr right;
};

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
    std::variant<ColumnRef, Value, ArithmeticRef, AggregateRef, FunctionRef, CaseRef, WindowRef> node;
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

} // namespace metal