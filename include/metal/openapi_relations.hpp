#pragma once

#include "metal/dto_relation_filter.hpp"
#include "metal/openapi.hpp"
#include "metal/query.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace metal {

using OpenApiSchemaMap = std::unordered_map<std::string, OpenApiSchema>;

struct OpenApiComponentSection {
    OpenApiSchemaMap schemas;
    std::unordered_map<std::string, OpenApiParameterObject> parameters;
    std::unordered_map<std::string, OpenApiResponseObject> responses;
};

inline OpenApiSchema schema_ref(std::string schema_name) {
    OpenApiSchema schema;
    schema.ref = "#/components/schemas/" + std::move(schema_name);
    return schema;
}

inline OpenApiComponentSection create_api_components_section(
    OpenApiSchemaMap schemas,
    std::unordered_map<std::string, OpenApiParameterObject> parameters = {},
    std::unordered_map<std::string, OpenApiResponseObject> responses = {}) {
    return OpenApiComponentSection{
        std::move(schemas),
        std::move(parameters),
        std::move(responses)};
}

namespace detail {

template <mapping::relation_kind Kind>
inline constexpr bool openapi_single_relation_v =
    Kind == mapping::relation_kind::belongs_to ||
    Kind == mapping::relation_kind::has_one ||
    Kind == mapping::relation_kind::morph_one;

template <mapping::relation_kind Kind>
inline constexpr bool openapi_many_relation_v =
    Kind == mapping::relation_kind::has_many ||
    Kind == mapping::relation_kind::many_to_many ||
    Kind == mapping::relation_kind::morph_many;

template <std::size_t Depth, reflect::Entity T>
OpenApiSchema nested_dto_openapi_impl(OpenApiDialect dialect) {
    OpenApiSchema root;
    root.types = {OpenApiType::object};
    if constexpr (Depth == 0) return root;

    root = dto_to_openapi_schema<T>(dialect);

    template for (constexpr auto Relation : reflect::data_members<T>()) {
        if constexpr (reflect::has_relation_annotation<Relation>()) {
            using A = reflect::relation_annotation_t<Relation>;
            using Traits = mapping::relation_annotation_traits<A>;
            const auto name = dto_relation_name<Relation>();

            if constexpr (Traits::kind == mapping::relation_kind::morph_to) {
                auto polymorphic = std::make_shared<OpenApiSchema>();
                polymorphic->types = {OpenApiType::object};
                polymorphic->description =
                    "Polymorphic morphTo relation; discriminator-specific oneOf is not inferred";
                root.properties.emplace(name, std::move(polymorphic));
            } else {
                using Target = relation_filter_target_t<Relation>;
                auto nested = nested_dto_openapi_impl<Depth - 1, Target>(dialect);

                if constexpr (openapi_many_relation_v<Traits::kind>) {
                    auto array = std::make_shared<OpenApiSchema>();
                    array->types = {OpenApiType::array};
                    array->items = std::make_shared<OpenApiSchema>(std::move(nested));
                    root.properties.emplace(name, std::move(array));
                } else if constexpr (openapi_single_relation_v<Traits::kind>) {
                    root.properties.emplace(
                        name,
                        std::make_shared<OpenApiSchema>(std::move(nested)));
                }
            }
        }
    }
    return root;
}

template <std::size_t Depth, reflect::Entity T>
OpenApiSchema nested_where_openapi_impl(OpenApiDialect dialect) {
    OpenApiSchema root = where_input_to_openapi_schema<T>(dialect);
    if constexpr (Depth == 0) return root;

    template for (constexpr auto Relation : reflect::data_members<T>()) {
        if constexpr (reflect::has_relation_annotation<Relation>()) {
            using A = reflect::relation_annotation_t<Relation>;
            using Traits = mapping::relation_annotation_traits<A>;
            const auto name = dto_relation_name<Relation>();

            if constexpr (Traits::kind == mapping::relation_kind::morph_to) {
                auto unsupported = std::make_shared<OpenApiSchema>();
                unsupported->types = {OpenApiType::object};
                unsupported->description =
                    "morphTo filtering is discriminator-dependent and unsupported";
                root.properties.emplace(name, std::move(unsupported));
            } else {
                using Target = relation_filter_target_t<Relation>;
                auto predicate = nested_where_openapi_impl<Depth - 1, Target>(dialect);

                OpenApiSchema relation;
                relation.types = {OpenApiType::object};
                relation.properties.emplace(
                    "some", std::make_shared<OpenApiSchema>(predicate));
                relation.properties.emplace(
                    "every", std::make_shared<OpenApiSchema>(predicate));
                relation.properties.emplace(
                    "none", std::make_shared<OpenApiSchema>(std::move(predicate)));

                auto boolean_schema = [] {
                    auto schema = std::make_shared<OpenApiSchema>();
                    schema->types = {OpenApiType::boolean};
                    return schema;
                };
                relation.properties.emplace("isEmpty", boolean_schema());
                relation.properties.emplace("isNotEmpty", boolean_schema());

                root.properties.emplace(
                    name,
                    std::make_shared<OpenApiSchema>(std::move(relation)));
            }
        }
    }
    return root;
}

template <std::size_t Depth, reflect::Entity T>
OpenApiSchema update_with_relations_impl(OpenApiDialect dialect) {
    auto root = update_dto_to_openapi_schema<T>(dialect);
    if constexpr (Depth == 0) return root;

    template for (constexpr auto Relation : reflect::data_members<T>()) {
        if constexpr (reflect::has_relation_annotation<Relation>()) {
            using A = reflect::relation_annotation_t<Relation>;
            using Traits = mapping::relation_annotation_traits<A>;
            if constexpr (Traits::kind != mapping::relation_kind::morph_to &&
                          openapi_single_relation_v<Traits::kind>) {
                using Target = relation_filter_target_t<Relation>;
                root.properties.emplace(
                    dto_relation_name<Relation>(),
                    std::make_shared<OpenApiSchema>(
                        update_with_relations_impl<Depth - 1, Target>(dialect)));
            }
        }
    }
    return root;
}

} // namespace detail

