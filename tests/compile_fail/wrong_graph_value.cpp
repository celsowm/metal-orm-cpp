#include <metal/metal.hpp>

#include <cstdint>
#include <string>

struct [[=metal::mapping::table{"bad_graph_users"}]] BadGraphUser {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::string name;
};

int main() {
    auto payload = metal::graph<BadGraphUser>()
        .set<^^BadGraphUser::name>(std::int64_t{42});
    (void)payload;
}
