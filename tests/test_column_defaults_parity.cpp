#include <metal/metal.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct [[=metal::mapping::table{"default_entities"}]] DefaultEntity {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    std::string required_name;

    [[=metal::mapping::default_text{"O'Reilly"}]]
    std::string label;

    [[=metal::mapping::default_value{0}]]
    std::int64_t retries{};

    [[=metal::mapping::default_value{false}]]
    bool enabled{};

    [[=metal::mapping::default_value{1.25}]]
    double ratio{};

    [[=metal::mapping::default_sql{"CURRENT_TIMESTAMP"}]]
    std::string created_at;

    [[=metal::mapping::default_null]]
    std::optional<std::string> note;
};

static_assert(metal::reflect::validate_mapping<DefaultEntity>());
static_assert(metal::reflect::validate_column_defaults<DefaultEntity>());

static const metal::DtoField& find_field(
    const metal::DtoDescriptor& descriptor,
    const std::string& name) {
    const auto found = std::find_if(
        descriptor.fields.begin(), descriptor.fields.end(),
        [&](const metal::DtoField& field) { return field.name == name; });
    assert(found != descriptor.fields.end());
    return *found;
}

static const metal::DatabaseColumn& find_column(
    const metal::DatabaseTable& table,
    const std::string& name) {
    const auto found = std::find_if(
        table.columns.begin(), table.columns.end(),
        [&](const metal::DatabaseColumn& column) { return column.name == name; });
    assert(found != table.columns.end());
    return *found;
}

static bool contains_required(
    const metal::OpenApiSchema& schema,
    const std::string& name) {
    return std::find(schema.required.begin(), schema.required.end(), name) != schema.required.end();
}

int main() {
    metal::SQLiteDialect dialect;

    const auto ddl = metal::create_table_sql<DefaultEntity>(dialect);
    assert(ddl.find("\"label\" TEXT NOT NULL DEFAULT 'O''Reilly'") != std::string::npos);
    assert(ddl.find("\"retries\" INTEGER NOT NULL DEFAULT 0") != std::string::npos);
    assert(ddl.find("\"enabled\" INTEGER NOT NULL DEFAULT 0") != std::string::npos);
    assert(ddl.find("\"ratio\" REAL NOT NULL DEFAULT 1.25") != std::string::npos);
    assert(ddl.find("\"created_at\" TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP") != std::string::npos);
    assert(ddl.find("\"note\" TEXT DEFAULT NULL") != std::string::npos);

    const auto expected = metal::expected_table<DefaultEntity>(dialect);
    assert(find_column(expected.table, "required_name").default_value == std::nullopt);
    assert(find_column(expected.table, "label").default_value == std::optional<std::string>{"'O''Reilly'"});
    assert(find_column(expected.table, "retries").default_value == std::optional<std::string>{"0"});
    assert(find_column(expected.table, "enabled").default_value == std::optional<std::string>{"0"});
    assert(find_column(expected.table, "ratio").default_value == std::optional<std::string>{"1.25"});
    assert(find_column(expected.table, "created_at").default_value == std::optional<std::string>{"CURRENT_TIMESTAMP"});
    assert(find_column(expected.table, "note").default_value == std::optional<std::string>{"NULL"});

    const auto create_dto = metal::describe_create_dto<DefaultEntity>();
    assert(find_field(create_dto, "required_name").required);
    assert(!find_field(create_dto, "required_name").has_default);
    assert(!find_field(create_dto, "label").required);
    assert(find_field(create_dto, "label").has_default);
    assert(!find_field(create_dto, "retries").required);
    assert(find_field(create_dto, "retries").has_default);
    assert(!find_field(create_dto, "enabled").required);
    assert(find_field(create_dto, "enabled").has_default);
    assert(!find_field(create_dto, "ratio").required);
    assert(!find_field(create_dto, "created_at").required);
    assert(!find_field(create_dto, "note").required);

    const auto create_openapi = metal::create_dto_to_openapi_schema<DefaultEntity>();
    assert(contains_required(create_openapi, "required_name"));
    assert(!contains_required(create_openapi, "label"));
    assert(!contains_required(create_openapi, "retries"));
    assert(!contains_required(create_openapi, "enabled"));
    assert(!contains_required(create_openapi, "ratio"));
    assert(!contains_required(create_openapi, "created_at"));
    assert(!contains_required(create_openapi, "note"));
    assert(!create_openapi.properties.contains("id"));

    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    db->execute(ddl);
    db->execute("INSERT INTO default_entities(required_name) VALUES ('required');");

    const auto rows = db->execute(
        "SELECT id, required_name, label, retries, enabled, ratio, created_at, note "
        "FROM default_entities;").rows;
    assert(rows.size() == 1);
    const auto& row = rows.front();
    assert(metal::from_value<std::string>(row.at("required_name")) == "required");
    assert(metal::from_value<std::string>(row.at("label")) == "O'Reilly");
    assert(metal::from_value<std::int64_t>(row.at("retries")) == 0);
    assert(!metal::from_value<bool>(row.at("enabled")));
    assert(metal::from_value<double>(row.at("ratio")) == 1.25);
    assert(!metal::from_value<std::string>(row.at("created_at")).empty());
    assert(std::holds_alternative<std::nullptr_t>(row.at("note")));

    const auto actual = metal::introspect_sqlite(*db);
    metal::ExpectedSchema expected_schema;
    expected_schema.tables.push_back(expected);
    const auto plan = metal::diff_schema(expected_schema, actual, dialect);
    assert(plan.changes.empty());
    assert(plan.warnings.empty());
}
