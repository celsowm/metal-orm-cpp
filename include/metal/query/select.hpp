#pragma once

#include "metal/query/functions.hpp"

namespace metal {

struct ProjectionSpec {
    ScalarPtr expression;
    std::optional<std::string> alias;
};

struct OrderSpec {
    ColumnRef column;
    bool ascending{true};
};

struct JoinSpec {
    JoinKind kind{JoinKind::Inner};
    mapping::relation_kind relation_kind{mapping::relation_kind::belongs_to};
    std::type_index owner_type{typeid(void)};
    std::type_index target_type{typeid(void)};
    std::string target_table;
    std::string owner_column;
    std::string target_column;
    std::optional<std::string> pivot_table;
    std::optional<std::string> pivot_root_column;
    std::optional<std::string> pivot_target_column;
};

struct CteJoinSpec {
    JoinKind kind{JoinKind::Inner};
    std::string cte_name;
    ColumnRef owner_column;
    std::string cte_column;
};

struct CteSpec {
    std::string name;
    std::vector<std::string> columns;
    bool recursive{false};
    std::function<CompiledQuery(const Dialect&)> compile_query;
};

struct SetOperationSpec {
    SetOperationKind kind{SetOperationKind::Union};
    std::size_t projection_arity{};
    std::function<CompiledQuery(const Dialect&)> compile_query;
};

struct DerivedSourceSpec {
    std::string alias;
    std::size_t projection_arity{};
    std::function<CompiledQuery(const Dialect&)> compile_query;
};

using ExtraWhereCompiler =
    std::function<CompiledQuery(const Dialect&, std::string_view root_alias)>;

struct SelectState {
    bool distinct{false};
    std::vector<ProjectionSpec> projections;
    std::vector<JoinSpec> joins;
    std::vector<CteJoinSpec> cte_joins;
    std::optional<ExprPtr> where;
    std::vector<ColumnRef> group_by;
    std::optional<ExprPtr> having;
    std::vector<OrderSpec> order_by;
    std::optional<std::size_t> limit;
    std::optional<std::size_t> offset;
    std::optional<std::string> from_name;
    std::optional<DerivedSourceSpec> derived_source;
    std::vector<CteSpec> ctes;
    std::vector<SetOperationSpec> set_operations;
};

template <std::meta::info Relation, mapping::relation_kind Kind>
struct relation_target_selector;

template <std::meta::info Relation>
struct relation_target_selector<Relation, mapping::relation_kind::belongs_to> {
    using type = reflect::single_target_t<reflect::member_type_t<Relation>>;
};

template <std::meta::info Relation>
struct relation_target_selector<Relation, mapping::relation_kind::has_one> {
    using type = reflect::single_target_t<reflect::member_type_t<Relation>>;
};

template <std::meta::info Relation>
struct relation_target_selector<Relation, mapping::relation_kind::has_many> {
    using type = reflect::many_target_t<reflect::member_type_t<Relation>>;
};

template <std::meta::info Relation>
struct relation_target_selector<Relation, mapping::relation_kind::many_to_many> {
    using type = reflect::many_target_t<reflect::member_type_t<Relation>>;
};

template <std::meta::info Relation>
using relation_target_t = typename relation_target_selector<
    Relation,
    mapping::relation_annotation_traits<reflect::relation_annotation_t<Relation>>::kind>::type;

template <std::meta::info Relation>
JoinSpec make_join_spec(JoinKind kind) {
    using Owner = reflect::owner_type_t<Relation>;
    using Target = relation_target_t<Relation>;
    using A = reflect::relation_annotation_t<Relation>;
    using Traits = mapping::relation_annotation_traits<A>;

    JoinSpec spec;
    spec.kind = kind;
    spec.relation_kind = Traits::kind;
    spec.owner_type = std::type_index(typeid(Owner));
    spec.target_type = std::type_index(typeid(Target));
    spec.target_table = reflect::table_name<Target>();

    if constexpr (Traits::kind == mapping::relation_kind::belongs_to) {
        constexpr auto foreign_key = Traits::foreign_key();
        constexpr auto target_key = reflect::key_or_primary<Target>(Traits::target_key());
        spec.owner_column = reflect::column_name<foreign_key>();
        spec.target_column = reflect::column_name<target_key>();
    } else if constexpr (Traits::kind == mapping::relation_kind::has_one ||
                         Traits::kind == mapping::relation_kind::has_many) {
        constexpr auto target_fk = Traits::target_foreign_key();
        constexpr auto local_key = reflect::key_or_primary<Owner>(Traits::local_key());
        spec.owner_column = reflect::column_name<local_key>();
        spec.target_column = reflect::column_name<target_fk>();
    } else {
        constexpr auto pivot_reflection = Traits::pivot();
        using Pivot = [: pivot_reflection :];
        constexpr auto pivot_root_fk = Traits::pivot_root_foreign_key();
        constexpr auto pivot_target_fk = Traits::pivot_target_foreign_key();
        constexpr auto local_key = reflect::key_or_primary<Owner>(Traits::local_key());
        constexpr auto target_key = reflect::key_or_primary<Target>(Traits::target_key());
        spec.owner_column = reflect::column_name<local_key>();
        spec.target_column = reflect::column_name<target_key>();
        spec.pivot_table = reflect::table_name<Pivot>();
        spec.pivot_root_column = reflect::column_name<pivot_root_fk>();
        spec.pivot_target_column = reflect::column_name<pivot_target_fk>();
    }
    return spec;
}

template <reflect::Entity Root, typename... Scope>
class BasicSelectQuery {
public:
    BasicSelectQuery() : state_(std::make_shared<SelectState>()) {}

