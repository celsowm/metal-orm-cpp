#include <metal/metal.hpp>

#include <cstdint>
#include <string>

struct [[=metal::mapping::table{"dto_users"}]] DtoUser {
    [[=metal::mapping::primary_key]]
    std::int64_t id{};
    std::string name;
};

struct [[=metal::mapping::table{"dto_posts"}]] DtoPost {
    [[=metal::mapping::primary_key]]
    std::int64_t id{};
    std::string title;
};

int main() {
    (void)metal::describe_response_dto<DtoUser, ^^DtoPost::title>();
}
