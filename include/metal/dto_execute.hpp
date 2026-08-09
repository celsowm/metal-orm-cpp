#pragma once

#include "metal/dto_filter.hpp"
#include "metal/dto_sort.hpp"
#include "metal/runtime_pagination.hpp"

#include <utility>

namespace metal {

template <
    reflect::Entity T,
    std::meta::info... FilterMembers,
    std::meta::info... SortMembers>
auto execute_filtered_paged(
    SelectQuery<T> query,
    Session& session,
    const FilterInput& filters,
    const SortInput& sort,
    PageOptions page,
    DtoMemberPolicy<FilterMembers...>,
    DtoMemberPolicy<SortMembers...>) {
    query = apply_filter<FilterMembers...>(std::move(query), filters);
    query = apply_sort<SortMembers...>(std::move(query), sort);
    return to_paged_response(execute_paged(query, session, page));
}

template <reflect::Entity T>
auto execute_filtered_paged(
    SelectQuery<T> query,
    Session& session,
    const FilterInput& filters,
    const SortInput& sort,
    PageOptions page) {
    return execute_filtered_paged(
        std::move(query),
        session,
        filters,
        sort,
        page,
        DtoMemberPolicy<>{},
        DtoMemberPolicy<>{});
}

} // namespace metal