    BasicSelectQuery& distinct(bool enabled = true) {
        state_->distinct = enabled;
        return *this;
    }

    template <typename... Owners>
    requires ((type_in_pack_v<Owners, Scope...>) && ...)
    BasicSelectQuery& where(Expression<Owners...> expression) {
        state_->where = expression.node();
        return *this;
    }

    template <typename... Owners>
    requires ((type_in_pack_v<Owners, Scope...>) && ...)
    BasicSelectQuery& having(Expression<Owners...> expression) {
        state_->having = expression.node();
        return *this;
    }

    template <std::meta::info Member>
    requires type_in_pack_v<typename Field<Member>::owner_type, Scope...>
    BasicSelectQuery& group_by(Field<Member> f) {
        state_->group_by.push_back(column_ref(f));
        return *this;
    }

    template <std::meta::info Member>
    requires type_in_pack_v<typename Field<Member>::owner_type, Scope...>
    BasicSelectQuery& order_by(Field<Member> f, bool ascending = true) {
        state_->order_by.push_back(OrderSpec{column_ref(f), ascending});
        return *this;
    }

    BasicSelectQuery& limit(std::size_t value) {
        state_->limit = value;
        return *this;
    }

    BasicSelectQuery& offset(std::size_t value) {
        state_->offset = value;
        return *this;
    }

    [[nodiscard]] BasicSelectQuery without_pagination() const {
        auto next = std::make_shared<SelectState>(*state_);
        next->limit.reset();
        next->offset.reset();
        return BasicSelectQuery{std::move(next)};
    }

    BasicSelectQuery& from(std::string source_name) {
        if (source_name.empty()) throw std::invalid_argument("MetalORM: FROM source name cannot be empty");
        state_->from_name = std::move(source_name);
        state_->derived_source.reset();
        return *this;
    }

    template <reflect::Entity SubRoot, typename... SubScope>
    BasicSelectQuery& from_subquery(
        BasicSelectQuery<SubRoot, SubScope...> query,
        std::string alias,
        std::vector<std::string> column_aliases = {}) {
        if (alias.empty()) throw std::invalid_argument("MetalORM: derived table requires a non-empty alias");
        if (!column_aliases.empty()) {
            throw std::logic_error(
                "MetalORM: SQLite derived tables do not support column alias lists; alias the subquery projections instead");
        }
        const auto arity = query.projection_arity();
        state_->from_name.reset();
        state_->derived_source = DerivedSourceSpec{
            std::move(alias),
            arity,
            [query = std::move(query)](const Dialect& dialect) mutable {
                return query.compile_subquery(dialect);
            }};
        return *this;
    }

    BasicSelectQuery& clear_projection() {
        state_->projections.clear();
        return *this;
    }

    template <std::meta::info Member>
    requires type_in_pack_v<typename Field<Member>::owner_type, Scope...>
    BasicSelectQuery& project(Field<Member> f) {
        auto value = as_scalar(f);
        state_->projections.push_back(ProjectionSpec{value.node(), std::nullopt});
        return *this;
    }

