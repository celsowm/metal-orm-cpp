#include <metal/query/core_types.hpp>
#include <metal/schema_diff.hpp>
#include <metal/schema_expected.hpp>
#include <metal/schema_introspection_dispatch.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

struct [[=metal::mapping::table{"postgres_type_probe"}]] PostgresTypeProbe {
    [[=metal::mapping::primary_key]]
    std::int64_t id{};
    bool active{};
    double score{};
    metal::Blob payload;
};

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

bool plan_has_statement(const metal::SchemaPlan& plan, const std::string& text) {
    for (const auto& change : plan.changes) {
        for (const auto& statement : change.statements) {
            if (statement.find(text) != std::string::npos) return true;
        }
    }
    return false;
}

bool plan_has_warning(const metal::SchemaPlan& plan, const std::string& text) {
    for (const auto& warning : plan.warnings) {
        if (warning.find(text) != std::string::npos) return true;
    }
    return false;
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
                    {"constraint_name", std::string{"users_identity_pk"}},
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
                    {"target_schema", std::string{"identity"}},
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
                    {"constraint_name", std::string{"users_business_rule"}},
                    {"expression", std::string{"owner_id > 0 OR email IS NOT NULL"}},
                    {"column_count", std::int64_t{2}},
                    {"column_name", nullptr}
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
        }
    }};

    const metal::PostgresDialect dialect;

    const auto expected = metal::expected_schema<PostgresTypeProbe>(dialect);
    assert(expected.tables.size() == 1);
    const auto& expected_types = expected.tables.front().table;
    assert(column_named(expected_types, "id").type == "BIGINT");
    assert(column_named(expected_types, "active").type == "BOOLEAN");
    assert(column_named(expected_types, "score").type == "DOUBLE PRECISION");
    assert(column_named(expected_types, "payload").type == "BYTEA");

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
    assert(users.primary_key_name == std::optional<std::string>{"users_identity_pk"});
    assert(users.comment == std::optional<std::string>{"Users table"});
    assert(users.checks.size() == 1);
    assert(users.checks.front().name == std::optional<std::string>{"users_business_rule"});
    assert(users.checks.front().expression == "owner_id > 0 OR email IS NOT NULL");

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
    assert(owner.references->schema == std::optional<std::string>{"identity"});

    const auto rendered_reference =
        metal::schema_detail::render_reference_definition(*owner.references, dialect);
    assert(rendered_reference.find("REFERENCES \"identity\".\"accounts\" (\"id\")") !=
           std::string::npos);
    assert(rendered_reference.find("\"identity.accounts\"") == std::string::npos);

    assert(users.indexes.size() == 1);
    assert(users.indexes.front().name == "users_email_partial_idx");
    assert(users.indexes.front().columns.size() == 1);
    assert(users.indexes.front().columns.front().column == "email");
    assert(users.indexes.front().where ==
           std::optional<std::string>{"email IS NOT NULL"});

    metal::ExpectedSchema desired;
    metal::ExpectedTable desired_users;
    desired_users.table.name = "planner_users";
    desired_users.table.columns = {
        metal::DatabaseColumn{
            .name = "id",
            .type = "BIGINT",
            .not_null = true,
            .auto_increment = true
        },
        metal::DatabaseColumn{
            .name = "email",
            .type = "TEXT",
            .not_null = true,
            .unique = true,
            .unique_name = std::string{"uq_planner_users_email"},
            .default_value = std::string{"'unknown'"},
            .check = std::string{"length(email) > 0"}
        },
        metal::DatabaseColumn{
            .name = "active",
            .type = "BOOLEAN",
            .not_null = true,
            .default_value = std::string{"false"}
        },
        metal::DatabaseColumn{
            .name = "owner_id",
            .type = "BIGINT",
            .references = metal::ForeignKeyReference{
                .table = "accounts",
                .column = "id",
                .on_delete = std::string{"CASCADE"},
                .schema = std::string{"identity"}
            }
        }
    };
    desired_users.table.primary_key = {"id", "email"};
    desired_users.table.checks = {
        metal::DatabaseCheck{
            .name = std::string{"planner_users_active_guard"},
            .expression = "active IN (true, false)"
        },
        metal::DatabaseCheck{
            .name = std::nullopt,
            .expression = "length(email) <= 320"
        }
    };
    desired_users.table.indexes = {
        metal::DatabaseIndex{
            .name = "planner_users_email_idx",
            .columns = {metal::DatabaseIndexColumn{"email"}}
        },
        metal::DatabaseIndex{
            .name = "planner_users_active_idx",
            .columns = {metal::DatabaseIndexColumn{"active"}},
            .where = std::string{"active = true"}
        }
    };
    desired.tables.push_back(std::move(desired_users));

    metal::DatabaseSchema current;
    metal::DatabaseTable current_users;
    current_users.name = "planner_users";
    current_users.columns = {
        metal::DatabaseColumn{
            .name = "id",
            .type = "INTEGER",
            .not_null = true,
            .auto_increment = false
        },
        metal::DatabaseColumn{
            .name = "email",
            .type = "TEXT",
            .not_null = false,
            .unique = true,
            .default_value = std::nullopt,
            .check = std::string{"length(email) > 1"}
        },
        metal::DatabaseColumn{
            .name = "owner_id",
            .type = "BIGINT"
        },
        metal::DatabaseColumn{
            .name = "legacy",
            .type = "TEXT"
        }
    };
    current_users.primary_key = {"id"};
    current_users.primary_key_name = std::string{"planner_users_legacy_pk"};
    current_users.checks = {
        metal::DatabaseCheck{
            .name = std::string{"planner_users_active_guard"},
            .expression = "active = true"
        },
        metal::DatabaseCheck{
            .name = std::string{"planner_users_email_length"},
            .expression = "length(email) <= 320"
        },
        metal::DatabaseCheck{
            .name = std::string{"planner_users_obsolete_check"},
            .expression = "legacy IS NOT NULL"
        }
    };
    current_users.indexes = {
        metal::DatabaseIndex{
            .name = "planner_users_email_idx",
            .columns = {metal::DatabaseIndexColumn{"legacy"}}
        },
        metal::DatabaseIndex{
            .name = "planner_users_old_idx",
            .columns = {metal::DatabaseIndexColumn{"legacy"}}
        }
    };
    current.tables.push_back(std::move(current_users));

    const auto safe_plan = metal::diff_schema(desired, current, dialect);
    assert(plan_has_statement(
        safe_plan,
        "ALTER TABLE \"planner_users\" ALTER COLUMN \"id\" TYPE BIGINT;"));
    assert(plan_has_statement(
        safe_plan,
        "ALTER TABLE \"planner_users\" ALTER COLUMN \"id\" ADD GENERATED BY DEFAULT AS IDENTITY;"));
    assert(plan_has_statement(
        safe_plan,
        "ALTER TABLE \"planner_users\" ALTER COLUMN \"email\" SET DEFAULT 'unknown';"));
    assert(plan_has_statement(
        safe_plan,
        "ALTER TABLE \"planner_users\" ALTER COLUMN \"email\" SET NOT NULL;"));
    assert(plan_has_statement(
        safe_plan,
        "ALTER TABLE \"planner_users\" ADD \"active\" BOOLEAN NOT NULL DEFAULT false;"));
    assert(plan_has_statement(
        safe_plan,
        "RENAME CONSTRAINT \"planner_users_email_key\" TO \"uq_planner_users_email\";"));
    assert(plan_has_statement(
        safe_plan,
        "DROP CONSTRAINT \"planner_users_email_check\";"));
    assert(plan_has_statement(
        safe_plan,
        "ADD CONSTRAINT \"planner_users_email_check\" CHECK (length(email) > 0);"));
    assert(plan_has_statement(
        safe_plan,
        "ADD CONSTRAINT \"planner_users_owner_id_fkey\" FOREIGN KEY (\"owner_id\") REFERENCES \"identity\".\"accounts\" (\"id\") ON DELETE CASCADE;"));
    assert(plan_has_statement(
        safe_plan,
        "DROP CONSTRAINT \"planner_users_active_guard\";"));
    assert(plan_has_statement(
        safe_plan,
        "ADD CONSTRAINT \"planner_users_active_guard\" CHECK (active IN (true, false));"));
    assert(!plan_has_statement(safe_plan, "planner_users_email_length"));
    assert(!plan_has_statement(safe_plan, "planner_users_legacy_pk"));
    assert(!plan_has_statement(safe_plan, "planner_users_obsolete_check"));
    assert(plan_has_statement(safe_plan, "DROP INDEX IF EXISTS \"planner_users_email_idx\";"));
    assert(plan_has_statement(
        safe_plan,
        "CREATE INDEX IF NOT EXISTS \"planner_users_email_idx\" ON \"planner_users\" (\"email\");"));
    assert(plan_has_statement(
        safe_plan,
        "CREATE INDEX IF NOT EXISTS \"planner_users_active_idx\" ON \"planner_users\" (\"active\") WHERE active = true;"));
    assert(!plan_has_statement(safe_plan, "DROP COLUMN \"legacy\""));
    assert(!plan_has_statement(safe_plan, "DROP INDEX IF EXISTS \"planner_users_old_idx\";"));
    assert(plan_has_warning(safe_plan, "identity properties"));

    const auto destructive_plan = metal::diff_schema(
        desired,
        current,
        dialect,
        metal::SchemaDiffOptions{.allow_destructive = true});
    assert(plan_has_statement(
        destructive_plan,
        "ALTER TABLE \"planner_users\" DROP CONSTRAINT \"planner_users_legacy_pk\";"));
    assert(plan_has_statement(
        destructive_plan,
        "ALTER TABLE \"planner_users\" ADD CONSTRAINT \"planner_users_pkey\" PRIMARY KEY (\"id\", \"email\");"));
    assert(plan_has_statement(
        destructive_plan,
        "ALTER TABLE \"planner_users\" DROP CONSTRAINT \"planner_users_obsolete_check\";"));
    assert(plan_has_statement(
        destructive_plan,
        "ALTER TABLE \"planner_users\" DROP COLUMN \"legacy\";"));
    assert(plan_has_statement(
        destructive_plan,
        "DROP INDEX IF EXISTS \"planner_users_old_idx\";"));
}
