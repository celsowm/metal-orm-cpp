#pragma once

#include "metal/query.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <utility>
#include <variant>
#include <vector>

namespace metal {

struct DmlExcluded {
    std::string column;
};

inline DmlExcluded excluded(std::string column) {
    return DmlExcluded{std::move(column)};
}

using DmlOperand = std::variant<Value, DmlExcluded>;

struct DmlAssignment {
    std::string column;
    DmlOperand value;
};

struct DmlPredicate {
    std::string column;
    CompareOp op{CompareOp::Eq};
    Value value;
};

struct DmlInPredicate {
    std::string column;
    std::vector<Value> values;
    bool negated{false};
};

using DmlWhereCompiler =
    std::function<std::string(const Dialect&, std::vector<Value>&)>;

struct DmlReturning {
    std::string column;
    std::optional<std::string> alias;
};

struct DmlConflictClause {
    std::vector<std::string> columns;
    bool do_nothing{false};
    std::vector<DmlAssignment> assignments;
    std::vector<DmlPredicate> predicates;
};

struct InsertNode {
    std::string table;
    std::vector<std::string> columns;
    std::vector<std::vector<DmlAssignment>> rows;
    std::function<CompiledQuery(const Dialect&)> select_source;
    std::optional<DmlConflictClause> conflict;
    std::vector<DmlReturning> returning;
};

struct UpdateNode {
    std::string table;
    std::vector<DmlAssignment> assignments;
    std::vector<DmlPredicate> predicates;
    std::vector<DmlInPredicate> in_predicates;
    DmlWhereCompiler extra_where;
    std::vector<DmlReturning> returning;
};

struct DeleteNode {
    std::string table;
    std::vector<DmlPredicate> predicates;
    std::vector<DmlInPredicate> in_predicates;
    DmlWhereCompiler extra_where;
    std::vector<DmlReturning> returning;
};

inline std::string compile_dml_operand(
    const DmlOperand& operand,
    const Dialect& dialect,
    std::vector<Value>& params,
    bool allow_excluded = false) {
    if (const auto* value = std::get_if<Value>(&operand)) {
        params.push_back(*value);
        return dialect.placeholder(params.size());
    }
    if (!allow_excluded) {
        throw std::logic_error("MetalORM: excluded() is valid only inside ON CONFLICT DO UPDATE");
    }
    const auto& value = std::get<DmlExcluded>(operand);
    return "excluded." + dialect.quote_identifier(value.column);
}

inline void append_dml_predicates(
    std::string& sql,
    const std::vector<DmlPredicate>& predicates,
    const Dialect& dialect,
    std::vector<Value>& params) {
    if (predicates.empty()) return;
    sql += " WHERE ";
    for (std::size_t i = 0; i < predicates.size(); ++i) {
        if (i) sql += " AND ";
        const auto& predicate = predicates[i];
        sql += dialect.quote_identifier(predicate.column) + " " + compare_token(predicate.op) + " " +
               dialect.placeholder(params.size() + 1);
        params.push_back(predicate.value);
    }
}

inline void append_dml_where(
    std::string& sql,
    const std::vector<DmlPredicate>& predicates,
    const std::vector<DmlInPredicate>& in_predicates,
    const DmlWhereCompiler& extra_where,
    const Dialect& dialect,
    std::vector<Value>& params) {
    if (predicates.empty() && in_predicates.empty() && !extra_where) return;

    sql += " WHERE ";
    bool has_term = false;
    const auto append_and = [&] {
        if (has_term) sql += " AND ";
        has_term = true;
    };

    for (const auto& predicate : predicates) {
        append_and();
        sql += dialect.quote_identifier(predicate.column) + " " + compare_token(predicate.op) + " " +
               dialect.placeholder(params.size() + 1);
        params.push_back(predicate.value);
    }

    for (const auto& predicate : in_predicates) {
        append_and();
        if (predicate.values.empty()) {
            sql += predicate.negated ? "1" : "0";
            continue;
        }

        sql += dialect.quote_identifier(predicate.column);
        sql += predicate.negated ? " NOT IN (" : " IN (";
        for (std::size_t i = 0; i < predicate.values.size(); ++i) {
            if (i) sql += ", ";
            sql += dialect.placeholder(params.size() + 1);
            params.push_back(predicate.values[i]);
        }
        sql += ")";
    }

    if (extra_where) {
        append_and();
        const auto fragment = extra_where(dialect, params);
        if (fragment.empty()) {
            throw std::logic_error("MetalORM: DML expression predicate compiled to an empty fragment");
        }
        sql += "(" + fragment + ")";
    }
}

template <reflect::Mapped T>
DmlWhereCompiler dml_where(Expression<T> expression) {
    static_assert(reflect::validate_mapping<T>());
    return [expression = std::move(expression)](
               const Dialect& dialect,
               std::vector<Value>& params) -> std::string {
        CompileContext ctx{
            dialect,
            {{std::type_index(typeid(T)), std::string{}}},
            params
        };
        return compile_expression(expression.node(), ctx);
    };
}

inline void append_returning(
    std::string& sql,
    const std::vector<DmlReturning>& returning,
    const Dialect& dialect) {
    if (returning.empty()) return;
    sql += " RETURNING ";
    for (std::size_t i = 0; i < returning.size(); ++i) {
        if (i) sql += ", ";
        sql += dialect.quote_identifier(returning[i].column);
        if (returning[i].alias) {
            sql += " AS " + dialect.quote_identifier(*returning[i].alias);
        }
    }
}

class InsertQueryBuilder;

class ConflictBuilder {
public:
    ConflictBuilder(InsertQueryBuilder& owner, std::vector<std::string> columns)
        : owner_(owner), columns_(std::move(columns)) {}

