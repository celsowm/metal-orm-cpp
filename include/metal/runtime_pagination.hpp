#pragma once

#include "metal/orm.hpp"
#include "metal/query/pagination.hpp"

#include <memory>
#include <stdexcept>
#include <type_traits>
#include <typeindex>
#include <unordered_set>
#include <utility>
#include <vector>

namespace metal {

namespace detail {

template <typename Query>
struct query_root;

template <reflect::Entity Root, typename... Scope>
struct query_root<BasicSelectQuery<Root, Scope...>> {
    using type = Root;
};

template <reflect::Entity Root, typename... Scope>
struct query_root<RelationFilteredQuery<Root, Scope...>> {
    using type = Root;
};

template <typename Query>
using query_root_t = typename query_root<std::remove_cvref_t<Query>>::type;

template <reflect::Entity T>
std::shared_ptr<T> hydrate_complete_entity_row(Session& session, const Row& row) {
    const auto pk_name = reflect::primary_key_name<T>();
    const auto pk = row.find(pk_name);
    if (pk == row.end()) {
        throw std::logic_error(
            "MetalORM: tracked pagination requires the root primary key in the projection");
    }
    if (auto existing = session.identity_map().get<T>(pk->second)) return existing;

    bool complete = true;
    reflect::for_each_column<T>([&]<std::meta::info Member>() {
        if (!row.contains(reflect::column_name<Member>())) complete = false;
    });
    if (!complete) {
        throw std::logic_error(
            "MetalORM: tracked pagination requires a complete root-entity projection; use the row pagination overload for DTO projections");
    }

    auto entity = std::make_shared<T>();
    reflect::for_each_column<T>([&]<std::meta::info Member>() {
        using M = reflect::member_type_t<Member>;
        entity->[:Member:] = from_value<M>(row.at(reflect::column_name<Member>()));
    });
    session.persist(entity);
    return entity;
}

template <reflect::Entity Root>
void validate_cursor_root(const std::vector<CursorOrderTerm>& order) {
    for (const auto& term : order) {
        if (term.owner != std::type_index(typeid(Root))) {
            throw std::logic_error(
                "MetalORM: cursor ORDER BY fields must belong to the query root entity");
        }
    }
}

inline std::vector<Row> distinct_root_rows(
    std::vector<Row> rows,
    std::string_view primary_key) {
    std::unordered_set<std::string> seen;
    std::vector<Row> unique;
    unique.reserve(rows.size());
    for (auto& row : rows) {
        auto found = row.find(std::string(primary_key));
        if (found == row.end()) {
            throw std::logic_error(
                "MetalORM: root-aware pagination requires the root primary key in the projection");
        }
        if (seen.insert(value_key(found->second)).second) {
            unique.push_back(std::move(row));
        }
    }
    return unique;
}

} // namespace detail

template <reflect::Entity T>
struct EntityPageResult {
    std::vector<std::shared_ptr<T>> items;
    std::size_t total_items{};
    std::size_t page{};
    std::size_t page_size{};
};

template <reflect::Entity T>
struct EntityCursorPageResult {
    std::vector<std::shared_ptr<T>> items;
    CursorPageInfo page_info;
};

template <typename Query>
requires detail::PageableSelectQuery<Query>
auto execute_paged(
    const Query& query,
    Session& session,
    PageOptions options) -> EntityPageResult<detail::query_root_t<Query>> {
    using Root = detail::query_root_t<Query>;
    if (options.page < 1) throw std::invalid_argument("MetalORM: page must be >= 1");
    if (options.page_size < 1) throw std::invalid_argument("MetalORM: page_size must be >= 1");

    const auto unpaged = query.without_pagination();
    const auto base = unpaged.compile_subquery(session.dialect());
    auto raw = session.executor().execute(base.sql, base.params);
    auto unique_rows = detail::distinct_root_rows(
        std::move(raw.rows), reflect::primary_key_name<Root>());

    EntityPageResult<Root> out;
    out.total_items = unique_rows.size();
    out.page = options.page;
    out.page_size = options.page_size;

    const auto offset = (options.page - 1) * options.page_size;
    if (offset >= unique_rows.size()) return out;
    const auto end = std::min(unique_rows.size(), offset + options.page_size);
    out.items.reserve(end - offset);
    for (std::size_t i = offset; i < end; ++i) {
        out.items.push_back(
            detail::hydrate_complete_entity_row<Root>(session, unique_rows[i]));
    }
    return out;
}

template <typename Query>
requires detail::PageableSelectQuery<Query>
auto execute_cursor(
    const Query& query,
    Session& session,
    std::vector<CursorOrderTerm> order,
    CursorPageOptions options) -> EntityCursorPageResult<detail::query_root_t<Query>> {
    using Root = detail::query_root_t<Query>;
    detail::validate_cursor_root<Root>(order);
    auto rows = detail::execute_cursor_impl(
        query,
        session.executor(),
        session.dialect(),
        std::move(order),
        std::move(options),
        reflect::primary_key_name<Root>());

    EntityCursorPageResult<Root> out;
    out.page_info = std::move(rows.page_info);
    out.items.reserve(rows.items.size());
    for (const auto& row : rows.items) {
        out.items.push_back(detail::hydrate_complete_entity_row<Root>(session, row));
    }
    return out;
}

} // namespace metal