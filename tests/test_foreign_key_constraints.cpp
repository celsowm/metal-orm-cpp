#include <metal/metal.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct [[=metal::mapping::table{"fk_parents"}]] FkParent {
    [[=metal::mapping::primary_key]]
    std::int64_t id{};
};

struct [[=metal::mapping::table{"fk_children"}]] FkChild {
    [[=metal::mapping::primary_key]]
    std::int64_t id{};

    [[=metal::mapping::reference<
        ^^FkParent::id,
        metal::mapping::referential_action::cascade,
        metal::mapping::referential_action::restrict,
        "fk_children_parent",
        true>{}]]
    std::optional<std::int64_t> parent_id;
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
    static_assert(metal::reflect::validate_physical_references<FkChild>());

    metal::SQLiteDialect dialect;
    const auto expected = metal::expected_schema<FkParent, FkChild>(dialect);
    const auto& expected_child = expected.tables[1];
    const auto& expected_parent_id = column_named(expected_child.table, "parent_id");
    assert(expected_parent_id.references);
    assert(expected_parent_id.references->name ==
           std::optional<std::string>{"fk_children_parent"});
    assert(expected_parent_id.references->deferrable);
    assert(expected_parent_id.references->on_delete ==
           std::optional<std::string>{"CASCADE"});
    assert(expected_parent_id.references->on_update ==
           std::optional<std::string>{"RESTRICT"});
    assert(contains(
        expected_child.create_table_sql,
        "CONSTRAINT \"fk_children_parent\" REFERENCES \"fk_parents\" (\"id\") "
        "ON DELETE CASCADE ON UPDATE RESTRICT DEFERRABLE INITIALLY DEFERRED"));

    {
        metal::DatabaseTable parsed;
        parsed.name = "parser_child";
        parsed.columns.push_back(metal::DatabaseColumn{.name = "parent_id"});
        parsed.columns.front().references = metal::ForeignKeyReference{
            .table = "parser_parent",
            .column = "id"
        };
        metal::schema_detail::parse_sqlite_foreign_key_modifiers(
            "CREATE TABLE parser_child ("
            "parent_id INTEGER, "
            "CONSTRAINT `fk_parser_parent` FOREIGN KEY(parent_id) "
            "REFERENCES parser_parent(id) NOT DEFERRABLE INITIALLY DEFERRED"
            ");",
            parsed);
        assert(parsed.columns.front().references->name ==
               std::optional<std::string>{"fk_parser_parent"});
        assert(!parsed.columns.front().references->deferrable);

        metal::schema_detail::parse_sqlite_foreign_key_modifiers(
            "CREATE TABLE parser_child ("
            "parent_id INTEGER CONSTRAINT [fk_inline] REFERENCES parser_parent(id) "
            "DEFERRABLE INITIALLY DEFERRED"
            ");",
            parsed);
        assert(parsed.columns.front().references->name ==
               std::optional<std::string>{"fk_inline"});
        assert(parsed.columns.front().references->deferrable);
    }

    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    db->execute("PRAGMA foreign_keys = ON;");
    db->execute(expected.tables[0].create_table_sql);
    db->execute(expected.tables[1].create_table_sql);

    const auto actual = metal::introspect_sqlite(
        *db,
        metal::IntrospectOptions{.include_tables = {"fk_parents", "fk_children"}});
    const auto& child = table_named(actual, "fk_children");
    const auto& parent_id = column_named(child, "parent_id");
    assert(parent_id.references);
    assert(parent_id.references->name ==
           std::optional<std::string>{"fk_children_parent"});
    assert(parent_id.references->deferrable);
    assert(parent_id.references->on_delete ==
           std::optional<std::string>{"CASCADE"});
    assert(parent_id.references->on_update ==
           std::optional<std::string>{"RESTRICT"});

    const auto final_plan = metal::diff_schema(expected, actual, dialect);
    assert(final_plan.changes.empty());
    assert(final_plan.warnings.empty());

    auto mismatch = actual;
    auto& mismatch_child = const_cast<metal::DatabaseTable&>(table_named(mismatch, "fk_children"));
    auto& mismatch_parent_id = const_cast<metal::DatabaseColumn&>(
        column_named(mismatch_child, "parent_id"));
    mismatch_parent_id.references->deferrable = false;
    const auto mismatch_plan = metal::diff_schema(expected, mismatch, dialect);
    assert(std::any_of(
        mismatch_plan.warnings.begin(), mismatch_plan.warnings.end(),
        [](const std::string& warning) {
            return warning.find("SQLite ALTER COLUMN is not supported") != std::string::npos;
        }));

    const auto generated = metal::generate_entity_header(actual);
    assert(contains(
        generated.code,
        "metal::mapping::reference_to<^^FkParent, \"id\", "
        "metal::mapping::referential_action::cascade, "
        "metal::mapping::referential_action::restrict, "
        "\"fk_children_parent\", true>{}"));

    db->execute("BEGIN;");
    db->execute("INSERT INTO fk_children(id, parent_id) VALUES (7, 99);");
    db->execute("INSERT INTO fk_parents(id) VALUES (99);");
    db->execute("COMMIT;");

    const auto persisted = db->execute(
        "SELECT COUNT(*) AS c FROM fk_children WHERE id = 7 AND parent_id = 99;");
    assert(metal::from_value<std::int64_t>(persisted.rows.front().at("c")) == 1);
}