    template <std::meta::info Member>
    requires type_in_pack_v<typename Field<Member>::owner_type, Scope...>
    BasicSelectQuery& project_as(Field<Member> f, std::string alias) {
        auto value = as_scalar(f);
        state_->projections.push_back(ProjectionSpec{value.node(), std::move(alias)});
        return *this;
    }

    template <typename Owner>
    requires type_in_pack_v<Owner, Scope...>
    BasicSelectQuery& project(AggregateTerm<Owner> aggregate) {
        state_->projections.push_back(ProjectionSpec{aggregate.node(), aggregate.alias()});
        return *this;
    }

    template <typename... Owners>
    requires ((type_in_pack_v<Owners, Scope...>) && ...)
    BasicSelectQuery& project(WindowTerm<Owners...> window) {
        state_->projections.push_back(ProjectionSpec{window.node(), window.alias()});
        return *this;
    }

    template <typename Result, typename... Owners>
    requires ((type_in_pack_v<Owners, Scope...>) && ...)
    BasicSelectQuery& project(ScalarTerm<Result, Owners...> expression) {
        state_->projections.push_back(ProjectionSpec{expression.node(), expression.alias()});
        return *this;
    }

    template <std::meta::info Relation>
    auto join() const {
        return join_impl<Relation>(JoinKind::Inner);
    }

    template <std::meta::info Relation>
    auto left_join() const {
        return join_impl<Relation>(JoinKind::Left);
    }

    template <std::meta::info OwnerMember>
    requires type_in_pack_v<typename Field<OwnerMember>::owner_type, Scope...>
    BasicSelectQuery& join_cte(
        std::string cte_name,
        std::string cte_column,
        JoinKind kind = JoinKind::Inner) {
        if (cte_name.empty() || cte_column.empty()) {
            throw std::invalid_argument("MetalORM: CTE join requires non-empty CTE and column names");
        }
        state_->cte_joins.push_back(CteJoinSpec{
            kind, std::move(cte_name), column_ref(field<OwnerMember>), std::move(cte_column)});
        return *this;
    }

    template <reflect::Entity CteRoot, typename... CteScope>
    BasicSelectQuery& with(
        std::string name,
        BasicSelectQuery<CteRoot, CteScope...> query,
        std::vector<std::string> columns = {}) {
        return add_cte(std::move(name), std::move(query), std::move(columns), false);
    }

    template <reflect::Entity CteRoot, typename... CteScope>
    BasicSelectQuery& with_recursive(
        std::string name,
        BasicSelectQuery<CteRoot, CteScope...> query,
        std::vector<std::string> columns = {}) {
        return add_cte(std::move(name), std::move(query), std::move(columns), true);
    }

    template <reflect::Entity OtherRoot, typename... OtherScope>
    BasicSelectQuery& union_with(BasicSelectQuery<OtherRoot, OtherScope...> query) {
        return add_set_operation(SetOperationKind::Union, std::move(query));
    }

    template <reflect::Entity OtherRoot, typename... OtherScope>
    BasicSelectQuery& union_all(BasicSelectQuery<OtherRoot, OtherScope...> query) {
        return add_set_operation(SetOperationKind::UnionAll, std::move(query));
    }

    template <reflect::Entity OtherRoot, typename... OtherScope>
    BasicSelectQuery& intersect(BasicSelectQuery<OtherRoot, OtherScope...> query) {
        return add_set_operation(SetOperationKind::Intersect, std::move(query));
    }

    template <reflect::Entity OtherRoot, typename... OtherScope>
    BasicSelectQuery& except_with(BasicSelectQuery<OtherRoot, OtherScope...> query) {
        return add_set_operation(SetOperationKind::Except, std::move(query));
    }

    BasicSelectQuery& where_column(std::string column, CompareOp op, Value value) {
        auto left = std::make_shared<ScalarNode>(ScalarNode{
            ColumnRef{std::type_index(typeid(Root)), std::move(column)}});
        auto right = std::make_shared<ScalarNode>(ScalarNode{std::move(value)});
        state_->where = Expression<Root>{ComparisonNode{std::move(left), op, std::move(right)}}.node();
        return *this;
    }

    [[nodiscard]] std::size_t projection_arity() const {
        if (!state_->projections.empty()) return state_->projections.size();
        std::size_t count = 0;
        reflect::for_each_column<Root>([&]<std::meta::info>() { ++count; });
        return count;
    }

