#include <metal/metal.hpp>

#include <cstdint>
#include <optional>
#include <string>

struct [[=metal::mapping::table{"bad_subject_posts"}]] BadSubjectPost {
    [[=metal::mapping::primary_key]] std::int64_t id{};
};

struct [[=metal::mapping::table{"bad_subject_videos"}]] BadSubjectVideo {
    [[=metal::mapping::primary_key]] std::int64_t id{};
};

struct [[=metal::mapping::table{"bad_activities"}]] BadActivity {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::optional<std::int64_t> subject_id;
    std::optional<std::string> subject_type;

    [[=metal::mapping::morph_to<
        ^^BadActivity::subject_type,
        ^^BadActivity::subject_id,
        metal::mapping::cascade_mode::none,
        metal::mapping::morph_target<"subject", ^^BadSubjectPost>,
        metal::mapping::morph_target<"subject", ^^BadSubjectVideo>>{}]]
    metal::morph_to_reference<BadSubjectPost, BadSubjectVideo> subject;
};

static_assert(metal::reflect::validate_mapping<BadActivity>());
