#include <metal/metal.hpp>

#include <cstdint>
#include <memory>
#include <optional>

struct [[=metal::mapping::table{"raw_parents"}]] RawParent {
    [[=metal::mapping::primary_key]] std::int64_t id{};
};

struct [[=metal::mapping::table{"raw_children"}]] RawChild {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::optional<std::int64_t> parent_id;

    [[=metal::mapping::belongs_to<^^RawChild::parent_id>{}]]
    std::shared_ptr<RawParent> parent;
};

static_assert(metal::reflect::validate_mapping<RawChild>());
