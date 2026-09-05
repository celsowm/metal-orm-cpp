#pragma once

#include "metal/execution.hpp"
#include "metal/orm.hpp"
#include "metal/query.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace metal {

enum class ProcedureDirection {
    In,
    Out,
    InOut
};

struct ProcedureRef {
    std::string name;
    std::optional<std::string> schema;
};

struct ProcedureParam {
    std::string name;
    ProcedureDirection direction{ProcedureDirection::In};
    ScalarPtr value;
    std::optional<std::string> db_type;
};

struct ProcedureCall {
    ProcedureRef ref;
    std::vector<ProcedureParam> params;
};

enum class ProcedureOutSource {
    None,
    FirstResultSet,
    LastResultSet
};

struct ProcedureOutParams {
    ProcedureOutSource source{ProcedureOutSource::None};
    std::vector<std::string> names;
};

struct CompiledProcedureCall : CompiledQuery {
    ProcedureOutParams out_params;
};

class ProcedureCompiler {
public:
    virtual ~ProcedureCompiler() = default;
    [[nodiscard]] virtual CompiledProcedureCall compile_procedure_call(
        const ProcedureCall& call) const = 0;
};

class ProcedureExecutor {
public:
    virtual ~ProcedureExecutor() = default;
    [[nodiscard]] virtual std::vector<QueryResult> execute_procedure(
        const CompiledProcedureCall& call) = 0;
};

struct ProcedureExecutionResult {
    std::vector<QueryResult> result_sets;
    std::unordered_map<std::string, Value> out;
};

namespace procedure_detail {

inline bool ascii_iequals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto l = static_cast<unsigned char>(left[i]);
        const auto r = static_cast<unsigned char>(right[i]);
        if (std::tolower(l) != std::tolower(r)) return false;
    }
    return true;
}

inline const Value* find_out_value(const Row& row, std::string_view name) {
    if (auto found = row.find(std::string(name)); found != row.end()) {
        return &found->second;
    }
    for (const auto& [column, value] : row) {
        if (ascii_iequals(column, name)) return &value;
    }
    return nullptr;
}

inline std::unordered_map<std::string, Value> extract_out_values(
    const CompiledProcedureCall& compiled,
    const std::vector<QueryResult>& result_sets) {
    if (compiled.out_params.names.empty() ||
        compiled.out_params.source == ProcedureOutSource::None) {
        return {};
    }

    const QueryResult* source = nullptr;
    if (!result_sets.empty()) {
        source = compiled.out_params.source == ProcedureOutSource::FirstResultSet
            ? &result_sets.front()
            : &result_sets.back();
    }
    if (!source) {
        throw std::runtime_error(
            "MetalORM: procedure expected OUT parameters, but no result set was returned");
    }
    if (source->rows.empty()) {
        throw std::runtime_error(
            "MetalORM: procedure expected OUT parameters, but the selected result set has no rows");
    }

    std::unordered_map<std::string, Value> out;
    const auto& first = source->rows.front();
    for (const auto& name : compiled.out_params.names) {
        const auto* value = find_out_value(first, name);
        if (!value) {
            throw std::runtime_error(
                "MetalORM: procedure OUT parameter '" + name +
                "' was not found in the selected result set");
        }
        out.emplace(name, *value);
    }
    return out;
}

