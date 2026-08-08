#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct [[=metal::mapping::table{"computed_people"}]] ComputedPerson {
    [[=metal::mapping::primary_key]]
    std::int64_t id{};
    std::string name;
    std::int64_t score{};
    bool active{};
    std::string created_at;
    std::string payload;
};

struct [[=metal::mapping::table{"computed_other"}]] ComputedOther {
    [[=metal::mapping::primary_key]]
    std::int64_t id{};
    std::string label;
};

static_assert(metal::reflect::validate_mapping<ComputedPerson>());
static_assert(metal::reflect::validate_mapping<ComputedOther>());

template <typename Q>
concept CanProjectForeignFunction = requires(Q query) {
    query.project(metal::lower(metal::field<^^ComputedOther::label>));
};

static_assert(!CanProjectForeignFunction<metal::SelectQuery<ComputedPerson>>);

int main() {
    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    metal::SQLiteDialect dialect;

    db->execute(metal::create_table_sql<ComputedPerson>(dialect));
    db->execute(
        "INSERT INTO computed_people(id, name, score, active, created_at, payload) VALUES "
        "(1, ' Alice ', 5, 1, '2026-01-15 10:20:30', '{\"theme\":\"dark\"}'), "
        "(2, 'Bob', 15, 1, '2025-06-01 08:00:00', '{\"theme\":\"light\"}'), "
        "(3, 'CAROL', 25, 0, '2024-12-31 23:59:59', '{\"theme\":\"dark\"}');");

    // Derived-table parity, including lexical parameter order:
    // projection parameter appears before the subquery WHERE parameter in SQL.
    auto source = metal::select<ComputedPerson>()
        .clear_projection()
        .project(metal::field<^^ComputedPerson::id>)
        .project(metal::field<^^ComputedPerson::name>)
        .project(metal::field<^^ComputedPerson::score>)
        .where(metal::field<^^ComputedPerson::score> >= 10);

    auto derived = metal::select<ComputedPerson>()
        .from_subquery(source, "high_scores", {"id", "name", "score"})
        .clear_projection()
        .project(metal::field<^^ComputedPerson::id>)
        .project(
            metal::concat(metal::field<^^ComputedPerson::name>, std::string{"!"})
                .as("display_name"))
        .where(metal::field<^^ComputedPerson::score> <= 20)
        .order_by(metal::field<^^ComputedPerson::id>);

    const auto derived_sql = derived.compile(dialect);
    assert(derived_sql.sql.find("FROM (SELECT") != std::string::npos);
    assert(derived_sql.sql.find("AS \"high_scores\" (\"id\", \"name\", \"score\")") != std::string::npos);
    assert(derived_sql.params.size() == 3);
    assert(metal::from_value<std::string>(derived_sql.params[0]) == "!");
    assert(metal::from_value<std::int64_t>(derived_sql.params[1]) == 10);
    assert(metal::from_value<std::int64_t>(derived_sql.params[2]) == 20);

    const auto derived_rows = db->execute(derived_sql.sql, derived_sql.params);
    assert(derived_rows.rows.size() == 1);
    assert(metal::from_value<std::int64_t>(derived_rows.rows[0].at("id")) == 2);
    assert(metal::from_value<std::string>(derived_rows.rows[0].at("display_name")) == "Bob!");

    auto band = metal::case_when(
        metal::field<^^ComputedPerson::score> > 20,
        std::string{"high"})
        .when(
            metal::field<^^ComputedPerson::score> > 10,
            std::string{"medium"})
        .otherwise(std::string{"low"});

    auto case_query = metal::select<ComputedPerson>()
        .clear_projection()
        .project(metal::field<^^ComputedPerson::id>)
        .project(band.as("band"))
        .order_by(metal::field<^^ComputedPerson::id>);

    const auto case_sql = case_query.compile(dialect);
    assert(case_sql.sql.find("CASE WHEN") != std::string::npos);
    const auto case_rows = db->execute(case_sql.sql, case_sql.params);
    assert(case_rows.rows.size() == 3);
    assert(metal::from_value<std::string>(case_rows.rows[0].at("band")) == "low");
    assert(metal::from_value<std::string>(case_rows.rows[1].at("band")) == "medium");
    assert(metal::from_value<std::string>(case_rows.rows[2].at("band")) == "high");

    auto high_only = metal::select<ComputedPerson>()
        .where(band == std::string{"high"});
    const auto high_sql = high_only.compile(dialect);
    const auto high_rows = db->execute(high_sql.sql, high_sql.params);
    assert(high_rows.rows.size() == 1);
    assert(metal::from_value<std::int64_t>(high_rows.rows[0].at("id")) == 3);

    auto functions = metal::select<ComputedPerson>()
        .clear_projection()
        .project(metal::field<^^ComputedPerson::id>)
        .project(metal::lower(metal::trim(metal::field<^^ComputedPerson::name>)).as("normalized"))
        .project(metal::length(metal::trim(metal::field<^^ComputedPerson::name>)).as("name_len"))
        .project(metal::substr(metal::trim(metal::field<^^ComputedPerson::name>), 1, 2).as("prefix"))
        .project(metal::replace(metal::trim(metal::field<^^ComputedPerson::name>), "o", "0").as("replaced"))
        .project(metal::greatest(metal::field<^^ComputedPerson::score>, 10).as("at_least_ten"))
        .project(metal::year(metal::field<^^ComputedPerson::created_at>).as("year"))
        .project(metal::json_path<std::string>(metal::field<^^ComputedPerson::payload>, "$.theme").as("theme"))
        .where(metal::lower(metal::trim(metal::field<^^ComputedPerson::name>)) == std::string{"alice"});

    const auto function_sql = functions.compile(dialect);
    const auto function_rows = db->execute(function_sql.sql, function_sql.params);
    assert(function_rows.rows.size() == 1);
    const auto& row = function_rows.rows[0];
    assert(metal::from_value<std::string>(row.at("normalized")) == "alice");
    assert(metal::from_value<std::int64_t>(row.at("name_len")) == 5);
    assert(metal::from_value<std::string>(row.at("prefix")) == "Al");
    assert(metal::from_value<std::string>(row.at("replaced")) == "Alice");
    assert(metal::from_value<std::int64_t>(row.at("at_least_ten")) == 10);
    assert(metal::from_value<std::int64_t>(row.at("year")) == 2026);
    assert(metal::from_value<std::string>(row.at("theme")) == "dark");

    auto control = metal::select<ComputedPerson>()
        .clear_projection()
        .project(metal::coalesce(
            metal::nullif(metal::trim(metal::field<^^ComputedPerson::name>), std::string{"Alice"}),
            std::string{"fallback"}).as("value"))
        .where(metal::field<^^ComputedPerson::id> == 1);
    const auto control_sql = control.compile(dialect);
    const auto control_rows = db->execute(control_sql.sql, control_sql.params);
    assert(control_rows.rows.size() == 1);
    assert(metal::from_value<std::string>(control_rows.rows[0].at("value")) == "fallback");

    // Compile-only catalog coverage for helpers whose runtime availability can depend
    // on how the system SQLite library was built (math extension set/version).
    auto catalog = metal::select<ComputedPerson>()
        .clear_projection()
        .project(metal::upper(metal::field<^^ComputedPerson::name>).as("upper_name"))
        .project(metal::left(metal::field<^^ComputedPerson::name>, 2).as("left_name"))
        .project(metal::right(metal::field<^^ComputedPerson::name>, 2).as("right_name"))
        .project(metal::ascii(metal::field<^^ComputedPerson::name>).as("ascii_code"))
        .project(metal::abs(metal::field<^^ComputedPerson::score>).as("abs_score"))
        .project(metal::sqrt(metal::field<^^ComputedPerson::score>).as("root_score"))
        .project(metal::date_add(metal::field<^^ComputedPerson::created_at>, 1, metal::date_part::day).as("tomorrow"))
        .project(metal::extract(metal::date_part::month, metal::field<^^ComputedPerson::created_at>).as("month"))
        .project(metal::json_length(std::string{"[1,2,3]"}).as("json_len"));
    const auto catalog_sql = catalog.compile(dialect);
    assert(catalog_sql.sql.find("UPPER(") != std::string::npos);
    assert(catalog_sql.sql.find("substr(") != std::string::npos);
    assert(catalog_sql.sql.find("SQRT(") != std::string::npos);
    assert(catalog_sql.sql.find("datetime(") != std::string::npos);
    assert(catalog_sql.sql.find("strftime(") != std::string::npos);
    assert(catalog_sql.sql.find("json_array_length(") != std::string::npos);
}
