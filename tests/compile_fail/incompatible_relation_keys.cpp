#include <metal/reflection.hpp>

#include <cstdint>
#include <string>

struct [[=metal::mapping::table{"posts"}]] BadPost {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string user_id;
};

struct [[=metal::mapping::table{"users"}]] BadUser {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::has_many<^^BadPost::user_id>{}]]
    metal::collection<BadPost> posts;
};

static_assert(metal::reflect::validate_mapping<BadUser>());

int main() {}
