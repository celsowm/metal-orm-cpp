#include <metal/metal.hpp>

#include <cstdint>
#include <string>

struct [[=metal::mapping::table{"schema_a"}]] SchemaA {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::string name;
};

struct [[=metal::mapping::table{"schema_b"}]] SchemaB {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::string email;
};

int main() {
    metal::SQLiteDialect dialect;
    auto expected = metal::expected_schema<SchemaA>(dialect);
    metal::add_expected_index<SchemaA, ^^SchemaB::email>(
        expected,
        dialect,
        "bad_index");
}
