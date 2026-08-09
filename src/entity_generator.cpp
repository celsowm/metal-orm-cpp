#include "metal/entity_generator.hpp"
#include "metal/schema_introspection.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace metal {
namespace {

std::string trim(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
    return std::string(value.substr(first, last - first));
}

std::string uppercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string cpp_string(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

bool is_cpp_keyword(std::string_view value) {
    static const std::unordered_set<std::string> keywords{
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool",
        "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class", "compl",
        "concept", "const", "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await",
        "co_return", "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast",
        "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto",
        "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
        "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
        "requires", "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
        "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
        "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"
    };
    return keywords.contains(std::string(value));
}

std::string identifier(std::string_view input, std::string_view fallback = "field") {
    std::string out;
    out.reserve(input.size() + 1);
    for (const unsigned char c : input) {
        out += (std::isalnum(c) || c == '_') ? static_cast<char>(c) : '_';
    }
    if (out.empty()) out = std::string(fallback);
    if (std::isdigit(static_cast<unsigned char>(out.front()))) out.insert(out.begin(), '_');
    if (is_cpp_keyword(out)) out += '_';
    return out;
}

std::string singularize(std::string value) {
    static const std::unordered_map<std::string, std::string> irregular{
        {"children", "child"},
        {"people", "person"},
        {"men", "man"},
        {"women", "woman"},
        {"mice", "mouse"},
        {"geese", "goose"}
    };

    const auto lower = lowercase(value);
    if (const auto found = irregular.find(lower); found != irregular.end()) return found->second;
    if (lower == "series" || lower == "species" || lower == "news" ||
        lower.ends_with("status") || lower.ends_with("analysis")) {
        return value;
    }
    if (value.size() > 3 && lower.ends_with("ies")) {
        value.resize(value.size() - 3);
        value += 'y';
    } else if (value.size() > 1 && lower.ends_with('s') && !lower.ends_with("ss") &&
               !lower.ends_with("us") && !lower.ends_with("is")) {
        value.pop_back();
    }
    return value;
}

std::string class_name(std::string_view table) {
    const auto singular = singularize(identifier(table, "Entity"));
    std::string out;
    bool capitalize = true;
    for (const unsigned char c : singular) {
        if (c == '_') {
            capitalize = true;
            continue;
        }
        out += capitalize ? static_cast<char>(std::toupper(c)) : static_cast<char>(c);
        capitalize = false;
    }
    if (out.empty()) out = "Entity";
    if (std::isdigit(static_cast<unsigned char>(out.front()))) out.insert(out.begin(), '_');
    if (is_cpp_keyword(out)) out += "Entity";
    return out;
}

std::string relation_name(const DatabaseColumn& column, std::string_view target) {
    std::string base = identifier(column.name, "relation");
    if (base.size() > 3 && base.ends_with("_id")) base.resize(base.size() - 3);
    if (base.empty() || base == column.name) {
        base = identifier(singularize(identifier(target, "relation")), "relation");
    }
    return base;
}

void append_comment(std::ostringstream& out, const std::optional<std::string>& comment, std::string_view indent) {
    if (!comment || comment->empty()) return;
    std::string line;
    std::istringstream input(*comment);
    while (std::getline(input, line)) out << indent << "/// " << line << '\n';
}

enum class ScalarKind { Integer, Real, Boolean, Text };

struct CppType {
    ScalarKind kind{ScalarKind::Text};
    std::string name{"std::string"};
};

CppType map_type(const DatabaseTable& table, const DatabaseColumn& column, std::vector<std::string>& warnings) {
    const auto type = uppercase(trim(column.type));
    if (type == "BIT" || type.find("BOOL") != std::string::npos || type.find("TINYINT(1)") != std::string::npos) {
        return {ScalarKind::Boolean, "bool"};
    }
    if (type.find("INT") != std::string::npos) return {ScalarKind::Integer, "std::int64_t"};
    if (type.find("REAL") != std::string::npos || type.find("FLOA") != std::string::npos ||
        type.find("DOUB") != std::string::npos || type.find("DEC") != std::string::npos ||
        type.find("NUM") != std::string::npos) {
        return {ScalarKind::Real, "double"};
    }
    if (type.find("BLOB") != std::string::npos || type.find("BINARY") != std::string::npos ||
        type.find("BYTEA") != std::string::npos) {
        warnings.push_back(
            "Column " + table.name + "." + column.name + " uses " + column.type +
            "; generated as std::string because the current Value/SQLiteExecutor surface has no BLOB value type yet.");
        return {ScalarKind::Text, "std::string"};
    }
    if (type.empty() || type.find("CHAR") != std::string::npos || type.find("CLOB") != std::string::npos ||
        type.find("TEXT") != std::string::npos || type.find("DATE") != std::string::npos ||
        type.find("TIME") != std::string::npos || type.find("JSON") != std::string::npos ||
        type.find("UUID") != std::string::npos) {
        return {ScalarKind::Text, "std::string"};
    }
    warnings.push_back(
        "Column " + table.name + "." + column.name + " has unrecognized declared type " + column.type +
        "; generated as std::string while database_type preserves the declaration.");
    return {ScalarKind::Text, "std::string"};
}

std::string strip_outer_parentheses(std::string value) {
    for (;;) {
        value = trim(value);
        if (value.size() < 2 || value.front() != '(' || value.back() != ')') return value;
        int depth = 0;
        bool balanced = true;
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '(') ++depth;
            else if (value[i] == ')') --depth;
            if (depth == 0 && i + 1 != value.size()) { balanced = false; break; }
            if (depth < 0) { balanced = false; break; }
        }
        if (!balanced || depth != 0) return value;
        value = value.substr(1, value.size() - 2);
    }
}

