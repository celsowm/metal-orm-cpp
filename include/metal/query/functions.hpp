#pragma once

#include "metal/query/expressions.hpp"

namespace metal {

template <typename Result, ScalarInput... Args>
auto sql_function(std::string name, Args&&... args) {
    validate_function_identifier(name);
    using Owners = type_list_concat_many_t<
        type_list<>, scalar_input_owners_t<Args>...>;
    std::vector<ScalarPtr> nodes;
    nodes.reserve(sizeof...(Args));
    (nodes.push_back(as_scalar(std::forward<Args>(args)).node()), ...);
    return scalar_from_list<Result>(
        std::make_shared<ScalarNode>(ScalarNode{FunctionRef{std::move(name), std::move(nodes)}}),
        Owners{});
}

template <typename T>
concept StringScalarInput = ScalarInput<T> &&
    std::same_as<optional_value_t<scalar_input_result_t<T>>, std::string>;

template <typename T>
concept NumericScalarInput = ScalarInput<T> &&
    std::is_arithmetic_v<optional_value_t<scalar_input_result_t<T>>>;

// Text helpers

template <StringScalarInput V>
auto lower(V&& value) { return sql_function<std::string>("LOWER", std::forward<V>(value)); }

template <StringScalarInput V>
auto upper(V&& value) { return sql_function<std::string>("UPPER", std::forward<V>(value)); }

template <StringScalarInput V>
auto trim(V&& value) { return sql_function<std::string>("TRIM", std::forward<V>(value)); }

template <StringScalarInput V>
auto ltrim(V&& value) { return sql_function<std::string>("LTRIM", std::forward<V>(value)); }

template <StringScalarInput V>
auto rtrim(V&& value) { return sql_function<std::string>("RTRIM", std::forward<V>(value)); }

template <StringScalarInput V>
auto length(V&& value) { return sql_function<std::int64_t>("LENGTH", std::forward<V>(value)); }

template <ScalarInput... Args>
requires (sizeof...(Args) > 0)
auto concat(Args&&... args) {
    return sql_function<std::string>("CONCAT", std::forward<Args>(args)...);
}

template <StringScalarInput Separator, ScalarInput... Args>
requires (sizeof...(Args) > 0)
auto concat_ws(Separator&& separator, Args&&... args) {
    return sql_function<std::string>(
        "CONCAT_WS", std::forward<Separator>(separator), std::forward<Args>(args)...);
}

template <StringScalarInput V, NumericScalarInput Start>
auto substr(V&& value, Start&& start) {
    return sql_function<std::string>("SUBSTR", std::forward<V>(value), std::forward<Start>(start));
}

template <StringScalarInput V, NumericScalarInput Start, NumericScalarInput Length>
auto substr(V&& value, Start&& start, Length&& count) {
    return sql_function<std::string>(
        "SUBSTR", std::forward<V>(value), std::forward<Start>(start), std::forward<Length>(count));
}

template <StringScalarInput V, NumericScalarInput Start>
auto substring(V&& value, Start&& start) {
    return substr(std::forward<V>(value), std::forward<Start>(start));
}

template <StringScalarInput V, NumericScalarInput Start, NumericScalarInput Length>
auto substring(V&& value, Start&& start, Length&& count) {
    return substr(std::forward<V>(value), std::forward<Start>(start), std::forward<Length>(count));
}

template <StringScalarInput V, StringScalarInput Search, StringScalarInput Replacement>
auto replace(V&& value, Search&& search, Replacement&& replacement) {
    return sql_function<std::string>(
        "REPLACE", std::forward<V>(value), std::forward<Search>(search), std::forward<Replacement>(replacement));
}

template <StringScalarInput V, NumericScalarInput Length>
auto left(V&& value, Length&& count) {
    return sql_function<std::string>("LEFT", std::forward<V>(value), std::forward<Length>(count));
}

template <StringScalarInput V, NumericScalarInput Length>
auto right(V&& value, Length&& count) {
    return sql_function<std::string>("RIGHT", std::forward<V>(value), std::forward<Length>(count));
}

template <StringScalarInput V>
auto ascii(V&& value) { return sql_function<std::int64_t>("ASCII", std::forward<V>(value)); }

template <NumericScalarInput Code>
auto chr(Code&& code) { return sql_function<std::string>("CHR", std::forward<Code>(code)); }

template <StringScalarInput V>
auto bit_length(V&& value) { return sql_function<std::int64_t>("BIT_LENGTH", std::forward<V>(value)); }

template <StringScalarInput V>
auto octet_length(V&& value) { return sql_function<std::int64_t>("OCTET_LENGTH", std::forward<V>(value)); }

template <StringScalarInput Needle, StringScalarInput Haystack>
auto position(Needle&& needle, Haystack&& haystack) {
    return sql_function<std::int64_t>("POSITION", std::forward<Needle>(needle), std::forward<Haystack>(haystack));
}

template <StringScalarInput Needle, StringScalarInput Haystack>
auto locate(Needle&& needle, Haystack&& haystack) {
    return sql_function<std::int64_t>("LOCATE", std::forward<Needle>(needle), std::forward<Haystack>(haystack));
}

template <StringScalarInput Haystack, StringScalarInput Needle>
auto instr(Haystack&& haystack, Needle&& needle) {
    return sql_function<std::int64_t>("INSTR", std::forward<Haystack>(haystack), std::forward<Needle>(needle));
}

template <StringScalarInput V, StringScalarInput Separator>
auto group_concat(V&& value, Separator&& separator) {
    return sql_function<std::string>("GROUP_CONCAT", std::forward<V>(value), std::forward<Separator>(separator));
}

template <StringScalarInput V>
auto group_concat(V&& value) {
    return sql_function<std::string>("GROUP_CONCAT", std::forward<V>(value));
}

// Numeric helpers

template <NumericScalarInput V>
auto abs(V&& value) { return sql_function<double>("ABS", std::forward<V>(value)); }

template <NumericScalarInput V>
auto acos(V&& value) { return sql_function<double>("ACOS", std::forward<V>(value)); }

template <NumericScalarInput V>
auto asin(V&& value) { return sql_function<double>("ASIN", std::forward<V>(value)); }

template <NumericScalarInput V>
auto atan(V&& value) { return sql_function<double>("ATAN", std::forward<V>(value)); }

template <NumericScalarInput Y, NumericScalarInput X>
auto atan2(Y&& y, X&& x) { return sql_function<double>("ATAN2", std::forward<Y>(y), std::forward<X>(x)); }

template <NumericScalarInput V>
auto cos(V&& value) { return sql_function<double>("COS", std::forward<V>(value)); }

template <NumericScalarInput V>
auto sin(V&& value) { return sql_function<double>("SIN", std::forward<V>(value)); }

template <NumericScalarInput V>
auto tan(V&& value) { return sql_function<double>("TAN", std::forward<V>(value)); }

template <NumericScalarInput V>
auto cot(V&& value) { return sql_function<double>("COT", std::forward<V>(value)); }

template <NumericScalarInput V>
auto ln(V&& value) { return sql_function<double>("LN", std::forward<V>(value)); }

template <NumericScalarInput V>
auto log(V&& value) { return sql_function<double>("LOG", std::forward<V>(value)); }

template <NumericScalarInput V>
auto log10(V&& value) { return sql_function<double>("LOG10", std::forward<V>(value)); }

template <NumericScalarInput V>
auto log2(V&& value) { return sql_function<double>("LOG2", std::forward<V>(value)); }

template <NumericScalarInput Base, NumericScalarInput Exponent>
auto pow(Base&& base, Exponent&& exponent) {
    return sql_function<double>("POW", std::forward<Base>(base), std::forward<Exponent>(exponent));
}

template <NumericScalarInput Base, NumericScalarInput Exponent>
auto power(Base&& base, Exponent&& exponent) {
    return sql_function<double>("POWER", std::forward<Base>(base), std::forward<Exponent>(exponent));
}

template <NumericScalarInput V>
auto exp(V&& value) { return sql_function<double>("EXP", std::forward<V>(value)); }

template <NumericScalarInput V>
auto sqrt(V&& value) { return sql_function<double>("SQRT", std::forward<V>(value)); }

template <NumericScalarInput V>
auto cbrt(V&& value) { return sql_function<double>("CBRT", std::forward<V>(value)); }

template <NumericScalarInput V>
auto ceil(V&& value) { return sql_function<double>("CEIL", std::forward<V>(value)); }

template <NumericScalarInput V>
auto ceiling(V&& value) { return sql_function<double>("CEILING", std::forward<V>(value)); }

template <NumericScalarInput V>
auto floor(V&& value) { return sql_function<double>("FLOOR", std::forward<V>(value)); }

template <NumericScalarInput V>
auto round(V&& value) { return sql_function<double>("ROUND", std::forward<V>(value)); }

template <NumericScalarInput V, NumericScalarInput Digits>
auto round(V&& value, Digits&& digits) {
    return sql_function<double>("ROUND", std::forward<V>(value), std::forward<Digits>(digits));
}

template <NumericScalarInput V>
auto trunc(V&& value) { return sql_function<double>("TRUNC", std::forward<V>(value)); }

template <NumericScalarInput V, NumericScalarInput Digits>
auto truncate(V&& value, Digits&& digits) {
    return sql_function<double>("TRUNCATE", std::forward<V>(value), std::forward<Digits>(digits));
}

template <NumericScalarInput V>
auto sign(V&& value) { return sql_function<std::int64_t>("SIGN", std::forward<V>(value)); }

template <NumericScalarInput Left, NumericScalarInput Right>
auto mod(Left&& left_value, Right&& right_value) {
    return sql_function<double>("MOD", std::forward<Left>(left_value), std::forward<Right>(right_value));
}

inline auto pi() { return sql_function<double>("PI"); }

template <NumericScalarInput V>
auto degrees(V&& value) { return sql_function<double>("DEGREES", std::forward<V>(value)); }

template <NumericScalarInput V>
auto radians(V&& value) { return sql_function<double>("RADIANS", std::forward<V>(value)); }

inline auto random() { return sql_function<double>("RANDOM"); }
inline auto rand() { return sql_function<double>("RAND"); }

// Control flow

template <ScalarInput First, ScalarInput... Rest>
auto coalesce(First&& first, Rest&&... rest) {
    using Result = scalar_input_result_t<First>;
    static_assert((scalar_results_compatible_v<Result, scalar_input_result_t<Rest>> && ...),
                  "MetalORM: COALESCE arguments must have compatible result types");
    return sql_function<Result>(
        "COALESCE", std::forward<First>(first), std::forward<Rest>(rest)...);
}

template <ScalarInput V, ScalarInput Fallback>
requires scalar_results_compatible_v<scalar_input_result_t<V>, scalar_input_result_t<Fallback>>
auto if_null(V&& value, Fallback&& fallback) {
    using Result = scalar_input_result_t<V>;
    return sql_function<Result>("IF_NULL", std::forward<V>(value), std::forward<Fallback>(fallback));
}

template <ScalarInput Left, ScalarInput Right>
requires scalar_results_compatible_v<scalar_input_result_t<Left>, scalar_input_result_t<Right>>
auto nullif(Left&& left_value, Right&& right_value) {
    using Result = std::optional<optional_value_t<scalar_input_result_t<Left>>>;
    return sql_function<Result>("NULLIF", std::forward<Left>(left_value), std::forward<Right>(right_value));
}

template <ScalarInput First, ScalarInput... Rest>
auto greatest(First&& first, Rest&&... rest) {
    using Result = scalar_input_result_t<First>;
    static_assert((scalar_results_compatible_v<Result, scalar_input_result_t<Rest>> && ...),
                  "MetalORM: GREATEST arguments must have compatible result types");
    return sql_function<Result>("GREATEST", std::forward<First>(first), std::forward<Rest>(rest)...);
}

template <ScalarInput First, ScalarInput... Rest>
auto least(First&& first, Rest&&... rest) {
    using Result = scalar_input_result_t<First>;
    static_assert((scalar_results_compatible_v<Result, scalar_input_result_t<Rest>> && ...),
                  "MetalORM: LEAST arguments must have compatible result types");
    return sql_function<Result>("LEAST", std::forward<First>(first), std::forward<Rest>(rest)...);
}

// Date/time helpers

enum class date_part { year, month, day, hour, minute, second, week, dow };

inline std::string date_part_token(date_part part) {
    switch (part) {
        case date_part::year: return "YEAR";
        case date_part::month: return "MONTH";
        case date_part::day: return "DAY";
        case date_part::hour: return "HOUR";
        case date_part::minute: return "MINUTE";
        case date_part::second: return "SECOND";
        case date_part::week: return "WEEK";
        case date_part::dow: return "DOW";
    }
    return "DAY";
}

inline auto now() { return sql_function<std::string>("NOW"); }
inline auto current_date() { return sql_function<std::string>("CURRENT_DATE"); }
inline auto current_time() { return sql_function<std::string>("CURRENT_TIME"); }
inline auto utc_now() { return sql_function<std::string>("UTC_NOW"); }
inline auto local_time() { return sql_function<std::string>("LOCAL_TIME"); }
inline auto local_timestamp() { return sql_function<std::string>("LOCAL_TIMESTAMP"); }

template <StringScalarInput Date, NumericScalarInput Interval>
auto date_add(Date&& date, Interval&& interval, date_part unit) {
    return sql_function<std::string>(
        "DATE_ADD_" + date_part_token(unit), std::forward<Date>(date), std::forward<Interval>(interval));
}

template <StringScalarInput Date, NumericScalarInput Interval>
auto date_sub(Date&& date, Interval&& interval, date_part unit) {
    return sql_function<std::string>(
        "DATE_SUB_" + date_part_token(unit), std::forward<Date>(date), std::forward<Interval>(interval));
}

template <StringScalarInput Left, StringScalarInput Right>
auto date_diff(Left&& left_date, Right&& right_date) {
    return sql_function<std::int64_t>(
        "DATE_DIFF", std::forward<Left>(left_date), std::forward<Right>(right_date));
}

template <StringScalarInput Date, StringScalarInput Format>
auto date_format(Date&& date, Format&& format) {
    return sql_function<std::string>(
        "DATE_FORMAT", std::forward<Date>(date), std::forward<Format>(format));
}

inline auto unix_timestamp() { return sql_function<std::int64_t>("UNIX_TIMESTAMP"); }

template <StringScalarInput Date>
auto unix_timestamp(Date&& date) {
    return sql_function<std::int64_t>("UNIX_TIMESTAMP", std::forward<Date>(date));
}

template <NumericScalarInput Timestamp>
auto from_unix_time(Timestamp&& timestamp) {
    return sql_function<std::string>("FROM_UNIXTIME", std::forward<Timestamp>(timestamp));
}

template <StringScalarInput Date>
auto end_of_month(Date&& date) {
    return sql_function<std::string>("END_OF_MONTH", std::forward<Date>(date));
}

template <StringScalarInput Date>
auto day_of_week(Date&& date) { return sql_function<std::int64_t>("DAY_OF_WEEK", std::forward<Date>(date)); }

template <StringScalarInput Date>
auto week_of_year(Date&& date) { return sql_function<std::int64_t>("WEEK_OF_YEAR", std::forward<Date>(date)); }

template <StringScalarInput Date>
auto date_trunc(date_part part, Date&& date) {
    return sql_function<std::string>("DATE_TRUNC_" + date_part_token(part), std::forward<Date>(date));
}

template <StringScalarInput Date>
auto extract(date_part part, Date&& date) {
    return sql_function<std::int64_t>("EXTRACT_" + date_part_token(part), std::forward<Date>(date));
}

template <StringScalarInput Date>
auto year(Date&& date) { return sql_function<std::int64_t>("YEAR", std::forward<Date>(date)); }

template <StringScalarInput Date>
auto month(Date&& date) { return sql_function<std::int64_t>("MONTH", std::forward<Date>(date)); }

template <StringScalarInput Date>
auto day(Date&& date) { return sql_function<std::int64_t>("DAY", std::forward<Date>(date)); }

template <StringScalarInput Date>
auto hour(Date&& date) { return sql_function<std::int64_t>("HOUR", std::forward<Date>(date)); }

template <StringScalarInput Date>
auto minute(Date&& date) { return sql_function<std::int64_t>("MINUTE", std::forward<Date>(date)); }

template <StringScalarInput Date>
auto second(Date&& date) { return sql_function<std::int64_t>("SECOND", std::forward<Date>(date)); }

template <StringScalarInput Date>
auto quarter(Date&& date) { return sql_function<std::int64_t>("QUARTER", std::forward<Date>(date)); }

// JSON helpers

template <typename Result = std::string, ScalarInput Json, StringScalarInput Path>
auto json_path(Json&& json, Path&& path) {
    return sql_function<Result>("JSON_PATH", std::forward<Json>(json), std::forward<Path>(path));
}

template <ScalarInput Json>
auto json_length(Json&& json) {
    return sql_function<std::int64_t>("JSON_LENGTH", std::forward<Json>(json));
}

template <ScalarInput Json, StringScalarInput Path>
auto json_length(Json&& json, Path&& path) {
    return sql_function<std::int64_t>("JSON_LENGTH", std::forward<Json>(json), std::forward<Path>(path));
}

template <ScalarInput V>
auto json_arrayagg(V&& value) {
    return sql_function<std::string>("JSON_ARRAYAGG", std::forward<V>(value));
}

} // namespace metal
