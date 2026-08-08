#pragma once

#include "metal/orm.hpp"
#include "metal/query/pagination.hpp"

#include <memory>
#include <stdexcept>
#include <type_traits>
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
requires detail::CompilableSelectQuery<Query>
auto execute_paged(
    const Query& query,
    Session& session,
    PageOptions options) -> EntityPageResult<detail::query_root_t<Query>> {
    using Root = detail::query_root_t<Query>;
    if (options.page < 1) throw std::invalid_argument("MetalORM: page must be >= 1");
    if (options.page_size < 1) throw std::invalid_argument("MetalORM: page_size must be >= 1");

    const auto base = query.compile_subquery(session.dialect());
    const std::string count_alias = "__metal_count";
    const auto pk = reflect::primary_key_name<Root>();
    const std::string count_sql =
        "SELECT COUNT(DISTINCT " + session.dialect().quote_identifier(count_alias) + "." +
        session.dialect().quote_identifier(pk) + ") AS \"total\" FROM (" + base.sql + ") AS " +
        session.dialect().quote_identifier(count_alias) + ";";
    const auto count_result = session.executor().execute(count_sql, base.params);
    std::size_t total = 0;
    if (!count_result.rows.empty()) {
        auto found = count_result.rows.front().find("total");
        if (found != count_result.rows.front().end()) {
            total = static_cast<std::size_t>(from_value<std::int64_t>(found->second));
        }
    }

    const auto offset = (options.page - 1) * options.page_size;
    const std::string page_sql =
        "SELECT * FROM (" + base.sql + ") AS \"__metal_page\" LIMIT " +
        std::to_string(options.page_size) + " OFFSET " + std::to_string(offset) + ";";
    auto result = session.executor().execute(page_sql, base.params);

    EntityPageResult<Root> out;
    out.total_items = total;
    out.page = options.page;
    out.page_size = options.page_size;
    out.items.reserve(result.rows.size());
    for (const auto& row : result.rows) {
        out.items.push_back(detail::hydrate_complete_entity_row<Root>(session, row));
    }
    return out;
}

template <typename Query>
requires detail::CompilableSelectQuery<Query>
auto execute_cursor(
    const Query& query,
    Session& session,
    std::vector<CursorOrderTerm> order,
    CursorPageOptions options) -> EntityCursorPageResult<detail::query_root_t<Query>> {
    using Root = detail::query_root_t<Query>;
    detail::validate_cursor_root<Root>(order);
    auto rows = execute_cursor(
        query,
        session.executor(),
        session.dialect(),
        std::move(order),
        std::move(options));

    EntityCursorPageResult<Root> out;
    out.page_info = std::move(rows.page_info);
    out.items.reserve(rows.items.size());
    for (const auto& row : rows.items) {
        out.items.push_back(detail::hydrate_complete_entity_row<Root>(session, row));
    }
    return out;
}

} // namespace metal
