#pragma once

#include "metal/dto_execute.hpp"
#include "metal/dto_relation_filter.hpp"

#include <utility>

namespace metal {

template <
    reflect::Entity T,
    std::meta::info... FilterMembers,
    std::meta::info... SortMembers,
    std::meta::info... RelationMembers>
auto execute_filtered_paged(
    SelectQuery<T> query,
    Session& session,
    const WhereInput& filters,
    const SortInput& sort,
    PageOptions page,
    DtoMemberPolicy<FilterMembers...> filter_policy,
    DtoMemberPolicy<SortMembers...>,
    DtoRelationPolicy<RelationMembers...> relation_policy) {
    query = apply_sort<SortMembers...>(std::move(query), sort);
    auto filtered = apply_where(
        std::move(query),
        filters,
        filter_policy,
        relation_policy);
    return to_paged_response(execute_paged(filtered, session, page));
}

template <reflect::Entity T>
auto execute_filtered_paged(
    SelectQuery<T> query,
    Session& session,
    const WhereInput& filters,
    const SortInput& sort,
    PageOptions page) {
    return execute_filtered_paged(
        std::move(query),
        session,
        filters,
        sort,
        page,
        DtoMemberPolicy<>{},
        DtoMemberPolicy<>{},
        DtoRelationPolicy<>{});
}

} // namespace metal
