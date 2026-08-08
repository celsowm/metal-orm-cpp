#include <metal/metal.hpp>

#include <cstdint>
#include <string>

struct [[=metal::mapping::table{"typed_patch"}]] TypedPatch {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::string label;
};

int main() {
    metal::pivot_patch<TypedPatch> patch;
    patch.set<^^TypedPatch::label>(std::int64_t{42});
}
