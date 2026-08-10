#include "metal/view_generator.hpp"
#include "metal/schema_introspection.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace metal {
namespace {

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
    for (const unsigned char c : input) out += (std::isalnum(c) || c == '_') ? static_cast<char>(c) : '_';
    if (out.empty()) out = std::string(fallback);
    if (std::isdigit(static_cast<unsigned char>(out.front()))) out.insert(out.begin(), '_');
    if (is_cpp_keyword(out)) out += '_';
    return out;
}

std::string view_type_name(std::string_view name) {
    const auto source = identifier(name, "View");
    std::string out;
    bool capitalize = true;
    for (const unsigned char c : source) {
        if (c == '_') {
            capitalize = true;
            continue;
        }
        out += capitalize ? static_cast<char>(std::toupper(c)) : static_cast<char>(c);
        capitalize = false;
    }
    if (out.empty()) out = "View";
    if (!out.ends_with("View")) out += "View";
    return out;
}

void append_comment(std::ostringstream& out, const std::optional<std::string>& comment, std::string_view indent) {
    if (!comment || comment->empty()) return;
    std::istringstream input(*comment);
    std::string line;
    while (std::getline(input, line)) out << indent << "/// " << line << '\n';
}

std::string uppercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

std::string trim(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
    return std::string(value.substr(first, last - first));
}

std::string view_cpp_type(const DatabaseView& view, const DatabaseColumn& column, std::vector<std::string>& warnings) {
    const auto type = uppercase(trim(column.type));
    if (type.empty()) return "metal::Value";
    if (type == "BIT" || type.find("BOOL") != std::string::npos || type.find("TINYINT(1)") != std::string::npos) {
        return "bool";
    }
    if (type.find("INT") != std::string::npos) return "std::int64_t";
    if (type.find("REAL") != std::string::npos || type.find("FLOA") != std::string::npos ||
        type.find("DOUB") != std::string::npos || type.find("DEC") != std::string::npos ||
        type.find("NUM") != std::string::npos) {
        return "double";
    }
    if (type.find("BLOB") != std::string::npos || type.find("BINARY") != std::string::npos ||
        type.find("BYTEA") != std::string::npos) {
        return "metal::Blob";
    }
    if (type.find("CHAR") != std::string::npos || type.find("CLOB") != std::string::npos ||
        type.find("TEXT") != std::string::npos || type.find("DATE") != std::string::npos ||
        type.find("TIME") != std::string::npos || type.find("JSON") != std::string::npos ||
        type.find("UUID") != std::string::npos) {
        return "std::string";
    }
    warnings.push_back(
        "View column " + view.name + "." + column.name + " has unrecognized declared type " + column.type +
        "; generated as std::string while database_type preserves the declaration.");
    return "std::string";
}

std::string generate_view_body(
    const DatabaseSchema& schema,
    const EntityGeneratorOptions& options,
    std::vector<std::string>& warnings) {
    std::ostringstream out;
    std::unordered_set<std::string> used_types;

    for (const auto& view : schema.views) {
        auto type_name = view_type_name(view.name);
        const auto base_type_name = type_name;
        std::size_t type_suffix = 2;
        while (used_types.contains(type_name)) type_name = base_type_name + std::to_string(type_suffix++);
        used_types.insert(type_name);

        if (options.emit_comments) append_comment(out, view.comment, "");
        out << "struct [[=metal::mapping::view{\"" << cpp_string(view.name) << "\"}]] " << type_name << " {\n";

        std::unordered_set<std::string> used_members;
        for (const auto& column : view.columns) {
            auto member = identifier(column.name);
            const auto base_member = member;
            std::size_t member_suffix = 2;
            while (used_members.contains(member)) member = base_member + '_' + std::to_string(member_suffix++);
            used_members.insert(member);

            const auto cpp_type = view_cpp_type(view, column, warnings);
            if (options.emit_comments) append_comment(out, column.comment, "    ");

            std::vector<std::string> annotations;
            if (member != column.name) {
                annotations.push_back("metal::mapping::column{\"" + cpp_string(column.name) + "\"}");
            }
            if (!column.type.empty()) {
                annotations.push_back("metal::mapping::database_type{\"" + cpp_string(column.type) + "\"}");
            }
            if (!annotations.empty()) {
                out << "    [[";
                for (std::size_t i = 0; i < annotations.size(); ++i) {
                    if (i) out << ", ";
                    out << '=' << annotations[i];
                }
                out << "]]\n";
            }

            if (cpp_type == "metal::Value") {
                out << "    metal::Value " << member << "{nullptr};\n";
            } else if (column.not_null) {
                out << "    " << cpp_type << ' ' << member << "{};\n";
            } else {
                out << "    std::optional<" << cpp_type << "> " << member << ";\n";
            }
        }
        out << "};\n\n";
    }
    return out.str();
}

GeneratedEntityHeader view_header_impl(
    const DatabaseSchema& schema,
    const EntityGeneratorOptions& options,
    bool self_contained) {
    GeneratedEntityHeader result;
    const auto body = generate_view_body(schema, options, result.warnings);
    if (!self_contained) {
        result.code = body;
        return result;
    }

    std::ostringstream out;
    out << "#pragma once\n\n#include <metal/metal.hpp>\n\n";
    out << "#include <cstdint>\n#include <optional>\n#include <string>\n\n";
    out << "namespace " << identifier(options.namespace_name, "entities") << " {\n\n";
    out << body;
    out << "} // namespace " << identifier(options.namespace_name, "entities") << "\n";
    result.code = out.str();
    return result;
}

} // namespace

GeneratedEntityHeader generate_view_header(
    const DatabaseSchema& schema,
    const EntityGeneratorOptions& options) {
    return view_header_impl(schema, options, true);
}

GeneratedEntityHeader generate_sqlite_view_header(
    DbExecutor& executor,
    const EntityGeneratorOptions& options,
    const IntrospectOptions& introspect_options) {
    auto resolved = introspect_options;
    resolved.include_views = true;
    return generate_view_header(introspect_sqlite(executor, resolved), options);
}

GeneratedEntityHeader generate_model_header(
    const DatabaseSchema& schema,
    const EntityGeneratorOptions& options) {
    DatabaseSchema tables_only = schema;
    tables_only.views.clear();
    auto result = generate_entity_header(tables_only, options);
    if (schema.views.empty()) return result;

    auto views = view_header_impl(schema, options, false);
    result.warnings.insert(result.warnings.end(), views.warnings.begin(), views.warnings.end());

    const auto namespace_name = identifier(options.namespace_name, "entities");
    const std::string closing = "} // namespace " + namespace_name + "\n";
    const auto position = result.code.rfind(closing);
    if (position == std::string::npos) {
        throw std::logic_error("MetalORM: generated entity header is missing its namespace terminator");
    }
    result.code.insert(position, views.code);
    return result;
}

GeneratedEntityHeader generate_sqlite_model_header(
    DbExecutor& executor,
    const EntityGeneratorOptions& options,
    const IntrospectOptions& introspect_options) {
    return generate_model_header(introspect_sqlite(executor, introspect_options), options);
}

} // namespace metal
