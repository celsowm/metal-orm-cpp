#pragma once

#include "metal/query.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace metal {

struct DmlAssignment {
    std::string column;
    Value value;
};

struct DmlPredicate {
    std::string column;
    CompareOp op{CompareOp::Eq};
    Value value;
};

struct InsertNode {
    std::string table;
    std::vector<DmlAssignment> values;
    bool conflict_do_nothing{false};
};

struct UpdateNode {
    std::string table;
    std::vector<DmlAssignment> assignments;
    std::vector<DmlPredicate> predicates;
};

struct DeleteNode {
    std::string table;
    std::vector<DmlPredicate> predicates;
};

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

class InsertQueryBuilder {
public:
    explicit InsertQueryBuilder(std::string table) : node_{std::move(table), {}, false} {}

    InsertQueryBuilder& values(std::vector<DmlAssignment> values) {
        node_.values = std::move(values);
        return *this;
    }

    InsertQueryBuilder& on_conflict_do_nothing(bool enabled = true) {
        node_.conflict_do_nothing = enabled;
        return *this;
    }

    [[nodiscard]] const InsertNode& ast() const noexcept { return node_; }

    [[nodiscard]] CompiledQuery compile(const Dialect& dialect) const {
        CompiledQuery out;
        out.sql = "INSERT INTO " + dialect.quote_identifier(node_.table);

        if (node_.values.empty()) {
            out.sql += " DEFAULT VALUES";
        } else {
            out.sql += " (";
            for (std::size_t i = 0; i < node_.values.size(); ++i) {
                if (i) out.sql += ", ";
                out.sql += dialect.quote_identifier(node_.values[i].column);
            }
            out.sql += ") VALUES (";
            for (std::size_t i = 0; i < node_.values.size(); ++i) {
                if (i) out.sql += ", ";
                out.params.push_back(node_.values[i].value);
                out.sql += dialect.placeholder(out.params.size());
            }
            out.sql += ")";
        }

        if (node_.conflict_do_nothing) out.sql += " ON CONFLICT DO NOTHING";
        out.sql += ";";
        return out;
    }

private:
    InsertNode node_;
};

class UpdateQueryBuilder {
public:
    explicit UpdateQueryBuilder(std::string table) : node_{std::move(table), {}, {}} {}

    UpdateQueryBuilder& set(std::vector<DmlAssignment> assignments) {
        node_.assignments = std::move(assignments);
        return *this;
    }

    UpdateQueryBuilder& where(std::vector<DmlPredicate> predicates) {
        node_.predicates = std::move(predicates);
        return *this;
    }

    UpdateQueryBuilder& where_eq(std::string column, Value value) {
        node_.predicates.push_back(DmlPredicate{std::move(column), CompareOp::Eq, std::move(value)});
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
                       dialect.placeholder(out.params.size() + 1);
            out.params.push_back(assignment.value);
        }
        append_dml_predicates(out.sql, node_.predicates, dialect, out.params);
        out.sql += ";";
        return out;
    }

private:
    UpdateNode node_;
};

class DeleteQueryBuilder {
public:
    explicit DeleteQueryBuilder(std::string table) : node_{std::move(table), {}} {}

    DeleteQueryBuilder& where(std::vector<DmlPredicate> predicates) {
        node_.predicates = std::move(predicates);
        return *this;
    }

    DeleteQueryBuilder& where_eq(std::string column, Value value) {
        node_.predicates.push_back(DmlPredicate{std::move(column), CompareOp::Eq, std::move(value)});
        return *this;
    }

    [[nodiscard]] const DeleteNode& ast() const noexcept { return node_; }

    [[nodiscard]] CompiledQuery compile(const Dialect& dialect) const {
        CompiledQuery out;
        out.sql = "DELETE FROM " + dialect.quote_identifier(node_.table);
        append_dml_predicates(out.sql, node_.predicates, dialect, out.params);
        out.sql += ";";
        return out;
    }

private:
    DeleteNode node_;
};

template <reflect::Mapped T>
InsertQueryBuilder insert_into() {
    return InsertQueryBuilder{reflect::table_name<T>()};
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
