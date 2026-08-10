#include <metal/metal.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct [[=metal::mapping::table{"checked_people"}]] CheckedPerson {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::check<"age >= 0">{}]]
    std::int64_t age{};
};

struct [[
    =metal::mapping::table{"checked_orders"},
    =metal::mapping::table_check<"quantity > 0">{},
    =metal::mapping::named_table_check<"note_shape", "instr(note, ',)') >= 0">{}
]] CheckedOrder {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    std::int64_t quantity{};
    std::string note;
};

struct [[=metal::mapping::table{"checked_add"}]] CheckedAdd {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::check<"score IS NULL OR score >= 0">{}]]
    std::optional<std::int64_t> score;
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

int main() {
    metal::SQLiteDialect dialect;

    {
        metal::DatabaseTable parsed;
        parsed.name = "parser_probe";
        parsed.columns.push_back(metal::DatabaseColumn{.name = "age"});
        parsed.columns.push_back(metal::DatabaseColumn{.name = "payload"});
        metal::schema_detail::parse_sqlite_check_constraints(
            R"SQL(CREATE TABLE "parser_probe" (
                "age" INTEGER CHECK (age >= 0 AND age < abs(-200)),
                "payload" TEXT CHECK (json_valid(payload) AND instr(payload, ',)') >= 0),
                CONSTRAINT "age_window" CHECK ((age % 2) = 0 AND length('x,y)') = 4),
                CHECK (age <> 13)
            );)SQL",
            parsed);

        assert(column_named(parsed, "age").check ==
               std::optional<std::string>{"age >= 0 AND age < abs(-200)"});
        assert(column_named(parsed, "payload").check ==
               std::optional<std::string>{"json_valid(payload) AND instr(payload, ',)') >= 0"});
        assert(parsed.checks.size() == 2);
        assert(parsed.checks[0].name == std::optional<std::string>{"age_window"});
        assert(parsed.checks[0].expression ==
               "(age % 2) = 0 AND length('x,y)') = 4");
        assert(!parsed.checks[1].name);
        assert(parsed.checks[1].expression == "age <> 13");
    }

    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    db->execute(
        "CREATE TABLE checked_add ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT"
        ");");

    auto expected = metal::expected_schema<CheckedPerson, CheckedOrder, CheckedAdd>(dialect);

    const auto& expected_person = expected.tables[0];
    assert(expected_person.create_table_sql.find("CHECK (age >= 0)") != std::string::npos);
    assert(column_named(expected_person.table, "age").check ==
           std::optional<std::string>{"age >= 0"});

    const auto& expected_order = expected.tables[1];
    assert(expected_order.table.checks.size() == 2);
    assert(expected_order.create_table_sql.find("CHECK (quantity > 0)") != std::string::npos);
    assert(expected_order.create_table_sql.find(
        "CONSTRAINT \"note_shape\" CHECK (instr(note, ',)') >= 0)") != std::string::npos);

    metal::IntrospectOptions inspect{
        .include_tables = {"checked_people", "checked_orders", "checked_add"}
    };

    const auto initial = metal::introspect_sqlite(*db, inspect);
    const auto initial_plan = metal::diff_schema(expected, initial, dialect);
    assert(!initial_plan.changes.empty());

    const auto applied = metal::synchronize_schema(expected, *db, dialect, {}, inspect);
    assert(!applied.changes.empty());

    const auto actual = metal::introspect_sqlite(*db, inspect);
    const auto& person = table_named(actual, "checked_people");
    assert(column_named(person, "age").check ==
           std::optional<std::string>{"age >= 0"});

    const auto& order = table_named(actual, "checked_orders");
    assert(order.checks.size() == 2);
    assert(std::any_of(
        order.checks.begin(), order.checks.end(),
        [](const metal::DatabaseCheck& check) {
            return !check.name && check.expression == "quantity > 0";
        }));
    assert(std::any_of(
        order.checks.begin(), order.checks.end(),
        [](const metal::DatabaseCheck& check) {
            return check.name == std::optional<std::string>{"note_shape"} &&
                   check.expression == "instr(note, ',)') >= 0";
        }));

    const auto& added = table_named(actual, "checked_add");
    assert(column_named(added, "score").check ==
           std::optional<std::string>{"score IS NULL OR score >= 0"});

    const auto final_plan = metal::diff_schema(expected, actual, dialect);
    assert(final_plan.changes.empty());
    assert(final_plan.warnings.empty());

    auto mismatched = actual;
    auto& mismatched_order = const_cast<metal::DatabaseTable&>(table_named(mismatched, "checked_orders"));
    assert(!mismatched_order.checks.empty());
    mismatched_order.checks.front().expression = "quantity >= 0";
    const auto mismatch_plan = metal::diff_schema(expected, mismatched, dialect);
    assert(std::any_of(
        mismatch_plan.warnings.begin(), mismatch_plan.warnings.end(),
        [](const std::string& warning) {
            return warning.find("table CHECK constraints") != std::string::npos;
        }));
}
