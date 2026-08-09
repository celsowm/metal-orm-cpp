#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>

struct [[=metal::mapping::table{"oar_comments"}]] OarComment {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::int64_t post_id{};
    std::string body;
};

struct [[=metal::mapping::table{"oar_posts"}]] OarPost {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::int64_t user_id{};
    std::string title;

    [[=metal::mapping::has_many<^^OarComment::post_id>{}]]
    metal::has_many_collection<OarComment> comments;
};

struct [[=metal::mapping::table{"oar_profiles"}]] OarProfile {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::int64_t user_id{};
    std::string bio;
};

struct [[=metal::mapping::table{"oar_roles"}]] OarRole {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::string name;
};

struct [[=metal::mapping::table{"oar_user_roles"}]] OarUserRole {
    [[=metal::mapping::primary_key]] std::int64_t user_id{};
    [[=metal::mapping::primary_key]] std::int64_t role_id{};
};

struct [[=metal::mapping::table{"oar_users"}]] OarUser {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::string name;

    [[=metal::mapping::has_many<^^OarPost::user_id>{}]]
    metal::has_many_collection<OarPost> posts;

    [[=metal::mapping::has_one<^^OarProfile::user_id>{}]]
    metal::has_one_reference<OarProfile> profile;

    [[=metal::mapping::many_to_many<
        ^^OarUserRole,
        ^^OarUserRole::user_id,
        ^^OarUserRole::role_id>{}]]
    metal::many_to_many_collection<OarRole, OarUserRole> roles;
};

static_assert(metal::reflect::validate_mapping<OarComment>());
static_assert(metal::reflect::validate_mapping<OarPost>());
static_assert(metal::reflect::validate_mapping<OarProfile>());
static_assert(metal::reflect::validate_mapping<OarRole>());
static_assert(metal::reflect::validate_mapping<OarUserRole>());
static_assert(metal::reflect::validate_mapping<OarUser>());

static bool has_type(const metal::OpenApiSchema& schema, metal::OpenApiType type) {
    for (const auto item : schema.types) {
        if (item == type) return true;
    }
    return false;
}

int main() {
    const auto nested = metal::nested_dto_to_openapi_schema<OarUser, 3>();
    assert(has_type(nested, metal::OpenApiType::object));
    assert(nested.properties.contains("id"));
    assert(nested.properties.contains("name"));
    assert(nested.properties.contains("posts"));
    assert(nested.properties.contains("profile"));
    assert(nested.properties.contains("roles"));

    const auto posts = nested.properties.at("posts");
    assert(has_type(*posts, metal::OpenApiType::array));
    assert(posts->items);
    assert(posts->items->properties.contains("title"));
    assert(posts->items->properties.contains("comments"));
    const auto comments = posts->items->properties.at("comments");
    assert(has_type(*comments, metal::OpenApiType::array));
    assert(comments->items);
    assert(comments->items->properties.contains("body"));

    const auto profile = nested.properties.at("profile");
    assert(has_type(*profile, metal::OpenApiType::object));
    assert(profile->properties.contains("bio"));

    const auto relation_filter =
        metal::where_input_with_relations_to_openapi_schema<OarUser, 2>(
            metal::OpenApiDialect::v3_1,
            metal::DtoMemberPolicy<^^OarUser::name>{},
            metal::DtoRelationPolicy<^^OarUser::posts>{});
    assert(relation_filter.properties.size() == 2);
    assert(relation_filter.properties.contains("name"));
    assert(relation_filter.properties.contains("posts"));
    assert(!relation_filter.properties.contains("id"));
    assert(!relation_filter.properties.contains("profile"));
    assert(!relation_filter.properties.contains("roles"));

    const auto posts_filter = relation_filter.properties.at("posts");
    assert(posts_filter->properties.contains("some"));
    assert(posts_filter->properties.contains("every"));
    assert(posts_filter->properties.contains("none"));
    assert(posts_filter->properties.contains("isEmpty"));
    assert(posts_filter->properties.contains("isNotEmpty"));

    const auto some_posts = posts_filter->properties.at("some");
    assert(some_posts->properties.contains("title"));
    assert(some_posts->properties.contains("comments"));
    const auto comments_filter = some_posts->properties.at("comments");
    assert(comments_filter->properties.contains("some"));
    assert(comments_filter->properties.at("some")->properties.contains("body"));

    const auto update =
        metal::update_dto_with_relations_to_openapi_schema<OarUser, 2>();
    assert(update.properties.contains("name"));
    assert(update.properties.contains("profile"));
    assert(!update.properties.contains("posts"));
    assert(!update.properties.contains("roles"));
    assert(update.properties.at("profile")->properties.contains("bio"));

    const auto components = metal::generate_relation_components<OarUser, 3>("User");
    assert(components.size() == 5);
    assert(components.contains("User"));
    assert(components.contains("UserCreate"));
    assert(components.contains("UserUpdate"));
    assert(components.contains("UserFilter"));
    assert(components.contains("UserNested"));
    assert(components.at("UserFilter").properties.contains("posts"));
    assert(components.at("UserNested").properties.contains("roles"));

    const auto section = metal::create_api_components_section(components);
    assert(section.schemas.size() == 5);

    const auto ref = metal::schema_ref("UserNested");
    assert(ref.ref);
    assert(*ref.ref == "#/components/schemas/UserNested");
    assert(metal::schema_to_ref("User").ref == "#/components/schemas/User");
    assert(metal::parameter_to_ref("Page").ref == "#/components/parameters/Page");
    assert(metal::response_to_ref("NotFound").ref == "#/components/responses/NotFound");

    const auto generated = metal::generate_component_schemas(
        metal::component_target<OarUser>("User"),
        metal::component_target<OarPost>("Post"));
    assert(generated.size() == 2);
    assert(generated.contains("User"));
    assert(generated.contains("Post"));

    const auto user_schema = metal::dto_to_openapi_schema<OarUser>();
    auto documented_user = metal::deep_clone_schema(user_schema);
    documented_user.description = "ignored by canonical component naming";
    assert(metal::compute_schema_hash(user_schema) ==
           metal::compute_schema_hash(documented_user));

    auto naming = metal::create_deterministic_naming_state();
    const auto first_name = metal::get_deterministic_component_name(
        "User!", user_schema, naming);
    assert(first_name == "User");
    const auto same_name = metal::get_deterministic_component_name(
        "DifferentBase", documented_user, naming);
    assert(same_name == "User");
    const auto collision = metal::get_deterministic_component_name(
        "User", metal::dto_to_openapi_schema<OarPost>(), naming);
    assert(collision.rfind("User_", 0) == 0);

    metal::OpenApiSchema holder;
    holder.types = {metal::OpenApiType::object};
    holder.properties.emplace(
        "user",
        std::make_shared<metal::OpenApiSchema>(user_schema));
    const auto replaced = metal::replace_with_refs(
        holder,
        metal::OpenApiSchemaMap{{"User", user_schema}});
    assert(replaced.properties.at("user")->ref);
    assert(*replaced.properties.at("user")->ref == "#/components/schemas/User");

    const auto extracted = metal::extract_reusable_schemas(nested, {}, "User");
    assert(!extracted.empty());
    assert(extracted.contains("User"));

    auto clone = metal::deep_clone_schema(nested);
    clone.properties.at("name")->description = "clone only";
    assert(!nested.properties.at("name")->description);

    const auto filter30 =
        metal::where_input_with_relations_to_openapi_schema<OarUser, 1>(
            metal::OpenApiDialect::v3_0);
    assert(filter30.properties.contains("posts"));
}
