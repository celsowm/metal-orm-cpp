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

static_assert(metal::reflect::validate_mapping<DeclaredType>());

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
                .name = "avatar",
                .type = "BLOB",
                .not_null = false
            }
        };
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
                .references = metal::ForeignKeyReference{.table = "users", .column = "id"}
            },
            metal::DatabaseColumn{.name = "title", .type = "TEXT", .not_null = true}
        };
        schema.tables.push_back(posts);
        schema.views.push_back(metal::DatabaseView{.name = "active_users"});

        const auto generated = metal::generate_entity_header(
            schema,
            metal::EntityGeneratorOptions{.namespace_name = "app_model"});

        assert(contains(generated.code, "namespace app_model {"));
        assert(contains(generated.code, "struct [[=metal::mapping::table{\"users\"}]] User"));
        assert(contains(generated.code, "=metal::mapping::generated"));
        assert(contains(generated.code, "=metal::mapping::column{\"display-name\"}"));
        assert(contains(generated.code, "=metal::mapping::database_type{\"VARCHAR(80)\"}"));
        assert(contains(generated.code, "=metal::mapping::default_text{\"guest\"}"));
        assert(contains(generated.code, "=metal::mapping::default_value{true}"));
        assert(contains(generated.code, "=metal::mapping::default_null"));
        assert(contains(generated.code, "std::optional<std::string> bio;"));
        assert(contains(generated.code, "[[=metal::mapping::belongs_to<^^Post::user_id>{}]]"));
        assert(contains(generated.code, "metal::belongs_to_reference<User> user;"));
        assert(contains(generated.code, "/// Application users"));
        assert(contains(generated.code, "/// Public display name"));
        assert(generated.warnings.size() == 2);
        assert(contains(generated.warnings[0], "BLOB") || contains(generated.warnings[1], "BLOB"));
        assert(contains(generated.warnings[0], "View") || contains(generated.warnings[1], "View"));
    }

    {
        metal::SQLiteExecutor db{":memory:"};
        db.execute(
            "CREATE TABLE parents ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "label VARCHAR(40) NOT NULL DEFAULT 'root'"
            ");");
        db.execute(
            "CREATE TABLE children ("
            "id INTEGER PRIMARY KEY, "
            "parent_id INTEGER NOT NULL REFERENCES parents(id), "
            "enabled BOOLEAN NOT NULL DEFAULT 1, "
            "note TEXT DEFAULT NULL"
            ");");

        const auto generated = metal::generate_sqlite_entity_header(db);
        assert(generated.warnings.empty());
        assert(contains(generated.code, "struct [[=metal::mapping::table{\"parents\"}]] Parent"));
        assert(contains(generated.code, "=metal::mapping::database_type{\"VARCHAR(40)\"}"));
        assert(contains(generated.code, "=metal::mapping::default_text{\"root\"}"));
        assert(contains(generated.code, "=metal::mapping::default_value{true}"));
        assert(contains(generated.code, "=metal::mapping::default_null"));
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
}