std::optional<std::string> unquote_sql_text(std::string_view value) {
    if (value.size() < 2 || value.front() != '\'' || value.back() != '\'') return std::nullopt;
    std::string out;
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        if (value[i] == '\'' && i + 2 < value.size() && value[i + 1] == '\'') {
            out += '\'';
            ++i;
        } else {
            out += value[i];
        }
    }
    return out;
}

std::optional<std::string> default_annotation(
    const DatabaseColumn& column,
    const CppType& type) {
    if (!column.default_value) return std::nullopt;
    auto value = strip_outer_parentheses(*column.default_value);
    const auto upper = uppercase(value);
    if (upper == "NULL") {
        if (!column.not_null) return "metal::mapping::default_null";
        return "metal::mapping::default_sql{\"NULL\"}";
    }

    if (type.kind == ScalarKind::Boolean && (value == "0" || value == "1" || upper == "TRUE" || upper == "FALSE")) {
        const bool enabled = value == "1" || upper == "TRUE";
        return std::string{"metal::mapping::default_value{"} + (enabled ? "true}" : "false}");
    }

    if (type.kind == ScalarKind::Integer) {
        std::int64_t parsed{};
        const auto* begin = value.data();
        const auto* end = value.data() + value.size();
        const auto [ptr, ec] = std::from_chars(begin, end, parsed);
        if (ec == std::errc{} && ptr == end) {
            return "metal::mapping::default_value{" + std::to_string(parsed) + "}";
        }
    }

    if (type.kind == ScalarKind::Real) {
        char* end{};
        const auto parsed = std::strtod(value.c_str(), &end);
        if (end == value.c_str() + value.size() && std::isfinite(parsed)) {
            std::ostringstream formatted;
            formatted << std::setprecision(17) << parsed;
            return "metal::mapping::default_value{" + formatted.str() + "}";
        }
    }

    if (const auto text = unquote_sql_text(value)) {
        return "metal::mapping::default_text{\"" + cpp_string(*text) + "\"}";
    }

    return "metal::mapping::default_sql{\"" + cpp_string(value) + "\"}";
}

std::unordered_map<std::string, std::string> make_class_names(const DatabaseSchema& schema) {
    std::unordered_map<std::string, std::string> result;
    std::unordered_set<std::string> used;
    for (const auto& table : schema.tables) {
        auto name = class_name(table.name);
        const auto base = name;
        std::size_t suffix = 2;
        while (used.contains(name)) name = base + std::to_string(suffix++);
        used.insert(name);
        result.emplace(table.name, std::move(name));
    }
    return result;
}

std::unordered_map<std::string, std::string> member_names(const DatabaseTable& table) {
    std::unordered_map<std::string, std::string> result;
    std::unordered_set<std::string> used;
    for (const auto& column : table.columns) {
        auto name = identifier(column.name);
        const auto base = name;
        std::size_t suffix = 2;
        while (used.contains(name)) name = base + '_' + std::to_string(suffix++);
        used.insert(name);
        result.emplace(column.name, std::move(name));
    }
    return result;
}

const DatabaseTable* find_table(const DatabaseSchema& schema, std::string_view name) {
    const auto found = std::find_if(schema.tables.begin(), schema.tables.end(), [&](const DatabaseTable& table) {
        return table.name == name;
    });
    return found == schema.tables.end() ? nullptr : &*found;
}

