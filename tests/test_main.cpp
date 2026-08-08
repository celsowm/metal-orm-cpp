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

struct [[=metal::mapping::table{"posts"}]] Post {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::int64_t user_id{};
    std::string title;
};

struct [[=metal::mapping::table{"profiles"}]] Profile {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::int64_t user_id{};
    std::string bio;
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

    [[=metal::mapping::column{"display_name"}]]
    std::string name;

    int age{};
    bool active{true};

    [[=metal::mapping::has_many<^^Post::user_id>{}]]
    std::vector<std::shared_ptr<Post>> posts;

    [[=metal::mapping::has_one<^^Profile::user_id>{}]]
    std::shared_ptr<Profile> profile;

    [[=metal::mapping::many_to_many<
        ^^UserRole,
        ^^UserRole::user_id,
        ^^UserRole::role_id>{}]]
    std::vector<std::shared_ptr<Role>> roles;
};

struct [[=metal::mapping::table{"comments"}]] Comment {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::int64_t user_id{};
    std::string body;

    [[=metal::mapping::belongs_to<^^Comment::user_id>{}]]
    std::shared_ptr<User> author;
};

static_assert(metal::reflect::Mapped<UserRole>);
static_assert(!metal::reflect::Entity<UserRole>);
static_assert(metal::reflect::Entity<User>);
static_assert(metal::reflect::Entity<Role>);
static_assert(metal::reflect::validate_mapping<UserRole>());
static_assert(metal::reflect::validate_mapping<User>());
static_assert(metal::reflect::validate_mapping<Comment>());
static_assert(metal::reflect::has<metal::mapping::primary_key_t>(^^User::id));
static_assert(metal::reflect::has<metal::mapping::generated_t>(^^User::id));
static_assert(metal::reflect::has_relation_annotation<^^User::roles>());
static_assert(metal::reflect::has_relation_annotation<^^User::posts>());
static_assert(metal::reflect::has_relation_annotation<^^User::profile>());
static_assert(metal::reflect::has_relation_annotation<^^Comment::author>());
static_assert(std::same_as<metal::reflect::member_type_t<^^User::age>, int>);
static_assert(std::same_as<metal::reflect::owner_type_t<^^User::age>, User>);
static_assert(metal::reflect::primary_key_count<UserRole>() == 2);

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

    const auto pivot_ddl = metal::create_table_sql<UserRole>(dialect);
    assert(pivot_ddl.find("PRIMARY KEY (\"user_id\", \"role_id\")") != std::string::npos);

    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    auto dialect_ptr = std::make_shared<metal::SQLiteDialect>();
    db->execute(metal::create_table_sql<Role>(*dialect_ptr));
    db->execute(metal::create_table_sql<Post>(*dialect_ptr));
    db->execute(metal::create_table_sql<Profile>(*dialect_ptr));
    db->execute(metal::create_table_sql<UserRole>(*dialect_ptr));
    db->execute(metal::create_table_sql<User>(*dialect_ptr));
    db->execute(metal::create_table_sql<Comment>(*dialect_ptr));

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

    auto p1 = std::make_shared<Post>();
    p1->user_id = celso->id;
    p1->title = "Reflection";
    session.persist(p1);

    auto p2 = std::make_shared<Post>();
    p2->user_id = celso->id;
    p2->title = "Splicing";
    session.persist(p2);

    auto p3 = std::make_shared<Post>();
    p3->user_id = levi->id;
    p3->title = "SQLite";
    session.persist(p3);

    auto profile = std::make_shared<Profile>();
    profile->user_id = celso->id;
    profile->bio = "C++26";
    session.persist(profile);

    auto comment = std::make_shared<Comment>();
    comment->user_id = celso->id;
    comment->body = "typed relation";
    session.persist(comment);

    session.commit();

    db->execute(
        "INSERT INTO user_roles(user_id, role_id) VALUES (?, ?), (?, ?), (?, ?);",
        {celso->id, developer->id, celso->id, admin->id, levi->id, developer->id});

    session.clear();

    auto users = session.query<User>()
        .where(metal::field<^^User::age> >= 18)
        .order_by(metal::field<^^User::id>)
        .include<^^User::posts>()
        .include<^^User::profile>()
        .include<^^User::roles>()
        .all();

    assert(users.size() == 2);
    assert(users[0]->posts.size() == 2);
    assert(users[1]->posts.size() == 1);
    assert(users[0]->profile);
    assert(users[0]->profile->bio == "C++26");
    assert(!users[1]->profile);
    assert(users[0]->roles.size() == 2);
    assert(users[1]->roles.size() == 1);

    std::shared_ptr<Role> shared_developer;
    for (const auto& role : users[0]->roles) {
        if (role->name == "developer") shared_developer = role;
    }
    assert(shared_developer);
    assert(users[1]->roles[0] == shared_developer);

    auto comments = session.query<Comment>()
        .include<^^Comment::author>()
        .all();
    assert(comments.size() == 1);
    assert(comments[0]->author == users[0]);

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
