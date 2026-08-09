#include <metal/metal.hpp>

struct [[=metal::mapping::table{"bad_tree"}]] BadTree {
    [[=metal::mapping::primary_key]]
    std::int64_t id{};
    [[=metal::mapping::tree_parent]]
    std::int64_t parent_id{};
    [[=metal::mapping::tree_left]]
    std::int64_t lft{};
    [[=metal::mapping::tree_right]]
    std::int64_t rght{};
};

static_assert(metal::reflect::validate_tree_mapping<BadTree>());
