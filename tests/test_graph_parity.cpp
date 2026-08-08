#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct GraphUser;

struct [[=metal::mapping::table{"graph_posts"}]] GraphPost {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::int64_t user_id{};
    std::string title;

    [[=metal::mapping::belongs_to<^^GraphPost::user_id>{}]]
    metal::belongs_to_reference<GraphUser> author;
};

struct [[=metal::mapping::table{"graph_profiles"}]] GraphProfile {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::int64_t user_id{};
    std::string bio;
};

struct [[=metal::mapping::table{"graph_roles"}]] GraphRole {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;
};

struct [[=metal::mapping::table{"graph_user_roles"}]] GraphUserRole {
    [[=metal::mapping::primary_key]] std::int64_t user_id{};
    [[=metal::mapping::primary_key]] std::int64_t role_id{};
    std::string label;
};

struct GraphCreated {
    std::int64_t id{};
};

struct [[=metal::mapping::table{"graph_users"}]] GraphUser {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;

    [[=metal::mapping::has_one<^^GraphProfile::user_id>{}]]
    metal::has_one_reference<GraphProfile> profile;

    [[=metal::mapping::has_many<
        ^^GraphPost::user_id,
        metal::mapping::cascade_mode::remove>{}]]
    metal::has_many_collection<GraphPost> posts;

    [[=metal::mapping::many_to_many<
        ^^GraphUserRole,
        ^^GraphUserRole::user_id,
        ^^GraphUserRole::role_id,
        metal::mapping::cascade_mode::link>{}]]
    metal::many_to_many_collection<GraphRole, GraphUserRole> roles;

    [[=metal::mapping::ignore]]
    metal::domain_event_queue<GraphCreated> events;
};

static_assert(metal::reflect::validate_mapping<GraphPost>());
static_assert(metal::reflect::validate_mapping<GraphUser>());
static_assert(std::same_as<
    metal::reflect::single_target_t<metal::belongs_to_reference<GraphUser>>,
    GraphUser>);
static_assert(std::same_as<
    metal::reflect::single_target_t<metal::has_one_reference<GraphProfile>>,
    GraphProfile>);

