#pragma once

#include "metal/reflection.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
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

enum class CompareOp { Eq, Ne, Gt, Ge, Lt, Le };
enum class LogicOp { And, Or };
enum class AggregateKind { Count, Sum, Avg, Min, Max };
enum class JoinKind { Inner, Left };

struct ColumnRef {
    std::type_index owner;
    std::string column;
};

struct AggregateRef {
    AggregateKind kind{AggregateKind::Count};
    std::optional<ColumnRef> column;
    bool distinct{false};
};

struct ExprNode;
using ExprPtr = std::shared_ptr<const ExprNode>;

struct ValueComparisonNode {
    ColumnRef column;
    CompareOp op;
    Value value;
};

struct ColumnComparisonNode {
    ColumnRef left;
    CompareOp op;
    ColumnRef right;
};

struct NullCheckNode {
    ColumnRef column;
    bool negated{false};
};

struct LikeNode {
    ColumnRef column;
    Value pattern;
    bool negated{false};
};

struct InValuesNode {
    ColumnRef column;
    std::vector<Value> values;
    bool negated{false};
};

struct InSubqueryNode {
    ColumnRef column;
    std::function<CompiledQuery(const Dialect&)> compile_subquery;
    bool negated{false};
};

struct AggregateComparisonNode {
    AggregateRef aggregate;
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
    std::variant<
        ValueComparisonNode,
        ColumnComparisonNode,
        NullCheckNode,
        LikeNode,
        InValuesNode,
        InSubqueryNode,
        AggregateComparisonNode,
        LogicalNode,
        NotNode> node;
};

template <typename... Owners>
class Expression {
public:
    explicit Expression(ExprPtr node) : node_(std::move(node)) {}

    template <typename Node>
    explicit Expression(Node node)
        : node_(std::make_shared<ExprNode>(ExprNode{std::move(node)})) {}

    const ExprPtr& node() const noexcept { return node_; }

private:
    ExprPtr node_;
};

template <std::meta::info Member>
struct Field {
    static_assert(std::meta::is_nonstatic_data_member(Member));
    static_assert(reflect::is_persistent_member<Member>(),
                  "MetalORM: field must refer to a persistent scalar member");

    using value_type = reflect::member_type_t<Member>;
    using owner_type = reflect::owner_type_t<Member>;
    static constexpr auto reflection = Member;
};

template <std::meta::info Member>
inline constexpr Field<Member> field{};

template <std::meta::info Member>
ColumnRef column_ref(Field<Member> = {}) {
    using Owner = typename Field<Member>::owner_type;
    return ColumnRef{std::type_index(typeid(Owner)), reflect::column_name<Member>()};
}

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
auto compare(Field<Member> f, CompareOp op, V&& value) {
    using Owner = typename Field<Member>::owner_type;
    return Expression<Owner>{ValueComparisonNode{
        column_ref(f), op, to_value(std::forward<V>(value))}};
}

