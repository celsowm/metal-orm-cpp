#include <metal/metal.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct [[=metal::mapping::table{"actual_users"}]] SchemaUser {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;
    std::optional<std::int64_t> age;
};

struct [[=metal::mapping::table{"actual_posts"}]] SchemaPost {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::reference<
        ^^SchemaUser::id,
        metal::mapping::referential_action::cascade>{}]]
    std::optional<std::int64_t> user_id;

    std::string title;

    [[=metal::mapping::belongs_to<^^SchemaPost::user_id>{}]]
    metal::belongs_to_reference<SchemaUser> user;
};

struct [[=metal::mapping::table{"audit_log"}]] SchemaAudit {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::reference<
        ^^SchemaUser::id,
        metal::mapping::referential_action::set_null,
        metal::mapping::referential_action::cascade>{}]]
    std::optional<std::int64_t> user_id;

    std::string message;
};

struct [[=metal::mapping::table{"mismatch"}]] SchemaMismatch {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::int64_t age{};
};

static const metal::DatabaseTable& table_named(
    const metal::DatabaseSchema& schema,
    const std::string& name) {
    const auto found = std::find_if(
        schema.tables.begin(), schema.tables.end(),
        [&](const metal::DatabaseTable& table) { return table.name == name; });
    assert(found != schema.tables.end());
    return *found;
}

static const metal::DatabaseColumn& column_named(
    const metal::DatabaseTable& table,
    const std::string& name) {
    const auto found = std::find_if(
        table.columns.begin(), table.columns.end(),
        [&](const metal::DatabaseColumn& column) { return column.name == name; });
    assert(found != table.columns.end());
    return *found;
}

static const metal::DatabaseIndex& index_named(
    const metal::DatabaseTable& table,
    const std::string& name) {
    const auto found = std::find_if(
        table.indexes.begin(), table.indexes.end(),
        [&](const metal::DatabaseIndex& index) { return index.name == name; });
    assert(found != table.indexes.end());
    return *found;
}

static bool has_change(const metal::SchemaPlan& plan, metal::SchemaChangeKind kind) {
    return std::any_of(
        plan.changes.begin(), plan.changes.end(),
        [&](const metal::SchemaChange& change) { return change.kind == kind; });
}

