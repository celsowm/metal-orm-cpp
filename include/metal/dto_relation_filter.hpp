#pragma once

#include "metal/dto_filter.hpp"
#include "metal/dto_sort.hpp"
#include "metal/query.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace metal {

struct WhereInput;

struct RelationFilterClause {
    std::string relation;
    std::shared_ptr<WhereInput> some_filter;
    std::shared_ptr<WhereInput> every_filter;
    std::shared_ptr<WhereInput> none_filter;
    std::optional<bool> is_empty;
    std::optional<bool> is_not_empty;

    RelationFilterClause& some(WhereInput input);
    RelationFilterClause& every(WhereInput input);
    RelationFilterClause& none(WhereInput input);
    RelationFilterClause& empty(bool value = true) noexcept;
    RelationFilterClause& not_empty(bool value = true) noexcept;
};

struct WhereInput {
    std::vector<FilterClause> clauses;
    std::vector<RelationFilterClause> relations;

    WhereInput() = default;
    explicit WhereInput(FilterInput scalar) : clauses(std::move(scalar.clauses)) {}
};

inline RelationFilterClause& RelationFilterClause::some(WhereInput input) {
    some_filter = std::make_shared<WhereInput>(std::move(input));
    return *this;
}

inline RelationFilterClause& RelationFilterClause::every(WhereInput input) {
    every_filter = std::make_shared<WhereInput>(std::move(input));
    return *this;
}

inline RelationFilterClause& RelationFilterClause::none(WhereInput input) {
    none_filter = std::make_shared<WhereInput>(std::move(input));
    return *this;
}

inline RelationFilterClause& RelationFilterClause::empty(bool value) noexcept {
    is_empty = value;
    return *this;
}

inline RelationFilterClause& RelationFilterClause::not_empty(bool value) noexcept {
    is_not_empty = value;
    return *this;
}

inline RelationFilterClause relation_filter(std::string relation) {
    if (relation.empty()) {
        throw std::invalid_argument("MetalORM: REST relation filter requires a non-empty relation name");
    }
    RelationFilterClause out;
    out.relation = std::move(relation);
    return out;
}

inline WhereInput where_input(FilterInput scalar = {}) {
    return WhereInput{std::move(scalar)};
}

template <std::meta::info... Relations>
struct DtoRelationPolicy {};

