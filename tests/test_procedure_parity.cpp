#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class TestProcedureDialect final : public metal::Dialect, public metal::ProcedureCompiler {
public:
    std::string quote_identifier(std::string_view id) const override {
        return "\"" + std::string(id) + "\"";
    }

    std::string placeholder(std::size_t index) const override {
        return "$" + std::to_string(index);
    }

    metal::CompiledProcedureCall compile_procedure_call(
        const metal::ProcedureCall& call) const override {
        metal::CompiledProcedureCall out;
        const auto qualified = call.ref.schema
            ? quote_identifier(*call.ref.schema) + "." + quote_identifier(call.ref.name)
            : quote_identifier(call.ref.name);

        std::vector<std::string> args;
        for (const auto& param : call.params) {
            if (param.direction == metal::ProcedureDirection::Out) continue;
            if (!param.value) {
                throw std::logic_error("missing procedure input value");
            }
            const auto* value = std::get_if<metal::Value>(&param.value->node);
            if (!value) {
                throw std::logic_error("test procedure dialect accepts literal inputs only");
            }
            out.params.push_back(*value);
            args.push_back(placeholder(out.params.size()));
        }

        for (const auto& param : call.params) {
            if (param.direction == metal::ProcedureDirection::Out ||
                param.direction == metal::ProcedureDirection::InOut) {
                out.out_params.names.push_back(param.name);
            }
        }
        out.out_params.source = out.out_params.names.empty()
            ? metal::ProcedureOutSource::None
            : metal::ProcedureOutSource::LastResultSet;

        out.sql = "CALL " + qualified + "(";
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i) out.sql += ", ";
            out.sql += args[i];
        }
        out.sql += ");";
        return out;
    }
};

class TestProcedureExecutor final : public metal::DbExecutor, public metal::ProcedureExecutor {
public:
    metal::QueryResult execute(
        const std::string&,
        const std::vector<metal::Value>& = {}) override {
        throw std::logic_error("ordinary execute should not be used for procedure capability test");
    }

    std::vector<metal::QueryResult> execute_procedure(
        const metal::CompiledProcedureCall& call) override {
        last_call = call;
        metal::QueryResult rows;
        rows.rows.push_back(metal::Row{
            {"message", metal::Value{std::string{"ok"}}}
        });

        metal::QueryResult out;
        out.rows.push_back(metal::Row{
            {"TOTAL", metal::Value{std::int64_t{42}}},
            {"state", metal::Value{std::string{"done"}}}
        });
        return {std::move(rows), std::move(out)};
    }

    metal::CompiledProcedureCall last_call;
};

int main() {
    auto base = metal::call_procedure("refresh_user", std::string{"admin"});
    auto call = base
        .in("user_id", std::int64_t{7})
        .out("total", std::string{"INTEGER"})
        .in_out("state", std::string{"pending"}, std::string{"TEXT"});

    assert(base.ast().params.empty());
    assert(call.ast().ref.name == "refresh_user");
    assert(call.ast().ref.schema == std::optional<std::string>{"admin"});
    assert(call.ast().params.size() == 3);
    assert(call.ast().params[0].direction == metal::ProcedureDirection::In);
    assert(call.ast().params[1].direction == metal::ProcedureDirection::Out);
    assert(call.ast().params[1].db_type == std::optional<std::string>{"INTEGER"});
    assert(call.ast().params[2].direction == metal::ProcedureDirection::InOut);
    assert(call.ast().params[2].db_type == std::optional<std::string>{"TEXT"});

    TestProcedureDialect dialect;
    const auto compiled = call.compile(dialect);
    assert(compiled.sql == "CALL \"admin\".\"refresh_user\"($1, $2);");
    assert(compiled.params.size() == 2);
    assert(metal::from_value<std::int64_t>(compiled.params[0]) == 7);
    assert(metal::from_value<std::string>(compiled.params[1]) == "pending");
    assert(compiled.out_params.source == metal::ProcedureOutSource::LastResultSet);
    assert(compiled.out_params.names == std::vector<std::string>({"total", "state"}));
    assert(call.to_sql(dialect) == compiled.sql);

    auto executor = std::make_shared<TestProcedureExecutor>();
    auto dialect_ptr = std::make_shared<TestProcedureDialect>();
    metal::Session session{executor, dialect_ptr};
    const auto result = call.execute(session);
    assert(result.result_sets.size() == 2);
    assert(metal::from_value<std::int64_t>(result.out.at("total")) == 42);
    assert(metal::from_value<std::string>(result.out.at("state")) == "done");
    assert(executor->last_call.sql == compiled.sql);

    metal::SQLiteDialect sqlite;
    bool sqlite_rejected = false;
    try {
        (void)call.compile(sqlite);
    } catch (const std::logic_error& error) {
        sqlite_rejected = std::string_view(error.what()).find("not supported") != std::string_view::npos;
    }
    assert(sqlite_rejected);

    auto sqlite_executor = std::make_shared<metal::SQLiteExecutor>(":memory:");
    metal::Session unsupported_executor_session{sqlite_executor, dialect_ptr};
    bool executor_rejected = false;
    try {
        (void)call.execute(unsupported_executor_session);
    } catch (const std::logic_error& error) {
        executor_rejected = std::string_view(error.what()).find("executor") != std::string_view::npos;
    }
    assert(executor_rejected);

    class EmptyOutExecutor final : public metal::DbExecutor, public metal::ProcedureExecutor {
    public:
        metal::QueryResult execute(
            const std::string&,
            const std::vector<metal::Value>& = {}) override {
            return {};
        }
        std::vector<metal::QueryResult> execute_procedure(
            const metal::CompiledProcedureCall&) override {
            return {metal::QueryResult{}};
        }
    };

    auto empty_executor = std::make_shared<EmptyOutExecutor>();
    metal::Session empty_session{empty_executor, dialect_ptr};
    bool empty_out_rejected = false;
    try {
        (void)call.execute(empty_session);
    } catch (const std::runtime_error& error) {
        empty_out_rejected = std::string_view(error.what()).find("no rows") != std::string_view::npos;
    }
    assert(empty_out_rejected);
}