    CompiledQuery compile(const Dialect& dialect) const {
        return compile_impl(dialect, true, {});
    }

    CompiledQuery compile_subquery(const Dialect& dialect) const {
        return compile_impl(dialect, false, {});
    }

    CompiledQuery compile_subquery_with_extra_where(
        const Dialect& dialect,
        ExtraWhereCompiler extra_where) const {
        return compile_impl(dialect, false, std::move(extra_where));
    }

    CompiledQuery compile_scalar_subquery(const Dialect& dialect) const {
        if (projection_arity() != 1) {
            throw std::logic_error("MetalORM: scalar subquery must project exactly one expression");
        }
        return compile_impl(dialect, false, {});
    }

private:
    template <reflect::Entity, typename...>
    friend class BasicSelectQuery;

    explicit BasicSelectQuery(std::shared_ptr<SelectState> state) : state_(std::move(state)) {}

    template <reflect::Entity CteRoot, typename... CteScope>
    BasicSelectQuery& add_cte(
        std::string name,
        BasicSelectQuery<CteRoot, CteScope...> query,
        std::vector<std::string> columns,
        bool recursive) {
        if (name.empty()) throw std::invalid_argument("MetalORM: CTE name cannot be empty");
        if (!columns.empty() && columns.size() != query.projection_arity()) {
            throw std::logic_error("MetalORM: CTE column list must match query projection arity");
        }
        state_->ctes.push_back(CteSpec{
            std::move(name),
            std::move(columns),
            recursive,
            [query = std::move(query)](const Dialect& dialect) mutable {
                return query.compile_subquery(dialect);
            }});
        return *this;
    }

    template <reflect::Entity OtherRoot, typename... OtherScope>
    BasicSelectQuery& add_set_operation(
        SetOperationKind kind,
        BasicSelectQuery<OtherRoot, OtherScope...> query) {
        const auto lhs_arity = projection_arity();
        const auto rhs_arity = query.projection_arity();
        if (lhs_arity != rhs_arity) {
            throw std::logic_error("MetalORM: set-operation operands must project the same number of columns");
        }
        state_->set_operations.push_back(SetOperationSpec{
            kind,
            rhs_arity,
            [query = std::move(query)](const Dialect& dialect) mutable {
                return query.compile_subquery(dialect);
            }});
        return *this;
    }

    template <std::meta::info Relation>
    auto join_impl(JoinKind kind) const {
        static_assert(reflect::has_relation_annotation<Relation>(),
                      "MetalORM: join<> requires a reflected relationship member");
        using Owner = reflect::owner_type_t<Relation>;
        using Target = relation_target_t<Relation>;
        static_assert(type_in_pack_v<Owner, Scope...>,
                      "MetalORM: joined relation owner must already be in query scope");
        static_assert(!type_in_pack_v<Target, Scope...>,
                      "MetalORM: the same entity type cannot be joined twice in one typed scope");
        static_assert(reflect::validate_mapping<Owner>());
        static_assert(reflect::validate_mapping<Target>());

        auto next = std::make_shared<SelectState>(*state_);
        next->joins.push_back(make_join_spec<Relation>(kind));
        return BasicSelectQuery<Root, Scope..., Target>{std::move(next)};
    }

    void append_projection_sql(std::string& sql, CompileContext& ctx, std::string_view root_alias) const {
        if (state_->projections.empty()) {
            bool first = true;
            reflect::for_each_column<Root>([&]<std::meta::info Member>() {
                if (!first) sql += ", ";
                first = false;
                const auto name = reflect::column_name<Member>();
                if (!root_alias.empty()) {
                    sql += ctx.dialect.quote_identifier(root_alias) + "." + ctx.dialect.quote_identifier(name) +
                           " AS " + ctx.dialect.quote_identifier(name);
                } else {
                    sql += ctx.dialect.quote_identifier(name);
                }
            });
            return;
        }

        for (std::size_t i = 0; i < state_->projections.size(); ++i) {
            if (i) sql += ", ";
            const auto& projection = state_->projections[i];
            sql += compile_scalar(projection.expression, ctx);
            if (projection.alias) sql += " AS " + ctx.dialect.quote_identifier(*projection.alias);
        }
    }

