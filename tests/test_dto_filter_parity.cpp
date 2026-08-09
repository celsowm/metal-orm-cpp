#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

struct [[=metal::mapping::table{"filter_users"}]] FilterUser {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::column{"display_name"}]]
    std::string displayName;

    std::int64_t age{};
    bool active{};
    std::optional<std::string> email;
};

static_assert(metal::reflect::validate_mapping<FilterUser>());

int main() {
    metal::SQLiteDialect dialect;
    metal::SQLiteExecutor db{":memory:"};
    db.execute(metal::create_table_sql<FilterUser>(dialect));

    db.execute(
        "INSERT INTO \"filter_users\" (\"display_name\", \"age\", \"active\", \"email\") VALUES (?, ?, ?, ?);",
        {metal::Value{std::string{"Celso"}}, metal::Value{std::int64_t{40}}, metal::Value{true}, metal::Value{std::string{"celso@example.com"}}});
    db.execute(
        "INSERT INTO \"filter_users\" (\"display_name\", \"age\", \"active\", \"email\") VALUES (?, ?, ?, ?);",
        {metal::Value{std::string{"Alice"}}, metal::Value{std::int64_t{29}}, metal::Value{true}, metal::Value{std::string{"alice@example.com"}}});
    db.execute(
        "INSERT INTO \"filter_users\" (\"display_name\", \"age\", \"active\", \"email\") VALUES (?, ?, ?, ?);",
        {metal::Value{std::string{"Grace"}}, metal::Value{std::int64_t{37}}, metal::Value{false}, metal::Value{nullptr}});

    metal::FilterInput filter{
        {
            metal::filter_clause(
                "displayName",
                metal::FilterOperator::contains,
                metal::Value{std::string{"ELS"}},
                metal::StringFilterMode::insensitive),
            metal::filter_clause(
                "age",
                metal::FilterOperator::gte,
                metal::Value{std::int64_t{30}}),
            metal::filter_clause(
                "active",
                metal::FilterOperator::equals,
                metal::Value{true})
        }
    };

    auto query = metal::apply_filter<
        ^^FilterUser::displayName,
        ^^FilterUser::age,
        ^^FilterUser::active>(metal::select<FilterUser>(), filter);
    auto compiled = query.compile(dialect);
    assert(compiled.sql.find("LOWER") != std::string::npos);
    assert(compiled.sql.find("display_name") != std::string::npos);
    assert(compiled.params.size() == 3);

    const auto filtered = db.execute(compiled.sql, compiled.params);
    assert(filtered.rows.size() == 1);
    assert(metal::from_value<std::string>(filtered.rows[0].at("display_name")) == "Celso");

    metal::FilterInput ids{
        {metal::filter_list_clause(
            "age",
            metal::FilterOperator::in,
            {metal::Value{std::int64_t{29}}, metal::Value{std::int64_t{37}}})}
    };
    auto in_query = metal::apply_filter<^^FilterUser::age>(metal::select<FilterUser>(), ids);
    const auto in_compiled = in_query.compile(dialect);
    const auto in_rows = db.execute(in_compiled.sql, in_compiled.params);
    assert(in_rows.rows.size() == 2);

    metal::FilterInput null_email{{metal::null_filter_clause("email")}};
    auto null_query = metal::apply_filter<^^FilterUser::email>(metal::select<FilterUser>(), null_email);
    const auto null_compiled = null_query.compile(dialect);
    const auto null_rows = db.execute(null_compiled.sql, null_compiled.params);
    assert(null_rows.rows.size() == 1);
    assert(metal::from_value<std::string>(null_rows.rows[0].at("display_name")) == "Grace");

    const auto api_expression = metal::build_filter_expression<FilterUser, ^^FilterUser::age>(
        metal::FilterInput{{metal::filter_clause(
            "age",
            metal::FilterOperator::gte,
            metal::Value{std::int64_t{30}})}});
    assert(api_expression);
    auto composed = metal::select<FilterUser>();
    composed.where((metal::field<^^FilterUser::active> == true) && *api_expression);
    const auto composed_sql = composed.compile(dialect);
    const auto composed_rows = db.execute(composed_sql.sql, composed_sql.params);
    assert(composed_rows.rows.size() == 1);
    assert(metal::from_value<std::string>(composed_rows.rows[0].at("display_name")) == "Celso");

    bool unknown = false;
    try {
        (void)metal::build_filter_expression<FilterUser>(
            metal::FilterInput{{metal::filter_clause(
                "doesNotExist",
                metal::FilterOperator::equals,
                metal::Value{std::int64_t{1}})}});
    } catch (const std::invalid_argument&) {
        unknown = true;
    }
    assert(unknown);

    bool disallowed = false;
    try {
        (void)metal::build_filter_expression<FilterUser, ^^FilterUser::age>(
            metal::FilterInput{{metal::filter_clause(
                "email",
                metal::FilterOperator::equals,
                metal::Value{std::string{"celso@example.com"}})}});
    } catch (const std::invalid_argument&) {
        disallowed = true;
    }
    assert(disallowed);

    bool wrong_type = false;
    try {
        (void)metal::build_filter_expression<FilterUser, ^^FilterUser::age>(
            metal::FilterInput{{metal::filter_clause(
                "age",
                metal::FilterOperator::equals,
                metal::Value{std::string{"forty"}})}});
    } catch (const std::runtime_error&) {
        wrong_type = true;
    }
    assert(wrong_type);
}