    InsertQueryBuilder& do_nothing();
    InsertQueryBuilder& do_update(
        std::vector<DmlAssignment> assignments,
        std::vector<DmlPredicate> predicates = {});

private:
    InsertQueryBuilder& owner_;
    std::vector<std::string> columns_;
};

class InsertQueryBuilder {
public:
    explicit InsertQueryBuilder(
        std::string table,
        std::vector<std::string> known_table_columns = {})
        : node_{std::move(table), {}, {}, {}, std::nullopt, {}},
          known_table_columns_(std::move(known_table_columns)) {}

    InsertQueryBuilder& columns(std::vector<std::string> columns) {
        if (!columns.empty()) node_.columns = std::move(columns);
        return *this;
    }

    InsertQueryBuilder& values(std::vector<DmlAssignment> row) {
        if (node_.select_source) {
            throw std::logic_error("MetalORM: cannot mix INSERT ... VALUES with INSERT ... SELECT");
        }
        if (row.empty()) return *this;
        if (node_.columns.empty()) {
            node_.columns.reserve(row.size());
            for (const auto& assignment : row) node_.columns.push_back(assignment.column);
        }
        node_.rows.push_back(std::move(row));
        return *this;
    }

    InsertQueryBuilder& values(std::vector<std::vector<DmlAssignment>> rows) {
        for (auto& row : rows) values(std::move(row));
        return *this;
    }

    template <reflect::Entity Root, typename... Scope>
    InsertQueryBuilder& from_select(
        BasicSelectQuery<Root, Scope...> query,
        std::vector<std::string> columns = {}) {
        if (!node_.rows.empty()) {
            throw std::logic_error("MetalORM: cannot mix INSERT ... VALUES with INSERT ... SELECT");
        }
        if (!columns.empty()) node_.columns = std::move(columns);
        if (node_.columns.empty()) node_.columns = known_table_columns_;
        if (node_.columns.empty()) {
            throw std::logic_error("MetalORM: INSERT ... SELECT requires destination columns");
        }
        node_.select_source = [query = std::move(query)](const Dialect& dialect) mutable {
            return query.compile_subquery(dialect);
        };
        return *this;
    }

    ConflictBuilder on_conflict(std::vector<std::string> columns) {
        return ConflictBuilder{*this, std::move(columns)};
    }

    InsertQueryBuilder& returning(std::vector<std::string> columns) {
        node_.returning.clear();
        node_.returning.reserve(columns.size());
        for (auto& column : columns) {
            node_.returning.push_back(DmlReturning{std::move(column), std::nullopt});
        }
        return *this;
    }

    InsertQueryBuilder& returning(std::vector<DmlReturning> columns) {
        node_.returning = std::move(columns);
        return *this;
    }

    [[nodiscard]] const InsertNode& ast() const noexcept { return node_; }

