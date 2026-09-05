#pragma once

#include "metal/openapi_relations.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace metal {

struct OpenApiReference {
    std::string ref;
};

inline OpenApiReference create_ref(std::string path) {
    return OpenApiReference{"#/components/" + std::move(path)};
}

inline OpenApiReference schema_to_ref(std::string schema_name) {
    return create_ref("schemas/" + std::move(schema_name));
}

inline OpenApiReference parameter_to_ref(std::string parameter_name) {
    return create_ref("parameters/" + std::move(parameter_name));
}

inline OpenApiReference response_to_ref(std::string response_name) {
    return create_ref("responses/" + std::move(response_name));
}

inline bool is_component_reference(const OpenApiSchema& schema) noexcept {
    return schema.ref.has_value();
}

namespace detail {

inline std::string openapi_value_fingerprint(const Value& value) {
    return std::visit([](const auto& item) -> std::string {
        using V = std::remove_cvref_t<decltype(item)>;
        if constexpr (std::same_as<V, std::nullptr_t>) {
            return "null";
        } else if constexpr (std::same_as<V, bool>) {
            return item ? "true" : "false";
        } else if constexpr (std::same_as<V, std::int64_t>) {
            return "i:" + std::to_string(item);
        } else if constexpr (std::same_as<V, double>) {
            std::ostringstream out;
            out << "d:" << std::setprecision(17) << item;
            return out.str();
        } else if constexpr (std::same_as<V, Blob>) {
            static constexpr char hex[] = "0123456789abcdef";
            std::string out = "b:" + std::to_string(item.size()) + ":";
            out.reserve(out.size() + item.size() * 2);
            for (const auto byte : item) {
                const auto value = static_cast<unsigned int>(byte);
                out.push_back(hex[(value >> 4) & 0x0f]);
                out.push_back(hex[value & 0x0f]);
            }
            return out;
        } else {
            return "s:" + std::to_string(item.size()) + ":" + item;
        }
    }, value);
}

inline void append_schema_fingerprint(
    std::string& out,
    const OpenApiSchema& schema,
    bool include_descriptive) {
    if (schema.ref) {
        out += "ref(" + *schema.ref + ")";
        return;
    }

    out += "types[";
    for (const auto type : schema.types) {
        out += std::to_string(static_cast<int>(type)) + ",";
    }
    out += "]";

    if (schema.format) out += "format(" + *schema.format + ")";
    out += schema.nullable ? "nullable(1)" : "nullable(0)";
    if (schema.minimum) out += "min(" + std::to_string(*schema.minimum) + ")";
    if (schema.maximum) out += "max(" + std::to_string(*schema.maximum) + ")";
    if (schema.default_value) {
        out += "default(" + openapi_value_fingerprint(*schema.default_value) + ")";
    }
    if (include_descriptive && schema.description) {
        out += "description(" + *schema.description + ")";
    }
    if (include_descriptive && schema.example) {
        out += "example(" + openapi_value_fingerprint(*schema.example) + ")";
    }

    out += "required[";
    for (const auto& item : schema.required) out += item + ",";
    out += "]enum[";
    for (const auto& item : schema.enum_values) {
        out += openapi_value_fingerprint(item) + ",";
    }
    out += "]";

    std::vector<std::string> keys;
    keys.reserve(schema.properties.size());
    for (const auto& [key, _] : schema.properties) keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    out += "properties{";
    for (const auto& key : keys) {
        out += key + ":";
        append_schema_fingerprint(out, *schema.properties.at(key), include_descriptive);
        out += ";";
    }
    out += "}";

    if (schema.items) {
        out += "items(";
        append_schema_fingerprint(out, *schema.items, include_descriptive);
        out += ")";
    }

    out += "allOf[";
    for (const auto& item : schema.all_of) {
        append_schema_fingerprint(out, *item, include_descriptive);
        out += ";";
    }
    out += "]oneOf[";
    for (const auto& item : schema.one_of) {
        append_schema_fingerprint(out, *item, include_descriptive);
        out += ";";
    }
    out += "]";
}

inline std::string schema_fingerprint(
    const OpenApiSchema& schema,
    bool include_descriptive) {
    std::string out;
    append_schema_fingerprint(out, schema, include_descriptive);
    return out;
}

inline std::shared_ptr<OpenApiSchema> clone_schema_ptr(
    const std::shared_ptr<OpenApiSchema>& value);

inline OpenApiSchema deep_clone_schema(const OpenApiSchema& schema) {
    OpenApiSchema out = schema;
    out.properties.clear();
    for (const auto& [key, value] : schema.properties) {
        out.properties.emplace(key, clone_schema_ptr(value));
    }
    out.items = clone_schema_ptr(schema.items);
    out.all_of.clear();
    out.all_of.reserve(schema.all_of.size());
    for (const auto& item : schema.all_of) out.all_of.push_back(clone_schema_ptr(item));
    out.one_of.clear();
    out.one_of.reserve(schema.one_of.size());
    for (const auto& item : schema.one_of) out.one_of.push_back(clone_schema_ptr(item));
    return out;
}

inline std::shared_ptr<OpenApiSchema> clone_schema_ptr(
    const std::shared_ptr<OpenApiSchema>& value) {
    if (!value) return {};
    return std::make_shared<OpenApiSchema>(deep_clone_schema(*value));
}

inline std::string normalized_component_name(std::string_view base) {
    std::string out;
    out.reserve(base.size());
    for (const unsigned char c : base) {
        if (std::isalnum(c) || c == '_') out.push_back(static_cast<char>(c));
    }
    return out;
}

inline std::string upper_first(std::string value) {
    if (!value.empty()) {
        value.front() = static_cast<char>(
            std::toupper(static_cast<unsigned char>(value.front())));
    }
    return value;
}

} // namespace detail

