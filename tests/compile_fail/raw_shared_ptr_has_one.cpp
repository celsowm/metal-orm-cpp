#include <metal/metal.hpp>

#include <cstdint>
#include <memory>
#include <optional>

struct [[=metal::mapping::table{"raw_profiles"}]] RawProfile {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::optional<std::int64_t> user_id;
};

struct [[=metal::mapping::table{"raw_users"}]] RawUser {
    [[=metal::mapping::primary_key]] std::int64_t id{};

    [[=metal::mapping::has_one<^^RawProfile::user_id>{}]]
    std::shared_ptr<RawProfile> profile;
};

static_assert(metal::reflect::validate_mapping<RawUser>());
