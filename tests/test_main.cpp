#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>

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
    [[=metal::mapping::primary_key]] std::int64_t user_id{};
    [[=metal::mapping::primary_key]] std::int64_t role_id{};
};

struct [[=metal::mapping::table{"users"}]] User {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::column{"display_name"}]]
    std::string name;

    int age{};
    bool active{true};

    [[=metal::mapping::has_many<
        ^^Post::user_id,
        metal::mapping::cascade_mode::all>{}]]
    metal::collection<Post> posts;

    [[=metal::mapping::has_one<^^Profile::user_id>{}]]
    std::shared_ptr<Profile> profile;

    [[=metal::mapping::many_to_many<
        ^^UserRole,
        ^^UserRole::user_id,
        ^^UserRole::role_id,
        metal::mapping::cascade_mode::persist>{}]]
    metal::collection<Role> roles;
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
static_assert(metal::reflect::validate_mapping<UserRole>());
static_assert(metal::reflect::validate_mapping<User>());
static_assert(metal::reflect::validate_mapping<Comment>());
static_assert(metal::reflect::primary_key_count<UserRole>() == 2);
static_assert(std::same_as<metal::reflect::member_type_t<^^User::roles>, metal::collection<Role>>);

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
    auto admin = std::make_shared<Role>();
    admin->name = "admin";

    auto p1 = std::make_shared<Post>();
    p1->title = "Reflection";
    auto p2 = std::make_shared<Post>();
    p2->title = "Splicing";
    auto p3 = std::make_shared<Post>();
    p3->title = "SQLite";

    auto celso = std::make_shared<User>();
    celso->name = "Celso";
    celso->age = 40;
    celso->roles.attach(developer);
    celso->roles.attach(admin);
    celso->posts.attach(p1);
    celso->posts.attach(p2);
    session.persist(celso);

    auto levi = std::make_shared<User>();
    levi->name = "Levi";
    levi->age = 18;
    levi->roles.attach(developer);
    levi->posts.attach(p3);
    session.persist(levi);

    session.commit();

    assert(celso->id > 0 && levi->id > 0);
    assert(developer->id > 0 && admin->id > 0);
    assert(p1->id > 0 && p2->id > 0 && p3->id > 0);
    assert(p1->user_id == celso->id && p2->user_id == celso->id);
    assert(p3->user_id == levi->id);
    assert(!celso->roles.dirty());
    assert(!celso->posts.dirty());

    auto profile = std::make_shared<Profile>();
    profile->user_id = celso->id;
    profile->bio = "C++26";
    session.persist(profile);

    auto comment = std::make_shared<Comment>();
    comment->user_id = celso->id;
    comment->body = "typed relation";
    session.persist(comment);
    session.commit();
    session.clear();

    auto users = session.query<User>()
        .where(metal::field<^^User::age> >= 18)
        .order_by(metal::field<^^User::id>)
        .include<^^User::posts>()
        .include<^^User::profile>()
        .include<^^User::roles>()
        .all();

    assert(users.size() == 2);
    assert(users[0]->posts.loaded() && users[0]->roles.loaded());
    assert(users[0]->posts.size() == 2);
    assert(users[1]->posts.size() == 1);
    assert(users[0]->profile && users[0]->profile->bio == "C++26");
    assert(!users[1]->profile);
    assert(users[0]->roles.size() == 2);
    assert(users[1]->roles.size() == 1);
    assert(!users[0]->posts.dirty() && !users[0]->roles.dirty());

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

    auto auditor = std::make_shared<Role>();
    auditor->name = "auditor";
    users[0]->roles.sync({shared_developer, auditor});

    const auto removed_post_id = users[0]->posts[0]->id;
    users[0]->posts.detach(users[0]->posts[0]);
    auto p4 = std::make_shared<Post>();
    p4->title = "Collections";
    users[0]->posts.attach(p4);

    assert(users[0]->roles.dirty());
    assert(users[0]->posts.dirty());
    session.commit();

    assert(auditor->id > 0);
    assert(p4->id > 0 && p4->user_id == users[0]->id);
    assert(!users[0]->roles.dirty());
    assert(!users[0]->posts.dirty());
    assert(!session.find<Post>(removed_post_id));

    const auto celso_id = users[0]->id;
    session.clear();

    auto reloaded = session.query<User>()
        .where(metal::field<^^User::id> == celso_id)
        .include<^^User::posts>()
        .include<^^User::roles>()
        .first();

    assert(reloaded);
    assert(reloaded->posts.size() == 2);
    assert(reloaded->roles.size() == 2);

    bool has_developer = false;
    bool has_auditor = false;
    bool has_admin = false;
    for (const auto& role : reloaded->roles) {
        has_developer |= role->name == "developer";
        has_auditor |= role->name == "auditor";
        has_admin |= role->name == "admin";
    }
    assert(has_developer && has_auditor && !has_admin);

    reloaded->name = "Celso Araujo";
    session.commit();
    session.clear();

    auto updated = session.find<User>(celso_id);
    assert(updated && updated->name == "Celso Araujo");
}
