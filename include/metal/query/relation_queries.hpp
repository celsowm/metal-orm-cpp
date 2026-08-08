#pragma once

#include "metal/query/select.hpp"

#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace metal {

namespace detail {

template <std::meta::info Relation, mapping::relation_kind Kind>
struct relation_filter_target_selector;

template <std::meta::info Relation>
struct relation_filter_target_selector<Relation, mapping::relation_kind::belongs_to> {
    using type = reflect::single_target_t<reflect::member_type_t<Relation>>;
};

template <std::meta::info Relation>
struct relation_filter_target_selector<Relation, mapping::relation_kind::has_one> {
    using type = reflect::single_target_t<reflect::member_type_t<Relation>>;
};

template <std::meta::info Relation>
struct relation_filter_target_selector<Relation, mapping::relation_kind::has_many> {
    using type = reflect::has_many_target_t<reflect::member_type_t<Relation>>;
};

template <std::meta::info Relation>
struct relation_filter_target_selector<Relation, mapping::relation_kind::many_to_many> {
    using type = reflect::many_to_many_target_t<reflect::member_type_t<Relation>>;
};

template <std::meta::info Relation>
struct relation_filter_target_selector<Relation, mapping::relation_kind::morph_one> {
    using type = reflect::morph_one_target_t<reflect::member_type_t<Relation>>;
};

template <std::meta::info Relation>
struct relation_filter_target_selector<Relation, mapping::relation_kind::morph_many> {
    using type = reflect::morph_many_target_t<reflect::member_type_t<Relation>>;
};

template <std::meta::info Relation>
using relation_filter_target_t = typename relation_filter_target_selector<
    Relation,
    mapping::relation_annotation_traits<reflect::relation_annotation_t<Relation>>::kind>::type;

struct RelationFilterSpec {
    bool negated{false};
    std::function<CompiledQuery(const Dialect&, std::string_view)> compile_exists;
};

inline std::string qcol(const Dialect& dialect, std::string_view alias, std::string_view column) {
    return dialect.quote_identifier(alias) + "." + dialect.quote_identifier(column);
}

template <std::meta::info Relation, reflect::Entity Target>
RelationFilterSpec make_relation_filter(SelectQuery<Target> child, bool negated) {
    using Owner = reflect::owner_type_t<Relation>;
    using A = reflect::relation_annotation_t<Relation>;
    using Traits = mapping::relation_annotation_traits<A>;
    static_assert(Traits::kind != mapping::relation_kind::morph_to,
                  "MetalORM: morph_to does not support where_has/where_has_not because its target table is discriminator-dependent");

    return RelationFilterSpec{
        negated,
        [child = std::move(child)](const Dialect& dialect, std::string_view root_alias) mutable {
            auto inner = child.compile_subquery(dialect);
            const std::string rel_alias = "__metal_rel";
            const std::string pivot_alias = "__metal_pivot";
            std::string sql = "SELECT 1 FROM (" + inner.sql + ") AS " + dialect.quote_identifier(rel_alias);
            std::vector<Value> params = std::move(inner.params);
            std::string correlation;

            if constexpr (Traits::kind == mapping::relation_kind::belongs_to) {
                constexpr auto foreign_key = Traits::foreign_key();
                constexpr auto target_key = reflect::key_or_primary<Target>(Traits::target_key());
                correlation = qcol(dialect, rel_alias, reflect::column_name<target_key>()) + " = " +
                              qcol(dialect, root_alias, reflect::column_name<foreign_key>());
            } else if constexpr (Traits::kind == mapping::relation_kind::has_one ||
                                 Traits::kind == mapping::relation_kind::has_many) {
                constexpr auto target_fk = Traits::target_foreign_key();
                constexpr auto local_key = reflect::key_or_primary<Owner>(Traits::local_key());
                correlation = qcol(dialect, rel_alias, reflect::column_name<target_fk>()) + " = " +
                              qcol(dialect, root_alias, reflect::column_name<local_key>());
            } else if constexpr (Traits::kind == mapping::relation_kind::many_to_many) {
                constexpr auto pivot_reflection = Traits::pivot();
                using Pivot = [: pivot_reflection :];
                constexpr auto pivot_root_fk = Traits::pivot_root_foreign_key();
                constexpr auto pivot_target_fk = Traits::pivot_target_foreign_key();
                constexpr auto local_key = reflect::key_or_primary<Owner>(Traits::local_key());
                constexpr auto target_key = reflect::key_or_primary<Target>(Traits::target_key());
                sql += " INNER JOIN " + dialect.quote_identifier(reflect::table_name<Pivot>()) + " AS " +
                       dialect.quote_identifier(pivot_alias) + " ON " +
                       qcol(dialect, pivot_alias, reflect::column_name<pivot_target_fk>()) + " = " +
                       qcol(dialect, rel_alias, reflect::column_name<target_key>());
                correlation = qcol(dialect, pivot_alias, reflect::column_name<pivot_root_fk>()) + " = " +
                              qcol(dialect, root_alias, reflect::column_name<local_key>());
            } else if constexpr (Traits::kind == mapping::relation_kind::morph_one ||
                                 Traits::kind == mapping::relation_kind::morph_many) {
                constexpr auto type_field = Traits::type_field();
                constexpr auto id_field = Traits::id_field();
                constexpr auto local_key = reflect::key_or_primary<Owner>(Traits::local_key());
                correlation = qcol(dialect, rel_alias, reflect::column_name<id_field>()) + " = " +
                              qcol(dialect, root_alias, reflect::column_name<local_key>()) + " AND " +
                              qcol(dialect, rel_alias, reflect::column_name<type_field>()) + " = " +
                              dialect.placeholder(params.size() + 1);
                params.push_back(Value{std::string(Traits::type_value.view())});
            }

            sql += " WHERE " + correlation;
            return CompiledQuery{std::move(sql), std::move(params)};
        }};
}

} // namespace detail

