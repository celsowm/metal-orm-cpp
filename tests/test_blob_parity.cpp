#include <metal/metal.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct [[=metal::mapping::table{"blob_records"}]] BlobRecord {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    metal::Blob payload;
    std::optional<metal::Blob> optional_payload;
};

static metal::Blob blob(std::initializer_list<unsigned int> values) {
    metal::Blob out;
    out.reserve(values.size());
    for (const auto value : values) {
        out.push_back(std::byte{static_cast<unsigned char>(value)});
    }
    return out;
}

static bool contains(const std::string& value, const std::string& needle) {
    return value.find(needle) != std::string::npos;
}

int main() {
    static_assert(metal::PersistableValue<metal::Blob>);
    static_assert(metal::PersistableValue<std::optional<metal::Blob>>);
    static_assert(metal::reflect::Mapped<BlobRecord>);
    static_assert(metal::reflect::validate_mapping<BlobRecord>());

    const auto binary = blob({0x00, 0xff, 0x41, 0x00, 0x7f});
    const metal::Blob empty;

    assert(metal::from_value<metal::Blob>(metal::to_value(binary)) == binary);
    assert(metal::value_key(metal::Value{binary}) == "x:00ff41007f");
    assert(metal::value_key(metal::Value{empty}) == "x:");

    auto dialect = std::make_shared<metal::SQLiteDialect>();
    const auto ddl = metal::create_table_sql<BlobRecord>(*dialect);
    assert(contains(ddl, "\"payload\" BLOB NOT NULL"));
    assert(contains(ddl, "\"optional_payload\" BLOB"));

    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    db->execute(ddl);

    metal::Session session{db, dialect};

    auto first = std::make_shared<BlobRecord>();
    first->payload = binary;
    first->optional_payload = empty;
    session.persist(first);

    auto second = std::make_shared<BlobRecord>();
    second->payload = empty;
    second->optional_payload = std::nullopt;
    session.persist(second);

    session.commit();
    assert(first->id > 0);
    assert(second->id > 0);

    session.clear();

    const auto loaded_first = session.find<BlobRecord>(first->id);
    assert(loaded_first);
    assert(loaded_first->payload == binary);
    assert(loaded_first->optional_payload.has_value());
    assert(loaded_first->optional_payload->empty());

    const auto loaded_second = session.find<BlobRecord>(second->id);
    assert(loaded_second);
    assert(loaded_second->payload.empty());
    assert(!loaded_second->optional_payload.has_value());

    const auto binary_matches = session.query<BlobRecord>()
        .where(metal::field<^^BlobRecord::payload> == binary)
        .all();
    assert(binary_matches.size() == 1);
    assert(binary_matches.front()->id == first->id);

    loaded_first->payload = blob({0x10, 0x00, 0x20});
    session.commit();
    session.clear();
    const auto updated = session.find<BlobRecord>(first->id);
    assert(updated);
    assert(updated->payload == blob({0x10, 0x00, 0x20}));

    const auto raw_types = db->execute(
        "SELECT id, typeof(payload) AS payload_type, length(payload) AS payload_length, "
        "typeof(optional_payload) AS optional_type, length(optional_payload) AS optional_length "
        "FROM blob_records ORDER BY id;");
    assert(raw_types.rows.size() == 2);
    assert(metal::from_value<std::string>(raw_types.rows[0].at("payload_type")) == "blob");
    assert(metal::from_value<std::string>(raw_types.rows[0].at("optional_type")) == "blob");
    assert(metal::from_value<std::int64_t>(raw_types.rows[0].at("optional_length")) == 0);
    assert(metal::from_value<std::string>(raw_types.rows[1].at("payload_type")) == "blob");
    assert(metal::from_value<std::int64_t>(raw_types.rows[1].at("payload_length")) == 0);
    assert(metal::from_value<std::string>(raw_types.rows[1].at("optional_type")) == "null");
    assert(std::holds_alternative<std::nullptr_t>(raw_types.rows[1].at("optional_length")));

    const auto introspected = metal::introspect_sqlite(
        *db,
        metal::IntrospectOptions{.include_tables = {"blob_records"}});
    assert(introspected.tables.size() == 1);
    const auto generated = metal::generate_entity_header(introspected);
    assert(contains(generated.code, "metal::Blob payload{};"));
    assert(contains(generated.code, "std::optional<metal::Blob> optional_payload;"));
    assert(std::none_of(
        generated.warnings.begin(), generated.warnings.end(),
        [](const std::string& warning) {
            return warning.find("BLOB value type") != std::string::npos;
        }));

    const auto schema = metal::dto_to_openapi_schema<BlobRecord>();
    const auto payload_schema = schema.properties.at("payload");
    assert(payload_schema->types.size() == 1);
    assert(payload_schema->types.front() == metal::OpenApiType::string);
    assert(payload_schema->format == std::optional<std::string>{"binary"});

    const auto optional_schema = schema.properties.at("optional_payload");
    assert(optional_schema->format == std::optional<std::string>{"binary"});
    assert(std::find(
        optional_schema->types.begin(), optional_schema->types.end(),
        metal::OpenApiType::null_value) != optional_schema->types.end());
}
