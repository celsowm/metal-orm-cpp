#include <metal/query/core_types.hpp>
#include <metal/schema_introspection_dispatch.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

struct ScriptStep {
    std::string marker;
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
        assert(sql.find(step.marker) != std::string::npos);
        assert(params.size() == 1);
        assert(metal::from_value<std::string>(params.front()) == "audit");
        return step.result;
    }

    [[nodiscard]] std::size_t calls() const noexcept { return cursor_; }

private:
    std::vector<ScriptStep> steps_;
    std::size_t cursor_{};
};

const metal::DatabaseColumn& column_named(
    const metal::DatabaseTable& table,
    const std::string& name) {
    for (const auto& column : table.columns) {
        if (column.name == name) return column;
    }
    assert(false);
    return table.columns.front();
}

} // namespace

int main() {
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
                    {"comment", nullptr}
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
        {"con.contype = 'c'", metal::QueryResult{}},
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
        }
    }};

    const metal::PostgresDialect dialect;
    const auto schema = metal::introspect_schema(
        executor,
        dialect,
        metal::IntrospectOptions{
            .schema = std::string{"audit"},
            .include_views = false
        });

    assert(executor.calls() == 7);
    assert(schema.tables.size() == 1);
    assert(schema.views.empty());

    const auto& users = schema.tables.front();
    assert(users.name == "users");
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

    const auto& owner = column_named(users, "owner_id");
    assert(owner.references);
    assert(owner.references->table == "accounts");
    assert(owner.references->column == "id");
    assert(!owner.references->name);
    assert(owner.references->on_delete == std::optional<std::string>{"CASCADE"});
    assert(owner.references->on_update == std::optional<std::string>{"NO ACTION"});
    assert(owner.references->deferrable);

    assert(users.indexes.size() == 1);
    assert(users.indexes.front().name == "users_email_partial_idx");
    assert(users.indexes.front().columns.size() == 1);
    assert(users.indexes.front().columns.front().column == "email");
    assert(users.indexes.front().where ==
           std::optional<std::string>{"email IS NOT NULL"});
}
