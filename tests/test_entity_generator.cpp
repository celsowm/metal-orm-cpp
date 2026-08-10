#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>

struct [[=metal::mapping::table{"declared_types"}]] DeclaredType {
    [[=metal::mapping::primary_key, =metal::mapping::database_type{"BIGINT"}]]
    std::int64_t id{};

    [[=metal::mapping::database_type{"VARCHAR(80)"}]]
    std::string name;
};

struct GeneratedReferenceTarget;

struct [[=metal::mapping::table{"generated_reference_sources"}]] GeneratedReferenceSource {
    [[=metal::mapping::primary_key]]
    std::int64_t id{};

    [[=metal::mapping::reference_to<
        ^^GeneratedReferenceTarget,
        "id",
        metal::mapping::referential_action::cascade>{}]]
    std::optional<std::int64_t> target_id;
};

struct [[=metal::mapping::table{"generated_reference_targets"}]] GeneratedReferenceTarget {
    [[=metal::mapping::primary_key]]
    std::int64_t id{};
};

static_assert(metal::reflect::validate_mapping<DeclaredType>());
static_assert(metal::reflect::validate_mapping<GeneratedReferenceSource>());
static_assert(metal::reflect::validate_mapping<GeneratedReferenceTarget>());
static_assert(metal::reflect::validate_physical_references<GeneratedReferenceSource>());

static bool contains(const std::string& value, const std::string& needle) {
    return value.find(needle) != std::string::npos;
}

