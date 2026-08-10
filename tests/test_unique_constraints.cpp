#include <metal/metal.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>

struct [[=metal::mapping::table{"unique_users"}]] UniqueUser {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::named_unique<"uq_unique_users_email">{}]]
    std::string email;

    [[=metal::mapping::unique]]
    std::optional<std::string> handle;
};

struct [[=metal::mapping::table{"unique_add"}]] UniqueAdd {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::unique]]
    std::optional<std::string> code;
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

static bool contains(const std::string& value, const std::string& needle) {
    return value.find(needle) != std::string::npos;
}

int main() {
    static_assert(metal::reflect::validate_physical_uniques<UniqueUser>());

    metal::SQLiteDialect dialect;

    {
        metal::DatabaseTable parsed;
        parsed.name = "parser_unique";
        parsed.columns.push_back(metal::DatabaseColumn{.name = "email"});
        parsed.columns.push_back(metal::DatabaseColumn{.name = "handle"});
        parsed.columns.push_back(metal::DatabaseColumn{.name = "alias"});

        metal::schema_detail::parse_sqlite_unique_constraints(
            R"SQL(CREATE TABLE parser_unique (
                email TEXT CONSTRAINT "uq_email" UNIQUE,
                handle TEXT UNIQUE,
                alias TEXT,
                CONSTRAINT `uq_alias` UNIQUE(alias),
                CONSTRAINT ignored_composite UNIQUE(email, handle)
            );)SQL",
            parsed);

        const auto& email = column_named(parsed, "email");
        assert(email.unique);
        assert(email.unique_name == std::optional<std::string>{"uq_email"});

        const auto& handle = column_named(parsed, "handle");
        assert(handle.unique);
        assert(!handle.unique_name);

        const auto& alias = column_named(parsed, "alias");
        assert(alias.unique);
        assert(alias.unique_name == std::optional<std::string>{"uq_alias"});
    }

    const auto expected = metal::expected_schema<UniqueUser>(dialect);
    assert(expected.tables.size() == 1);
    const auto& expected_user = expected.tables.front();

    const auto& expected_email = column_named(expected_user.table, "email");
    assert(expected_email.unique);
    assert(expected_email.unique_name ==
           std::optional<std::string>{"uq_unique_users_email"});
    assert(contains(
        expected_user.create_table_sql,
        "\"email\" TEXT NOT NULL CONSTRAINT \"uq_unique_users_email\" UNIQUE"));

    const auto& expected_handle = column_named(expected_user.table, "handle");
    assert(expected_handle.unique);
    assert(!expected_handle.unique_name);
    assert(contains(expected_user.create_table_sql, "\"handle\" TEXT UNIQUE"));

    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    db->execute(expected_user.create_table_sql);

    const auto actual = metal::introspect_sqlite(
        *db,
        metal::IntrospectOptions{.include_tables = {"unique_users"}});
    const auto& user = table_named(actual, "unique_users");

    const auto& email = column_named(user, "email");
    assert(email.unique);
    assert(email.unique_name == std::optional<std::string>{"uq_unique_users_email"});

    const auto& handle = column_named(user, "handle");
    assert(handle.unique);
    assert(!handle.unique_name);

    const auto final_plan = metal::diff_schema(expected, actual, dialect);
    assert(final_plan.changes.empty());
    assert(final_plan.warnings.empty());

    db->execute("INSERT INTO unique_users(email, handle) VALUES ('one@example.test', 'one');");
    bool duplicate_rejected = false;
    try {
        db->execute("INSERT INTO unique_users(email, handle) VALUES ('one@example.test', 'two');");
    } catch (const std::exception&) {
        duplicate_rejected = true;
    }
    assert(duplicate_rejected);

    const auto generated = metal::generate_entity_header(actual);
    assert(contains(
        generated.code,
        "metal::mapping::named_unique<\"uq_unique_users_email\">{}"));
    assert(contains(generated.code, "metal::mapping::unique"));

    auto mismatched = actual;
    auto& mismatched_user = const_cast<metal::DatabaseTable&>(table_named(mismatched, "unique_users"));
    auto& mismatched_email = const_cast<metal::DatabaseColumn&>(
        column_named(mismatched_user, "email"));
    mismatched_email.unique_name = "uq_wrong";
    const auto mismatch_plan = metal::diff_schema(expected, mismatched, dialect);
    assert(std::any_of(
        mismatch_plan.warnings.begin(), mismatch_plan.warnings.end(),
        [](const std::string& warning) {
            return warning.find("SQLite ALTER COLUMN is not supported") != std::string::npos;
        }));

    auto add_db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    add_db->execute(
        "CREATE TABLE unique_add (id INTEGER PRIMARY KEY AUTOINCREMENT);");
    const auto add_expected = metal::expected_schema<UniqueAdd>(dialect);
    const auto add_actual = metal::introspect_sqlite(
        *add_db,
        metal::IntrospectOptions{.include_tables = {"unique_add"}});
    const auto add_plan = metal::diff_schema(add_expected, add_actual, dialect);
    const auto add_change = std::find_if(
        add_plan.changes.begin(), add_plan.changes.end(),
        [](const metal::SchemaChange& change) {
            return change.kind == metal::SchemaChangeKind::AddColumn;
        });
    assert(add_change != add_plan.changes.end());
    assert(add_change->statements.empty());
    assert(!add_change->safe);
    assert(std::any_of(
        add_plan.warnings.begin(), add_plan.warnings.end(),
        [](const std::string& warning) {
            return warning.find("ADD COLUMN does not permit UNIQUE") != std::string::npos;
        }));
}
