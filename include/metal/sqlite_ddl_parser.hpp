#pragma once

#include "metal/schema_types.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace metal::schema_detail {

inline std::string_view trim_sql_view(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

inline bool sql_identifier_char(char ch) {
    const auto c = static_cast<unsigned char>(ch);
    return std::isalnum(c) || ch == '_';
}

inline bool sql_keyword_at(std::string_view text, std::size_t pos, std::string_view keyword) {
    if (pos + keyword.size() > text.size()) return false;
    if (pos > 0 && sql_identifier_char(text[pos - 1])) return false;
    if (pos + keyword.size() < text.size() && sql_identifier_char(text[pos + keyword.size()])) {
        return false;
    }
    for (std::size_t i = 0; i < keyword.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(text[pos + i])) !=
            std::toupper(static_cast<unsigned char>(keyword[i]))) {
            return false;
        }
    }
    return true;
}

inline void skip_sql_space(std::string_view text, std::size_t& pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
}

inline std::optional<std::string> read_sql_identifier(std::string_view text, std::size_t& pos) {
    skip_sql_space(text, pos);
    if (pos >= text.size()) return std::nullopt;

    const char quote = text[pos];
    if (quote == '"' || quote == '`' || quote == '[') {
        const char close = quote == '[' ? ']' : quote;
        ++pos;
        std::string result;
        while (pos < text.size()) {
            const char ch = text[pos++];
            if (ch == close) {
                if (pos < text.size() && text[pos] == close) {
                    result.push_back(close);
                    ++pos;
                    continue;
                }
                return result;
            }
            result.push_back(ch);
        }
        return std::nullopt;
    }

    const auto start = pos;
    while (pos < text.size() &&
           !std::isspace(static_cast<unsigned char>(text[pos])) &&
           text[pos] != '(' && text[pos] != ')' && text[pos] != ',') {
        ++pos;
    }
    if (pos == start) return std::nullopt;
    return std::string(text.substr(start, pos - start));
}

