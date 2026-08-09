#pragma once

#include "metal/openapi.hpp"
#include "metal/tree.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace metal {

inline std::shared_ptr<OpenApiSchema> tree_integer_schema(
    std::optional<std::string> description = std::nullopt,
    std::optional<double> minimum = std::nullopt) {
    auto schema = std::make_shared<OpenApiSchema>();
    schema->types = {OpenApiType::integer};
    schema->format = "int64";
    schema->description = std::move(description);
    schema->minimum = minimum;
    return schema;
}

inline std::shared_ptr<OpenApiSchema> tree_boolean_schema(std::string description) {
    auto schema = std::make_shared<OpenApiSchema>();
    schema->types = {OpenApiType::boolean};
    schema->description = std::move(description);
    return schema;
}

template <reflect::Entity T, std::meta::info... Excluded>
OpenApiSchema tree_node_result_to_openapi_schema(
    OpenApiDialect dialect = OpenApiDialect::v3_1) {
    static_assert(reflect::validate_tree_mapping<T>());
    constexpr auto Parent = reflect::tree_parent_member<T>();
    using ParentType = reflect::member_type_t<Parent>;

    OpenApiSchema root;
    root.types = {OpenApiType::object};
    root.description = "Tree node result with nested-set boundaries and metadata";
    root.properties.emplace(
        "data",
        std::make_shared<OpenApiSchema>(dto_to_openapi_schema<T, Excluded...>(dialect)));
    root.properties.emplace(
        "lft",
        tree_integer_schema("Left boundary value (nested set)"));
    root.properties.emplace(
        "rght",
        tree_integer_schema("Right boundary value (nested set)"));

    auto parent = std::make_shared<OpenApiSchema>(
        detail::scalar_openapi_schema<ParentType>(dialect));
    parent->description = "Parent identifier (null for roots)";
    root.properties.emplace("parentId", std::move(parent));

    constexpr auto Depth = reflect::tree_depth_member<T>();
    if constexpr (Depth != std::meta::info{}) {
        root.properties.emplace(
            "depth",
            tree_integer_schema("Depth level (0 = root)", 0.0));
    }

    root.properties.emplace(
        "isLeaf",
        tree_boolean_schema("Whether this node has no children"));
    root.properties.emplace(
        "isRoot",
        tree_boolean_schema("Whether this node has no parent"));
    root.properties.emplace(
        "childCount",
        tree_integer_schema("Number of descendants", 0.0));

    root.required = {
        "data", "lft", "rght", "parentId", "isLeaf", "isRoot", "childCount"
    };
    return root;
}

template <reflect::Entity T, std::meta::info... Excluded>
OpenApiSchema threaded_tree_node_to_openapi_schema(
    std::optional<std::string> component_name = std::nullopt,
    OpenApiDialect dialect = OpenApiDialect::v3_1) {
    static_assert(reflect::validate_tree_mapping<T>());

    OpenApiSchema root;
    root.types = {OpenApiType::object};
    root.description = "Threaded tree node with nested children";
    root.properties.emplace(
        "node",
        std::make_shared<OpenApiSchema>(dto_to_openapi_schema<T, Excluded...>(dialect)));

    auto children = std::make_shared<OpenApiSchema>();
    children->types = {OpenApiType::array};
    children->description = "Child nodes in the tree hierarchy";
    children->items = std::make_shared<OpenApiSchema>();
    if (component_name && !component_name->empty()) {
        children->items->ref = "#/components/schemas/" + *component_name;
    } else {
        children->items->types = {OpenApiType::object};
    }
    root.properties.emplace("children", std::move(children));
    root.required = {"node", "children"};
    return root;
}

inline OpenApiSchema tree_list_entry_to_openapi_schema(
    OpenApiType key_type = OpenApiType::integer,
    OpenApiType value_type = OpenApiType::string) {
    OpenApiSchema root;
    root.types = {OpenApiType::object};
    root.description = "Tree list entry for select/dropdown rendering";

    auto key = std::make_shared<OpenApiSchema>();
    key->types = {key_type};
    key->description = "Entry key (normally the primary key)";
    root.properties.emplace("key", std::move(key));

    auto value = std::make_shared<OpenApiSchema>();
    value->types = {value_type};
    value->description = "Display value with depth prefix";
    root.properties.emplace("value", std::move(value));
    root.properties.emplace("depth", tree_integer_schema("Depth level", 0.0));
    root.required = {"key", "value", "depth"};
    return root;
}

template <reflect::Entity T, std::meta::info... Excluded>
std::unordered_map<std::string, OpenApiSchema> generate_tree_components(
    std::string base_name,
    OpenApiDialect dialect = OpenApiDialect::v3_1) {
    static_assert(reflect::validate_tree_mapping<T>());
    const auto threaded_name = base_name + "TreeNode";

    std::unordered_map<std::string, OpenApiSchema> out;
    out.emplace(base_name, dto_to_openapi_schema<T, Excluded...>(dialect));
    out.emplace(
        base_name + "NodeResult",
        tree_node_result_to_openapi_schema<T, Excluded...>(dialect));
    out.emplace(
        threaded_name,
        threaded_tree_node_to_openapi_schema<T, Excluded...>(threaded_name, dialect));

    OpenApiSchema list;
    list.types = {OpenApiType::array};
    list.items = std::make_shared<OpenApiSchema>(tree_list_entry_to_openapi_schema());
    list.description = "Flat tree entries for select/dropdown rendering";
    out.emplace(base_name + "TreeList", std::move(list));

    OpenApiSchema threaded;
    threaded.types = {OpenApiType::array};
    threaded.items = std::make_shared<OpenApiSchema>();
    threaded.items->ref = "#/components/schemas/" + threaded_name;
    threaded.description = "Threaded tree structure";
    out.emplace(base_name + "ThreadedTree", std::move(threaded));
    return out;
}

} // namespace metal
