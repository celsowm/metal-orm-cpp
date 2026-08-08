#include <metal/reflection.hpp>

#include <cstdint>
#include <memory>

struct [[=metal::mapping::table{"posts"}]] BadShapePost {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::int64_t user_id{};
};

struct [[=metal::mapping::table{"users"}]] BadShapeUser {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::has_many<^^BadShapePost::user_id>{}]]
    std::shared_ptr<BadShapePost> post;
};

static_assert(metal::reflect::validate_mapping<BadShapeUser>());

int main() {}
