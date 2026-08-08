#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>

struct [[=metal::mapping::table{"collection_posts"}]] CollectionPost {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::int64_t user_id{};
    std::string title;
};

struct [[=metal::mapping::table{"collection_users"}]] CollectionUser {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;

    [[=metal::mapping::has_many<
        ^^CollectionPost::user_id,
        metal::mapping::cascade_mode::all>{}]]
    metal::has_many_collection<CollectionPost> posts;
};

static_assert(metal::reflect::validate_mapping<CollectionUser>());

int main() {
    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    auto dialect = std::make_shared<metal::SQLiteDialect>();
    db->execute(metal::create_table_sql<CollectionUser>(*dialect));
    db->execute(metal::create_table_sql<CollectionPost>(*dialect));

    metal::Session session{db, dialect};

    auto user = std::make_shared<CollectionUser>();
    user->name = "Celso";
    session.persist(user);
    session.commit();
    const auto user_id = user->id;

    session.clear();
    auto loaded = session.find<CollectionUser>(user_id);
    assert(loaded);
    assert(!loaded->posts.loaded());

    auto post = loaded->posts.add();
    post->title = "C++26";

    // MetalORM HasManyCollection.attach mutates the child FK immediately.
    assert(post->user_id == loaded->id);
    assert(loaded->posts.dirty());

    session.commit();
    assert(post->id != 0);
    assert(!loaded->posts.dirty());

    session.clear();
    auto reloaded = session.find<CollectionUser>(user_id);
    assert(reloaded && !reloaded->posts.loaded());
    const auto& posts = reloaded->posts.load();
    assert(posts.size() == 1);
    assert(posts[0]->user_id == user_id);
    assert(posts[0]->title == "C++26");

    assert(reloaded->posts.remove(posts[0]));
    session.commit();
    assert(!session.find<CollectionPost>(post->id));
}
