#include <metal/metal.hpp>

#include <cstdint>
#include <string>

struct [[=metal::mapping::table{"patch_a"}]] PatchA {
    [[=metal::mapping::primary_key]] std::int64_t left_id{};
    std::string label;
};

struct [[=metal::mapping::table{"patch_b"}]] PatchB {
    [[=metal::mapping::primary_key]] std::int64_t right_id{};
    std::string label;
};

int main() {
    metal::pivot_patch<PatchA> patch;
    patch.set<^^PatchB::label>(std::string{"wrong"});
}
