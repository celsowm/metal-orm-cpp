#pragma once

#include "metal/dto.hpp"
#include "metal/query.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace metal {

enum class FilterOperator {
    equals,
    not_equals,
    in,
    not_in,
    lt,
    lte,
    gt,
    gte,
    contains,
    starts_with,
    ends_with,
    is_null,
    is_not_null
};

enum class StringFilterMode {
    default_mode,
    insensitive
};

struct FilterClause {
    std::string field;
    FilterOperator op{FilterOperator::equals};
    Value value{nullptr};
    std::vector<Value> values;
    StringFilterMode mode{StringFilterMode::default_mode};
};

struct FilterInput {
    std::vector<FilterClause> clauses;
};

inline FilterClause filter_clause(
    std::string field,
    FilterOperator op,
    Value value,
    StringFilterMode mode = StringFilterMode::default_mode) {
    return FilterClause{std::move(field), op, std::move(value), {}, mode};
}

inline FilterClause filter_list_clause(
    std::string field,
    FilterOperator op,
    std::vector<Value> values) {
    if (op != FilterOperator::in && op != FilterOperator::not_in) {
        throw std::invalid_argument("MetalORM: list filter requires IN or NOT IN");
    }
    return FilterClause{std::move(field), op, Value{nullptr}, std::move(values), StringFilterMode::default_mode};
}

inline FilterClause null_filter_clause(std::string field, bool negated = false) {
    return FilterClause{
        std::move(field),
        negated ? FilterOperator::is_not_null : FilterOperator::is_null,
        Value{nullptr},
        {},
        StringFilterMode::default_mode
    };
}

namespace detail {

template <std::meta::info Member, std::meta::info... Allowed>
consteval bool filter_member_allowed() {
    if constexpr (sizeof...(Allowed) == 0) return true;
    return ((Member == Allowed) || ...);
}

inline ScalarPtr filter_literal(Value value) {
    return std::make_shared<ScalarNode>(ScalarNode{std::move(value)});
}

inline std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

template <typename M>
Value normalize_filter_value(const Value& value) {
    if (std::holds_alternative<std::nullptr_t>(value)) return Value{nullptr};
    return to_value(from_value<M>(value));
}

template <std::meta::info Member>
ExprPtr build_member_filter(const FilterClause& clause) {
    using M = reflect::member_type_t<Member>;
    using U = optional_value_t<M>;
    const auto operand = as_scalar(field<Member>).node();

    const auto comparison = [&](CompareOp op) -> ExprPtr {
        if (std::holds_alternative<std::nullptr_t>(clause.value)) {
            if (op == CompareOp::Eq) {
                return std::make_shared<ExprNode>(ExprNode{NullCheckNode{operand, false}});
            }
            if (op == CompareOp::Ne) {
                return std::make_shared<ExprNode>(ExprNode{NullCheckNode{operand, true}});
            }
            throw std::invalid_argument("MetalORM: ordered REST filters cannot compare against null");
        }
        auto normalized = normalize_filter_value<M>(clause.value);
        return std::make_shared<ExprNode>(ExprNode{
            ComparisonNode{operand, op, filter_literal(std::move(normalized))}});
    };

    switch (clause.op) {
        case FilterOperator::equals:
            return comparison(CompareOp::Eq);
        case FilterOperator::not_equals:
            return comparison(CompareOp::Ne);
        case FilterOperator::lt:
            return comparison(CompareOp::Lt);
        case FilterOperator::lte:
            return comparison(CompareOp::Le);
        case FilterOperator::gt:
            return comparison(CompareOp::Gt);
        case FilterOperator::gte:
            return comparison(CompareOp::Ge);
        case FilterOperator::is_null:
            return std::make_shared<ExprNode>(ExprNode{NullCheckNode{operand, false}});
        case FilterOperator::is_not_null:
            return std::make_shared<ExprNode>(ExprNode{NullCheckNode{operand, true}});
        case FilterOperator::in:
        case FilterOperator::not_in: {
            std::vector<Value> values;
            values.reserve(clause.values.size());
            for (const auto& value : clause.values) {
                if (std::holds_alternative<std::nullptr_t>(value)) {
                    throw std::invalid_argument("MetalORM: IN/NOT IN REST filters do not accept null; use is_null/is_not_null");
                }
                values.push_back(normalize_filter_value<M>(value));
            }
            return std::make_shared<ExprNode>(ExprNode{InValuesNode{
                operand,
                std::move(values),
                clause.op == FilterOperator::not_in
            }});
        }
        case FilterOperator::contains:
        case FilterOperator::starts_with:
        case FilterOperator::ends_with: {
            if constexpr (!std::same_as<U, std::string>) {
                throw std::invalid_argument("MetalORM: contains/starts_with/ends_with require a string DTO field");
            } else {
                if (std::holds_alternative<std::nullptr_t>(clause.value)) {
                    throw std::invalid_argument("MetalORM: string REST filters cannot compare against null");
                }
                auto text = from_value<std::string>(clause.value);
                if (clause.op == FilterOperator::contains) text = "%" + text + "%";
                else if (clause.op == FilterOperator::starts_with) text += "%";
                else text = "%" + text;

                ScalarPtr string_operand = operand;
                if (clause.mode == StringFilterMode::insensitive) {
                    string_operand = lower(field<Member>).node();
                    text = ascii_lower(std::move(text));
                }
                return std::make_shared<ExprNode>(ExprNode{LikeNode{
                    std::move(string_operand),
                    filter_literal(Value{std::move(text)}),
                    false
                }});
            }
        }
    }
    throw std::logic_error("MetalORM: unknown REST filter operator");
}

inline ExprPtr and_filter_nodes(ExprPtr left, ExprPtr right) {
    if (!left) return right;
    if (!right) return left;
    return std::make_shared<ExprNode>(ExprNode{LogicalNode{
        LogicOp::And,
        std::move(left),
        std::move(right)
    }});
}

} // namespace detail

template <reflect::Entity T, std::meta::info... Allowed>
std::optional<Expression<T>> build_filter_expression(const FilterInput& input) {
    static_assert(detail::validate_dto_members<T, Allowed...>());

    ExprPtr combined;
    for (const auto& clause : input.clauses) {
        bool known = false;
        bool allowed = false;
        ExprPtr node;

        reflect::for_each_column<T>([&]<std::meta::info Member>() {
            if (known) return;
            if (detail::dto_member_name<Member>() != clause.field) return;
            known = true;
            if constexpr (detail::filter_member_allowed<Member, Allowed...>()) {
                allowed = true;
                node = detail::build_member_filter<Member>(clause);
            }
        });

        if (!known) {
            throw std::invalid_argument(
                "MetalORM: unknown REST filter field '" + clause.field + "'");
        }
        if (!allowed) {
            throw std::invalid_argument(
                "MetalORM: REST filter field '" + clause.field + "' is not allowed by the reflected filter policy");
        }
        combined = detail::and_filter_nodes(std::move(combined), std::move(node));
    }

    if (!combined) return std::nullopt;
    return Expression<T>{std::move(combined)};
}

template <std::meta::info... Allowed, reflect::Entity T>
SelectQuery<T> apply_filter(SelectQuery<T> query, const FilterInput& input) {
    auto expression = build_filter_expression<T, Allowed...>(input);
    if (expression) query.where(std::move(*expression));
    return query;
}

} // namespace metal