inline OpenApiSchema deep_clone_schema(const OpenApiSchema& schema) {
    return detail::deep_clone_schema(schema);
}

inline std::string canonicalize_schema(const OpenApiSchema& schema) {
    return detail::schema_fingerprint(schema, false);
}

inline std::string compute_schema_hash(const OpenApiSchema& schema) {
    const auto canonical = canonicalize_schema(schema);
    std::int32_t hash = 0;
    for (const unsigned char c : canonical) {
        hash = static_cast<std::int32_t>(
            static_cast<std::uint32_t>(hash) * 31u + c);
    }

    std::uint32_t magnitude = hash < 0
        ? static_cast<std::uint32_t>(-(static_cast<std::int64_t>(hash)))
        : static_cast<std::uint32_t>(hash);
    std::ostringstream out;
    out << std::hex << std::nouppercase << magnitude;
    auto hex = out.str();
    if (hex.size() < 8) hex.insert(hex.begin(), 8 - hex.size(), '0');
    if (hex.size() > 6) hex.resize(6);
    return hex;
}

struct DeterministicNamingState {
    std::unordered_map<std::string, std::string> content_hash_to_name;
    std::unordered_map<std::string, std::string> name_to_content_hash;
};

inline DeterministicNamingState create_deterministic_naming_state() {
    return {};
}

inline std::string get_deterministic_component_name(
    std::string base_name,
    const OpenApiSchema& schema,
    DeterministicNamingState& state) {
    const auto hash = compute_schema_hash(schema);
    auto normalized = detail::normalized_component_name(base_name);
    if (normalized.empty()) normalized = "Schema";

    if (auto found = state.content_hash_to_name.find(hash);
        found != state.content_hash_to_name.end()) {
        return found->second;
    }

    if (auto found = state.name_to_content_hash.find(normalized);
        found == state.name_to_content_hash.end()) {
        state.content_hash_to_name.emplace(hash, normalized);
        state.name_to_content_hash.emplace(normalized, hash);
        return normalized;
    } else if (found->second == hash) {
        return normalized;
    }

    const auto unique = normalized + "_" + hash;
    state.content_hash_to_name.emplace(hash, unique);
    state.name_to_content_hash.emplace(unique, hash);
    return unique;
}

