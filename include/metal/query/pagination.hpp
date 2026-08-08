#pragma once

#include "metal/execution.hpp"
#include "metal/query/relation_queries.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

namespace metal {

struct PageOptions {
    std::size_t page{1};
    std::size_t page_size{20};
};

struct PageResult {
    std::vector<Row> items;
    std::size_t total_items{};
    std::size_t page{};
    std::size_t page_size{};
};

struct CursorPageOptions {
    std::optional<std::size_t> first;
    std::optional<std::string> after;
    std::optional<std::size_t> last;
    std::optional<std::string> before;
};

struct CursorPageInfo {
    bool has_next_page{false};
    bool has_previous_page{false};
    std::optional<std::string> start_cursor;
    std::optional<std::string> end_cursor;
};

struct CursorPageResult {
    std::vector<Row> items;
    CursorPageInfo page_info;
};

struct CursorOrderTerm {
    std::type_index owner{typeid(void)};
    std::string table;
    std::string column;
    bool ascending{true};
};

template <std::meta::info Member>
CursorOrderTerm cursor_order(Field<Member> = {}, bool ascending = true) {
    using Owner = typename Field<Member>::owner_type;
    return CursorOrderTerm{
        std::type_index(typeid(Owner)),
        reflect::table_name<Owner>(),
        reflect::column_name<Member>(),
        ascending};
}

namespace detail {

inline constexpr char base64url_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

inline std::string base64url_encode(std::string_view input) {
    std::string out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (unsigned char c : input) {
        buffer = (buffer << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(base64url_alphabet[(buffer >> bits) & 0x3f]);
        }
    }
    if (bits > 0) out.push_back(base64url_alphabet[(buffer << (6 - bits)) & 0x3f]);
    return out;
}

inline int base64url_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

inline std::string base64url_decode(std::string_view input) {
    std::string out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (char c : input) {
        const int value = base64url_value(c);
        if (value < 0) throw std::invalid_argument("MetalORM: invalid cursor format");
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xff));
        }
    }
    return out;
}

inline std::string cursor_value_text(const Value& value, char& tag) {
    return std::visit([&](const auto& v) -> std::string {
        using V = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::same_as<V, std::nullptr_t>) {
            throw std::invalid_argument("MetalORM: cursor pagination requires non-null ORDER BY values");
        } else if constexpr (std::same_as<V, std::int64_t>) {
            tag = 'i';
            return std::to_string(v);
        } else if constexpr (std::same_as<V, double>) {
            tag = 'd';
            std::ostringstream out;
            out << std::setprecision(17) << v;
            return out.str();
        } else if constexpr (std::same_as<V, std::string>) {
            tag = 's';
            return v;
        } else {
            tag = 'b';
            return v ? "1" : "0";
        }
    }, value);
}

inline Value parse_cursor_value(char tag, std::string_view text) {
    switch (tag) {
        case 'i': return Value{static_cast<std::int64_t>(std::stoll(std::string(text)))};
        case 'd': return Value{std::stod(std::string(text))};
        case 's': return Value{std::string(text)};
        case 'b':
            if (text == "1") return Value{true};
            if (text == "0") return Value{false};
            break;
    }
    throw std::invalid_argument("MetalORM: invalid cursor payload");
}

inline std::string order_signature(const std::vector<CursorOrderTerm>& order) {
    std::string out;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (i) out += ',';
        out += order[i].table + "." + order[i].column + (order[i].ascending ? ":ASC" : ":DESC");
    }
    return out;
}

inline std::string encode_cursor(
    const std::vector<Value>& values,
    const std::vector<CursorOrderTerm>& order) {
    std::string plain = "2\n" + order_signature(order) + "\n" + std::to_string(values.size()) + "\n";
    for (const auto& value : values) {
        char tag = 0;
        const auto text = cursor_value_text(value, tag);
        plain.push_back(tag);
        plain += std::to_string(text.size());
        plain.push_back(':');
        plain += text;
        plain.push_back('\n');
    }
    return base64url_encode(plain);
}

