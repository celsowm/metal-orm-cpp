#pragma once

#include "metal/query/core.hpp"

#include <ranges>

namespace metal {

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

template <typename T>
concept ValueInput = PersistableValue<T> || std::is_convertible_v<T, std::string_view>;

template <typename T>
using literal_result_t = std::conditional_t<
    std::is_convertible_v<T, std::string_view>,
    std::string,
    std::remove_cvref_t<T>>;

template <typename Member, typename V>
concept FieldComparable = [] {
    using M = optional_value_t<Member>;
    using R = optional_value_t<std::remove_cvref_t<V>>;
    if constexpr (std::is_same_v<M, std::string>) {
        return std::is_convertible_v<V, std::string_view> || std::is_same_v<R, std::string>;
    } else if constexpr (std::is_same_v<M, bool>) {
        return std::is_same_v<R, bool> || std::is_integral_v<R>;
    } else if constexpr (std::is_arithmetic_v<M>) {
        return std::is_arithmetic_v<R>;
    } else {
        return std::same_as<M, R>;
    }
}();

template <typename Left, typename Right>
inline constexpr bool scalar_results_compatible_v = [] {
    using L = optional_value_t<Left>;
    using R = optional_value_t<Right>;
    if constexpr (std::same_as<L, std::string> && std::same_as<R, std::string>) return true;
    else if constexpr (std::is_arithmetic_v<L> && std::is_arithmetic_v<R>) return true;
    else return std::same_as<L, R>;
}();

template <std::meta::info Member>
auto as_scalar(Field<Member> f = {}) {
    using Owner = typename Field<Member>::owner_type;
    using Result = typename Field<Member>::value_type;
    return ScalarTerm<Result, Owner>{column_ref(f)};
}

template <typename Result, typename... Owners>
auto as_scalar(ScalarTerm<Result, Owners...> value) {
    return value;
}

template <ValueInput V>
auto as_scalar(V&& value) {
    using Result = literal_result_t<V>;
    return ScalarTerm<Result>{Value{to_value(std::forward<V>(value))}};
}

template <typename T, typename = void>
struct scalar_input_traits;

template <typename T>
struct scalar_input_traits<T, std::enable_if_t<ValueInput<T>>> {
    using result_type = literal_result_t<T>;
    using owners = type_list<>;
};

template <std::meta::info Member>
struct scalar_input_traits<Field<Member>, void> {
    using result_type = typename Field<Member>::value_type;
    using owners = type_list<typename Field<Member>::owner_type>;
};

template <typename Result, typename... Owners>
struct scalar_input_traits<ScalarTerm<Result, Owners...>, void> {
    using result_type = Result;
    using owners = type_list<Owners...>;
};

template <typename T>
concept ScalarInput = requires {
    typename scalar_input_traits<std::remove_cvref_t<T>>::result_type;
    typename scalar_input_traits<std::remove_cvref_t<T>>::owners;
};

template <typename T>
using scalar_input_result_t = typename scalar_input_traits<std::remove_cvref_t<T>>::result_type;

template <typename T>
using scalar_input_owners_t = typename scalar_input_traits<std::remove_cvref_t<T>>::owners;

template <typename Result, typename... Owners>
auto scalar_from_list(ScalarPtr node, type_list<Owners...>) {
    return ScalarTerm<Result, Owners...>{std::move(node)};
}

template <typename Result, typename... LeftOwners, typename... RightOwners>
auto compare(
    ScalarTerm<Result, LeftOwners...> left,
    CompareOp op,
    ScalarTerm<Result, RightOwners...> right) {
    return Expression<LeftOwners..., RightOwners...>{ComparisonNode{left.node(), op, right.node()}};
}

template <typename LeftResult, typename... LeftOwners, typename RightResult, typename... RightOwners>
requires scalar_results_compatible_v<LeftResult, RightResult>
auto compare(
    ScalarTerm<LeftResult, LeftOwners...> left,
    CompareOp op,
    ScalarTerm<RightResult, RightOwners...> right) {
    return Expression<LeftOwners..., RightOwners...>{ComparisonNode{left.node(), op, right.node()}};
}

template <typename Result, typename... Owners, ValueInput V>
requires FieldComparable<Result, V>
auto compare(ScalarTerm<Result, Owners...> left, CompareOp op, V&& value) {
    auto right = as_scalar(std::forward<V>(value));
    return Expression<Owners...>{ComparisonNode{left.node(), op, right.node()}};
}

template <std::meta::info Member, typename V>
requires ValueInput<V> && FieldComparable<typename Field<Member>::value_type, V>
auto compare(Field<Member> f, CompareOp op, V&& value) {
    return compare(as_scalar(f), op, std::forward<V>(value));
}

template <std::meta::info Left, std::meta::info Right>
requires (reflect::key_types_compatible<Left, Right>())
auto compare(Field<Left> left, CompareOp op, Field<Right> right) {
    using L = typename Field<Left>::owner_type;
    using R = typename Field<Right>::owner_type;
    return Expression<L, R>{ComparisonNode{as_scalar(left).node(), op, as_scalar(right).node()}};
}

#define METAL_DEFINE_FIELD_VALUE_COMPARE(OP, ENUM) \
    template <std::meta::info Member, typename V> \
    requires ValueInput<V> && FieldComparable<typename Field<Member>::value_type, V> \
    auto operator OP(Field<Member> f, V&& value) { return compare(f, CompareOp::ENUM, std::forward<V>(value)); }

METAL_DEFINE_FIELD_VALUE_COMPARE(==, Eq)
METAL_DEFINE_FIELD_VALUE_COMPARE(!=, Ne)
METAL_DEFINE_FIELD_VALUE_COMPARE(>, Gt)
METAL_DEFINE_FIELD_VALUE_COMPARE(>=, Ge)
METAL_DEFINE_FIELD_VALUE_COMPARE(<, Lt)
METAL_DEFINE_FIELD_VALUE_COMPARE(<=, Le)

#undef METAL_DEFINE_FIELD_VALUE_COMPARE

template <std::meta::info Left, std::meta::info Right>
requires (reflect::key_types_compatible<Left, Right>())
auto operator==(Field<Left> left, Field<Right> right) { return compare(left, CompareOp::Eq, right); }

template <std::meta::info Left, std::meta::info Right>
requires (reflect::key_types_compatible<Left, Right>())
auto operator!=(Field<Left> left, Field<Right> right) { return compare(left, CompareOp::Ne, right); }

#define METAL_DEFINE_SCALAR_COMPARE(OP, ENUM) \
    template <typename L, typename... LO, typename R, typename... RO> \
    requires scalar_results_compatible_v<L, R> \
    auto operator OP(ScalarTerm<L, LO...> left, ScalarTerm<R, RO...> right) { \
        return compare(left, CompareOp::ENUM, right); \
    } \
    template <typename L, typename... LO, ValueInput V> \
    requires FieldComparable<L, V> \
    auto operator OP(ScalarTerm<L, LO...> left, V&& value) { \
        return compare(left, CompareOp::ENUM, std::forward<V>(value)); \
    }

METAL_DEFINE_SCALAR_COMPARE(==, Eq)
METAL_DEFINE_SCALAR_COMPARE(!=, Ne)
METAL_DEFINE_SCALAR_COMPARE(>, Gt)
METAL_DEFINE_SCALAR_COMPARE(>=, Ge)
METAL_DEFINE_SCALAR_COMPARE(<, Lt)
METAL_DEFINE_SCALAR_COMPARE(<=, Le)

#undef METAL_DEFINE_SCALAR_COMPARE

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

template <typename Result, typename... Owners>
auto is_null(ScalarTerm<Result, Owners...> value) {
    return Expression<Owners...>{NullCheckNode{value.node(), false}};
}

template <typename Result, typename... Owners>
auto is_not_null(ScalarTerm<Result, Owners...> value) {
    return Expression<Owners...>{NullCheckNode{value.node(), true}};
}

template <std::meta::info Member>
auto is_null(Field<Member> f) { return is_null(as_scalar(f)); }

template <std::meta::info Member>
auto is_not_null(Field<Member> f) { return is_not_null(as_scalar(f)); }

template <typename... Owners, typename Pattern>
requires std::is_convertible_v<Pattern, std::string_view>
auto like(ScalarTerm<std::string, Owners...> value, Pattern&& pattern) {
    return Expression<Owners...>{LikeNode{
        value.node(), as_scalar(std::string_view(std::forward<Pattern>(pattern))).node(), false}};
}

template <typename... Owners, typename Pattern>
requires std::is_convertible_v<Pattern, std::string_view>
auto not_like(ScalarTerm<std::string, Owners...> value, Pattern&& pattern) {
    return Expression<Owners...>{LikeNode{
        value.node(), as_scalar(std::string_view(std::forward<Pattern>(pattern))).node(), true}};
}

template <std::meta::info Member, typename Pattern>
requires std::same_as<optional_value_t<typename Field<Member>::value_type>, std::string> &&
         std::is_convertible_v<Pattern, std::string_view>
auto like(Field<Member> f, Pattern&& pattern) {
    using Owner = typename Field<Member>::owner_type;
    return Expression<Owner>{LikeNode{
        as_scalar(f).node(), as_scalar(std::string_view(std::forward<Pattern>(pattern))).node(), false}};
}

template <std::meta::info Member, typename Pattern>
requires std::same_as<optional_value_t<typename Field<Member>::value_type>, std::string> &&
         std::is_convertible_v<Pattern, std::string_view>
auto not_like(Field<Member> f, Pattern&& pattern) {
    using Owner = typename Field<Member>::owner_type;
    return Expression<Owner>{LikeNode{
        as_scalar(f).node(), as_scalar(std::string_view(std::forward<Pattern>(pattern))).node(), true}};
}

template <typename Result, typename... Owners, typename Lower, typename Upper>
requires ValueInput<Lower> && ValueInput<Upper> &&
         FieldComparable<Result, Lower> && FieldComparable<Result, Upper>
auto between(ScalarTerm<Result, Owners...> value, Lower&& lower, Upper&& upper) {
    return Expression<Owners...>{BetweenNode{
        value.node(), as_scalar(std::forward<Lower>(lower)).node(),
        as_scalar(std::forward<Upper>(upper)).node(), false}};
}

template <typename Result, typename... Owners, typename Lower, typename Upper>
requires ValueInput<Lower> && ValueInput<Upper> &&
         FieldComparable<Result, Lower> && FieldComparable<Result, Upper>
auto not_between(ScalarTerm<Result, Owners...> value, Lower&& lower, Upper&& upper) {
    return Expression<Owners...>{BetweenNode{
        value.node(), as_scalar(std::forward<Lower>(lower)).node(),
        as_scalar(std::forward<Upper>(upper)).node(), true}};
}

template <std::meta::info Member, typename Lower, typename Upper>
requires ValueInput<Lower> && ValueInput<Upper> &&
         FieldComparable<typename Field<Member>::value_type, Lower> &&
         FieldComparable<typename Field<Member>::value_type, Upper>
auto between(Field<Member> f, Lower&& lower, Upper&& upper) {
    return between(as_scalar(f), std::forward<Lower>(lower), std::forward<Upper>(upper));
}

template <std::meta::info Member, typename Lower, typename Upper>
requires ValueInput<Lower> && ValueInput<Upper> &&
         FieldComparable<typename Field<Member>::value_type, Lower> &&
         FieldComparable<typename Field<Member>::value_type, Upper>
auto not_between(Field<Member> f, Lower&& lower, Upper&& upper) {
    return not_between(as_scalar(f), std::forward<Lower>(lower), std::forward<Upper>(upper));
}

template <std::meta::info Member, std::ranges::input_range Range>
requires FieldComparable<typename Field<Member>::value_type, std::ranges::range_value_t<Range>>
auto in(Field<Member> f, const Range& values) {
    using Owner = typename Field<Member>::owner_type;
    std::vector<Value> params;
    for (const auto& value : values) params.push_back(to_value(value));
    return Expression<Owner>{InValuesNode{as_scalar(f).node(), std::move(params), false}};
}

template <std::meta::info Member, std::ranges::input_range Range>
requires FieldComparable<typename Field<Member>::value_type, std::ranges::range_value_t<Range>>
auto not_in(Field<Member> f, const Range& values) {
    using Owner = typename Field<Member>::owner_type;
    std::vector<Value> params;
    for (const auto& value : values) params.push_back(to_value(value));
    return Expression<Owner>{InValuesNode{as_scalar(f).node(), std::move(params), true}};
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
    ScalarPtr node() const { return std::make_shared<ScalarNode>(ScalarNode{aggregate_}); }

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

template <std::meta::info Member>
requires std::is_arithmetic_v<optional_value_t<typename Field<Member>::value_type>>
auto stddev(Field<Member> f) {
    using Owner = typename Field<Member>::owner_type;
    return AggregateTerm<Owner>{AggregateRef{AggregateKind::Stddev, column_ref(f), false}};
}

template <std::meta::info Member>
requires std::is_arithmetic_v<optional_value_t<typename Field<Member>::value_type>>
auto variance(Field<Member> f) {
    using Owner = typename Field<Member>::owner_type;
    return AggregateTerm<Owner>{AggregateRef{AggregateKind::Variance, column_ref(f), false}};
}

template <typename Owner, typename V>
requires std::is_arithmetic_v<std::remove_cvref_t<V>>
auto compare(AggregateTerm<Owner> aggregate, CompareOp op, V&& value) {
    return Expression<Owner>{ComparisonNode{
        aggregate.node(), op, as_scalar(std::forward<V>(value)).node()}};
}

#define METAL_DEFINE_AGG_COMPARE(OP, ENUM) \
    template <typename Owner, typename V> \
    requires std::is_arithmetic_v<std::remove_cvref_t<V>> \
    auto operator OP(AggregateTerm<Owner> aggregate, V&& value) { \
        return compare(aggregate, CompareOp::ENUM, std::forward<V>(value)); \
    }

METAL_DEFINE_AGG_COMPARE(==, Eq)
METAL_DEFINE_AGG_COMPARE(!=, Ne)
METAL_DEFINE_AGG_COMPARE(>, Gt)
METAL_DEFINE_AGG_COMPARE(>=, Ge)
METAL_DEFINE_AGG_COMPARE(<, Lt)
METAL_DEFINE_AGG_COMPARE(<=, Le)

#undef METAL_DEFINE_AGG_COMPARE

template <typename... Owners>
class WindowTerm {
public:
    template <typename...>
    friend class WindowTerm;

    explicit WindowTerm(WindowRef window) : window_(std::move(window)) {}

    WindowTerm as(std::string alias) const {
        WindowTerm copy = *this;
        copy.alias_ = std::move(alias);
        return copy;
    }

    template <std::meta::info Member>
    auto partition_by(Field<Member> f) const {
        using Owner = typename Field<Member>::owner_type;
        auto copy = window_;
        copy.partition_by.push_back(column_ref(f));
        WindowTerm<Owners..., Owner> out{std::move(copy)};
        out.alias_ = alias_;
        return out;
    }

    template <std::meta::info Member>
    auto order_by(Field<Member> f, bool ascending = true) const {
        using Owner = typename Field<Member>::owner_type;
        auto copy = window_;
        copy.order_by.push_back(WindowOrderRef{column_ref(f), ascending});
        WindowTerm<Owners..., Owner> out{std::move(copy)};
        out.alias_ = alias_;
        return out;
    }

    const WindowRef& window() const noexcept { return window_; }
    const std::optional<std::string>& alias() const noexcept { return alias_; }
    ScalarPtr node() const { return std::make_shared<ScalarNode>(ScalarNode{window_}); }

private:
    WindowRef window_;
    std::optional<std::string> alias_;
};

inline auto row_number() { return WindowTerm<>{WindowRef{"ROW_NUMBER", {}, {}, {}}}; }
inline auto rank() { return WindowTerm<>{WindowRef{"RANK", {}, {}, {}}}; }
inline auto dense_rank() { return WindowTerm<>{WindowRef{"DENSE_RANK", {}, {}, {}}}; }
inline auto ntile(std::int64_t buckets) {
    return WindowTerm<>{WindowRef{"NTILE", {as_scalar(buckets).node()}, {}, {}}};
}

template <std::meta::info Member>
auto lag(Field<Member> f, std::int64_t offset = 1) {
    using Owner = typename Field<Member>::owner_type;
    return WindowTerm<Owner>{WindowRef{
        "LAG", {as_scalar(f).node(), as_scalar(offset).node()}, {}, {}}};
}

template <std::meta::info Member, typename Default>
requires ValueInput<Default> && FieldComparable<typename Field<Member>::value_type, Default>
auto lag(Field<Member> f, std::int64_t offset, Default&& value) {
    using Owner = typename Field<Member>::owner_type;
    return WindowTerm<Owner>{WindowRef{
        "LAG", {as_scalar(f).node(), as_scalar(offset).node(), as_scalar(std::forward<Default>(value)).node()}, {}, {}}};
}

template <std::meta::info Member>
auto lead(Field<Member> f, std::int64_t offset = 1) {
    using Owner = typename Field<Member>::owner_type;
    return WindowTerm<Owner>{WindowRef{
        "LEAD", {as_scalar(f).node(), as_scalar(offset).node()}, {}, {}}};
}

template <std::meta::info Member, typename Default>
requires ValueInput<Default> && FieldComparable<typename Field<Member>::value_type, Default>
auto lead(Field<Member> f, std::int64_t offset, Default&& value) {
    using Owner = typename Field<Member>::owner_type;
    return WindowTerm<Owner>{WindowRef{
        "LEAD", {as_scalar(f).node(), as_scalar(offset).node(), as_scalar(std::forward<Default>(value)).node()}, {}, {}}};
}

template <std::meta::info Member>
auto first_value(Field<Member> f) {
    using Owner = typename Field<Member>::owner_type;
    return WindowTerm<Owner>{WindowRef{"FIRST_VALUE", {as_scalar(f).node()}, {}, {}}};
}

template <std::meta::info Member>
auto last_value(Field<Member> f) {
    using Owner = typename Field<Member>::owner_type;
    return WindowTerm<Owner>{WindowRef{"LAST_VALUE", {as_scalar(f).node()}, {}, {}}};
}

inline auto window_function(std::string name) {
    validate_function_identifier(name);
    return WindowTerm<>{WindowRef{std::move(name), {}, {}, {}}};
}

template <typename Result, typename OwnerList>
struct case_builder_from_list;

template <typename Result, typename... Owners>
class CaseBuilder {
public:
    CaseBuilder() = default;
    explicit CaseBuilder(std::vector<CaseBranch> branches) : branches_(std::move(branches)) {}

    template <typename... CondOwners, ScalarInput Then>
    requires scalar_results_compatible_v<Result, scalar_input_result_t<Then>>
    auto when(Expression<CondOwners...> condition, Then&& then_value) const {
        auto scalar = as_scalar(std::forward<Then>(then_value));
        using NextOwners = type_list_concat_many_t<
            type_list<Owners...>,
            type_list<CondOwners...>,
            scalar_input_owners_t<Then>>;
        using NextBuilder = typename case_builder_from_list<Result, NextOwners>::type;
        auto branches = branches_;
        branches.push_back(CaseBranch{condition.node(), scalar.node()});
        return NextBuilder{std::move(branches)};
    }

    template <ScalarInput Else>
    requires scalar_results_compatible_v<Result, scalar_input_result_t<Else>>
    auto otherwise(Else&& else_value) const {
        auto scalar = as_scalar(std::forward<Else>(else_value));
        using NextOwners = type_list_concat_many_t<type_list<Owners...>, scalar_input_owners_t<Else>>;
        CaseRef node{branches_, scalar.node()};
        return scalar_from_list<Result>(
            std::make_shared<ScalarNode>(ScalarNode{std::move(node)}),
            NextOwners{});
    }

    auto end() const {
        CaseRef node{branches_, std::nullopt};
        return ScalarTerm<std::optional<Result>, Owners...>{std::move(node)};
    }

private:
    std::vector<CaseBranch> branches_;
};

template <typename Result, typename... Owners>
struct case_builder_from_list<Result, type_list<Owners...>> {
    using type = CaseBuilder<Result, Owners...>;
};

template <typename... CondOwners, ScalarInput Then>
auto case_when(Expression<CondOwners...> condition, Then&& then_value) {
    auto scalar = as_scalar(std::forward<Then>(then_value));
    using Result = scalar_input_result_t<Then>;
    using Owners = type_list_concat_many_t<
        type_list<CondOwners...>,
        scalar_input_owners_t<Then>>;
    using Builder = typename case_builder_from_list<Result, Owners>::type;
    return Builder{std::vector<CaseBranch>{CaseBranch{condition.node(), scalar.node()}}};
}

} // namespace metal
