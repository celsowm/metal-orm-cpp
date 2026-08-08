#include <metal/metal.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct [[=metal::mapping::table{"bad_morph_covers"}]] BadMorphCover {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::optional<std::int64_t> imageable_id;
    std::optional<std::string> imageable_type;
};

struct [[=metal::mapping::table{"bad_morph_posts"}]] BadMorphPost {
    [[=metal::mapping::primary_key]] std::int64_t id{};

    [[=metal::mapping::morph_one<
        ^^BadMorphCover::imageable_type,
        ^^BadMorphCover::imageable_id,
        "post">{}]]
    std::shared_ptr<BadMorphCover> cover;
};

static_assert(metal::reflect::validate_mapping<BadMorphPost>());