    [[nodiscard]] CompiledQuery compile(const Dialect& dialect) const {
        CompiledQuery out;
        out.sql = "INSERT INTO " + dialect.quote_identifier(node_.table);

        if (node_.columns.empty()) {
            if (node_.rows.empty() && !node_.select_source) {
                out.sql += " DEFAULT VALUES";
            } else {
                throw std::logic_error("MetalORM: INSERT requires destination columns");
            }
        } else {
            out.sql += " (";
            for (std::size_t i = 0; i < node_.columns.size(); ++i) {
                if (i) out.sql += ", ";
                out.sql += dialect.quote_identifier(node_.columns[i]);
            }
            out.sql += ") ";

            if (node_.select_source) {
                auto source = node_.select_source(dialect);
                out.sql += source.sql;
                out.params.insert(out.params.end(), source.params.begin(), source.params.end());
            } else {
                if (node_.rows.empty()) {
                    throw std::logic_error("MetalORM: INSERT ... VALUES requires at least one row");
                }
                out.sql += "VALUES ";
                for (std::size_t row_index = 0; row_index < node_.rows.size(); ++row_index) {
                    if (row_index) out.sql += ", ";
                    out.sql += "(";
                    const auto& row = node_.rows[row_index];
                    for (std::size_t column_index = 0; column_index < node_.columns.size(); ++column_index) {
                        if (column_index) out.sql += ", ";
                        const auto& column = node_.columns[column_index];
                        auto value = std::find_if(row.begin(), row.end(), [&](const auto& assignment) {
                            return assignment.column == column;
                        });
                        if (value == row.end()) {
                            throw std::logic_error(
                                "MetalORM: INSERT row is missing destination column '" + column + "'");
                        }
                        out.sql += compile_dml_operand(value->value, dialect, out.params);
                    }
                    out.sql += ")";
                }
            }
        }

        if (node_.conflict) {
            const auto& conflict = *node_.conflict;
            if (conflict.columns.empty()) {
                throw std::logic_error("MetalORM: SQLite ON CONFLICT requires conflict columns");
            }

            out.sql += " ON CONFLICT (";
            for (std::size_t i = 0; i < conflict.columns.size(); ++i) {
                if (i) out.sql += ", ";
                out.sql += dialect.quote_identifier(conflict.columns[i]);
            }
            out.sql += ")";
            if (conflict.do_nothing) {
                out.sql += " DO NOTHING";
            } else {
                if (conflict.assignments.empty()) {
                    throw std::logic_error(
                        "MetalORM: SQLite ON CONFLICT DO UPDATE requires at least one assignment");
                }
                out.sql += " DO UPDATE SET ";
                for (std::size_t i = 0; i < conflict.assignments.size(); ++i) {
                    if (i) out.sql += ", ";
                    const auto& assignment = conflict.assignments[i];
                    out.sql += dialect.quote_identifier(assignment.column) + " = " +
                               compile_dml_operand(assignment.value, dialect, out.params, true);
                }
                append_dml_predicates(out.sql, conflict.predicates, dialect, out.params);
            }
        }

        append_returning(out.sql, node_.returning, dialect);
        out.sql += ";";
        return out;
    }

private:
    friend class ConflictBuilder;

    InsertQueryBuilder& set_conflict(DmlConflictClause clause) {
        node_.conflict = std::move(clause);
        return *this;
    }

    InsertNode node_;
    std::vector<std::string> known_table_columns_;
};

inline InsertQueryBuilder& ConflictBuilder::do_nothing() {
    return owner_.set_conflict(DmlConflictClause{std::move(columns_), true, {}, {}});
}

inline InsertQueryBuilder& ConflictBuilder::do_update(
    std::vector<DmlAssignment> assignments,
    std::vector<DmlPredicate> predicates) {
    if (assignments.empty()) {
        throw std::logic_error("MetalORM: ON CONFLICT DO UPDATE requires at least one assignment");
    }
    return owner_.set_conflict(DmlConflictClause{
        std::move(columns_), false, std::move(assignments), std::move(predicates)});
}

class UpdateQueryBuilder {
public:
    explicit UpdateQueryBuilder(std::string table)
        : node_{std::move(table), {}, {}, {}, {}, {}} {}

    UpdateQueryBuilder& set(std::vector<DmlAssignment> assignments) {
        node_.assignments = std::move(assignments);
        return *this;
    }

    UpdateQueryBuilder& where(std::vector<DmlPredicate> predicates) {
        node_.predicates = std::move(predicates);
        node_.in_predicates.clear();
        node_.extra_where = {};
        return *this;
    }

    template <reflect::Mapped T>
    UpdateQueryBuilder& where(Expression<T> expression) {
        if (node_.table != reflect::table_name<T>()) {
            throw std::logic_error("MetalORM: UPDATE expression owner does not match target table");
        }
        node_.predicates.clear();
        node_.in_predicates.clear();
        node_.extra_where = dml_where<T>(std::move(expression));
        return *this;
    }

    UpdateQueryBuilder& where_eq(std::string column, Value value) {
        node_.predicates.push_back(DmlPredicate{std::move(column), CompareOp::Eq, std::move(value)});
        return *this;
    }

    UpdateQueryBuilder& where_in(std::string column, std::vector<Value> values, bool negated = false) {
        node_.in_predicates.push_back(DmlInPredicate{std::move(column), std::move(values), negated});
        return *this;
    }

    template <reflect::Mapped T>
    UpdateQueryBuilder& and_where(Expression<T> expression) {
        if (node_.table != reflect::table_name<T>()) {
            throw std::logic_error("MetalORM: UPDATE expression owner does not match target table");
        }
        node_.extra_where = dml_where<T>(std::move(expression));
        return *this;
    }