int main() {
    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    auto dialect = std::make_shared<metal::SQLiteDialect>();
    db->execute(metal::create_table_sql<GraphPost>(*dialect));
    db->execute(metal::create_table_sql<GraphProfile>(*dialect));
    db->execute(metal::create_table_sql<GraphRole>(*dialect));
    db->execute(metal::create_table_sql<GraphUserRole>(*dialect));
    db->execute(metal::create_table_sql<GraphUser>(*dialect));

    metal::Session session{db, dialect};
    int hook_calls = 0;
    int event_calls = 0;
    session.register_table_hooks<GraphUser>({
        .before_insert = [&](metal::Session&, GraphUser& user) {
            ++hook_calls;
            user.name += "!";
        },
        .after_insert = [](metal::Session&, GraphUser& user) {
            user.events.raise(GraphCreated{user.id});
        }
    });
    session.register_domain_event_handler<GraphCreated>(
        [&](const GraphCreated& event, metal::Session&) {
            ++event_calls;
            assert(event.id != 0);
        });

    metal::pivot_patch<GraphUserRole> owner;
    owner.set<^^GraphUserRole::label>(std::string{"owner"});

    auto create = metal::graph<GraphUser>()
        .set<^^GraphUser::name>(std::string{"Celso"})
        .relation<^^GraphUser::profile>(
            metal::graph<GraphProfile>()
                .set<^^GraphProfile::bio>(std::string{"C++26"}))
        .relation<^^GraphUser::posts>([](auto& posts) {
            posts.add(metal::graph<GraphPost>()
                .set<^^GraphPost::title>(std::string{"Reflection"}));
            posts.add(metal::graph<GraphPost>()
                .set<^^GraphPost::title>(std::string{"Splicing"}));
        })
        .relation<^^GraphUser::roles>([&](auto& roles) {
            roles.add(
                metal::graph<GraphRole>()
                    .set<^^GraphRole::name>(std::string{"admin"}),
                owner);
        });

    auto user = metal::save_graph(session, create);
    assert(user);
    assert(user->id != 0);
    assert(user->name == "Celso!");
    assert(hook_calls == 1);
    assert(event_calls == 1);

    auto profile_rows = db->execute(
        "SELECT user_id, bio FROM graph_profiles WHERE user_id = ?;",
        {user->id});
    assert(profile_rows.rows.size() == 1);
    assert(metal::from_value<std::int64_t>(profile_rows.rows[0].at("user_id")) == user->id);
    assert(metal::from_value<std::string>(profile_rows.rows[0].at("bio")) == "C++26");

    auto post_rows = db->execute(
        "SELECT id, title FROM graph_posts WHERE user_id = ? ORDER BY id;",
        {user->id});
    assert(post_rows.rows.size() == 2);
    const auto removed_post_id = metal::from_value<std::int64_t>(post_rows.rows[0].at("id"));
    const auto kept_post_id = metal::from_value<std::int64_t>(post_rows.rows[1].at("id"));

    auto pivot_rows = db->execute(
        "SELECT p.label, r.name FROM graph_user_roles p "
        "JOIN graph_roles r ON r.id = p.role_id WHERE p.user_id = ?;",
        {user->id});
    assert(pivot_rows.rows.size() == 1);
    assert(metal::from_value<std::string>(pivot_rows.rows[0].at("label")) == "owner");
    assert(metal::from_value<std::string>(pivot_rows.rows[0].at("name")) == "admin");

    const auto user_id = user->id;
    session.clear();

    auto update = metal::graph<GraphUser>()
        .set<^^GraphUser::id>(user_id)
        .set<^^GraphUser::name>(std::string{"Celso Araujo"})
        .relation<^^GraphUser::posts>([&](auto& posts) {
            posts.add_id(kept_post_id);
        });

    auto updated = metal::update_graph(
        session,
        update,
        metal::GraphOptions{.prune_missing = true});
    assert(updated);
    assert(updated->name == "Celso Araujo");

    auto old_post = db->execute(
        "SELECT COUNT(*) AS c FROM graph_posts WHERE id = ?;",
        {removed_post_id});
    auto kept_post = db->execute(
        "SELECT COUNT(*) AS c FROM graph_posts WHERE id = ?;",
        {kept_post_id});
    assert(metal::from_value<std::int64_t>(old_post.rows[0].at("c")) == 0);
    assert(metal::from_value<std::int64_t>(kept_post.rows[0].at("c")) == 1);

    auto patch = metal::graph<GraphUser>()
        .set<^^GraphUser::id>(user_id)
        .set<^^GraphUser::name>(std::string{"Patched"});
    auto patched = metal::patch_graph(session, patch);
    assert(patched);
    assert(patched->name == "Patched");

    auto post_count_after_patch = db->execute(
        "SELECT COUNT(*) AS c FROM graph_posts WHERE user_id = ?;",
        {user_id});
    assert(metal::from_value<std::int64_t>(post_count_after_patch.rows[0].at("c")) == 1);

    session.clear();
    auto comment_graph = metal::graph<GraphPost>()
        .set<^^GraphPost::title>(std::string{"Nested belongs-to"})
        .relation<^^GraphPost::author>(
            metal::graph<GraphUser>()
                .set<^^GraphUser::name>(std::string{"New Parent"}));
    auto nested = metal::save_graph(session, comment_graph);
    assert(nested);
    assert(nested->id != 0);
    assert(nested->user_id != 0);
    assert(nested->author);
    assert(nested->author->id == nested->user_id);

    auto missing = metal::graph<GraphUser>()
        .set<^^GraphUser::id>(std::int64_t{999999})
        .set<^^GraphUser::name>(std::string{"missing"});
    assert(!metal::update_graph(session, missing));
}