int main() {
    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    metal::SQLiteDialect dialect;

    db->execute(
        "CREATE TABLE actual_users ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "name TEXT NOT NULL"
        ");");
    db->execute(
        "CREATE TABLE actual_posts ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "user_id INTEGER REFERENCES actual_users(id) ON DELETE CASCADE, "
        "title TEXT NOT NULL"
        ");");
    db->execute("CREATE INDEX old_posts_title_idx ON actual_posts(title);");
    db->execute("CREATE TABLE extra_table (id INTEGER PRIMARY KEY);");
    db->execute(
        "CREATE TABLE mismatch ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "age TEXT NOT NULL, "
        "legacy TEXT"
        ");");
    db->execute("CREATE VIEW user_names AS SELECT id, name FROM actual_users;");
    db->execute(
        "CREATE TABLE schema_comments ("
        "object_type TEXT NOT NULL, table_name TEXT NOT NULL, "
        "column_name TEXT, comment TEXT NOT NULL"
        ");");
    db->execute(
        "INSERT INTO schema_comments(object_type, table_name, column_name, comment) VALUES "
        "('table', 'actual_users', NULL, 'User table'),"
        "('column', 'actual_users', 'name', 'Display name');");

    metal::IntrospectOptions inspect_options{
        .exclude_tables = {"schema_comments", "mismatch"},
        .include_views = true
    };
    const auto inspected = metal::introspect_sqlite(*db, inspect_options);

    const auto& users = table_named(inspected, "actual_users");
    const auto& user_id = column_named(users, "id");
    const auto& user_name = column_named(users, "name");
    assert(user_id.not_null);
    assert(user_id.auto_increment);
    assert(user_name.not_null);
    assert(users.primary_key == std::vector<std::string>{"id"});
    assert(users.comment && *users.comment == "User table");
    assert(user_name.comment && *user_name.comment == "Display name");

    const auto& posts = table_named(inspected, "actual_posts");
    const auto& post_user_id = column_named(posts, "user_id");
    assert(post_user_id.references);
    assert(post_user_id.references->table == "actual_users");
    assert(post_user_id.references->column == "id");
    assert(post_user_id.references->on_delete == std::optional<std::string>{"CASCADE"});
    assert(posts.indexes.size() == 1);
    assert(posts.indexes.front().name == "old_posts_title_idx");
    assert(posts.indexes.front().columns.size() == 1);
    assert(posts.indexes.front().columns.front().column == "title");

    assert(inspected.views.size() == 1);
    assert(inspected.views.front().name == "user_names");
    assert(inspected.views.front().definition);

    auto expected = metal::expected_schema<SchemaUser, SchemaPost, SchemaAudit>(dialect);
    metal::add_expected_index<SchemaUser, ^^SchemaUser::name>(
        expected, dialect, "actual_users_name_idx");
    metal::add_expected_index<SchemaUser, ^^SchemaUser::age>(
        expected,
        dialect,
        "actual_users_age_present_idx",
        false,
        std::string{"age IS NOT NULL"});

    const auto& expected_users = expected.tables.front().table;
    assert(expected_users.name == "actual_users");
    const auto& expected_partial = index_named(expected_users, "actual_users_age_present_idx");
    assert(expected_partial.where == std::optional<std::string>{"age IS NOT NULL"});
    assert(std::any_of(
        expected.tables.front().create_index_sql.begin(),
        expected.tables.front().create_index_sql.end(),
        [](const std::string& sql) {
            return sql.find("WHERE age IS NOT NULL") != std::string::npos;
        }));

    const auto& expected_posts = expected.tables[1].table;
    const auto& expected_post_user = column_named(expected_posts, "user_id");
    assert(expected_post_user.references);
    assert(expected_post_user.references->table == "actual_users");
    assert(expected_post_user.references->column == "id");
    assert(expected_post_user.references->on_delete == std::optional<std::string>{"CASCADE"});
    assert(!expected_post_user.references->on_update);

    const auto& expected_audit = expected.tables[2];
    assert(expected_audit.create_table_sql.find(
        "REFERENCES \"actual_users\" (\"id\") ON DELETE SET NULL ON UPDATE CASCADE") !=
        std::string::npos);

    metal::IntrospectOptions sync_introspection{
        .exclude_tables = {"schema_comments", "mismatch"}
    };
    const auto actual_before = metal::introspect_sqlite(*db, sync_introspection);
    const auto plan = metal::diff_schema(expected, actual_before, dialect);
    assert(has_change(plan, metal::SchemaChangeKind::AddColumn));
    assert(has_change(plan, metal::SchemaChangeKind::AddIndex));
    assert(has_change(plan, metal::SchemaChangeKind::CreateTable));
    assert(has_change(plan, metal::SchemaChangeKind::DropIndex));
    assert(has_change(plan, metal::SchemaChangeKind::DropTable));

    const auto dry = metal::synchronize_schema(
        expected,
        *db,
        dialect,
        metal::SynchronizeOptions{.allow_destructive = true, .dry_run = true},
        sync_introspection);
    assert(!dry.changes.empty());
    auto still_no_age = db->execute(
        "SELECT COUNT(*) AS c FROM pragma_table_info('actual_users') WHERE name='age';");
    assert(metal::from_value<std::int64_t>(still_no_age.rows.front().at("c")) == 0);

    const auto safe_plan = metal::synchronize_schema(
        expected,
        *db,
        dialect,
        metal::SynchronizeOptions{.allow_destructive = false},
        sync_introspection);
    assert(!safe_plan.changes.empty());

    auto age_exists = db->execute(
        "SELECT COUNT(*) AS c FROM pragma_table_info('actual_users') WHERE name='age';");
    assert(metal::from_value<std::int64_t>(age_exists.rows.front().at("c")) == 1);
    auto audit_exists = db->execute(
        "SELECT COUNT(*) AS c FROM sqlite_master WHERE type='table' AND name='audit_log';");
    assert(metal::from_value<std::int64_t>(audit_exists.rows.front().at("c")) == 1);
    auto expected_index_exists = db->execute(
        "SELECT COUNT(*) AS c FROM sqlite_master WHERE type='index' AND name='actual_users_name_idx';");
    assert(metal::from_value<std::int64_t>(expected_index_exists.rows.front().at("c")) == 1);
    auto partial_index_sql = db->execute(
        "SELECT sql FROM sqlite_master WHERE type='index' AND name='actual_users_age_present_idx';");
    assert(partial_index_sql.rows.size() == 1);
    const auto stored_partial_sql =
        metal::from_value<std::string>(partial_index_sql.rows.front().at("sql"));
    assert(stored_partial_sql.find("WHERE age IS NOT NULL") != std::string::npos);
    auto old_index_exists = db->execute(
        "SELECT COUNT(*) AS c FROM sqlite_master WHERE type='index' AND name='old_posts_title_idx';");
    assert(metal::from_value<std::int64_t>(old_index_exists.rows.front().at("c")) == 1);
    auto extra_exists = db->execute(
        "SELECT COUNT(*) AS c FROM sqlite_master WHERE type='table' AND name='extra_table';");
    assert(metal::from_value<std::int64_t>(extra_exists.rows.front().at("c")) == 1);

    const auto synchronized = metal::introspect_sqlite(*db, sync_introspection);
    const auto& synchronized_users = table_named(synchronized, "actual_users");
    const auto& synchronized_partial = index_named(
        synchronized_users, "actual_users_age_present_idx");
    assert(synchronized_partial.where == std::optional<std::string>{"age IS NOT NULL"});

    const auto& synchronized_audit = table_named(synchronized, "audit_log");
    const auto& synchronized_audit_user = column_named(synchronized_audit, "user_id");
    assert(synchronized_audit_user.references);
    assert(synchronized_audit_user.references->table == "actual_users");
    assert(synchronized_audit_user.references->column == "id");
    assert(synchronized_audit_user.references->on_delete ==
           std::optional<std::string>{"SET NULL"});
    assert(synchronized_audit_user.references->on_update ==
           std::optional<std::string>{"CASCADE"});

    const auto destructive_plan = metal::synchronize_schema(
        expected,
        *db,
        dialect,
        metal::SynchronizeOptions{.allow_destructive = true},
        sync_introspection);
    assert(has_change(destructive_plan, metal::SchemaChangeKind::DropIndex));
    assert(has_change(destructive_plan, metal::SchemaChangeKind::DropTable));

    old_index_exists = db->execute(
        "SELECT COUNT(*) AS c FROM sqlite_master WHERE type='index' AND name='old_posts_title_idx';");
    assert(metal::from_value<std::int64_t>(old_index_exists.rows.front().at("c")) == 0);
    extra_exists = db->execute(
        "SELECT COUNT(*) AS c FROM sqlite_master WHERE type='table' AND name='extra_table';");
    assert(metal::from_value<std::int64_t>(extra_exists.rows.front().at("c")) == 0);

    const auto final_schema = metal::introspect_sqlite(*db, sync_introspection);
    const auto final_plan = metal::diff_schema(expected, final_schema, dialect);
    assert(final_plan.changes.empty());
    assert(final_plan.warnings.empty());

    const auto mismatch_actual = metal::introspect_sqlite(
        *db,
        metal::IntrospectOptions{.include_tables = {"mismatch"}});
    const auto mismatch_expected = metal::expected_schema<SchemaMismatch>(dialect);
    const auto mismatch_plan = metal::diff_schema(mismatch_expected, mismatch_actual, dialect);
    assert(!has_change(mismatch_plan, metal::SchemaChangeKind::AlterColumn));
    assert(has_change(mismatch_plan, metal::SchemaChangeKind::DropColumn));
    assert(std::any_of(
        mismatch_plan.warnings.begin(), mismatch_plan.warnings.end(),
        [](const std::string& warning) {
            return warning.find("SQLite ALTER COLUMN is not supported") != std::string::npos;
        }));
}
