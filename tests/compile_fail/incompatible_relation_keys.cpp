#include <metal/reflection.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct [[=metal::mapping::table{"posts"}]] BadPost {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string user_id;
};

struct [[=metal::mapping::table{"users"}]] BadUser {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::has_many<^^BadPost::user_id>{}]]
    std::vector<std::shared_ptr<BadPost>> posts;
};

static_assert(metal::reflect::validate_mapping<BadUser>());

int main() {}
