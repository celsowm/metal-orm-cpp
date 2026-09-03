#include <metal/postgres_execution.hpp>
#include <metal/postgres_schema_introspection.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct ScriptStep {
    std::string sql_marker;
    metal::QueryResult result;
};

class ScriptedExecutor final : public metal::DbExecutor {
public:
    explicit ScriptedExecutor(std::vector<ScriptStep> steps)
        : steps_(std::move(steps)) {}

    metal::QueryResult execute(
        const std::string& sql,
        const std::vector<metal::Value>& params = {}) override {
        assert(cursor_ < steps_.size());
        const auto& step = steps_[cursor_++];
        assert(sql.find(step.sql_marker) != std::string::npos);
        assert(params.size() == 1);
        assert(metal::from_value<std::string>(params.front()) == "audit");
        return step.result;
    }

    [[nodiscard]] std::size_t calls() const noexcept { return cursor_; }

private:
    std::vector<ScriptStep> steps_;
    std::size_t cursor_{};
};

const metal::DatabaseTable& table_named(
    const metal::DatabaseSchema& schema,
    const std::string& name) {
    const auto found = std::find_if(
        schema.tables.begin(), schema.tables.end(),
        [&](const metal::DatabaseTable& table) { return table.name == name; });
    assert(found != schema.tables.end());
    return *found;
}

const metal::DatabaseColumn& column_named(
    const metal::DatabaseTable& table,
    const std::string& name) {
    const auto found = std::find_if(
        table.columns.begin(), table.columns.end(),
        [&](const metal::DatabaseColumn& column) { return column.name == name; });
    assert(found != table.columns.end());
    return *found;
}

