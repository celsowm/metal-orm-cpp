#pragma once

#include "metal/dto.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace metal {

enum class OpenApiType {
    string,
    number,
    integer,
    boolean,
    array,
    object,
    null_value
};

enum class OpenApiDialect {
    v3_0,
    v3_1
};

struct OpenApiSchema {
    std::vector<OpenApiType> types;
    std::unordered_map<std::string, std::shared_ptr<OpenApiSchema>> properties;
    std::shared_ptr<OpenApiSchema> items;
    std::vector<std::string> required;
    std::vector<Value> enum_values;
    std::optional<std::string> format;
    std::optional<std::string> description;
    std::optional<Value> example;
    bool nullable{false};
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::optional<Value> default_value;
    std::optional<std::string> ref;
    std::vector<std::shared_ptr<OpenApiSchema>> all_of;
    std::vector<std::shared_ptr<OpenApiSchema>> one_of;
};

enum class OpenApiParameterLocation {
    query,
    path,
    header,
    cookie
};

struct OpenApiParameterObject {
    std::string name;
    OpenApiParameterLocation in{OpenApiParameterLocation::query};
    bool required{false};
    std::shared_ptr<OpenApiSchema> schema;
    std::optional<std::string> description;
};

struct OpenApiResponseObject {
    std::string description;
    std::shared_ptr<OpenApiSchema> json_schema;
};

struct OpenApiRequestBody {
    std::optional<std::string> description;
    bool required{false};
    std::shared_ptr<OpenApiSchema> json_schema;
};

struct OpenApiOperation {
    std::optional<std::string> summary;
    std::optional<std::string> description;
    std::vector<OpenApiParameterObject> parameters;
    std::optional<OpenApiRequestBody> request_body;
    std::unordered_map<std::string, OpenApiResponseObject> responses;
};

struct OpenApiDocumentInfo {
    std::string title;
    std::string version;
    std::optional<std::string> description;
};

enum class OpenApiHttpMethod {
    get,
    post,
    put,
    patch,
    delete_method
};

struct ApiRouteDefinition {
    std::string path;
    OpenApiHttpMethod method{OpenApiHttpMethod::get};
    OpenApiOperation operation;
};

struct OpenApiDocument {
    std::string openapi;
    OpenApiDocumentInfo info;
    std::unordered_map<std::string, std::unordered_map<std::string, OpenApiOperation>> paths;
};

inline std::string openapi_version(OpenApiDialect dialect) {
    return dialect == OpenApiDialect::v3_0 ? "3.0.3" : "3.1.0";
}

inline std::string openapi_method_name(OpenApiHttpMethod method) {
    switch (method) {
        case OpenApiHttpMethod::get: return "get";
        case OpenApiHttpMethod::post: return "post";
        case OpenApiHttpMethod::put: return "put";
        case OpenApiHttpMethod::patch: return "patch";
        case OpenApiHttpMethod::delete_method: return "delete";
    }
    return "get";
}

inline OpenApiDocument generate_openapi_document(
    OpenApiDocumentInfo info,
    std::vector<ApiRouteDefinition> routes,
    OpenApiDialect dialect = OpenApiDialect::v3_1) {
    OpenApiDocument out{openapi_version(dialect), std::move(info), {}};
    for (auto& route : routes) {
        out.paths[route.path][openapi_method_name(route.method)] = std::move(route.operation);
    }
    return out;
}