    std::string compile_from_source(const Dialect& dialect, CompiledQuery& out, std::string_view root_alias) const {
        if (state_->derived_source) {
            const auto& derived = *state_->derived_source;
            auto source = derived.compile_query(dialect);
            out.params.insert(out.params.end(), source.params.begin(), source.params.end());
            return "(" + source.sql + ") AS " + dialect.quote_identifier(derived.alias);
        }

        std::string sql = dialect.quote_identifier(
            state_->from_name ? *state_->from_name : reflect::table_name<Root>());
        if (!root_alias.empty()) sql += " AS " + dialect.quote_identifier(root_alias);
        return sql;
    }

    CompiledQuery compile_impl(
        const Dialect& dialect,
        bool terminate,
        const ExtraWhereCompiler& extra_where) const {
        CompiledQuery out;

        if (!state_->ctes.empty()) {
            bool recursive = false;
            for (const auto& cte : state_->ctes) recursive = recursive || cte.recursive;
            out.sql += recursive ? "WITH RECURSIVE " : "WITH ";
            for (std::size_t i = 0; i < state_->ctes.size(); ++i) {
                if (i) out.sql += ", ";
                const auto& cte = state_->ctes[i];
                out.sql += dialect.quote_identifier(cte.name);
                if (!cte.columns.empty()) {
                    out.sql += " (";
                    for (std::size_t c = 0; c < cte.columns.size(); ++c) {
                        if (c) out.sql += ", ";
                        out.sql += dialect.quote_identifier(cte.columns[c]);
                    }
                    out.sql += ")";
                }
                auto compiled_cte = cte.compile_query(dialect);
                out.params.insert(out.params.end(), compiled_cte.params.begin(), compiled_cte.params.end());
                out.sql += " AS (" + compiled_cte.sql + ")";
            }
            out.sql += " ";
        }

        const bool has_joins = !state_->joins.empty() || !state_->cte_joins.empty();
        const bool needs_root_alias = has_joins || static_cast<bool>(extra_where);
        const std::string root_alias = state_->derived_source
            ? state_->derived_source->alias
            : (needs_root_alias ? "t0" : "");

        CompileContext ctx{dialect, {}, out.params};
        ctx.aliases.emplace(std::type_index(typeid(Root)), root_alias);

        std::vector<std::string> join_sql;
        std::size_t entity_alias = 1;
        std::size_t pivot_alias = 0;
        for (const auto& join : state_->joins) {
            const auto owner_it = ctx.aliases.find(join.owner_type);
            if (owner_it == ctx.aliases.end()) {
                throw std::logic_error("MetalORM: join owner was not introduced before its relation");
            }
            const std::string owner_alias = owner_it->second;
            const std::string target_alias = "t" + std::to_string(entity_alias++);
            ctx.aliases.emplace(join.target_type, target_alias);
            const std::string keyword = join.kind == JoinKind::Left ? " LEFT JOIN " : " INNER JOIN ";

            if (join.relation_kind == mapping::relation_kind::many_to_many) {
                const std::string pivot_alias_name = "p" + std::to_string(pivot_alias++);
                join_sql.push_back(
                    keyword + dialect.quote_identifier(*join.pivot_table) + " AS " +
                    dialect.quote_identifier(pivot_alias_name) + " ON " +
                    dialect.quote_identifier(pivot_alias_name) + "." +
                    dialect.quote_identifier(*join.pivot_root_column) + " = " +
                    dialect.quote_identifier(owner_alias) + "." +
                    dialect.quote_identifier(join.owner_column));
                join_sql.push_back(
                    keyword + dialect.quote_identifier(join.target_table) + " AS " +
                    dialect.quote_identifier(target_alias) + " ON " +
                    dialect.quote_identifier(target_alias) + "." +
                    dialect.quote_identifier(join.target_column) + " = " +
                    dialect.quote_identifier(pivot_alias_name) + "." +
                    dialect.quote_identifier(*join.pivot_target_column));
            } else {
                join_sql.push_back(
                    keyword + dialect.quote_identifier(join.target_table) + " AS " +
                    dialect.quote_identifier(target_alias) + " ON " +
                    dialect.quote_identifier(owner_alias) + "." +
                    dialect.quote_identifier(join.owner_column) + " = " +
                    dialect.quote_identifier(target_alias) + "." +
                    dialect.quote_identifier(join.target_column));
            }
        }

        for (const auto& join : state_->cte_joins) {
            const std::string keyword = join.kind == JoinKind::Left ? " LEFT JOIN " : " INNER JOIN ";
            join_sql.push_back(
                keyword + dialect.quote_identifier(join.cte_name) + " ON " +
                compile_column(join.owner_column, ctx) + " = " +
                dialect.quote_identifier(join.cte_name) + "." + dialect.quote_identifier(join.cte_column));
        }

        std::string base = "SELECT ";
        if (state_->distinct) base += "DISTINCT ";
        append_projection_sql(base, ctx, root_alias);
        base += " FROM ";
        base += compile_from_source(dialect, out, root_alias);
        for (const auto& clause : join_sql) base += clause;

        std::optional<std::string> extra_where_sql;
        std::vector<Value> extra_where_params;
        if (extra_where) {
            auto compiled_extra = extra_where(dialect, root_alias);
            if (!compiled_extra.sql.empty()) {
                extra_where_sql = std::move(compiled_extra.sql);
                extra_where_params = std::move(compiled_extra.params);
            }
        }

        if (state_->where || extra_where_sql) {
            base += " WHERE ";
            if (state_->where) {
                base += "(" + compile_expression(*state_->where, ctx) + ")";
            }
            if (state_->where && extra_where_sql) base += " AND ";
            if (extra_where_sql) {
                base += "(" + *extra_where_sql + ")";
                out.params.insert(
                    out.params.end(), extra_where_params.begin(), extra_where_params.end());
            }
        }
        if (!state_->group_by.empty()) {
            base += " GROUP BY ";
            for (std::size_t i = 0; i < state_->group_by.size(); ++i) {
                if (i) base += ", ";
                base += compile_column(state_->group_by[i], ctx);
            }
        }
        if (state_->having) base += " HAVING " + compile_expression(*state_->having, ctx);

        out.sql += base;
        for (const auto& set_op : state_->set_operations) {
            auto rhs = set_op.compile_query(dialect);
            out.params.insert(out.params.end(), rhs.params.begin(), rhs.params.end());
            out.sql += " " + set_operation_token(set_op.kind) + " " + rhs.sql;
        }

        if (!state_->order_by.empty()) {
            out.sql += " ORDER BY ";
            for (std::size_t i = 0; i < state_->order_by.size(); ++i) {
                if (i) out.sql += ", ";
                out.sql += compile_column(state_->order_by[i].column, ctx) +
                           (state_->order_by[i].ascending ? " ASC" : " DESC");
            }
        }
        if (state_->limit) out.sql += " LIMIT " + std::to_string(*state_->limit);
        if (state_->offset) out.sql += " OFFSET " + std::to_string(*state_->offset);
        if (terminate) out.sql += ";";
        return out;
    }

