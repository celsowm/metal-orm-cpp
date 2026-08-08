#include <metal/metal.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

struct [[=metal::mapping::table{"roles"}]] Role {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;
};

struct [[=metal::mapping::table{"user_roles"}]] UserRole {
    [[=metal::mapping::primary_key]]
    std::int64_t user_id{};

    [[=metal::mapping::primary_key]]
    std::int64_t role_id{};
};

struct [[=metal::mapping::table{"users"}]] User {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;
    bool active{true};

    [[=metal::mapping::many_to_many<
        ^^UserRole,
        ^^UserRole::user_id,
        ^^UserRole::role_id>{}]]
    std::vector<std::shared_ptr<Role>> roles;
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
    session.persist(developer);

    auto admin = std::make_shared<Role>();
    admin->name = "admin";
    session.persist(admin);

    auto celso = std::make_shared<User>();
    celso->name = "Celso";
    session.persist(celso);
    session.commit();

    db->execute("INSERT INTO user_roles(user_id, role_id) VALUES (?, ?), (?, ?);",
                {celso->id, developer->id, celso->id, admin->id});

    session.clear();

    auto users = session.query<User>()
        .where(metal::field<^^User::active> == true)
        .order_by(metal::field<^^User::name>)
        .include<^^User::roles>()
        .all();

    for (const auto& user : users) {
        std::cout << user->name << '\n';
        for (const auto& role : user->roles) {
            std::cout << "  - " << role->name << '\n';
        }
    }
}
