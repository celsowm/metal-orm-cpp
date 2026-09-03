#pragma once

#include "metal/query/select.hpp"

#include <concepts>
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

template <std::meta::info Relation>
CompiledQuery compile_relation_correlation(
    const Dialect& dialect,
    std::string_view child_alias,
    std::string_view outer_alias) {
    using Owner = reflect::owner_type_t<Relation>;
    using A = reflect::relation_annotation_t<Relation>;
    using Traits = mapping::relation_annotation_traits<A>;
    using Target = relation_filter_target_t<Relation>;
    static_assert(Traits::kind != mapping::relation_kind::morph_to,
                  "MetalORM: morph_to does not support where_has/where_has_not because its target table is discriminator-dependent");

    CompiledQuery out;
    if constexpr (Traits::kind == mapping::relation_kind::belongs_to) {
        constexpr auto foreign_key = Traits::foreign_key();
        constexpr auto target_key = reflect::key_or_primary<Target>(Traits::target_key());
        out.sql = qcol(dialect, child_alias, reflect::column_name<target_key>()) + " = " +
                  qcol(dialect, outer_alias, reflect::column_name<foreign_key>());
    } else if constexpr (Traits::kind == mapping::relation_kind::has_one ||
                         Traits::kind == mapping::relation_kind::has_many) {
        constexpr auto target_fk = Traits::target_foreign_key();
        constexpr auto local_key = reflect::key_or_primary<Owner>(Traits::local_key());
        out.sql = qcol(dialect, child_alias, reflect::column_name<target_fk>()) + " = " +
                  qcol(dialect, outer_alias, reflect::column_name<local_key>());
    } else if constexpr (Traits::kind == mapping::relation_kind::many_to_many) {
        constexpr auto pivot_reflection = Traits::pivot();
        using Pivot = [: pivot_reflection :];
        constexpr auto pivot_root_fk = Traits::pivot_root_foreign_key();
        constexpr auto pivot_target_fk = Traits::pivot_target_foreign_key();
        constexpr auto local_key = reflect::key_or_primary<Owner>(Traits::local_key());
        constexpr auto target_key = reflect::key_or_primary<Target>(Traits::target_key());
        const std::string pivot_alias = std::string(child_alias) + "_pivot";
        out.sql =
            "EXISTS (SELECT 1 FROM " + dialect.quote_identifier(reflect::table_name<Pivot>()) + " AS " +
            dialect.quote_identifier(pivot_alias) + " WHERE " +
            qcol(dialect, pivot_alias, reflect::column_name<pivot_target_fk>()) + " = " +
            qcol(dialect, child_alias, reflect::column_name<target_key>()) + " AND " +
            qcol(dialect, pivot_alias, reflect::column_name<pivot_root_fk>()) + " = " +
            qcol(dialect, outer_alias, reflect::column_name<local_key>()) + ")";
    } else if constexpr (Traits::kind == mapping::relation_kind::morph_one ||
                         Traits::kind == mapping::relation_kind::morph_many) {
        constexpr auto type_field = Traits::type_field();
        constexpr auto id_field = Traits::id_field();
        constexpr auto local_key = reflect::key_or_primary<Owner>(Traits::local_key());
        out.sql = qcol(dialect, child_alias, reflect::column_name<id_field>()) + " = " +
                  qcol(dialect, outer_alias, reflect::column_name<local_key>()) + " AND " +
                  qcol(dialect, child_alias, reflect::column_name<type_field>()) + " = " +
                  dialect.placeholder(1);
        out.params.push_back(Value{std::string(Traits::type_value.view())});
    }
    return out;
}

template <typename Query>
concept CorrelatableSelectQuery = requires(
    const Query& query,
    const Dialect& dialect,
    ExtraWhereCompiler extra_where,
    std::string alias) {
    { query.compile_subquery_with_extra_where(
          dialect, std::move(extra_where), std::move(alias)) } ->
        std::same_as<CompiledQuery>;
};

template <std::meta::info Relation, CorrelatableSelectQuery ChildQuery>
RelationFilterSpec make_relation_filter(ChildQuery child, bool negated) {
    return RelationFilterSpec{
        negated,
        [child = std::move(child)](const Dialect& dialect, std::string_view outer_alias) mutable {
            const std::string child_alias = std::string(outer_alias) + "_rel";
            return child.compile_subquery_with_extra_where(
                dialect,
                [outer = std::string(outer_alias)](
                    const Dialect& nested_dialect,
                    std::string_view nested_child_alias) {
                    return compile_relation_correlation<Relation>(
                        nested_dialect, nested_child_alias, outer);
                },
                child_alias);
        }};
}

template <std::meta::info Relation, typename Callback>
RelationFilterSpec configured_relation_filter(Callback&& callback, bool negated) {
    static_assert(reflect::has_relation_annotation<Relation>(),
                  "MetalORM: relation filter requires a reflected relationship member");
    using A = reflect::relation_annotation_t<Relation>;
    using Traits = mapping::relation_annotation_traits<A>;
    static_assert(Traits::kind != mapping::relation_kind::morph_to,
                  "MetalORM: morph_to does not support where_has/where_has_not");
    using Target = relation_filter_target_t<Relation>;
    static_assert(reflect::Entity<Target>);

    auto child = select<Target>();
    using Result = std::invoke_result_t<Callback, SelectQuery<Target>&>;
    if constexpr (std::is_void_v<Result>) {
        std::invoke(std::forward<Callback>(callback), child);
        return make_relation_filter<Relation>(std::move(child), negated);
    } else {
        auto configured = std::invoke(std::forward<Callback>(callback), child);
        static_assert(CorrelatableSelectQuery<decltype(configured)>,
                      "MetalORM: where_has callback must return a correlatable select query");
        return make_relation_filter<Relation>(std::move(configured), negated);
    }
}