namespace detail {

template <typename T>
OpenApiSchema scalar_openapi_schema(OpenApiDialect dialect) {
    using Raw = std::remove_cvref_t<T>;
    using U = optional_value_t<Raw>;
    static_assert(PersistableValue<U>,
                  "MetalORM: OpenAPI schema generation requires a persistent scalar type");

    OpenApiSchema schema;
    if constexpr (std::same_as<U, bool>) {
        schema.types.push_back(OpenApiType::boolean);
    } else if constexpr (std::is_integral_v<U>) {
        schema.types.push_back(OpenApiType::integer);
        schema.format = sizeof(U) <= 4 ? "int32" : "int64";
    } else if constexpr (std::is_floating_point_v<U>) {
        schema.types.push_back(OpenApiType::number);
        schema.format = "double";
    } else {
        schema.types.push_back(OpenApiType::string);
    }

    if constexpr (is_optional_v<Raw>) {
        if (dialect == OpenApiDialect::v3_0) {
            schema.nullable = true;
        } else {
            schema.types.push_back(OpenApiType::null_value);
        }
    }
    return schema;
}

template <DtoMode Mode, reflect::Entity T, std::meta::info... Excluded>
OpenApiSchema dto_openapi_schema_impl(OpenApiDialect dialect) {
    static_assert(validate_dto_members<T, Excluded...>());

    OpenApiSchema root;
    root.types.push_back(OpenApiType::object);

    reflect::for_each_column<T>([&]<std::meta::info Member>() {
        if constexpr (!dto_member_excluded<Member, Excluded...>()) {
            constexpr bool generated = reflect::has<mapping::generated_t>(Member);
            constexpr bool primary = reflect::has<mapping::primary_key_t>(Member);
            using M = reflect::member_type_t<Member>;
            constexpr bool nullable = is_optional_v<M>;

            if constexpr ((Mode == DtoMode::create || Mode == DtoMode::update) && generated) {
                return;
            }

            const auto name = dto_member_name<Member>();
            root.properties.emplace(
                name,
                std::make_shared<OpenApiSchema>(scalar_openapi_schema<M>(dialect)));

            if constexpr (Mode == DtoMode::response) {
                if constexpr (!nullable || primary) root.required.push_back(name);
            } else if constexpr (Mode == DtoMode::create) {
                // Reflected DB defaults are not present yet in the C++ mapping layer.
                // Optional members are therefore the only create-time omission signal.
                if constexpr (!nullable) root.required.push_back(name);
            }
        }
    });

    return root;
}

} // namespace detail

template <reflect::Entity T, std::meta::info... Excluded>
OpenApiSchema dto_to_openapi_schema(OpenApiDialect dialect = OpenApiDialect::v3_1) {
    return detail::dto_openapi_schema_impl<DtoMode::response, T, Excluded...>(dialect);
}

template <reflect::Entity T, std::meta::info... Excluded>
OpenApiSchema create_dto_to_openapi_schema(OpenApiDialect dialect = OpenApiDialect::v3_1) {
    return detail::dto_openapi_schema_impl<DtoMode::create, T, Excluded...>(dialect);
}

template <reflect::Entity T, std::meta::info... Excluded>
OpenApiSchema update_dto_to_openapi_schema(OpenApiDialect dialect = OpenApiDialect::v3_1) {
    return detail::dto_openapi_schema_impl<DtoMode::update, T, Excluded...>(dialect);
}

inline std::vector<OpenApiParameterObject> pagination_params_schema() {
    auto page = std::make_shared<OpenApiSchema>();
    page->types = {OpenApiType::integer};
    page->format = "int32";
    page->minimum = 1.0;

    auto page_size = std::make_shared<OpenApiSchema>(*page);
    auto sort_by = std::make_shared<OpenApiSchema>();
    sort_by->types = {OpenApiType::string};
    auto sort_order = std::make_shared<OpenApiSchema>();
    sort_order->types = {OpenApiType::string};
    sort_order->enum_values = {Value{std::string{"asc"}}, Value{std::string{"desc"}}};

    return {
        OpenApiParameterObject{"page", OpenApiParameterLocation::query, false, page, "Page number (1-based)"},
        OpenApiParameterObject{"pageSize", OpenApiParameterLocation::query, false, page_size, "Number of items per page"},
        OpenApiParameterObject{"sortBy", OpenApiParameterLocation::query, false, sort_by, "Public sort key"},
        OpenApiParameterObject{"sortOrder", OpenApiParameterLocation::query, false, sort_order, "Sort direction"}
    };
}

inline OpenApiSchema paged_response_to_openapi_schema(OpenApiSchema item_schema) {
    OpenApiSchema root;
    root.types = {OpenApiType::object};

    auto items = std::make_shared<OpenApiSchema>();
    items->types = {OpenApiType::array};
    items->items = std::make_shared<OpenApiSchema>(std::move(item_schema));
    root.properties.emplace("items", std::move(items));

    auto integer = [] {
        auto schema = std::make_shared<OpenApiSchema>();
        schema->types = {OpenApiType::integer};
        schema->format = "int64";
        return schema;
    };
    auto boolean = [] {
        auto schema = std::make_shared<OpenApiSchema>();
        schema->types = {OpenApiType::boolean};
        return schema;
    };

    root.properties.emplace("totalItems", integer());
    root.properties.emplace("page", integer());
    root.properties.emplace("pageSize", integer());
    root.properties.emplace("totalPages", integer());
    root.properties.emplace("hasNextPage", boolean());
    root.properties.emplace("hasPrevPage", boolean());
    root.required = {
        "items", "totalItems", "page", "pageSize", "totalPages",
        "hasNextPage", "hasPrevPage"
    };
    return root;
}

} // namespace metal
