#pragma once

#include "metal/query/expressions.hpp"

#include <optional>
#include <type_traits>
#include <utility>

namespace metal {

template <typename T>
concept ArithmeticScalarInput = ScalarInput<T> &&
    std::is_arithmetic_v<optional_value_t<scalar_input_result_t<T>>> &&
    (!std::same_as<optional_value_t<scalar_input_result_t<T>>, bool>);

template <typename Left, typename Right>
using arithmetic_common_t = std::common_type_t<
    optional_value_t<scalar_input_result_t<Left>>,
    optional_value_t<scalar_input_result_t<Right>>>;

template <typename Left, typename Right>
using arithmetic_result_t = std::conditional_t<
    is_optional_v<scalar_input_result_t<Left>> || is_optional_v<scalar_input_result_t<Right>>,
    std::optional<arithmetic_common_t<Left, Right>>,
    arithmetic_common_t<Left, Right>>;

template <ArithmeticScalarInput Left, ArithmeticScalarInput Right>
auto arithmetic(Left&& left, ArithmeticOp op, Right&& right) {
    auto lhs = as_scalar(std::forward<Left>(left));
    auto rhs = as_scalar(std::forward<Right>(right));
    using Result = arithmetic_result_t<Left, Right>;
    using Owners = type_list_concat_many_t<
        scalar_input_owners_t<Left>,
        scalar_input_owners_t<Right>>;

    auto node = std::make_shared<ScalarNode>(ScalarNode{
        ArithmeticRef{lhs.node(), op, rhs.node()}});
    return scalar_from_list<Result>(std::move(node), Owners{});
}

template <ArithmeticScalarInput Left, ArithmeticScalarInput Right>
auto add(Left&& left, Right&& right) {
    return arithmetic(std::forward<Left>(left), ArithmeticOp::Add, std::forward<Right>(right));
}

template <ArithmeticScalarInput Left, ArithmeticScalarInput Right>
auto subtract(Left&& left, Right&& right) {
    return arithmetic(std::forward<Left>(left), ArithmeticOp::Subtract, std::forward<Right>(right));
}

template <ArithmeticScalarInput Left, ArithmeticScalarInput Right>
auto multiply(Left&& left, Right&& right) {
    return arithmetic(std::forward<Left>(left), ArithmeticOp::Multiply, std::forward<Right>(right));
}

template <ArithmeticScalarInput Left, ArithmeticScalarInput Right>
auto divide(Left&& left, Right&& right) {
    return arithmetic(std::forward<Left>(left), ArithmeticOp::Divide, std::forward<Right>(right));
}

template <ArithmeticScalarInput Left, ArithmeticScalarInput Right>
requires std::is_integral_v<optional_value_t<scalar_input_result_t<Left>>> &&
         std::is_integral_v<optional_value_t<scalar_input_result_t<Right>>>
auto modulo(Left&& left, Right&& right) {
    return arithmetic(std::forward<Left>(left), ArithmeticOp::Modulo, std::forward<Right>(right));
}

template <ArithmeticScalarInput Left, ArithmeticScalarInput Right>
requires (!ValueInput<std::remove_cvref_t<Left>> || !ValueInput<std::remove_cvref_t<Right>>)
auto operator+(Left&& left, Right&& right) {
    return add(std::forward<Left>(left), std::forward<Right>(right));
}

template <ArithmeticScalarInput Left, ArithmeticScalarInput Right>
requires (!ValueInput<std::remove_cvref_t<Left>> || !ValueInput<std::remove_cvref_t<Right>>)
auto operator-(Left&& left, Right&& right) {
    return subtract(std::forward<Left>(left), std::forward<Right>(right));
}

template <ArithmeticScalarInput Left, ArithmeticScalarInput Right>
requires (!ValueInput<std::remove_cvref_t<Left>> || !ValueInput<std::remove_cvref_t<Right>>)
auto operator*(Left&& left, Right&& right) {
    return multiply(std::forward<Left>(left), std::forward<Right>(right));
}

template <ArithmeticScalarInput Left, ArithmeticScalarInput Right>
requires (!ValueInput<std::remove_cvref_t<Left>> || !ValueInput<std::remove_cvref_t<Right>>)
auto operator/(Left&& left, Right&& right) {
    return divide(std::forward<Left>(left), std::forward<Right>(right));
}

template <ArithmeticScalarInput Left, ArithmeticScalarInput Right>
requires (!ValueInput<std::remove_cvref_t<Left>> || !ValueInput<std::remove_cvref_t<Right>>) &&
         std::is_integral_v<optional_value_t<scalar_input_result_t<Left>>> &&
         std::is_integral_v<optional_value_t<scalar_input_result_t<Right>>>
auto operator%(Left&& left, Right&& right) {
    return modulo(std::forward<Left>(left), std::forward<Right>(right));
}

} // namespace metal