    std::shared_ptr<SelectState> state_;
};

template <reflect::Entity T>
using SelectQuery = BasicSelectQuery<T, T>;

template <reflect::Entity T>
SelectQuery<T> select() { return {}; }

template <std::meta::info Member, reflect::Entity Root, typename... Scope>
auto in(Field<Member> f, BasicSelectQuery<Root, Scope...> query) {
    using Owner = typename Field<Member>::owner_type;
    return Expression<Owner>{InSubqueryNode{
        as_scalar(f).node(),
        [query = std::move(query)](const Dialect& dialect) mutable {
            return query.compile_scalar_subquery(dialect);
        },
        false}};
}

template <std::meta::info Member, reflect::Entity Root, typename... Scope>
auto not_in(Field<Member> f, BasicSelectQuery<Root, Scope...> query) {
    using Owner = typename Field<Member>::owner_type;
    return Expression<Owner>{InSubqueryNode{
        as_scalar(f).node(),
        [query = std::move(query)](const Dialect& dialect) mutable {
            return query.compile_scalar_subquery(dialect);
        },
        true}};
}

template <reflect::Entity Root, typename... Scope>
auto exists(BasicSelectQuery<Root, Scope...> query) {
    return Expression<>{ExistsNode{
        [query = std::move(query)](const Dialect& dialect) mutable {
            return query.compile_subquery(dialect);
        },
        false}};
}

template <reflect::Entity Root, typename... Scope>
auto not_exists(BasicSelectQuery<Root, Scope...> query) {
    return Expression<>{ExistsNode{
        [query = std::move(query)](const Dialect& dialect) mutable {
            return query.compile_subquery(dialect);
        },
        true}};
}

} // namespace metal