inline std::size_t find_sql_matching_paren(std::string_view text, std::size_t open) {
    if (open >= text.size() || text[open] != '(') return std::string_view::npos;

    int depth = 1;
    char quote = 0;
    bool bracket = false;
    bool line_comment = false;
    bool block_comment = false;

    for (std::size_t i = open + 1; i < text.size(); ++i) {
        const char ch = text[i];
        const char next = i + 1 < text.size() ? text[i + 1] : '\0';

        if (line_comment) {
            if (ch == '\n' || ch == '\r') line_comment = false;
            continue;
        }
        if (block_comment) {
            if (ch == '*' && next == '/') {
                block_comment = false;
                ++i;
            }
            continue;
        }
        if (quote != 0) {
            if (ch == quote) {
                if (next == quote) {
                    ++i;
                } else {
                    quote = 0;
                }
            }
            continue;
        }
        if (bracket) {
            if (ch == ']') {
                if (next == ']') ++i;
                else bracket = false;
            }
            continue;
        }
        if (ch == '-' && next == '-') {
            line_comment = true;
            ++i;
            continue;
        }
        if (ch == '/' && next == '*') {
            block_comment = true;
            ++i;
            continue;
        }
        if (ch == '\'' || ch == '"' || ch == '`') {
            quote = ch;
            continue;
        }
        if (ch == '[') {
            bracket = true;
            continue;
        }
        if (ch == '(') {
            ++depth;
        } else if (ch == ')') {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::string_view::npos;
}

inline std::size_t find_sql_keyword_top_level(
    std::string_view text,
    std::string_view keyword,
    std::size_t start = 0) {
    int depth = 0;
    char quote = 0;
    bool bracket = false;
    bool line_comment = false;
    bool block_comment = false;

    for (std::size_t i = start; i < text.size(); ++i) {
        const char ch = text[i];
        const char next = i + 1 < text.size() ? text[i + 1] : '\0';

        if (line_comment) {
            if (ch == '\n' || ch == '\r') line_comment = false;
            continue;
        }
        if (block_comment) {
            if (ch == '*' && next == '/') {
                block_comment = false;
                ++i;
            }
            continue;
        }
        if (quote != 0) {
            if (ch == quote) {
                if (next == quote) ++i;
                else quote = 0;
            }
            continue;
        }
        if (bracket) {
            if (ch == ']') {
                if (next == ']') ++i;
                else bracket = false;
            }
            continue;
        }
        if (ch == '-' && next == '-') {
            line_comment = true;
            ++i;
            continue;
        }
        if (ch == '/' && next == '*') {
            block_comment = true;
            ++i;
            continue;
        }
        if (ch == '\'' || ch == '"' || ch == '`') {
            quote = ch;
            continue;
        }
        if (ch == '[') {
            bracket = true;
            continue;
        }
        if (ch == '(') {
            ++depth;
            continue;
        }
        if (ch == ')') {
            if (depth > 0) --depth;
            continue;
        }
        if (depth == 0 && sql_keyword_at(text, i, keyword)) return i;
    }
    return std::string_view::npos;
}

inline std::optional<std::string> extract_check_expression(
    std::string_view segment,
    std::size_t check_pos) {
    auto pos = check_pos + std::string_view{"CHECK"}.size();
    skip_sql_space(segment, pos);
    if (pos >= segment.size() || segment[pos] != '(') return std::nullopt;
    const auto close = find_sql_matching_paren(segment, pos);
    if (close == std::string_view::npos) return std::nullopt;
    return std::string(trim_sql_view(segment.substr(pos + 1, close - pos - 1)));
}

inline std::vector<std::string_view> split_sqlite_table_body(std::string_view create_sql) {
    std::size_t open = std::string_view::npos;
    char quote = 0;
    bool bracket = false;
    for (std::size_t i = 0; i < create_sql.size(); ++i) {
        const char ch = create_sql[i];
        const char next = i + 1 < create_sql.size() ? create_sql[i + 1] : '\0';
        if (quote != 0) {
            if (ch == quote) {
                if (next == quote) ++i;
                else quote = 0;
            }
            continue;
        }
        if (bracket) {
            if (ch == ']') bracket = false;
            continue;
        }
        if (ch == '\'' || ch == '"' || ch == '`') {
            quote = ch;
            continue;
        }
        if (ch == '[') {
            bracket = true;
            continue;
        }
        if (ch == '(') {
            open = i;
            break;
        }
    }
    if (open == std::string_view::npos) return {};
    const auto close = find_sql_matching_paren(create_sql, open);
    if (close == std::string_view::npos || close <= open) return {};

    const auto body = create_sql.substr(open + 1, close - open - 1);
    std::vector<std::string_view> result;
    std::size_t start = 0;
    int depth = 0;
    quote = 0;
    bracket = false;
    bool line_comment = false;
    bool block_comment = false;

    for (std::size_t i = 0; i < body.size(); ++i) {
        const char ch = body[i];
        const char next = i + 1 < body.size() ? body[i + 1] : '\0';
        if (line_comment) {
            if (ch == '\n' || ch == '\r') line_comment = false;
            continue;
        }
        if (block_comment) {
            if (ch == '*' && next == '/') {
                block_comment = false;
                ++i;
            }
            continue;
        }
        if (quote != 0) {
            if (ch == quote) {
                if (next == quote) ++i;
                else quote = 0;
            }
            continue;
        }
        if (bracket) {
            if (ch == ']') {
                if (next == ']') ++i;
                else bracket = false;
            }
            continue;
        }
        if (ch == '-' && next == '-') {
            line_comment = true;
            ++i;
            continue;
        }
        if (ch == '/' && next == '*') {
            block_comment = true;
            ++i;
            continue;
        }
        if (ch == '\'' || ch == '"' || ch == '`') {
            quote = ch;
            continue;
        }
        if (ch == '[') {
            bracket = true;
            continue;
        }
        if (ch == '(') ++depth;
        else if (ch == ')' && depth > 0) --depth;
        else if (ch == ',' && depth == 0) {
            result.push_back(trim_sql_view(body.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (start <= body.size()) result.push_back(trim_sql_view(body.substr(start)));
    return result;
}

inline void parse_sqlite_check_constraints(std::string_view create_sql, DatabaseTable& table) {
    for (auto segment : split_sqlite_table_body(create_sql)) {
        if (segment.empty()) continue;
        std::size_t pos = 0;
        skip_sql_space(segment, pos);

        std::optional<std::string> constraint_name;
        bool table_check = false;
        std::size_t check_pos = std::string_view::npos;

        if (sql_keyword_at(segment, pos, "CONSTRAINT")) {
            pos += std::string_view{"CONSTRAINT"}.size();
            constraint_name = read_sql_identifier(segment, pos);
            skip_sql_space(segment, pos);
            if (sql_keyword_at(segment, pos, "CHECK")) {
                table_check = true;
                check_pos = pos;
            }
        } else if (sql_keyword_at(segment, pos, "CHECK")) {
            table_check = true;
            check_pos = pos;
        }

        if (table_check) {
            if (auto expression = extract_check_expression(segment, check_pos)) {
                table.checks.push_back(DatabaseCheck{
                    .name = std::move(constraint_name),
                    .expression = std::move(*expression)
                });
            }
            continue;
        }

        pos = 0;
        const auto column_name = read_sql_identifier(segment, pos);
        if (!column_name) continue;
        const auto column = std::find_if(
            table.columns.begin(), table.columns.end(),
            [&](const DatabaseColumn& candidate) { return candidate.name == *column_name; });
        if (column == table.columns.end()) continue;

        check_pos = find_sql_keyword_top_level(segment, "CHECK", pos);
        if (check_pos == std::string_view::npos) continue;
        if (auto expression = extract_check_expression(segment, check_pos)) {
            column->check = std::move(*expression);
        }
    }
}

} // namespace metal::schema_detail
