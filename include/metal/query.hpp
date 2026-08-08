#pragma once

#include "metal/reflection.hpp"

#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace metal {

enum class CompareOp { Eq, Ne, Gt, Ge, Lt, Le };
enum class LogicOp { And, Or };

struct ExprNode;
using ExprPtr = std::shared_ptr<const ExprNode>;

struct ComparisonNode {
    std::string column;
    CompareOp op;
    Value value;
};

struct LogicalNode {
    LogicOp op;
    ExprPtr left;
    ExprPtr right;
};

struct NotNode {
    ExprPtr operand;
};

struct ExprNode {
    std::variant<ComparisonNode, LogicalNode, NotNode> node;
};

template <reflect::Entity Owner>
class Expression {
public:
    explicit Expression(ComparisonNode comparison)
        : node_(std::make_shared<ExprNode>(ExprNode{std::move(comparison)})) {}

    explicit Expression(LogicalNode logical)
        : node_(std::make_shared<ExprNode>(ExprNode{std::move(logical)})) {}

    explicit Expression(NotNode negation)
        : node_(std::make_shared<ExprNode>(ExprNode{std::move(negation)})) {}

    const ExprPtr& node() const noexcept { return node_; }

private:
    ExprPtr node_;
};

template <std::meta::info Member>
struct Field {
    static_assert(std::meta::is_nonstatic_data_member(Member));
    static_assert(reflect::is_persistent_member<Member>(), "MetalORM: field must refer to a persistent scalar member");

    using value_type = reflect::member_type_t<Member>;
    using owner_type = reflect::owner_type_t<Member>;
    static constexpr auto reflection = Member;
};

template <std::meta::info Member>
inline constexpr Field<Member> field{};

template <typename Member, typename V>
concept FieldComparable = [] {
    using M = optional_value_t<Member>;
    using R = std::remove_cvref_t<V>;
    if constexpr (std::is_same_v<M, std::string>) {
        return std::is_convertible_v<V, std::string_view> || std::is_same_v<R, std::string>;
    } else if constexpr (std::is_same_v<M, bool>) {
        return std::is_same_v<R, bool> || std::is_integral_v<R>;
    } else if constexpr (std::is_arithmetic_v<M>) {
        return std::is_arithmetic_v<R>;
    } else {
        return false;
    }
}();

template <std::meta::info Member, typename V>
requires FieldComparable<typename Field<Member>::value_type, V>
auto compare(Field<Member>, CompareOp op, V&& value) {
    using Owner = typename Field<Member>::owner_type;
    return Expression<Owner>{ComparisonNode{
        reflect::column_name<Member>(),
        op,
        to_value(std::forward<V>(value))
    }};
}

template <std::meta::info Member, typename V>
requires FieldComparable<typename Field<Member>::value_type, V>
auto operator==(Field<Member> f, V&& value) { return compare(f, CompareOp::Eq, std::forward<V>(value)); }

template <std::meta::info Member, typename V>
requires FieldComparable<typename Field<Member>::value_type, V>
auto operator!=(Field<Member> f, V&& value) { return compare(f, CompareOp::Ne, std::forward<V>(value)); }

template <std::meta::info Member, typename V>
requires FieldComparable<typename Field<Member>::value_type, V>
auto operator>(Field<Member> f, V&& value) { return compare(f, CompareOp::Gt, std::forward<V>(value)); }

template <std::meta::info Member, typename V>
requires FieldComparable<typename Field<Member>::value_type, V>
auto operator>=(Field<Member> f, V&& value) { return compare(f, CompareOp::Ge, std::forward<V>(value)); }

template <std::meta::info Member, typename V>
requires FieldComparable<typename Field<Member>::value_type, V>
auto operator<(Field<Member> f, V&& value) { return compare(f, CompareOp::Lt, std::forward<V>(value)); }

template <std::meta::info Member, typename V>
requires FieldComparable<typename Field<Member>::value_type, V>
auto operator<=(Field<Member> f, V&& value) { return compare(f, CompareOp::Le, std::forward<V>(value)); }

template <reflect::Entity Owner>
Expression<Owner> operator&&(Expression<Owner> left, Expression<Owner> right) {
    return Expression<Owner>{LogicalNode{LogicOp::And, left.node(), right.node()}};
}

template <reflect::Entity Owner>
Expression<Owner> operator||(Expression<Owner> left, Expression<Owner> right) {
    return Expression<Owner>{LogicalNode{LogicOp::Or, left.node(), right.node()}};
}

template <reflect::Entity Owner>
Expression<Owner> operator!(Expression<Owner> value) {
    return Expression<Owner>{NotNode{value.node()}};
}

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

inline std::string compile_expression(
    const ExprPtr& expression,
    const Dialect& dialect,
    std::vector<Value>& params) {
    return std::visit([&](const auto& node) -> std::string {
        using N = std::remove_cvref_t<decltype(node)>;
        if constexpr (std::is_same_v<N, ComparisonNode>) {
            params.push_back(node.value);
            return dialect.quote_identifier(node.column) + " " + compare_token(node.op) + " " +
                   dialect.placeholder(params.size());
        } else if constexpr (std::is_same_v<N, LogicalNode>) {
            const auto left = compile_expression(node.left, dialect, params);
            const auto right = compile_expression(node.right, dialect, params);
            return "(" + left + (node.op == LogicOp::And ? " AND " : " OR ") + right + ")";
        } else {
            return "NOT (" + compile_expression(node.operand, dialect, params) + ")";
        }
    }, expression->node);
}

template <reflect::Entity T>
class SelectQuery {
public:
    SelectQuery& where(Expression<T> expression) {
        where_ = std::move(expression);
        return *this;
    }

    template <std::meta::info Member>
    requires std::same_as<typename Field<Member>::owner_type, T>
    SelectQuery& order_by(Field<Member>, bool ascending = true) {
        order_column_ = reflect::column_name<Member>();
        order_ascending_ = ascending;
        return *this;
    }

    SelectQuery& limit(std::size_t value) {
        limit_ = value;
        return *this;
    }

    SelectQuery& where_column(std::string column, CompareOp op, Value value) {
        where_ = Expression<T>{ComparisonNode{std::move(column), op, std::move(value)}};
        return *this;
    }

    CompiledQuery compile(const Dialect& dialect) const {
        CompiledQuery out;
        out.sql = "SELECT ";
        bool first = true;
        reflect::for_each_column<T>([&]<std::meta::info Member>() {
            if (!first) out.sql += ", ";
            first = false;
            out.sql += dialect.quote_identifier(reflect::column_name<Member>());
        });
        out.sql += " FROM " + dialect.quote_identifier(reflect::table_name<T>());

        if (where_) {
            out.sql += " WHERE " + compile_expression(where_->node(), dialect, out.params);
        }
        if (order_column_) {
            out.sql += " ORDER BY " + dialect.quote_identifier(*order_column_) +
                       (order_ascending_ ? " ASC" : " DESC");
        }
        if (limit_) out.sql += " LIMIT " + std::to_string(*limit_);
        out.sql += ";";
        return out;
    }

private:
    std::optional<Expression<T>> where_;
    std::optional<std::string> order_column_;
    bool order_ascending_{true};
    std::optional<std::size_t> limit_;
};

template <reflect::Entity T>
SelectQuery<T> select() { return {}; }

} // namespace metal
