#include <metal/metal.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <string>

struct [[=metal::mapping::table{"api_categories"}]] ApiCategory {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;

    [[=metal::mapping::tree_parent]]
    std::optional<std::int64_t> parentId;
    [[=metal::mapping::tree_left]]
    std::int64_t lft{};
    [[=metal::mapping::tree_right]]
    std::int64_t rght{};
    [[=metal::mapping::tree_depth]]
    std::optional<std::int64_t> depth;
};

static_assert(metal::reflect::validate_mapping<ApiCategory>());
static_assert(metal::reflect::validate_tree_mapping<ApiCategory>());

static bool has_type(const metal::OpenApiSchema& schema, metal::OpenApiType type) {
    return std::find(schema.types.begin(), schema.types.end(), type) != schema.types.end();
}

static bool required(const metal::OpenApiSchema& schema, const std::string& name) {
    return std::find(schema.required.begin(), schema.required.end(), name) != schema.required.end();
}

int main() {
    const auto result = metal::tree_node_result_to_openapi_schema<ApiCategory>();
    assert(has_type(result, metal::OpenApiType::object));
    assert(result.properties.contains("data"));
    assert(result.properties.contains("lft"));
    assert(result.properties.contains("rght"));
    assert(result.properties.contains("parentId"));
    assert(result.properties.contains("depth"));
    assert(result.properties.contains("isLeaf"));
    assert(result.properties.contains("isRoot"));
    assert(result.properties.contains("childCount"));
    assert(required(result, "data"));
    assert(required(result, "parentId"));
    assert(!required(result, "depth"));

    const auto parent31 = result.properties.at("parentId");
    assert(has_type(*parent31, metal::OpenApiType::integer));
    assert(has_type(*parent31, metal::OpenApiType::null_value));
    assert(parent31->format && *parent31->format == "int64");

    const auto result30 = metal::tree_node_result_to_openapi_schema<ApiCategory>(
        metal::OpenApiDialect::v3_0);
    const auto parent30 = result30.properties.at("parentId");
    assert(parent30->nullable);
    assert(!has_type(*parent30, metal::OpenApiType::null_value));

    const auto threaded = metal::threaded_tree_node_to_openapi_schema<ApiCategory>(
        std::string{"CategoryTreeNode"});
    assert(threaded.properties.contains("node"));
    assert(threaded.properties.contains("children"));
    const auto children = threaded.properties.at("children");
    assert(has_type(*children, metal::OpenApiType::array));
    assert(children->items);
    assert(children->items->ref);
    assert(*children->items->ref == "#/components/schemas/CategoryTreeNode");

    const auto list_entry = metal::tree_list_entry_to_openapi_schema();
    assert(list_entry.properties.contains("key"));
    assert(list_entry.properties.contains("value"));
    assert(list_entry.properties.contains("depth"));
    assert(list_entry.required.size() == 3);

    const auto components = metal::generate_tree_components<ApiCategory>("Category");
    assert(components.size() == 5);
    assert(components.contains("Category"));
    assert(components.contains("CategoryNodeResult"));
    assert(components.contains("CategoryTreeNode"));
    assert(components.contains("CategoryTreeList"));
    assert(components.contains("CategoryThreadedTree"));

    const auto& tree_list = components.at("CategoryTreeList");
    assert(has_type(tree_list, metal::OpenApiType::array));
    assert(tree_list.items);

    const auto& threaded_tree = components.at("CategoryThreadedTree");
    assert(threaded_tree.items);
    assert(threaded_tree.items->ref);
    assert(*threaded_tree.items->ref == "#/components/schemas/CategoryTreeNode");
}