template <reflect::Entity T, std::size_t Depth = 2>
OpenApiSchema nested_dto_to_openapi_schema(
    OpenApiDialect dialect = OpenApiDialect::v3_1) {
    return detail::nested_dto_openapi_impl<Depth, T>(dialect);
}

template <
    reflect::Entity T,
    std::size_t Depth = 3,
    std::meta::info... AllowedFields,
    std::meta::info... AllowedRelations>
OpenApiSchema where_input_with_relations_to_openapi_schema(
    OpenApiDialect dialect,
    DtoMemberPolicy<AllowedFields...>,
    DtoRelationPolicy<AllowedRelations...>) {
    static_assert(detail::validate_dto_members<T, AllowedFields...>());
    static_assert(detail::validate_dto_relations<T, AllowedRelations...>());

    auto root = where_input_to_openapi_schema<T, AllowedFields...>(dialect);
    if constexpr (Depth == 0) return root;

    template for (constexpr auto Relation : reflect::data_members<T>()) {
        if constexpr (reflect::has_relation_annotation<Relation>()) {
            if constexpr (detail::dto_relation_allowed<Relation, AllowedRelations...>()) {
                using A = reflect::relation_annotation_t<Relation>;
                using Traits = mapping::relation_annotation_traits<A>;
                const auto name = detail::dto_relation_name<Relation>();

                if constexpr (Traits::kind == mapping::relation_kind::morph_to) {
                    auto unsupported = std::make_shared<OpenApiSchema>();
                    unsupported->types = {OpenApiType::object};
                    unsupported->description =
                        "morphTo filtering is discriminator-dependent and unsupported";
                    root.properties.emplace(name, std::move(unsupported));
                } else {
                    using Target = detail::relation_filter_target_t<Relation>;
                    auto predicate = detail::nested_where_openapi_impl<Depth - 1, Target>(dialect);

                    OpenApiSchema relation;
                    relation.types = {OpenApiType::object};
                    relation.properties.emplace(
                        "some", std::make_shared<OpenApiSchema>(predicate));
                    relation.properties.emplace(
                        "every", std::make_shared<OpenApiSchema>(predicate));
                    relation.properties.emplace(
                        "none", std::make_shared<OpenApiSchema>(std::move(predicate)));

                    auto boolean_schema = [] {
                        auto schema = std::make_shared<OpenApiSchema>();
                        schema->types = {OpenApiType::boolean};
                        return schema;
                    };
                    relation.properties.emplace("isEmpty", boolean_schema());
                    relation.properties.emplace("isNotEmpty", boolean_schema());

                    root.properties.emplace(
                        name,
                        std::make_shared<OpenApiSchema>(std::move(relation)));
                }
            }
        }
    }
    return root;
}

template <reflect::Entity T, std::size_t Depth = 3>
OpenApiSchema where_input_with_relations_to_openapi_schema(
    OpenApiDialect dialect = OpenApiDialect::v3_1) {
    return where_input_with_relations_to_openapi_schema<T, Depth>(
        dialect,
        DtoMemberPolicy<>{},
        DtoRelationPolicy<>{});
}

template <reflect::Entity T, std::size_t Depth = 2>
OpenApiSchema update_dto_with_relations_to_openapi_schema(
    OpenApiDialect dialect = OpenApiDialect::v3_1) {
    return detail::update_with_relations_impl<Depth, T>(dialect);
}

template <reflect::Entity T, std::size_t Depth = 2>
OpenApiSchemaMap generate_relation_components(
    std::string base_name,
    OpenApiDialect dialect = OpenApiDialect::v3_1) {
    OpenApiSchemaMap out;
    out.emplace(base_name, dto_to_openapi_schema<T>(dialect));
    out.emplace(
        base_name + "Create",
        create_dto_to_openapi_schema<T>(dialect));
    out.emplace(
        base_name + "Update",
        update_dto_with_relations_to_openapi_schema<T, Depth>(dialect));
    out.emplace(
        base_name + "Filter",
        where_input_with_relations_to_openapi_schema<T, Depth>(dialect));
    out.emplace(
        base_name + "Nested",
        nested_dto_to_openapi_schema<T, Depth>(dialect));
    return out;
}

} // namespace metal