template <reflect::Entity Root, typename... Scope>
class RelationFilteredQuery {
public:
    using root_type = Root;

    explicit RelationFilteredQuery(BasicSelectQuery<Root, Scope...> base)
        : base_(std::move(base)) {}

    RelationFilteredQuery& add_filter(detail::RelationFilterSpec filter) {
        filters_.push_back(std::move(filter));
        return *this;
    }

    [[nodiscard]] CompiledQuery compile_subquery(const Dialect& dialect) const {
        auto base = base_.compile_subquery(dialect);
        const std::string root_alias = "__metal_root";
        CompiledQuery out;
        out.params = std::move(base.params);
        out.sql = "SELECT * FROM (" + base.sql + ") AS " + dialect.quote_identifier(root_alias);
        if (!filters_.empty()) {
            out.sql += " WHERE ";
            for (std::size_t i = 0; i < filters_.size(); ++i) {
                if (i) out.sql += " AND ";
                auto exists_query = filters_[i].compile_exists(dialect, root_alias);
                out.params.insert(out.params.end(), exists_query.params.begin(), exists_query.params.end());
                out.sql += filters_[i].negated ? "NOT EXISTS (" : "EXISTS (";
                out.sql += exists_query.sql + ")";
            }
        }
        return out;
    }

    [[nodiscard]] CompiledQuery compile(const Dialect& dialect) const {
        auto out = compile_subquery(dialect);
        out.sql += ";";
        return out;
    }

private:
    BasicSelectQuery<Root, Scope...> base_;
    std::vector<detail::RelationFilterSpec> filters_;
};

template <std::meta::info Relation, reflect::Entity Root, typename... Scope, typename Callback>
auto where_has(BasicSelectQuery<Root, Scope...> base, Callback&& callback) {
    static_assert(reflect::has_relation_annotation<Relation>(),
                  "MetalORM: where_has<> requires a reflected relationship member");
    static_assert(std::same_as<reflect::owner_type_t<Relation>, Root>,
                  "MetalORM: where_has<> relation must belong to the query root entity");
    using A = reflect::relation_annotation_t<Relation>;
    using Traits = mapping::relation_annotation_traits<A>;
    static_assert(Traits::kind != mapping::relation_kind::morph_to,
                  "MetalORM: morph_to does not support where_has/where_has_not");
    using Target = detail::relation_filter_target_t<Relation>;
    static_assert(reflect::Entity<Target>);

    auto child = select<Target>();
    if constexpr (std::is_void_v<std::invoke_result_t<Callback, SelectQuery<Target>&>>) {
        std::invoke(std::forward<Callback>(callback), child);
    } else {
        child = std::invoke(std::forward<Callback>(callback), child);
    }

    RelationFilteredQuery<Root, Scope...> out{std::move(base)};
    out.add_filter(detail::make_relation_filter<Relation>(std::move(child), false));
    return out;
}

template <std::meta::info Relation, reflect::Entity Root, typename... Scope>
auto where_has(BasicSelectQuery<Root, Scope...> base) {
    return where_has<Relation>(std::move(base), [](auto&) {});
}

template <std::meta::info Relation, reflect::Entity Root, typename... Scope, typename Callback>
auto where_has_not(BasicSelectQuery<Root, Scope...> base, Callback&& callback) {
    static_assert(reflect::has_relation_annotation<Relation>(),
                  "MetalORM: where_has_not<> requires a reflected relationship member");
    static_assert(std::same_as<reflect::owner_type_t<Relation>, Root>,
                  "MetalORM: where_has_not<> relation must belong to the query root entity");
    using A = reflect::relation_annotation_t<Relation>;
    using Traits = mapping::relation_annotation_traits<A>;
    static_assert(Traits::kind != mapping::relation_kind::morph_to,
                  "MetalORM: morph_to does not support where_has/where_has_not");
    using Target = detail::relation_filter_target_t<Relation>;
    static_assert(reflect::Entity<Target>);

    auto child = select<Target>();
    if constexpr (std::is_void_v<std::invoke_result_t<Callback, SelectQuery<Target>&>>) {
        std::invoke(std::forward<Callback>(callback), child);
    } else {
        child = std::invoke(std::forward<Callback>(callback), child);
    }

    RelationFilteredQuery<Root, Scope...> out{std::move(base)};
    out.add_filter(detail::make_relation_filter<Relation>(std::move(child), true));
    return out;
}

template <std::meta::info Relation, reflect::Entity Root, typename... Scope>
auto where_has_not(BasicSelectQuery<Root, Scope...> base) {
    return where_has_not<Relation>(std::move(base), [](auto&) {});
}

template <std::meta::info Relation, reflect::Entity Root, typename... Scope, typename... Owners>
auto where_relation(BasicSelectQuery<Root, Scope...> base, Expression<Owners...> predicate) {
    using Target = detail::relation_filter_target_t<Relation>;
    static_assert((std::same_as<Owners, Target> && ...),
                  "MetalORM: where_relation predicate must reference only the relation target type");
    return where_has<Relation>(std::move(base), [predicate = std::move(predicate)](auto& child) mutable {
        child.where(std::move(predicate));
    });
}

} // namespace metal
