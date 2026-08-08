#include <metal/metal.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

struct [[=metal::mapping::table{"roles"}]] Role {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;
};

struct [[=metal::mapping::table{"user_roles"}]] UserRole {
    [[=metal::mapping::primary_key]] std::int64_t user_id{};
    [[=metal::mapping::primary_key]] std::int64_t role_id{};
};

struct [[=metal::mapping::table{"users"}]] User {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;
    bool active{true};

    [[=metal::mapping::many_to_many<
        ^^UserRole,
        ^^UserRole::user_id,
        ^^UserRole::role_id,
        metal::mapping::cascade_mode::persist>{}]]
    metal::many_to_many_collection<Role, UserRole> roles;
};

int main() {
    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    auto dialect = std::make_shared<metal::SQLiteDialect>();

    db->execute(metal::create_table_sql<Role>(*dialect));
    db->execute(metal::create_table_sql<UserRole>(*dialect));
    db->execute(metal::create_table_sql<User>(*dialect));

    metal::Session session{db, dialect};

    auto developer = std::make_shared<Role>();
    developer->name = "developer";

    auto admin = std::make_shared<Role>();
    admin->name = "admin";

    auto celso = std::make_shared<User>();
    celso->name = "Celso";
    celso->roles.attach(developer);
    celso->roles.attach(admin);

    session.persist(celso);
    session.commit();
    session.clear();

    auto user = session.query<User>()
        .where(metal::field<^^User::active> == true)
        .first();

    if (user) {
        user->roles.load();
        std::cout << user->name << '\n';
        for (const auto& role : user->roles) {
            std::cout << "  - " << role->name << '\n';
        }
    }
}
