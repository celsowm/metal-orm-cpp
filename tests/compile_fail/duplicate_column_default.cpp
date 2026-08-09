#include <metal/metal.hpp>

#include <cstdint>

struct [[=metal::mapping::table{"bad_defaults"}]] BadDefaults {
    [[=metal::mapping::primary_key]]
    std::int64_t id{};

    [[=metal::mapping::default_value{1}, =metal::mapping::default_value{2}]]
    std::int64_t value{};
};

int main() {
    metal::SQLiteDialect dialect;
    (void)metal::create_table_sql<BadDefaults>(dialect);
}