    UpdateQueryBuilder& returning(std::vector<std::string> columns) {
        node_.returning.clear();
        node_.returning.reserve(columns.size());
        for (auto& column : columns) {
            node_.returning.push_back(DmlReturning{std::move(column), std::nullopt});
        }
        return *this;
    }

    UpdateQueryBuilder& returning(std::vector<DmlReturning> columns) {
        node_.returning = std::move(columns);
        return *this;
    }

    [[nodiscard]] const UpdateNode& ast() const noexcept { return node_; }

    [[nodiscard]] CompiledQuery compile(const Dialect& dialect) const {
        if (node_.assignments.empty()) {
            throw std::logic_error("MetalORM: UPDATE requires at least one assignment");
        }
        CompiledQuery out;
        out.sql = "UPDATE " + dialect.quote_identifier(node_.table) + " SET ";
        for (std::size_t i = 0; i < node_.assignments.size(); ++i) {
            if (i) out.sql += ", ";
            const auto& assignment = node_.assignments[i];
            out.sql += dialect.quote_identifier(assignment.column) + " = " +
                       compile_dml_operand(assignment.value, dialect, out.params);
        }
        append_dml_where(
            out.sql,
            node_.predicates,
            node_.in_predicates,
            node_.extra_where,
            dialect,
            out.params);
        append_returning(out.sql, node_.returning, dialect);
        out.sql += ";";
        return out;
    }

private:
    UpdateNode node_;
};

class DeleteQueryBuilder {
public:
    explicit DeleteQueryBuilder(std::string table)
        : node_{std::move(table), {}, {}, {}, {}} {}

    DeleteQueryBuilder& where(std::vector<DmlPredicate> predicates) {
        node_.predicates = std::move(predicates);
        node_.in_predicates.clear();
        node_.extra_where = {};
        return *this;
    }

    template <reflect::Mapped T>
    DeleteQueryBuilder& where(Expression<T> expression) {
        if (node_.table != reflect::table_name<T>()) {
            throw std::logic_error("MetalORM: DELETE expression owner does not match target table");
        }
        node_.predicates.clear();
        node_.in_predicates.clear();
        node_.extra_where = dml_where<T>(std::move(expression));
        return *this;
    }

    DeleteQueryBuilder& where_eq(std::string column, Value value) {
        node_.predicates.push_back(DmlPredicate{std::move(column), CompareOp::Eq, std::move(value)});
        return *this;
    }

    DeleteQueryBuilder& where_in(std::string column, std::vector<Value> values, bool negated = false) {
        node_.in_predicates.push_back(DmlInPredicate{std::move(column), std::move(values), negated});
        return *this;
    }

    template <reflect::Mapped T>
    DeleteQueryBuilder& and_where(Expression<T> expression) {
        if (node_.table != reflect::table_name<T>()) {
            throw std::logic_error("MetalORM: DELETE expression owner does not match target table");
        }
        node_.extra_where = dml_where<T>(std::move(expression));
        return *this;
    }

    DeleteQueryBuilder& returning(std::vector<std::string> columns) {
        node_.returning.clear();
        node_.returning.reserve(columns.size());
        for (auto& column : columns) {
            node_.returning.push_back(DmlReturning{std::move(column), std::nullopt});
        }
        return *this;
    }

    DeleteQueryBuilder& returning(std::vector<DmlReturning> columns) {
        node_.returning = std::move(columns);
        return *this;
    }

    [[nodiscard]] const DeleteNode& ast() const noexcept { return node_; }

    [[nodiscard]] CompiledQuery compile(const Dialect& dialect) const {
        CompiledQuery out;
        out.sql = "DELETE FROM " + dialect.quote_identifier(node_.table);
        append_dml_where(
            out.sql,
            node_.predicates,
            node_.in_predicates,
            node_.extra_where,
            dialect,
            out.params);
        append_returning(out.sql, node_.returning, dialect);
        out.sql += ";";
        return out;
    }

private:
    DeleteNode node_;
};

template <reflect::Mapped T>
InsertQueryBuilder insert_into() {
    std::vector<std::string> columns;
    reflect::for_each_column<T>([&]<std::meta::info Member>() {
        columns.push_back(reflect::column_name<Member>());
    });
    return InsertQueryBuilder{reflect::table_name<T>(), std::move(columns)};
}

template <reflect::Mapped T>
UpdateQueryBuilder update() {
    return UpdateQueryBuilder{reflect::table_name<T>()};
}

template <reflect::Mapped T>
DeleteQueryBuilder delete_from() {
    return DeleteQueryBuilder{reflect::table_name<T>()};
}

} // namespace metal