inline CompiledProcedureCall compile_postgres_procedure_call(
    const ProcedureCall& call,
    const Dialect& dialect) {
    CompiledProcedureCall out;
    CompileContext ctx{dialect, {}, out.params};

    const auto qualified = call.ref.schema
        ? dialect.quote_identifier(*call.ref.schema) + "." + dialect.quote_identifier(call.ref.name)
        : dialect.quote_identifier(call.ref.name);

    std::vector<std::string> args;
    args.reserve(call.params.size());
    for (const auto& param : call.params) {
        switch (param.direction) {
            case ProcedureDirection::In:
            case ProcedureDirection::InOut:
                if (!param.value) {
                    throw std::logic_error(
                        "MetalORM: PostgreSQL procedure input parameter '" + param.name +
                        "' has no value");
                }
                args.push_back(compile_scalar(param.value, ctx));
                break;
            case ProcedureDirection::Out:
                // PostgreSQL CALL requires an argument position for OUT parameters.
                // NULL is conventional and is not evaluated by the procedure.
                args.push_back("NULL");
                break;
        }

        if (param.direction == ProcedureDirection::Out ||
            param.direction == ProcedureDirection::InOut) {
            out.out_params.names.push_back(param.name);
        }
    }

    out.out_params.source = out.out_params.names.empty()
        ? ProcedureOutSource::None
        : ProcedureOutSource::LastResultSet;

    out.sql = "CALL " + qualified + "(";
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) out.sql += ", ";
        out.sql += args[i];
    }
    out.sql += ");";
    return out;
}

} // namespace procedure_detail

class ProcedureCallBuilder {
public:
    explicit ProcedureCallBuilder(
        std::string name,
        std::optional<std::string> schema = std::nullopt)
        : ast_{ProcedureRef{std::move(name), std::move(schema)}, {}} {}

    explicit ProcedureCallBuilder(ProcedureCall ast)
        : ast_(std::move(ast)) {}

    template <ScalarInput V>
    [[nodiscard]] ProcedureCallBuilder in(std::string name, V&& value) const {
        auto next = ast_;
        next.params.push_back(ProcedureParam{
            std::move(name),
            ProcedureDirection::In,
            as_scalar(std::forward<V>(value)).node(),
            std::nullopt});
        return ProcedureCallBuilder{std::move(next)};
    }

    [[nodiscard]] ProcedureCallBuilder out(
        std::string name,
        std::optional<std::string> db_type = std::nullopt) const {
        auto next = ast_;
        next.params.push_back(ProcedureParam{
            std::move(name),
            ProcedureDirection::Out,
            {},
            std::move(db_type)});
        return ProcedureCallBuilder{std::move(next)};
    }

    template <ScalarInput V>
    [[nodiscard]] ProcedureCallBuilder in_out(
        std::string name,
        V&& value,
        std::optional<std::string> db_type = std::nullopt) const {
        auto next = ast_;
        next.params.push_back(ProcedureParam{
            std::move(name),
            ProcedureDirection::InOut,
            as_scalar(std::forward<V>(value)).node(),
            std::move(db_type)});
        return ProcedureCallBuilder{std::move(next)};
    }

    [[nodiscard]] const ProcedureCall& ast() const noexcept { return ast_; }
    [[nodiscard]] ProcedureCall get_ast() const { return ast_; }

    [[nodiscard]] CompiledProcedureCall compile(const Dialect& dialect) const {
        if (dialect.family() == DialectFamily::PostgreSQL) {
            return procedure_detail::compile_postgres_procedure_call(ast_, dialect);
        }
        const auto* compiler = dynamic_cast<const ProcedureCompiler*>(&dialect);
        if (!compiler) {
            throw std::logic_error(
                "MetalORM: stored procedures are not supported by this dialect");
        }
        return compiler->compile_procedure_call(ast_);
    }

    [[nodiscard]] std::string to_sql(const Dialect& dialect) const {
        return compile(dialect).sql;
    }

    [[nodiscard]] ProcedureExecutionResult execute(Session& session) const {
        const auto compiled = compile(session.dialect());
        auto* executor = dynamic_cast<ProcedureExecutor*>(&session.executor());
        if (!executor) {
            throw std::logic_error(
                "MetalORM: stored procedure execution is not supported by this executor");
        }
        auto result_sets = executor->execute_procedure(compiled);
        auto out = procedure_detail::extract_out_values(compiled, result_sets);
        return ProcedureExecutionResult{
            std::move(result_sets),
            std::move(out)};
    }

private:
    ProcedureCall ast_;
};

inline ProcedureCallBuilder call_procedure(
    std::string name,
    std::optional<std::string> schema = std::nullopt) {
    return ProcedureCallBuilder{std::move(name), std::move(schema)};
}

} // namespace metal
