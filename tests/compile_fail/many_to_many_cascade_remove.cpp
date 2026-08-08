#include <metal/reflection.hpp>

#include <cstdint>

struct [[=metal::mapping::table{"roles"}]] BadCascadeRole {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
};

struct [[=metal::mapping::table{"user_roles"}]] BadCascadePivot {
    [[=metal::mapping::primary_key]] std::int64_t user_id{};
    [[=metal::mapping::primary_key]] std::int64_t role_id{};
};

struct [[=metal::mapping::table{"users"}]] BadCascadeUser {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::many_to_many<
        ^^BadCascadePivot,
        ^^BadCascadePivot::user_id,
        ^^BadCascadePivot::role_id,
        metal::mapping::cascade_mode::remove>{}]]
    metal::collection<BadCascadeRole> roles;
};

static_assert(metal::reflect::validate_mapping<BadCascadeUser>());

int main() {}