int main() {
    {
        metal::DatabaseSchema schema;

        metal::DatabaseTable users;
        users.name = "users";
        users.comment = "Application users";
        users.primary_key = {"id"};
        users.columns = {
            metal::DatabaseColumn{
                .name = "id",
                .type = "INTEGER",
                .not_null = true,
                .auto_increment = true
            },
            metal::DatabaseColumn{
                .name = "display-name",
                .type = "VARCHAR(80)",
                .not_null = true,
                .default_value = "'guest'",
                .check = "length(\"display-name\") > 0",
                .comment = "Public display name"
            },
            metal::DatabaseColumn{
                .name = "enabled",
                .type = "BOOLEAN",
                .not_null = true,
                .default_value = "1"
            },
            metal::DatabaseColumn{
                .name = "bio",
                .type = "TEXT",
                .not_null = false,
                .default_value = "NULL"
            },
            metal::DatabaseColumn{
                .name = "required_note",
                .type = "TEXT",
                .not_null = true,
                .default_value = "NULL"
            },
            metal::DatabaseColumn{
                .name = "avatar",
                .type = "BLOB",
                .not_null = false
            }
        };
        users.checks.push_back(metal::DatabaseCheck{
            .name = "user_enabled",
            .expression = "enabled IN (0, 1)"
        });
        schema.tables.push_back(users);

        metal::DatabaseTable posts;
        posts.name = "posts";
        posts.primary_key = {"id"};
        posts.columns = {
            metal::DatabaseColumn{.name = "id", .type = "INTEGER", .not_null = true},
            metal::DatabaseColumn{
                .name = "user_id",
                .type = "INTEGER",
                .not_null = true,
                .references = metal::ForeignKeyReference{
                    .table = "users",
                    .column = "id",
                    .on_delete = "CASCADE",
                    .on_update = "RESTRICT"
                }
            },
            metal::DatabaseColumn{
                .name = "title",
                .type = "TEXT",
                .not_null = true,
                .check = "length(title) > 0"
            }
        };
        schema.tables.push_back(posts);

        metal::DatabaseTable status;
        status.name = "status";
        status.primary_key = {"id"};
        status.columns = {
            metal::DatabaseColumn{.name = "id", .type = "INTEGER", .not_null = true}
        };
        schema.tables.push_back(status);

        schema.views.push_back(metal::DatabaseView{.name = "active_users"});

        const auto generated = metal::generate_entity_header(
            schema,
            metal::EntityGeneratorOptions{.namespace_name = "app_model"});

        assert(contains(generated.code, "namespace app_model {"));
        assert(contains(generated.code, "=metal::mapping::table{\"users\"}"));
        assert(contains(generated.code, "=metal::mapping::named_table_check<\"user_enabled\", \"enabled IN (0, 1)\">{}"));
        assert(contains(generated.code, "struct [[=metal::mapping::table{\"status\"}]] Status"));
        assert(contains(generated.code, "=metal::mapping::generated"));
        assert(contains(generated.code, "=metal::mapping::column{\"display-name\"}"));
        assert(contains(generated.code, "=metal::mapping::database_type{\"VARCHAR(80)\"}"));
        assert(contains(generated.code, "=metal::mapping::default_text{\"guest\"}"));
        assert(contains(generated.code, "=metal::mapping::default_value{true}"));
        assert(contains(generated.code, "=metal::mapping::default_null"));
        assert(contains(generated.code, "=metal::mapping::default_sql{\"NULL\"}"));
        assert(contains(generated.code,
            "=metal::mapping::check<\"length(\\\"display-name\\\") > 0\">{}"));
        assert(contains(generated.code,
            "=metal::mapping::check<\"length(title) > 0\">{}"));
        assert(contains(generated.code,
            "=metal::mapping::reference_to<^^User, \"id\", metal::mapping::referential_action::cascade, metal::mapping::referential_action::restrict>{}"));
        assert(contains(generated.code, "std::optional<std::string> bio;"));
        assert(contains(generated.code, "std::optional<metal::Blob> avatar;"));
        assert(contains(generated.code, "[[=metal::mapping::belongs_to<^^Post::user_id>{}]]"));
        assert(contains(generated.code, "metal::belongs_to_reference<User> user;"));
        assert(contains(generated.code, "/// Application users"));
        assert(contains(generated.code, "/// Public display name"));
        assert(generated.warnings.size() == 1);
        assert(contains(generated.warnings.front(), "View"));
    }

    {
        metal::SQLiteExecutor db{":memory:"};
        db.execute(
            "CREATE TABLE parents ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "label VARCHAR(40) NOT NULL DEFAULT 'root' CHECK (length(label) > 0), "
            "CONSTRAINT label_shape CHECK (instr(label, ',)') >= 0)"
            ");");
        db.execute(
            "CREATE TABLE children ("
            "id INTEGER PRIMARY KEY, "
            "parent_id INTEGER NOT NULL REFERENCES parents(id) ON DELETE CASCADE, "
            "enabled BOOLEAN NOT NULL DEFAULT 1 CHECK (enabled IN (0, 1)), "
            "note TEXT DEFAULT NULL"
            ");");

        const auto generated = metal::generate_sqlite_entity_header(db);
        assert(generated.warnings.empty());
        assert(contains(generated.code, "=metal::mapping::table{\"parents\"}"));
        assert(contains(generated.code,
            "=metal::mapping::named_table_check<\"label_shape\", \"instr(label, ',)') >= 0\">{}"));
        assert(contains(generated.code, "=metal::mapping::table{\"children\"}"));
        assert(contains(generated.code, "=metal::mapping::database_type{\"VARCHAR(40)\"}"));
        assert(contains(generated.code, "=metal::mapping::default_text{\"root\"}"));
        assert(contains(generated.code, "=metal::mapping::default_value{true}"));
        assert(contains(generated.code, "=metal::mapping::default_null"));
        assert(contains(generated.code,
            "=metal::mapping::check<\"length(label) > 0\">{}"));
        assert(contains(generated.code,
            "=metal::mapping::check<\"enabled IN (0, 1)\">{}"));
        assert(contains(generated.code,
            "=metal::mapping::reference_to<^^Parent, \"id\", metal::mapping::referential_action::cascade, metal::mapping::referential_action::no_action>{}"));
        assert(contains(generated.code, "metal::belongs_to_reference<Parent> parent;"));
    }

    {
        metal::SQLiteDialect dialect;
        const auto sql = metal::create_table_sql<DeclaredType>(dialect);
        assert(contains(sql, "\"id\" BIGINT"));
        assert(contains(sql, "\"name\" VARCHAR(80)"));

        const auto expected = metal::expected_table<DeclaredType>(dialect);
        assert(expected.table.columns.size() == 2);
        assert(expected.table.columns[0].type == "BIGINT");
        assert(expected.table.columns[1].type == "VARCHAR(80)");
    }

    {
        metal::SQLiteDialect dialect;
        const auto expected = metal::expected_table<GeneratedReferenceSource>(dialect);
        assert(expected.table.columns[1].references);
        assert(expected.table.columns[1].references->table == "generated_reference_targets");
        assert(expected.table.columns[1].references->column == "id");
        assert(expected.table.columns[1].references->on_delete ==
               std::optional<std::string>{"CASCADE"});
    }
}