template <std::meta::info Left, std::meta::info Right>
requires reflect::key_types_compatible<Left, Right>()
auto compare(Field<Left> left, CompareOp op, Field<Right> right) {
    using L = typename Field<Left>::owner_type;
    using R = typename Field<Right>::owner_type;
    return Expression<L, R>{ColumnComparisonNode{column_ref(left), op, column_ref(right)}};
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

template <std::meta::info Left, std::meta::info Right>
requires reflect::key_types_compatible<Left, Right>()
auto operator==(Field<Left> left, Field<Right> right) { return compare(left, CompareOp::Eq, right); }

template <std::meta::info Left, std::meta::info Right>
requires reflect::key_types_compatible<Left, Right>()
auto operator!=(Field<Left> left, Field<Right> right) { return compare(left, CompareOp::Ne, right); }

template <typename... Left, typename... Right>
auto operator&&(Expression<Left...> left, Expression<Right...> right) {
    return Expression<Left..., Right...>{LogicalNode{LogicOp::And, left.node(), right.node()}};
}

template <typename... Left, typename... Right>
auto operator||(Expression<Left...> left, Expression<Right...> right) {
    return Expression<Left..., Right...>{LogicalNode{LogicOp::Or, left.node(), right.node()}};
}

template <typename... Owners>
auto operator!(Expression<Owners...> value) {
    return Expression<Owners...>{NotNode{value.node()}};
}

template <std::meta::info Member>
auto is_null(Field<Member> f) {
    using Owner = typename Field<Member>::owner_type;
    return Expression<Owner>{NullCheckNode{column_ref(f), false}};
}

template <std::meta::info Member>
auto is_not_null(Field<Member> f) {
    using Owner = typename Field<Member>::owner_type;
    return Expression<Owner>{NullCheckNode{column_ref(f), true}};
}

template <std::meta::info Member, typename Pattern>
requires std::same_as<optional_value_t<typename Field<Member>::value_type>, std::string> &&
         std::is_convertible_v<Pattern, std::string_view>
auto like(Field<Member> f, Pattern&& pattern) {
    using Owner = typename Field<Member>::owner_type;
    return Expression<Owner>{LikeNode{
        column_ref(f), to_value(std::string_view(std::forward<Pattern>(pattern))), false}};
}

template <std::meta::info Member, typename Pattern>
requires std::same_as<optional_value_t<typename Field<Member>::value_type>, std::string> &&
         std::is_convertible_v<Pattern, std::string_view>
auto not_like(Field<Member> f, Pattern&& pattern) {
    using Owner = typename Field<Member>::owner_type;
    return Expression<Owner>{LikeNode{
        column_ref(f), to_value(std::string_view(std::forward<Pattern>(pattern))), true}};
}

template <std::meta::info Member, std::ranges::input_range Range>
requires FieldComparable<typename Field<Member>::value_type, std::ranges::range_value_t<Range>>
auto in(Field<Member> f, const Range& values) {
    using Owner = typename Field<Member>::owner_type;
    std::vector<Value> params;
    for (const auto& value : values) params.push_back(to_value(value));
    return Expression<Owner>{InValuesNode{column_ref(f), std::move(params), false}};
}

template <std::meta::info Member, std::ranges::input_range Range>
requires FieldComparable<typename Field<Member>::value_type, std::ranges::range_value_t<Range>>
auto not_in(Field<Member> f, const Range& values) {
    using Owner = typename Field<Member>::owner_type;
    std::vector<Value> params;
    for (const auto& value : values) params.push_back(to_value(value));
    return Expression<Owner>{InValuesNode{column_ref(f), std::move(params), true}};
}

template <typename Owner>
class AggregateTerm {
public:
    explicit AggregateTerm(AggregateRef aggregate) : aggregate_(std::move(aggregate)) {}

    AggregateTerm as(std::string alias) const {
        AggregateTerm copy = *this;
        copy.alias_ = std::move(alias);
        return copy;
    }

    const AggregateRef& aggregate() const noexcept { return aggregate_; }
    const std::optional<std::string>& alias() const noexcept { return alias_; }

private:
    AggregateRef aggregate_;
    std::optional<std::string> alias_;
};

template <std::meta::info Member>
auto count(Field<Member> f, bool distinct = false) {
    using Owner = typename Field<Member>::owner_type;
    return AggregateTerm<Owner>{AggregateRef{AggregateKind::Count, column_ref(f), distinct}};
}

template <reflect::Entity Owner>
auto count_all() {
    return AggregateTerm<Owner>{AggregateRef{AggregateKind::Count, std::nullopt, false}};
}

template <std::meta::info Member>
requires std::is_arithmetic_v<optional_value_t<typename Field<Member>::value_type>>
auto sum(Field<Member> f) {
    using Owner = typename Field<Member>::owner_type;
    return AggregateTerm<Owner>{AggregateRef{AggregateKind::Sum, column_ref(f), false}};
}

template <std::meta::info Member>
requires std::is_arithmetic_v<optional_value_t<typename Field<Member>::value_type>>
auto avg(Field<Member> f) {
    using Owner = typename Field<Member>::owner_type;
    return AggregateTerm<Owner>{AggregateRef{AggregateKind::Avg, column_ref(f), false}};
}

template <std::meta::info Member>
auto min(Field<Member> f) {
    using Owner = typename Field<Member>::owner_type;
    return AggregateTerm<Owner>{AggregateRef{AggregateKind::Min, column_ref(f), false}};
}

template <std::meta::info Member>
auto max(Field<Member> f) {
    using Owner = typename Field<Member>::owner_type;
    return AggregateTerm<Owner>{AggregateRef{AggregateKind::Max, column_ref(f), false}};
}

template <typename Owner, typename V>
requires std::is_arithmetic_v<std::remove_cvref_t<V>>
auto compare(AggregateTerm<Owner> aggregate, CompareOp op, V&& value) {
    return Expression<Owner>{AggregateComparisonNode{
        aggregate.aggregate(), op, to_value(std::forward<V>(value))}};
}

template <typename Owner, typename V>
requires std::is_arithmetic_v<std::remove_cvref_t<V>>
auto operator==(AggregateTerm<Owner> a, V&& v) { return compare(a, CompareOp::Eq, std::forward<V>(v)); }

template <typename Owner, typename V>
requires std::is_arithmetic_v<std::remove_cvref_t<V>>
auto operator!=(AggregateTerm<Owner> a, V&& v) { return compare(a, CompareOp::Ne, std::forward<V>(v)); }

template <typename Owner, typename V>
requires std::is_arithmetic_v<std::remove_cvref_t<V>>
auto operator>(AggregateTerm<Owner> a, V&& v) { return compare(a, CompareOp::Gt, std::forward<V>(v)); }

template <typename Owner, typename V>
requires std::is_arithmetic_v<std::remove_cvref_t<V>>
auto operator>=(AggregateTerm<Owner> a, V&& v) { return compare(a, CompareOp::Ge, std::forward<V>(v)); }

template <typename Owner, typename V>
requires std::is_arithmetic_v<std::remove_cvref_t<V>>
auto operator<(AggregateTerm<Owner> a, V&& v) { return compare(a, CompareOp::Lt, std::forward<V>(v)); }

template <typename Owner, typename V>
requires std::is_arithmetic_v<std::remove_cvref_t<V>>
auto operator<=(AggregateTerm<Owner> a, V&& v) { return compare(a, CompareOp::Le, std::forward<V>(v)); }

struct ProjectionSpec {
    std::variant<ColumnRef, AggregateRef> expression;
    std::optional<std::string> alias;
};

struct OrderSpec {
    ColumnRef column;
    bool ascending{true};
};

struct JoinSpec {
    JoinKind kind{JoinKind::Inner};
    mapping::relation_kind relation_kind{mapping::relation_kind::belongs_to};
    std::type_index owner_type{typeid(void)};
    std::type_index target_type{typeid(void)};
    std::string target_table;
    std::string owner_column;
    std::string target_column;
    std::optional<std::string> pivot_table;
    std::optional<std::string> pivot_root_column;
    std::optional<std::string> pivot_target_column;
};

struct SelectState {
    bool distinct{false};
    std::vector<ProjectionSpec> projections;
    std::vector<JoinSpec> joins;
    std::optional<ExprPtr> where;
    std::vector<ColumnRef> group_by;
    std::optional<ExprPtr> having;
    std::vector<OrderSpec> order_by;
    std::optional<std::size_t> limit;
    std::optional<std::size_t> offset;
};

struct CompileContext {
    const Dialect& dialect;
    std::unordered_map<std::type_index, std::string> aliases;
    std::vector<Value>& params;
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

inline std::string aggregate_token(AggregateKind kind) {
    switch (kind) {
        case AggregateKind::Count: return "COUNT";
        case AggregateKind::Sum: return "SUM";
        case AggregateKind::Avg: return "AVG";
        case AggregateKind::Min: return "MIN";
        case AggregateKind::Max: return "MAX";
    }
    return "COUNT";
}

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

inline std::string compile_aggregate(const AggregateRef& aggregate, const CompileContext& ctx) {
    std::string out = aggregate_token(aggregate.kind) + "(";
    if (aggregate.distinct) out += "DISTINCT ";
    out += aggregate.column ? compile_column(*aggregate.column, ctx) : "*";
    out += ")";
    return out;
}

inline std::string compile_expression(const ExprPtr& expression, CompileContext& ctx) {
    return std::visit([&](const auto& node) -> std::string {
        using N = std::remove_cvref_t<decltype(node)>;
        if constexpr (std::same_as<N, ValueComparisonNode>) {
            ctx.params.push_back(node.value);
            return compile_column(node.column, ctx) + " " + compare_token(node.op) + " " +
                   ctx.dialect.placeholder(ctx.params.size());
        } else if constexpr (std::same_as<N, ColumnComparisonNode>) {
            return compile_column(node.left, ctx) + " " + compare_token(node.op) + " " +
                   compile_column(node.right, ctx);
        } else if constexpr (std::same_as<N, NullCheckNode>) {
            return compile_column(node.column, ctx) + (node.negated ? " IS NOT NULL" : " IS NULL");
        } else if constexpr (std::same_as<N, LikeNode>) {
            ctx.params.push_back(node.pattern);
            return compile_column(node.column, ctx) + (node.negated ? " NOT LIKE " : " LIKE ") +
                   ctx.dialect.placeholder(ctx.params.size());
        } else if constexpr (std::same_as<N, InValuesNode>) {
            if (node.values.empty()) return node.negated ? "1 = 1" : "0 = 1";
            std::string out = compile_column(node.column, ctx) + (node.negated ? " NOT IN (" : " IN (");
            for (std::size_t i = 0; i < node.values.size(); ++i) {
                if (i) out += ", ";
                ctx.params.push_back(node.values[i]);
                out += ctx.dialect.placeholder(ctx.params.size());
            }
            out += ")";
            return out;
        } else if constexpr (std::same_as<N, InSubqueryNode>) {
            auto sub = node.compile_subquery(ctx.dialect);
            ctx.params.insert(ctx.params.end(), sub.params.begin(), sub.params.end());
            return compile_column(node.column, ctx) + (node.negated ? " NOT IN (" : " IN (") +
                   sub.sql + ")";
        } else if constexpr (std::same_as<N, AggregateComparisonNode>) {
            ctx.params.push_back(node.value);
            return compile_aggregate(node.aggregate, ctx) + " " + compare_token(node.op) + " " +
                   ctx.dialect.placeholder(ctx.params.size());
        } else if constexpr (std::same_as<N, LogicalNode>) {
            const auto left = compile_expression(node.left, ctx);
            const auto right = compile_expression(node.right, ctx);
            return "(" + left + (node.op == LogicOp::And ? " AND " : " OR ") + right + ")";
        } else {
            return "NOT (" + compile_expression(node.operand, ctx) + ")";
        }
    }, expression->node);
}

template <std::meta::info Relation, mapping::relation_kind Kind>
struct relation_target_selector;

template <std::meta::info Relation>
struct relation_target_selector<Relation, mapping::relation_kind::belongs_to> {
    using type = reflect::single_target_t<reflect::member_type_t<Relation>>;
};

template <std::meta::info Relation>
struct relation_target_selector<Relation, mapping::relation_kind::has_one> {
    using type = reflect::single_target_t<reflect::member_type_t<Relation>>;
};

template <std::meta::info Relation>
struct relation_target_selector<Relation, mapping::relation_kind::has_many> {
    using type = reflect::many_target_t<reflect::member_type_t<Relation>>;
};

template <std::meta::info Relation>
struct relation_target_selector<Relation, mapping::relation_kind::many_to_many> {
    using type = reflect::many_target_t<reflect::member_type_t<Relation>>;
};

template <std::meta::info Relation>
using relation_target_t = typename relation_target_selector<
    Relation,
    mapping::relation_annotation_traits<reflect::relation_annotation_t<Relation>>::kind>::type;

template <std::meta::info Relation>
JoinSpec make_join_spec(JoinKind kind) {
    using Owner = reflect::owner_type_t<Relation>;
    using Target = relation_target_t<Relation>;
    using A = reflect::relation_annotation_t<Relation>;
    using Traits = mapping::relation_annotation_traits<A>;

    JoinSpec spec;
    spec.kind = kind;
    spec.relation_kind = Traits::kind;
    spec.owner_type = std::type_index(typeid(Owner));
    spec.target_type = std::type_index(typeid(Target));
    spec.target_table = reflect::table_name<Target>();

    if constexpr (Traits::kind == mapping::relation_kind::belongs_to) {
        constexpr auto foreign_key = Traits::foreign_key();
        constexpr auto target_key = reflect::key_or_primary<Target>(Traits::target_key());
        spec.owner_column = reflect::column_name<foreign_key>();
        spec.target_column = reflect::column_name<target_key>();
    } else if constexpr (Traits::kind == mapping::relation_kind::has_one ||
                         Traits::kind == mapping::relation_kind::has_many) {
        constexpr auto target_fk = Traits::target_foreign_key();
        constexpr auto local_key = reflect::key_or_primary<Owner>(Traits::local_key());
        spec.owner_column = reflect::column_name<local_key>();
        spec.target_column = reflect::column_name<target_fk>();
    } else {
        constexpr auto pivot_reflection = Traits::pivot();
        using Pivot = [: pivot_reflection :];
        constexpr auto pivot_root_fk = Traits::pivot_root_foreign_key();
        constexpr auto pivot_target_fk = Traits::pivot_target_foreign_key();
        constexpr auto local_key = reflect::key_or_primary<Owner>(Traits::local_key());
        constexpr auto target_key = reflect::key_or_primary<Target>(Traits::target_key());
        spec.owner_column = reflect::column_name<local_key>();
        spec.target_column = reflect::column_name<target_key>();
        spec.pivot_table = reflect::table_name<Pivot>();
        spec.pivot_root_column = reflect::column_name<pivot_root_fk>();
        spec.pivot_target_column = reflect::column_name<pivot_target_fk>();
    }
    return spec;
}

template <reflect::Entity Root, typename... Scope>
class BasicSelectQuery {
public:
    BasicSelectQuery() : state_(std::make_shared<SelectState>()) {}

    BasicSelectQuery& distinct(bool enabled = true) {
        state_->distinct = enabled;
        return *this;
    }

    template <typename... Owners>
    requires ((type_in_pack_v<Owners, Scope...>) && ...)
    BasicSelectQuery& where(Expression<Owners...> expression) {
        state_->where = expression.node();
        return *this;
    }

    template <typename... Owners>
    requires ((type_in_pack_v<Owners, Scope...>) && ...)
    BasicSelectQuery& having(Expression<Owners...> expression) {
        state_->having = expression.node();
        return *this;
    }

    template <std::meta::info Member>
    requires type_in_pack_v<typename Field<Member>::owner_type, Scope...>
    BasicSelectQuery& group_by(Field<Member> f) {
        state_->group_by.push_back(column_ref(f));
        return *this;
    }

    template <std::meta::info Member>
    requires type_in_pack_v<typename Field<Member>::owner_type, Scope...>
    BasicSelectQuery& order_by(Field<Member> f, bool ascending = true) {
        state_->order_by.push_back(OrderSpec{column_ref(f), ascending});
        return *this;
    }

    BasicSelectQuery& limit(std::size_t value) {
        state_->limit = value;
        return *this;
    }

    BasicSelectQuery& offset(std::size_t value) {
        state_->offset = value;
        return *this;
    }

    BasicSelectQuery& clear_projection() {
        state_->projections.clear();
        return *this;
    }

    template <std::meta::info Member>
    requires type_in_pack_v<typename Field<Member>::owner_type, Scope...>
    BasicSelectQuery& project(Field<Member> f) {
        state_->projections.push_back(ProjectionSpec{column_ref(f), std::nullopt});
        return *this;
    }

    template <std::meta::info Member>
    requires type_in_pack_v<typename Field<Member>::owner_type, Scope...>
    BasicSelectQuery& project_as(Field<Member> f, std::string alias) {
        state_->projections.push_back(ProjectionSpec{column_ref(f), std::move(alias)});
        return *this;
    }

    template <typename Owner>
    requires type_in_pack_v<Owner, Scope...>
    BasicSelectQuery& project(AggregateTerm<Owner> aggregate) {
        state_->projections.push_back(ProjectionSpec{aggregate.aggregate(), aggregate.alias()});
        return *this;
    }

    template <std::meta::info Relation>
    auto join() const {
        return join_impl<Relation>(JoinKind::Inner);
    }

    template <std::meta::info Relation>
    auto left_join() const {
        return join_impl<Relation>(JoinKind::Left);
    }

    BasicSelectQuery& where_column(std::string column, CompareOp op, Value value) {
        state_->where = Expression<Root>{ValueComparisonNode{
            ColumnRef{std::type_index(typeid(Root)), std::move(column)}, op, std::move(value)}}.node();
        return *this;
    }

    CompiledQuery compile(const Dialect& dialect) const {
        return compile_impl(dialect, true);
    }

    CompiledQuery compile_subquery(const Dialect& dialect) const {
        return compile_impl(dialect, false);
    }

    CompiledQuery compile_scalar_subquery(const Dialect& dialect) const {
        if (state_->projections.size() != 1) {
            throw std::logic_error("MetalORM: scalar subquery must project exactly one expression");
        }
        return compile_impl(dialect, false);
    }

private:
    template <reflect::Entity, typename...>
    friend class BasicSelectQuery;

    explicit BasicSelectQuery(std::shared_ptr<SelectState> state) : state_(std::move(state)) {}

    template <std::meta::info Relation>
    auto join_impl(JoinKind kind) const {
        static_assert(reflect::has_relation_annotation<Relation>(),
                      "MetalORM: join<> requires a reflected relationship member");
        using Owner = reflect::owner_type_t<Relation>;
        using Target = relation_target_t<Relation>;
        static_assert(type_in_pack_v<Owner, Scope...>,
                      "MetalORM: joined relation owner must already be in query scope");
        static_assert(!type_in_pack_v<Target, Scope...>,
                      "MetalORM 0.0.4: the same entity type cannot be joined twice in one typed scope");
        static_assert(reflect::validate_mapping<Owner>());
        static_assert(reflect::validate_mapping<Target>());

        auto next = std::make_shared<SelectState>(*state_);
        next->joins.push_back(make_join_spec<Relation>(kind));
        return BasicSelectQuery<Root, Scope..., Target>{std::move(next)};
    }

    CompiledQuery compile_impl(const Dialect& dialect, bool terminate) const {
        CompiledQuery out;
        CompileContext ctx{dialect, {}, out.params};
        const bool has_joins = !state_->joins.empty();
        ctx.aliases.emplace(std::type_index(typeid(Root)), has_joins ? "t0" : "");

        std::vector<std::string> join_sql;
        std::size_t entity_alias = 1;
        std::size_t pivot_alias = 0;
        for (const auto& join : state_->joins) {
            const auto owner_it = ctx.aliases.find(join.owner_type);
            if (owner_it == ctx.aliases.end()) {
                throw std::logic_error("MetalORM: join owner was not introduced before its relation");
            }
            const std::string owner_alias = owner_it->second;
            const std::string target_alias = "t" + std::to_string(entity_alias++);
            ctx.aliases.emplace(join.target_type, target_alias);
            const std::string keyword = join.kind == JoinKind::Left ? " LEFT JOIN " : " INNER JOIN ";

            if (join.relation_kind == mapping::relation_kind::many_to_many) {
                const std::string pivot_alias_name = "p" + std::to_string(pivot_alias++);
                join_sql.push_back(
                    keyword + dialect.quote_identifier(*join.pivot_table) + " AS " +
                    dialect.quote_identifier(pivot_alias_name) + " ON " +
                    dialect.quote_identifier(pivot_alias_name) + "." +
                    dialect.quote_identifier(*join.pivot_root_column) + " = " +
                    dialect.quote_identifier(owner_alias) + "." +
                    dialect.quote_identifier(join.owner_column));
                join_sql.push_back(
                    keyword + dialect.quote_identifier(join.target_table) + " AS " +
                    dialect.quote_identifier(target_alias) + " ON " +
                    dialect.quote_identifier(target_alias) + "." +
                    dialect.quote_identifier(join.target_column) + " = " +
                    dialect.quote_identifier(pivot_alias_name) + "." +
                    dialect.quote_identifier(*join.pivot_target_column));
            } else {
                join_sql.push_back(
                    keyword + dialect.quote_identifier(join.target_table) + " AS " +
                    dialect.quote_identifier(target_alias) + " ON " +
                    dialect.quote_identifier(owner_alias) + "." +
                    dialect.quote_identifier(join.owner_column) + " = " +
                    dialect.quote_identifier(target_alias) + "." +
                    dialect.quote_identifier(join.target_column));
            }
        }

        out.sql = "SELECT ";
        if (state_->distinct) out.sql += "DISTINCT ";

        if (state_->projections.empty()) {
            bool first = true;
            reflect::for_each_column<Root>([&]<std::meta::info Member>() {
                if (!first) out.sql += ", ";
                first = false;
                const auto name = reflect::column_name<Member>();
                if (has_joins) {
                    out.sql += dialect.quote_identifier("t0") + "." + dialect.quote_identifier(name) +
                               " AS " + dialect.quote_identifier(name);
                } else {
                    out.sql += dialect.quote_identifier(name);
                }
            });
        } else {
            for (std::size_t i = 0; i < state_->projections.size(); ++i) {
                if (i) out.sql += ", ";
                const auto& projection = state_->projections[i];
                if (const auto* column = std::get_if<ColumnRef>(&projection.expression)) {
                    out.sql += compile_column(*column, ctx);
                } else {
                    out.sql += compile_aggregate(std::get<AggregateRef>(projection.expression), ctx);
                }
                if (projection.alias) {
                    out.sql += " AS " + dialect.quote_identifier(*projection.alias);
                }
            }
        }

        out.sql += " FROM " + dialect.quote_identifier(reflect::table_name<Root>());
        if (has_joins) out.sql += " AS " + dialect.quote_identifier("t0");
        for (const auto& clause : join_sql) out.sql += clause;

        if (state_->where) {
            out.sql += " WHERE " + compile_expression(*state_->where, ctx);
        }
        if (!state_->group_by.empty()) {
            out.sql += " GROUP BY ";
            for (std::size_t i = 0; i < state_->group_by.size(); ++i) {
                if (i) out.sql += ", ";
                out.sql += compile_column(state_->group_by[i], ctx);
            }
        }
        if (state_->having) {
            out.sql += " HAVING " + compile_expression(*state_->having, ctx);
        }
        if (!state_->order_by.empty()) {
            out.sql += " ORDER BY ";
            for (std::size_t i = 0; i < state_->order_by.size(); ++i) {
                if (i) out.sql += ", ";
                out.sql += compile_column(state_->order_by[i].column, ctx) +
                           (state_->order_by[i].ascending ? " ASC" : " DESC");
            }
        }
        if (state_->limit) out.sql += " LIMIT " + std::to_string(*state_->limit);
        if (state_->offset) out.sql += " OFFSET " + std::to_string(*state_->offset);
        if (terminate) out.sql += ";";
        return out;
    }

    std::shared_ptr<SelectState> state_;
};

template <reflect::Entity T>
using SelectQuery = BasicSelectQuery<T, T>;

template <reflect::Entity T>
SelectQuery<T> select() { return {}; }

template <std::meta::info Member, reflect::Entity Root, typename... Scope>
auto in(Field<Member> f, BasicSelectQuery<Root, Scope...> query) {
    using Owner = typename Field<Member>::owner_type;
    return Expression<Owner>{InSubqueryNode{
        column_ref(f),
        [query = std::move(query)](const Dialect& dialect) mutable {
            return query.compile_scalar_subquery(dialect);
        },
        false}};
}

template <std::meta::info Member, reflect::Entity Root, typename... Scope>
auto not_in(Field<Member> f, BasicSelectQuery<Root, Scope...> query) {
    using Owner = typename Field<Member>::owner_type;
    return Expression<Owner>{InSubqueryNode{
        column_ref(f),
        [query = std::move(query)](const Dialect& dialect) mutable {
            return query.compile_scalar_subquery(dialect);
        },
        true}};
}

} // namespace metal