inline OpenApiSchema replace_with_refs(
    const OpenApiSchema& schema,
    const OpenApiSchemaMap& schema_map,
    std::string_view path = "components/schemas") {
    if (schema.ref) return deep_clone_schema(schema);

    const auto exact = detail::schema_fingerprint(schema, true);
    for (const auto& [name, candidate] : schema_map) {
        if (detail::schema_fingerprint(candidate, true) == exact) {
            OpenApiSchema ref;
            ref.ref = "#/" + std::string(path) + "/" + name;
            return ref;
        }
    }

    auto out = deep_clone_schema(schema);
    for (auto& [_, value] : out.properties) {
        *value = replace_with_refs(*value, schema_map, path);
    }
    if (out.items) {
        *out.items = replace_with_refs(*out.items, schema_map, path);
    }
    for (auto& item : out.all_of) {
        *item = replace_with_refs(*item, schema_map, path);
    }
    for (auto& item : out.one_of) {
        *item = replace_with_refs(*item, schema_map, path);
    }
    return out;
}

inline void extract_reusable_schemas_impl(
    const OpenApiSchema& schema,
    OpenApiSchemaMap& existing,
    std::string prefix) {
    if (schema.ref) return;

    if (!schema.properties.empty()) {
        std::vector<std::string> keys;
        keys.reserve(schema.properties.size());
        for (const auto& [key, _] : schema.properties) keys.push_back(key);
        std::sort(keys.begin(), keys.end());
        for (const auto& key : keys) {
            extract_reusable_schemas_impl(
                *schema.properties.at(key),
                existing,
                prefix + detail::upper_first(key));
        }
    } else if (schema.items) {
        extract_reusable_schemas_impl(*schema.items, existing, prefix);
    } else {
        for (const auto& item : schema.all_of) {
            extract_reusable_schemas_impl(*item, existing, prefix);
        }
        for (const auto& item : schema.one_of) {
            extract_reusable_schemas_impl(*item, existing, prefix);
        }
    }

    if (!prefix.empty() &&
        std::find(schema.types.begin(), schema.types.end(), OpenApiType::object) != schema.types.end() &&
        !schema.properties.empty() &&
        !existing.contains(prefix)) {
        existing.emplace(prefix, deep_clone_schema(schema));
    }
}

inline OpenApiSchemaMap extract_reusable_schemas(
    const OpenApiSchema& schema,
    OpenApiSchemaMap existing = {},
    std::string prefix = {}) {
    extract_reusable_schemas_impl(schema, existing, std::move(prefix));
    return existing;
}

template <reflect::Entity T>
struct OpenApiComponentTarget {
    using entity_type = T;
    std::string name;
};

template <reflect::Entity T>
OpenApiComponentTarget<T> component_target(std::string name) {
    return OpenApiComponentTarget<T>{std::move(name)};
}

template <reflect::Entity... Ts>
OpenApiSchemaMap generate_component_schemas(
    std::string prefix,
    OpenApiDialect dialect,
    OpenApiComponentTarget<Ts>... targets) {
    OpenApiSchemaMap out;
    auto add = [&]<reflect::Entity T>(OpenApiComponentTarget<T> target) {
        out.emplace(
            prefix + target.name,
            dto_to_openapi_schema<T>(dialect));
    };
    (add(std::move(targets)), ...);
    return out;
}

template <reflect::Entity... Ts>
OpenApiSchemaMap generate_component_schemas(
    OpenApiComponentTarget<Ts>... targets) {
    return generate_component_schemas(
        std::string{},
        OpenApiDialect::v3_1,
        std::move(targets)...);
}

} // namespace metal
