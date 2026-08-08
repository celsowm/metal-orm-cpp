#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct [[=metal::mapping::table{"roles"}]] Role {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;
};

struct [[=metal::mapping::table{"users"}]] User {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::column{"display_name"}]]
    std::string name;

    int age{};
    bool active{true};

    [[=metal::mapping::many_to_many{"user_roles", "user_id", "role_id"}]]
    std::vector<std::shared_ptr<Role>> roles;
};

static_assert(metal::reflect::Entity<User>);
static_assert(metal::reflect::Entity<Role>);
static_assert(metal::reflect::has<metal::mapping::primary_key_t>(^^User::id));
static_assert(metal::reflect::has<metal::mapping::generated_t>(^^User::id));
static_assert(metal::reflect::has<metal::mapping::many_to_many>(^^User::roles));
static_assert(std::same_as<metal::reflect::member_type_t<^^User::age>, int>);
static_assert(std::same_as<metal::reflect::owner_type_t<^^User::age>, User>);

int main() {
    metal::SQLiteDialect dialect;

    const auto query = metal::select<User>()
        .where((metal::field<^^User::age> >= 18) && (metal::field<^^User::active> == true))
        .order_by(metal::field<^^User::name>)
        .limit(10)
        .compile(dialect);

    assert(query.sql ==
        "SELECT \"id\", \"display_name\", \"age\", \"active\" FROM \"users\" "
        "WHERE (\"age\" >= ? AND \"active\" = ?) ORDER BY \"display_name\" ASC LIMIT 10;");
    assert(query.params.size() == 2);

    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    auto dialect_ptr = std::make_shared<metal::SQLiteDialect>();
    db->execute(metal::create_table_sql<Role>(*dialect_ptr));
    db->execute(metal::create_table_sql<User>(*dialect_ptr));
    db->execute("CREATE TABLE user_roles (user_id INTEGER NOT NULL, role_id INTEGER NOT NULL, PRIMARY KEY(user_id, role_id));");

    metal::Session session{db, dialect_ptr};

    auto developer = std::make_shared<Role>();
    developer->name = "developer";
    session.persist(developer);

    auto admin = std::make_shared<Role>();
    admin->name = "admin";
    session.persist(admin);

    auto celso = std::make_shared<User>();
    celso->name = "Celso";
    celso->age = 40;
    session.persist(celso);

    auto levi = std::make_shared<User>();
    levi->name = "Levi";
    levi->age = 18;
    session.persist(levi);

    session.commit();
    assert(celso->id > 0 && levi->id > 0 && developer->id > 0 && admin->id > 0);

    db->execute("INSERT INTO user_roles(user_id, role_id) VALUES (?, ?), (?, ?), (?, ?);",
                {celso->id, developer->id, celso->id, admin->id, levi->id, developer->id});

    session.clear();

    auto users = session.query<User>()
        .where(metal::field<^^User::age> >= 18)
        .order_by(metal::field<^^User::id>)
        .include<^^User::roles>()
        .all();

    assert(users.size() == 2);
    assert(users[0]->roles.size() == 2);
    assert(users[1]->roles.size() == 1);

    std::shared_ptr<Role> shared_developer;
    for (const auto& role : users[0]->roles) {
        if (role->name == "developer") shared_developer = role;
    }
    assert(shared_developer);
    assert(users[1]->roles[0] == shared_developer);

    auto same_celso = session.find<User>(users[0]->id);
    assert(same_celso == users[0]);

    same_celso->name = "Celso Araujo";
    session.commit();
    session.clear();

    auto updated = session.find<User>(celso->id);
    assert(updated);
    assert(updated->name == "Celso Araujo");

    session.remove(updated);
    session.commit();
    session.clear();
    assert(!session.find<User>(celso->id));
}
