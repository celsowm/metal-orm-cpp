#include <metal/metal.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

struct [[=metal::mapping::table{"api_users"}]] ApiUser {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::column{"display_name"}]]
    std::string displayName;

    std::optional<std::string> bio;
    bool active{};
    double score{};
};

static_assert(metal::reflect::validate_mapping<ApiUser>());

static const metal::DtoField& field(const metal::DtoDescriptor& dto, const std::string& name) {
    auto found = std::find_if(dto.fields.begin(), dto.fields.end(), [&](const auto& item) {
        return item.name == name;
    });
    assert(found != dto.fields.end());
    return *found;
}

static bool required(const metal::OpenApiSchema& schema, const std::string& name) {
    return std::find(schema.required.begin(), schema.required.end(), name) != schema.required.end();
}

static bool has_type(const metal::OpenApiSchema& schema, metal::OpenApiType type) {
    return std::find(schema.types.begin(), schema.types.end(), type) != schema.types.end();
}

int main() {
    ApiUser user{
        .id = 42,
        .displayName = "Celso",
        .bio = std::string{"C++26"},
        .active = true,
        .score = 9.5
    };

    const auto response_desc = metal::describe_response_dto<ApiUser, ^^ApiUser::score>();
    assert(response_desc.table == "api_users");
    assert(response_desc.fields.size() == 4);
    assert(field(response_desc, "id").required);
    assert(field(response_desc, "id").generated);
    assert(field(response_desc, "displayName").column_name == "display_name");
    assert(field(response_desc, "displayName").required);
    assert(field(response_desc, "bio").nullable);
    assert(!field(response_desc, "bio").required);

    const auto create_desc = metal::describe_create_dto<ApiUser>();
    assert(create_desc.fields.size() == 4);
    assert(std::none_of(create_desc.fields.begin(), create_desc.fields.end(), [](const auto& item) {
        return item.name == "id";
    }));
    assert(field(create_desc, "displayName").required);
    assert(!field(create_desc, "bio").required);
    assert(field(create_desc, "active").required);
    assert(field(create_desc, "score").required);

    const auto update_desc = metal::describe_update_dto<ApiUser>();
    assert(update_desc.fields.size() == 4);
    assert(std::all_of(update_desc.fields.begin(), update_desc.fields.end(), [](const auto& item) {
        return !item.required;
    }));

    const auto response = metal::to_response_dto<ApiUser, ^^ApiUser::score>(user);
    assert(response.size() == 4);
    assert(response.contains("displayName"));
    assert(!response.contains("display_name"));
    assert(!response.contains("score"));
    assert(metal::from_value<std::string>(response.at("displayName")) == "Celso");

    const auto create = metal::to_create_dto<ApiUser>(user);
    assert(!create.contains("id"));
    assert(create.contains("displayName"));
    assert(create.contains("bio"));

    const auto picked = metal::pick_dto<^^ApiUser::id, ^^ApiUser::displayName>(user);
    assert(picked.size() == 2);
    assert(metal::from_value<std::int64_t>(picked.at("id")) == 42);

    const auto merged = metal::to_response(
        metal::Row{{"name", metal::Value{std::string{"input"}}}},
        metal::Row{
            {"id", metal::Value{std::int64_t{7}}},
            {"name", metal::Value{std::string{"generated"}}}
        });
    assert(metal::from_value<std::int64_t>(merged.at("id")) == 7);
    assert(metal::from_value<std::string>(merged.at("name")) == "generated");

    const auto defaults = metal::with_defaults(
        metal::Row{{"active", metal::Value{false}}},
        metal::Row{
            {"active", metal::Value{true}},
            {"score", metal::Value{1.0}}
        });
    assert(!metal::from_value<bool>(defaults.at("active")));
    assert(metal::from_value<double>(defaults.at("score")) == 1.0);

    const auto excluded = metal::exclude_fields(response, {"bio"});
    assert(!excluded.contains("bio"));
    const auto public_only = metal::pick_fields(response, {"id", "displayName"});
    assert(public_only.size() == 2);
    const auto mapped = metal::map_fields(public_only, {{"displayName", "display_name"}});
    assert(mapped.contains("display_name"));
    assert(!mapped.contains("displayName"));

    metal::PageResult page;
    page.items = {
        metal::Row{{"id", metal::Value{std::int64_t{21}}}},
        metal::Row{{"id", metal::Value{std::int64_t{22}}}}
    };
    page.total_items = 41;
    page.page = 2;
    page.page_size = 20;
    const auto paged = metal::to_paged_response(std::move(page));
    assert(paged.items.size() == 2);
    assert(paged.total_pages == 3);
    assert(paged.has_next_page);
    assert(paged.has_prev_page);
    assert(metal::calculate_total_pages(0, 20) == 1);
    bool invalid_page_size = false;
    try {
        (void)metal::calculate_total_pages(1, 0);
    } catch (const std::invalid_argument&) {
        invalid_page_size = true;
    }
    assert(invalid_page_size);

    const auto response_schema = metal::dto_to_openapi_schema<ApiUser, ^^ApiUser::score>();
    assert(has_type(response_schema, metal::OpenApiType::object));
    assert(response_schema.properties.size() == 4);
    assert(response_schema.properties.contains("displayName"));
    assert(!response_schema.properties.contains("display_name"));
    assert(!response_schema.properties.contains("score"));
    assert(required(response_schema, "id"));
    assert(required(response_schema, "displayName"));
    assert(!required(response_schema, "bio"));

    const auto id_schema = response_schema.properties.at("id");
    assert(has_type(*id_schema, metal::OpenApiType::integer));
    assert(id_schema->format && *id_schema->format == "int64");

    const auto bio31 = response_schema.properties.at("bio");
    assert(has_type(*bio31, metal::OpenApiType::string));
    assert(has_type(*bio31, metal::OpenApiType::null_value));
    assert(!bio31->nullable);

    const auto response30 = metal::dto_to_openapi_schema<ApiUser>(metal::OpenApiDialect::v3_0);
    const auto bio30 = response30.properties.at("bio");
    assert(has_type(*bio30, metal::OpenApiType::string));
    assert(!has_type(*bio30, metal::OpenApiType::null_value));
    assert(bio30->nullable);

    const auto create_schema = metal::create_dto_to_openapi_schema<ApiUser>();
    assert(!create_schema.properties.contains("id"));
    assert(required(create_schema, "displayName"));
    assert(!required(create_schema, "bio"));

    const auto update_schema = metal::update_dto_to_openapi_schema<ApiUser>();
    assert(!update_schema.properties.contains("id"));
    assert(update_schema.required.empty());

    const auto filter_schema = metal::where_input_to_openapi_schema<
        ApiUser,
        ^^ApiUser::displayName,
        ^^ApiUser::bio,
        ^^ApiUser::active,
        ^^ApiUser::score>();
    assert(filter_schema.properties.size() == 4);
    assert(filter_schema.properties.contains("displayName"));
    assert(!filter_schema.properties.contains("display_name"));
    assert(!filter_schema.properties.contains("id"));

    const auto display_filter = filter_schema.properties.at("displayName");
    assert(display_filter->properties.contains("equals"));
    assert(display_filter->properties.contains("not"));
    assert(display_filter->properties.contains("in"));
    assert(display_filter->properties.contains("notIn"));
    assert(display_filter->properties.contains("contains"));
    assert(display_filter->properties.contains("startsWith"));
    assert(display_filter->properties.contains("endsWith"));
    assert(display_filter->properties.contains("mode"));
    assert(!display_filter->properties.contains("gte"));

    const auto active_filter = filter_schema.properties.at("active");
    assert(active_filter->properties.size() == 2);
    assert(active_filter->properties.contains("equals"));
    assert(active_filter->properties.contains("not"));

    const auto score_filter = filter_schema.properties.at("score");
    assert(score_filter->properties.contains("equals"));
    assert(score_filter->properties.contains("in"));
    assert(score_filter->properties.contains("lt"));
    assert(score_filter->properties.contains("lte"));
    assert(score_filter->properties.contains("gt"));
    assert(score_filter->properties.contains("gte"));
    assert(!score_filter->properties.contains("contains"));

    const auto bio_filter = filter_schema.properties.at("bio");
    assert(has_type(*bio_filter, metal::OpenApiType::object));
    assert(has_type(*bio_filter, metal::OpenApiType::null_value));
    const auto filter30 = metal::where_input_to_openapi_schema<ApiUser>(metal::OpenApiDialect::v3_0);
    assert(filter30.properties.at("bio")->nullable);

    const auto params = metal::pagination_params_schema();
    assert(params.size() == 4);
    assert(params[0].name == "page");
    assert(params[0].schema && params[0].schema->minimum == 1.0);

    const auto paged_schema = metal::paged_response_to_openapi_schema(
        metal::dto_to_openapi_schema<ApiUser>());
    assert(paged_schema.properties.size() == 7);
    assert(paged_schema.properties.at("items")->items);
    assert(paged_schema.required.size() == 7);

    metal::OpenApiOperation operation;
    operation.summary = "List users";
    operation.parameters = metal::pagination_params_schema();
    operation.responses.emplace(
        "200",
        metal::OpenApiResponseObject{
            "OK",
            std::make_shared<metal::OpenApiSchema>(paged_schema)
        });

    const auto document = metal::generate_openapi_document(
        metal::OpenApiDocumentInfo{"Metal API", "1.0.0", std::nullopt},
        {metal::ApiRouteDefinition{"/users", metal::OpenApiHttpMethod::get, std::move(operation)}});
    assert(document.openapi == "3.1.0");
    assert(document.paths.contains("/users"));
    assert(document.paths.at("/users").contains("get"));
}