namespace detail {

template <reflect::Entity T, std::meta::info... Relations>
consteval bool validate_dto_relations() {
    if constexpr (sizeof...(Relations) > 0) {
        static_assert((std::meta::is_nonstatic_data_member(Relations) && ...),
                      "MetalORM: DTO relation policy requires reflected data members");
        static_assert((std::same_as<reflect::owner_type_t<Relations>, T> && ...),
                      "MetalORM: DTO relation policy members must belong to the declared entity");
        static_assert((reflect::has_relation_annotation<Relations>() && ...),
                      "MetalORM: DTO relation policy members must be reflected relationships");
    }
    return true;
}

template <std::meta::info Relation, std::meta::info... Allowed>
consteval bool dto_relation_allowed() {
    if constexpr (sizeof...(Allowed) == 0) return true;
    return ((Relation == Allowed) || ...);
}

template <std::meta::info Relation>
std::string dto_relation_name() {
    return std::string(std::meta::identifier_of(Relation));
}

inline bool where_has_conditions(const WhereInput& input) noexcept {
    return !input.clauses.empty() || !input.relations.empty();
}

inline bool relation_clause_has_operator(const RelationFilterClause& clause) noexcept {
    return static_cast<bool>(clause.some_filter) ||
           static_cast<bool>(clause.every_filter) ||
           static_cast<bool>(clause.none_filter) ||
           clause.is_empty.has_value() ||
           clause.is_not_empty.has_value();
}

template <reflect::Entity T>
RelationFilteredQuery<T, T> build_nested_where_query(const WhereInput& input);

template <std::meta::info Relation, CorrelatableSelectQuery PredicateQuery>
RelationFilterSpec make_every_relation_filter(PredicateQuery predicate) {
    using A = reflect::relation_annotation_t<Relation>;
    using Traits = mapping::relation_annotation_traits<A>;
    static_assert(Traits::kind != mapping::relation_kind::morph_to,
                  "MetalORM: morph_to does not support relation REST filters");
    using Target = relation_filter_target_t<Relation>;
    constexpr auto TargetPk = reflect::primary_key_member<Target>();

    return RelationFilterSpec{
        true,
        [predicate = std::move(predicate)](
            const Dialect& dialect,
            std::string_view outer_alias) mutable {
            const std::string child_alias = std::string(outer_alias) + "_rel";
            auto all_related = select<Target>();
            return all_related.compile_subquery_with_extra_where(
                dialect,
                [outer = std::string(outer_alias), &predicate](
                    const Dialect& nested_dialect,
                    std::string_view nested_child_alias) mutable {
                    auto correlation = compile_relation_correlation<Relation>(
                        nested_dialect, nested_child_alias, outer);

                    const std::string predicate_alias =
                        std::string(nested_child_alias) + "_every";
                    auto matching = predicate.compile_subquery_with_extra_where(
                        nested_dialect,
                        [child = std::string(nested_child_alias)](
                            const Dialect& predicate_dialect,
                            std::string_view predicate_root_alias) {
                            CompiledQuery same_row;
                            same_row.sql =
                                qcol(
                                    predicate_dialect,
                                    predicate_root_alias,
                                    reflect::column_name<TargetPk>()) +
                                " = " +
                                qcol(
                                    predicate_dialect,
                                    child,
                                    reflect::column_name<TargetPk>());
                            return same_row;
                        },
                        predicate_alias);

                    CompiledQuery extra;
                    extra.sql = std::move(correlation.sql) +
                                " AND NOT EXISTS (" + matching.sql + ")";
                    extra.params = std::move(correlation.params);
                    extra.params.insert(
                        extra.params.end(),
                        matching.params.begin(),
                        matching.params.end());
                    return extra;
                },
                child_alias);
        }};
}

template <std::meta::info Relation, reflect::Entity Root>
void append_relation_clause(
    RelationFilteredQuery<Root, Root>& query,
    const RelationFilterClause& clause) {
    using A = reflect::relation_annotation_t<Relation>;
    using Traits = mapping::relation_annotation_traits<A>;

    if constexpr (Traits::kind == mapping::relation_kind::morph_to) {
        throw std::invalid_argument(
            "MetalORM: morph_to REST relation filters are discriminator-dependent and unsupported");
    } else {
        using Target = relation_filter_target_t<Relation>;

        if (clause.some_filter && where_has_conditions(*clause.some_filter)) {
            auto child = build_nested_where_query<Target>(*clause.some_filter);
            query.add_filter(make_relation_filter<Relation>(std::move(child), false));
        }

        if (clause.none_filter && where_has_conditions(*clause.none_filter)) {
            auto child = build_nested_where_query<Target>(*clause.none_filter);
            query.add_filter(make_relation_filter<Relation>(std::move(child), true));
        }

        if (clause.every_filter && where_has_conditions(*clause.every_filter)) {
            auto predicate = build_nested_where_query<Target>(*clause.every_filter);
            query.add_filter(make_every_relation_filter<Relation>(std::move(predicate)));
        }

        if (clause.is_empty.has_value()) {
            auto child = select<Target>();
            query.add_filter(make_relation_filter<Relation>(
                std::move(child), *clause.is_empty));
        }

        if (clause.is_not_empty.has_value()) {
            auto child = select<Target>();
            query.add_filter(make_relation_filter<Relation>(
                std::move(child), !*clause.is_not_empty));
        }
    }
}

template <reflect::Entity T, std::meta::info... AllowedRelations>
RelationFilteredQuery<T, T> apply_relation_filters_impl(
    SelectQuery<T> base,
    const WhereInput& input,
    DtoRelationPolicy<AllowedRelations...>) {
    static_assert(validate_dto_relations<T, AllowedRelations...>());

    RelationFilteredQuery<T, T> out{std::move(base)};
    for (const auto& clause : input.relations) {
        if (!relation_clause_has_operator(clause)) {
            throw std::invalid_argument(
                "MetalORM: relation filter '" + clause.relation +
                "' must include some/every/none/isEmpty/isNotEmpty");
        }

        bool known = false;
        bool allowed = false;

        template for (constexpr auto Relation : reflect::data_members<T>()) {
            if constexpr (reflect::has_relation_annotation<Relation>()) {
                if (!known && dto_relation_name<Relation>() == clause.relation) {
                    known = true;
                    if constexpr (dto_relation_allowed<Relation, AllowedRelations...>()) {
                        allowed = true;
                        append_relation_clause<Relation>(out, clause);
                    }
                }
            }
        }

        if (!known) {
            throw std::invalid_argument(
                "MetalORM: unknown REST relation filter '" + clause.relation + "'");
        }
        if (!allowed) {
            throw std::invalid_argument(
                "MetalORM: REST relation filter '" + clause.relation +
                "' is not allowed by the reflected relation policy");
        }
    }
    return out;
}

template <reflect::Entity T>
RelationFilteredQuery<T, T> build_nested_where_query(const WhereInput& input) {
    FilterInput scalar{input.clauses};
    auto base = apply_filter<>(select<T>(), scalar);
    return apply_relation_filters_impl(
        std::move(base), input, DtoRelationPolicy<>{});
}

} // namespace detail

template <
    reflect::Entity T,
    std::meta::info... AllowedFields,
    std::meta::info... AllowedRelations>
RelationFilteredQuery<T, T> apply_where(
    SelectQuery<T> query,
    const WhereInput& input,
    DtoMemberPolicy<AllowedFields...>,
    DtoRelationPolicy<AllowedRelations...>) {
    FilterInput scalar{input.clauses};
    query = apply_filter<AllowedFields...>(std::move(query), scalar);
    return detail::apply_relation_filters_impl(
        std::move(query), input, DtoRelationPolicy<AllowedRelations...>{});
}

template <reflect::Entity T>
RelationFilteredQuery<T, T> apply_where(
    SelectQuery<T> query,
    const WhereInput& input) {
    return apply_where(
        std::move(query),
        input,
        DtoMemberPolicy<>{},
        DtoRelationPolicy<>{});
}

} // namespace metal
