#pragma once

#include "metal/query/relation_queries.hpp"

namespace metal {

template <std::meta::info Relation, reflect::Entity Root, typename... Scope>
auto match_relation(BasicSelectQuery<Root, Scope...> base) {
    return where_has<Relation>(std::move(base));
}

template <std::meta::info Relation, reflect::Entity Root, typename... Scope, typename... Owners>
auto match_relation(BasicSelectQuery<Root, Scope...> base, Expression<Owners...> predicate) {
    return where_relation<Relation>(std::move(base), std::move(predicate));
}

template <std::meta::info Relation, reflect::Entity Root, typename... Scope>
auto match_relation(RelationFilteredQuery<Root, Scope...> base) {
    return where_has<Relation>(std::move(base));
}

template <std::meta::info Relation, reflect::Entity Root, typename... Scope, typename... Owners>
auto match_relation(RelationFilteredQuery<Root, Scope...> base, Expression<Owners...> predicate) {
    return where_relation<Relation>(std::move(base), std::move(predicate));
}

} // namespace metal