struct DecodedCursor {
    std::vector<Value> values;
    std::string signature;
};

inline std::string read_line(std::string_view text, std::size_t& position) {
    const auto end = text.find('\n', position);
    if (end == std::string_view::npos) throw std::invalid_argument("MetalORM: invalid cursor payload");
    std::string out(text.substr(position, end - position));
    position = end + 1;
    return out;
}

inline DecodedCursor decode_cursor(std::string_view cursor) {
    const std::string plain = base64url_decode(cursor);
    std::size_t pos = 0;
    if (read_line(plain, pos) != "2") throw std::invalid_argument("MetalORM: invalid cursor payload version");
    DecodedCursor out;
    out.signature = read_line(plain, pos);
    const auto count = static_cast<std::size_t>(std::stoull(read_line(plain, pos)));
    out.values.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (pos >= plain.size()) throw std::invalid_argument("MetalORM: invalid cursor payload");
        const char tag = plain[pos++];
        const auto colon = plain.find(':', pos);
        if (colon == std::string::npos) throw std::invalid_argument("MetalORM: invalid cursor payload");
        const auto length = static_cast<std::size_t>(std::stoull(plain.substr(pos, colon - pos)));
        pos = colon + 1;
        if (pos + length > plain.size()) throw std::invalid_argument("MetalORM: invalid cursor payload");
        out.values.push_back(parse_cursor_value(tag, std::string_view(plain).substr(pos, length)));
        pos += length;
        if (pos >= plain.size() || plain[pos] != '\n') throw std::invalid_argument("MetalORM: invalid cursor payload");
        ++pos;
    }
    return out;
}

inline std::vector<Value> cursor_values_from_row(
    const Row& row,
    const std::vector<CursorOrderTerm>& order) {
    std::vector<Value> values;
    values.reserve(order.size());
    for (const auto& term : order) {
        auto found = row.find(term.column);
        if (found == row.end()) {
            throw std::logic_error(
                "MetalORM: cursor ORDER BY column '" + term.column + "' is not present in the query projection");
        }
        if (std::holds_alternative<std::nullptr_t>(found->second)) {
            throw std::logic_error("MetalORM: cursor pagination requires non-null ORDER BY values");
        }
        values.push_back(found->second);
    }
    return values;
}

inline void append_cursor_predicate(
    std::string& sql,
    std::vector<Value>& params,
    std::string_view alias,
    const Dialect& dialect,
    const std::vector<CursorOrderTerm>& order,
    const std::vector<Value>& values,
    bool after) {
    if (values.size() != order.size()) throw std::invalid_argument("MetalORM: invalid cursor payload");
    sql += " WHERE (";
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (i) sql += " OR ";
        sql += "(";
        for (std::size_t j = 0; j < i; ++j) {
            if (j) sql += " AND ";
            sql += dialect.quote_identifier(alias) + "." + dialect.quote_identifier(order[j].column) + " = " +
                   dialect.placeholder(params.size() + 1);
            params.push_back(values[j]);
        }
        if (i) sql += " AND ";
        const bool greater = after ? order[i].ascending : !order[i].ascending;
        sql += dialect.quote_identifier(alias) + "." + dialect.quote_identifier(order[i].column) +
               (greater ? " > " : " < ") + dialect.placeholder(params.size() + 1);
        params.push_back(values[i]);
        sql += ")";
    }
    sql += ")";
}

template <typename Query>
concept CompilableSelectQuery = requires(const Query& query, const Dialect& dialect) {
    { query.compile_subquery(dialect) } -> std::same_as<CompiledQuery>;
};

} // namespace detail