inline CompiledQuery compile_relation_filter_list(
    const std::vector<RelationFilterSpec>& filters,
    const Dialect& dialect,
    std::string_view root_alias,
    const ExtraWhereCompiler& outer_extra = {}) {
    CompiledQuery out;
    auto append_predicate = [&](std::string sql, std::vector<Value> params) {
        if (sql.empty()) return;
        if (!out.sql.empty()) out.sql += " AND ";
        out.sql += "(" + std::move(sql) + ")";
        out.params.insert(out.params.end(), params.begin(), params.end());
    };

    for (const auto& filter : filters) {
        const auto nested_dialect = offset_placeholders(dialect, out.params.size());
        auto child = filter.compile_exists(nested_dialect, root_alias);
        append_predicate(
            std::string(filter.negated ? "NOT EXISTS (" : "EXISTS (") + child.sql + ")",
            std::move(child.params));
    }

    if (outer_extra) {
        const auto nested_dialect = offset_placeholders(dialect, out.params.size());
        auto extra = outer_extra(nested_dialect, root_alias);
        append_predicate(std::move(extra.sql), std::move(extra.params));
    }
    return out;
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

    [[nodiscard]] RelationFilteredQuery without_pagination() const {
        auto copy = *this;
        copy.base_ = base_.without_pagination();
        return copy;
    }

    [[nodiscard]] CompiledQuery compile_subquery_with_extra_where(
        const Dialect& dialect,
        ExtraWhereCompiler extra_where,
        std::string root_alias_override = {}) const {
        return base_.compile_subquery_with_extra_where(
            dialect,
            [this, extra_where = std::move(extra_where)](
                const Dialect& nested_dialect,
                std::string_view root_alias) {
                return detail::compile_relation_filter_list(
                    filters_, nested_dialect, root_alias, extra_where);
            },
            std::move(root_alias_override));
    }

    [[nodiscard]] CompiledQuery compile_subquery(const Dialect& dialect) const {
        return compile_subquery_with_extra_where(dialect, {}, {});
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
    static_assert(std::same_as<reflect::owner_type_t<Relation>, Root>,
                  "MetalORM: where_has<> relation must belong to the query root entity");
    RelationFilteredQuery<Root, Scope...> out{std::move(base)};
    out.add_filter(detail::configured_relation_filter<Relation>(std::forward<Callback>(callback), false));
    return out;
}

template <std::meta::info Relation, reflect::Entity Root, typename... Scope>
auto where_has(BasicSelectQuery<Root, Scope...> base) {
    return where_has<Relation>(std::move(base), [](auto&) {});
}

template <std::meta::info Relation, reflect::Entity Root, typename... Scope, typename Callback>
auto where_has(RelationFilteredQuery<Root, Scope...> base, Callback&& callback) {
    static_assert(std::same_as<reflect::owner_type_t<Relation>, Root>,
                  "MetalORM: where_has<> relation must belong to the query root entity");
    base.add_filter(detail::configured_relation_filter<Relation>(std::forward<Callback>(callback), false));
    return base;
}

template <std::meta::info Relation, reflect::Entity Root, typename... Scope>
auto where_has(RelationFilteredQuery<Root, Scope...> base) {
    return where_has<Relation>(std::move(base), [](auto&) {});
}

template <std::meta::info Relation, reflect::Entity Root, typename... Scope, typename Callback>
auto where_has_not(BasicSelectQuery<Root, Scope...> base, Callback&& callback) {
    static_assert(std::same_as<reflect::owner_type_t<Relation>, Root>,
                  "MetalORM: where_has_not<> relation must belong to the query root entity");
    RelationFilteredQuery<Root, Scope...> out{std::move(base)};
    out.add_filter(detail::configured_relation_filter<Relation>(std::forward<Callback>(callback), true));
    return out;
}

template <std::meta::info Relation, reflect::Entity Root, typename... Scope>
auto where_has_not(BasicSelectQuery<Root, Scope...> base) {
    return where_has_not<Relation>(std::move(base), [](auto&) {});
}

template <std::meta::info Relation, reflect::Entity Root, typename... Scope, typename Callback>
auto where_has_not(RelationFilteredQuery<Root, Scope...> base, Callback&& callback) {
    static_assert(std::same_as<reflect::owner_type_t<Relation>, Root>,
                  "MetalORM: where_has_not<> relation must belong to the query root entity");
    base.add_filter(detail::configured_relation_filter<Relation>(std::forward<Callback>(callback), true));
    return base;
}

template <std::meta::info Relation, reflect::Entity Root, typename... Scope>
auto where_has_not(RelationFilteredQuery<Root, Scope...> base) {
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

template <std::meta::info Relation, reflect::Entity Root, typename... Scope, typename... Owners>
auto where_relation(RelationFilteredQuery<Root, Scope...> base, Expression<Owners...> predicate) {
    using Target = detail::relation_filter_target_t<Relation>;
    static_assert((std::same_as<Owners, Target> && ...),
                  "MetalORM: where_relation predicate must reference only the relation target type");
    return where_has<Relation>(std::move(base), [predicate = std::move(predicate)](auto& child) mutable {
        child.where(std::move(predicate));
    });
}

} // namespace metal