bool reference_targets_single_primary_key(const DatabaseTable& target, const ForeignKeyReference& reference) {
    return target.primary_key.size() == 1 && target.primary_key.front() == reference.column;
}

} // namespace

GeneratedEntityHeader generate_entity_header(
    const DatabaseSchema& schema,
    const EntityGeneratorOptions& options) {
    GeneratedEntityHeader result;
    const auto classes = make_class_names(schema);
    const auto namespace_name = identifier(options.namespace_name, "entities");

    std::ostringstream out;
    out << "#pragma once\n\n";
    out << "#include <metal/metal.hpp>\n\n";
    out << "#include <cstdint>\n#include <optional>\n#include <string>\n\n";
    out << "namespace " << namespace_name << " {\n\n";

    for (const auto& table : schema.tables) {
        out << "struct " << classes.at(table.name) << ";\n";
    }
    if (!schema.tables.empty()) out << '\n';

    for (const auto& table : schema.tables) {
        const auto& type_name = classes.at(table.name);
        const auto members = member_names(table);
        std::unordered_set<std::string> used_members;
        for (const auto& [_, name] : members) used_members.insert(name);

        if (options.emit_comments) append_comment(out, table.comment, "");
        out << "struct [[=metal::mapping::table{\"" << cpp_string(table.name) << "\"}]] "
            << type_name << " {\n";

        for (const auto& column : table.columns) {
            const auto cpp_type = map_type(table, column, result.warnings);
            const auto& member = members.at(column.name);
            std::vector<std::string> annotations;
            if (member != column.name) {
                annotations.push_back("metal::mapping::column{\"" + cpp_string(column.name) + "\"}");
            }
            if (std::find(table.primary_key.begin(), table.primary_key.end(), column.name) != table.primary_key.end()) {
                annotations.push_back("metal::mapping::primary_key");
            }
            if (column.auto_increment) annotations.push_back("metal::mapping::generated");
            if (!column.type.empty()) {
                annotations.push_back("metal::mapping::database_type{\"" + cpp_string(column.type) + "\"}");
            }
            if (const auto def = default_annotation(column, cpp_type)) annotations.push_back(*def);

            if (options.emit_comments) append_comment(out, column.comment, "    ");
            if (!annotations.empty()) {
                out << "    [[";
                for (std::size_t i = 0; i < annotations.size(); ++i) {
                    if (i) out << ", ";
                    out << '=' << annotations[i];
                }
                out << "]]\n";
            }

            if (column.not_null) {
                out << "    " << cpp_type.name << ' ' << member << "{};\n";
            } else {
                out << "    std::optional<" << cpp_type.name << "> " << member << ";\n";
            }
        }

        if (options.emit_relations) {
            for (const auto& column : table.columns) {
                if (!column.references) continue;
                const auto* target = find_table(schema, column.references->table);
                if (!target) {
                    result.warnings.push_back(
                        "Foreign key " + table.name + "." + column.name + " references excluded/unavailable table " +
                        column.references->table + "; relation wrapper was not generated.");
                    continue;
                }
                if (!reference_targets_single_primary_key(*target, *column.references)) {
                    result.warnings.push_back(
                        "Foreign key " + table.name + "." + column.name + " targets " + target->name + "." +
                        column.references->column + " rather than the target's single primary key; relation wrapper was not generated "
                        "because C++ reflection would require a target-member dependency cycle.");
                    continue;
                }

                auto property = relation_name(column, target->name);
                if (used_members.contains(property)) property += "_relation";
                const auto base = property;
                std::size_t suffix = 2;
                while (used_members.contains(property)) property = base + '_' + std::to_string(suffix++);
                used_members.insert(property);

                out << '\n';
                out << "    [[=metal::mapping::belongs_to<^^" << type_name << "::" << members.at(column.name) << ">{}]]\n";
                out << "    metal::belongs_to_reference<" << classes.at(target->name) << "> " << property << ";\n";
            }
        }

        out << "};\n\n";
    }

    if (!schema.views.empty()) {
        for (const auto& view : schema.views) {
            result.warnings.push_back(
                "View " + view.name + " was introspected but not emitted as an ORM entity: MetalORM C++ does not yet expose a "
                "read-only mapped-view runtime contract.");
        }
    }

    out << "} // namespace " << namespace_name << "\n";
    result.code = out.str();
    return result;
}

GeneratedEntityHeader generate_sqlite_entity_header(
    DbExecutor& executor,
    const EntityGeneratorOptions& options,
    const IntrospectOptions& introspect_options) {
    return generate_entity_header(introspect_sqlite(executor, introspect_options), options);
}

} // namespace metal