template <detail::CompilableSelectQuery Query>
PageResult execute_paged(
    const Query& query,
    DbExecutor& executor,
    const Dialect& dialect,
    PageOptions options) {
    if (options.page < 1) throw std::invalid_argument("MetalORM: page must be >= 1");
    if (options.page_size < 1) throw std::invalid_argument("MetalORM: page_size must be >= 1");

    const auto base = query.compile_subquery(dialect);
    const std::string count_sql =
        "SELECT COUNT(*) AS \"total\" FROM (" + base.sql + ") AS \"__metal_count\";";
    const auto count_result = executor.execute(count_sql, base.params);
    std::size_t total = 0;
    if (!count_result.rows.empty()) {
        auto found = count_result.rows.front().find("total");
        if (found != count_result.rows.front().end()) {
            total = static_cast<std::size_t>(from_value<std::int64_t>(found->second));
        }
    }

    const auto offset = (options.page - 1) * options.page_size;
    const std::string page_sql =
        "SELECT * FROM (" + base.sql + ") AS \"__metal_page\" LIMIT " +
        std::to_string(options.page_size) + " OFFSET " + std::to_string(offset) + ";";
    auto page_result = executor.execute(page_sql, base.params);
    return PageResult{
        std::move(page_result.rows), total, options.page, options.page_size};
}

template <detail::CompilableSelectQuery Query>
CursorPageResult execute_cursor(
    const Query& query,
    DbExecutor& executor,
    const Dialect& dialect,
    std::vector<CursorOrderTerm> order,
    CursorPageOptions options) {
    if (order.empty()) throw std::invalid_argument("MetalORM: cursor pagination requires at least one reflected ORDER BY field");
    if (options.first && options.last) {
        throw std::invalid_argument("MetalORM: cursor first and last cannot be used together");
    }
    if (options.after && options.before) {
        throw std::invalid_argument("MetalORM: cursor after and before cannot be used together");
    }
    if (!options.first && !options.last) {
        throw std::invalid_argument("MetalORM: cursor pagination requires first or last");
    }
    const std::size_t limit = options.first.value_or(options.last.value_or(0));
    if (limit < 1) throw std::invalid_argument("MetalORM: cursor page size must be >= 1");

    const bool backward = options.last.has_value();
    const auto cursor = options.after ? options.after : options.before;
    const auto base = query.compile_subquery(dialect);
    const std::string alias = "__metal_cursor";
    std::string sql = "SELECT * FROM (" + base.sql + ") AS " + dialect.quote_identifier(alias);
    std::vector<Value> params = base.params;

    if (cursor) {
        const auto decoded = detail::decode_cursor(*cursor);
        if (decoded.signature != detail::order_signature(order)) {
            throw std::invalid_argument(
                "MetalORM: cursor ORDER BY signature does not match the current reflected order");
        }
        const bool after = options.after.has_value();
        detail::append_cursor_predicate(sql, params, alias, dialect, order, decoded.values, after);
    }

    sql += " ORDER BY ";
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (i) sql += ", ";
        const bool asc = backward ? !order[i].ascending : order[i].ascending;
        sql += dialect.quote_identifier(alias) + "." + dialect.quote_identifier(order[i].column) +
               (asc ? " ASC" : " DESC");
    }
    sql += " LIMIT " + std::to_string(limit + 1) + ";";

    auto result = executor.execute(sql, params);
    const bool has_extra = result.rows.size() > limit;
    if (has_extra) result.rows.pop_back();
    if (backward) std::reverse(result.rows.begin(), result.rows.end());

    CursorPageInfo info;
    if (!result.rows.empty()) {
        info.has_next_page = backward ? options.before.has_value() : has_extra;
        info.has_previous_page = backward ? has_extra : options.after.has_value();
        info.start_cursor = detail::encode_cursor(
            detail::cursor_values_from_row(result.rows.front(), order), order);
        info.end_cursor = detail::encode_cursor(
            detail::cursor_values_from_row(result.rows.back(), order), order);
    }

    return CursorPageResult{std::move(result.rows), std::move(info)};
}

} // namespace metal