void test_schema_introspection() {
    ScriptedExecutor executor{{
        {
            "pg_catalog.pg_attrdef",
            metal::QueryResult{.rows = {
                metal::Row{
                    {"table_name", std::string{"users"}},
                    {"column_name", std::string{"id"}},
                    {"data_type", std::string{"bigint"}},
                    {"not_null", true},
                    {"column_default", nullptr},
                    {"ordinal_position", std::int64_t{1}},
                    {"is_identity", true},
                    {"comment", std::string{" Identifier "}}
                },
                metal::Row{
                    {"table_name", std::string{"users"}},
                    {"column_name", std::string{"email"}},
                    {"data_type", std::string{"text"}},
                    {"not_null", true},
                    {"column_default", nullptr},
                    {"ordinal_position", std::int64_t{2}},
                    {"is_identity", false},
                    {"comment", std::string{" Email address "}}
                },
                metal::Row{
                    {"table_name", std::string{"users"}},
                    {"column_name", std::string{"owner_id"}},
                    {"data_type", std::string{"bigint"}},
                    {"not_null", false},
                    {"column_default", nullptr},
                    {"ordinal_position", std::int64_t{3}},
                    {"is_identity", false},
                    {"comment", nullptr}
                },
                metal::Row{
                    {"table_name", std::string{"users"}},
                    {"column_name", std::string{"age"}},
                    {"data_type", std::string{"integer"}},
                    {"not_null", false},
                    {"column_default", std::string{"0"}},
                    {"ordinal_position", std::int64_t{4}},
                    {"is_identity", false},
                    {"comment", nullptr}
                },
                metal::Row{
                    {"table_name", std::string{"ignored"}},
                    {"column_name", std::string{"id"}},
                    {"data_type", std::string{"bigint"}},
                    {"not_null", true},
                    {"column_default", nullptr},
                    {"ordinal_position", std::int64_t{1}},
                    {"is_identity", false},
                    {"comment", nullptr}
                }
            }}
        },
        {
            "obj_description",
            metal::QueryResult{.rows = {
                metal::Row{
                    {"table_name", std::string{"users"}},
                    {"comment", std::string{" Users table "}}
                }
            }}
        },
        {
            "con.contype = 'p'",
            metal::QueryResult{.rows = {
                metal::Row{
                    {"table_name", std::string{"users"}},
                    {"column_name", std::string{"id"}},
                    {"ordinal_position", std::int64_t{1}}
                }
            }}
        },
        {
            "con.contype = 'u'",
            metal::QueryResult{.rows = {
                metal::Row{
                    {"table_name", std::string{"users"}},
                    {"constraint_name", std::string{"users_email_key"}},
                    {"column_name", std::string{"email"}},
                    {"column_count", std::int64_t{1}}
                }
            }}
        },
        {
            "con.contype = 'f'",
            metal::QueryResult{.rows = {
                metal::Row{
                    {"table_name", std::string{"users"}},
                    {"constraint_name", std::string{"users_owner_id_fkey"}},
                    {"column_name", std::string{"owner_id"}},
                    {"target_schema", std::string{"audit"}},
                    {"target_table", std::string{"accounts"}},
                    {"target_column", std::string{"id"}},
                    {"on_delete", std::string{"CASCADE"}},
                    {"on_update", std::string{"NO ACTION"}},
                    {"deferrable", true}
                }
            }}
        },
        {
            "con.contype = 'c'",
            metal::QueryResult{.rows = {
                metal::Row{
                    {"table_name", std::string{"users"}},
                    {"constraint_name", std::string{"users_age_check"}},
                    {"expression", std::string{" (age >= 0) "}},
                    {"column_count", std::int64_t{1}},
                    {"column_name", std::string{"age"}}
                },
                metal::Row{
                    {"table_name", std::string{"users"}},
                    {"constraint_name", std::string{"users_email_nonempty"}},
                    {"expression", std::string{" (length(email) > 0) "}},
                    {"column_count", std::int64_t{1}},
                    {"column_name", std::string{"email"}}
                }
            }}
        },
        {
            "pg_catalog.pg_index",
            metal::QueryResult{.rows = {
                metal::Row{
                    {"table_name", std::string{"users"}},
                    {"index_name", std::string{"users_email_partial_idx"}},
                    {"is_unique", false},
                    {"predicate", std::string{" email IS NOT NULL "}},
                    {"column_name", std::string{"email"}},
                    {"ordinal_position", std::int64_t{1}}
                }
            }}
        },
        {
            "pg_get_viewdef",
            metal::QueryResult{.rows = {
                metal::Row{
                    {"view_name", std::string{"active_users"}},
                    {"definition", std::string{" SELECT id, email FROM users WHERE age >= 18 "}},
                    {"comment", std::string{" Active users "}}
                }
            }}
        },
        {
            "cls.relkind = 'v'",
            metal::QueryResult{.rows = {
                metal::Row{
                    {"view_name", std::string{"active_users"}},
                    {"column_name", std::string{"id"}},
                    {"data_type", std::string{"bigint"}},
                    {"not_null", false},
                    {"comment", std::string{" User identifier "}}
                },
                metal::Row{
                    {"view_name", std::string{"active_users"}},
                    {"column_name", std::string{"email"}},
                    {"data_type", std::string{"text"}},
                    {"not_null", false},
                    {"comment", nullptr}
                }
            }}
        }
    }};

    const auto schema = metal::introspect_postgres(
        executor,
        metal::IntrospectOptions{
            .schema = std::string{"audit"},
            .exclude_tables = {"ignored"},
            .include_views = true
        });

    assert(executor.calls() == 9);
    assert(schema.tables.size() == 1);
    const auto& users = table_named(schema, "users");
    assert(users.primary_key == std::vector<std::string>{"id"});
    assert(users.comment == std::optional<std::string>{"Users table"});

    const auto& id = column_named(users, "id");
    assert(id.type == "bigint");
    assert(id.not_null);
    assert(id.auto_increment);
    assert(id.comment == std::optional<std::string>{"Identifier"});

    const auto& email = column_named(users, "email");
    assert(email.unique);
    assert(!email.unique_name);
    assert(email.comment == std::optional<std::string>{"Email address"});

    const auto& owner_id = column_named(users, "owner_id");
    assert(owner_id.references);
    assert(owner_id.references->table == "accounts");
    assert(owner_id.references->column == "id");
    assert(!owner_id.references->name);
    assert(owner_id.references->on_delete == std::optional<std::string>{"CASCADE"});
    assert(owner_id.references->on_update == std::optional<std::string>{"NO ACTION"});
    assert(owner_id.references->deferrable);

    const auto& age = column_named(users, "age");
    assert(age.default_value == std::optional<std::string>{"0"});
    assert(age.check == std::optional<std::string>{"(age >= 0)"});

    assert(users.checks.size() == 1);
    assert(users.checks.front().name == std::optional<std::string>{"users_email_nonempty"});
    assert(users.checks.front().expression == "(length(email) > 0)");

    assert(users.indexes.size() == 1);
    assert(users.indexes.front().name == "users_email_partial_idx");
    assert(users.indexes.front().columns.size() == 1);
    assert(users.indexes.front().columns.front().column == "email");
    assert(users.indexes.front().where == std::optional<std::string>{"email IS NOT NULL"});

    assert(schema.views.size() == 1);
    assert(schema.views.front().name == "active_users");
    assert(schema.views.front().definition ==
           std::optional<std::string>{"SELECT id, email FROM users WHERE age >= 18"});
    assert(schema.views.front().comment == std::optional<std::string>{"Active users"});
    assert(schema.views.front().columns.size() == 2);
    assert(schema.views.front().columns.front().comment ==
           std::optional<std::string>{"User identifier"});
}

void test_unreachable_connection() {
    bool failed = false;
    try {
        metal::PostgresExecutor executor{
            "host=127.0.0.1 port=1 dbname=metal_orm_unreachable connect_timeout=1"};
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        failed = message.find("MetalORM: PostgreSQL connection failed:") != std::string::npos;
    }
    assert(failed);
}

} // namespace

int main() {
    test_schema_introspection();
    test_unreachable_connection();
}
