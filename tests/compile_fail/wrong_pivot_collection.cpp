#include <metal/reflection.hpp>

#include <cstdint>

struct [[=metal::mapping::table{"roles"}]] WrongPivotRole {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
};

struct [[=metal::mapping::table{"user_roles"}]] ExpectedPivot {
    [[=metal::mapping::primary_key]] std::int64_t user_id{};
    [[=metal::mapping::primary_key]] std::int64_t role_id{};
};

struct [[=metal::mapping::table{"other_pivot"}]] WrongPivot {
    [[=metal::mapping::primary_key]] std::int64_t user_id{};
    [[=metal::mapping::primary_key]] std::int64_t role_id{};
};

struct [[=metal::mapping::table{"users"}]] WrongPivotUser {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::many_to_many<
        ^^ExpectedPivot,
        ^^ExpectedPivot::user_id,
        ^^ExpectedPivot::role_id>{}]]
    metal::many_to_many_collection<WrongPivotRole, WrongPivot> roles;
};

static_assert(metal::reflect::validate_mapping<WrongPivotUser>());

int main() {}
