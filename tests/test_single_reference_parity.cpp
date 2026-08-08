#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

struct SingleUser;

struct [[=metal::mapping::table{"single_profiles"}]] SingleProfile {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::optional<std::int64_t> user_id;
    std::string bio;
};

struct [[=metal::mapping::table{"single_users"}]] SingleUser {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;

    [[=metal::mapping::has_one<
        ^^SingleProfile::user_id,
        metal::mapping::cascade_mode::persist>{}]]
    metal::has_one_reference<SingleProfile> profile;
};

struct [[=metal::mapping::table{"single_comments"}]] SingleComment {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::optional<std::int64_t> user_id;
    std::string body;

    [[=metal::mapping::belongs_to<^^SingleComment::user_id>{}]]
    metal::belongs_to_reference<SingleUser> author;
};

static_assert(metal::reflect::validate_mapping<SingleProfile>());
static_assert(metal::reflect::validate_mapping<SingleUser>());
static_assert(metal::reflect::validate_mapping<SingleComment>());
static_assert(metal::reflect::is_has_one_reference_v<
    metal::reflect::member_type_t<^^SingleUser::profile>>);
static_assert(metal::reflect::is_belongs_to_reference_v<
    metal::reflect::member_type_t<^^SingleComment::author>>);

int main() {
    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    auto dialect = std::make_shared<metal::SQLiteDialect>();
    db->execute(metal::create_table_sql<SingleProfile>(*dialect));
    db->execute(metal::create_table_sql<SingleUser>(*dialect));
    db->execute(metal::create_table_sql<SingleComment>(*dialect));

    metal::Session session{db, dialect};

    // A new root and new has-one target can be assembled before either object
    // is Session-bound. Cascade persist tracks the target, the first flush
    // generates the root key, and relation processing repairs the child FK.
    auto user = std::make_shared<SingleUser>();
    user->name = "Celso";
    auto profile = std::make_shared<SingleProfile>();
    profile->bio = "C++26";
    user->profile.set(profile);
    assert(user->profile.dirty());
    session.persist(user);
    session.commit();

    assert(user->id != 0);
    assert(profile->id != 0);
    assert(profile->user_id == user->id);
    assert(!user->profile.dirty());

    auto profile_row = db->execute(
        "SELECT user_id, bio FROM single_profiles WHERE id = ?;",
        {profile->id});
    assert(profile_row.rows.size() == 1);
    assert(metal::from_value<std::int64_t>(profile_row.rows[0].at("user_id")) == user->id);

    // Belongs-to can likewise be assembled before the child is tracked. The
    // relation processor synchronizes the root FK after the first UoW flush.
    auto comment = std::make_shared<SingleComment>();
    comment->body = "typed reference";
    comment->author.set(user);
    session.persist(comment);
    session.commit();
    assert(comment->id != 0);
    assert(comment->user_id == user->id);
    assert(!comment->author.dirty());

    const auto user_id = user->id;
    const auto comment_id = comment->id;
    const auto profile_id = profile->id;
    session.clear();

    // Lazy belongs-to loading is Session-bound and hydration establishes the
    // baseline instead of masquerading as a user mutation.
    auto loaded_comment = session.find<SingleComment>(comment_id);
    assert(loaded_comment);
    assert(!loaded_comment->author.loaded());
    const auto& lazy_author = loaded_comment->author.load();
    assert(lazy_author);
    assert(lazy_author->id == user_id);
    assert(loaded_comment->author.loaded());
    assert(!loaded_comment->author.dirty());
    assert(session.find<SingleUser>(user_id) == lazy_author);

    // Setting a loaded belongs-to reference changes the FK immediately, and
    // commit accepts the new baseline only after persistence succeeds.
    auto other = std::make_shared<SingleUser>();
    other->name = "Levi";
    session.persist(other);
    session.commit();
    loaded_comment->author.set(other);
    assert(loaded_comment->author.dirty());
    assert(loaded_comment->user_id == other->id);
    session.commit();
    assert(!loaded_comment->author.dirty());

    auto comment_fk = db->execute(
        "SELECT user_id FROM single_comments WHERE id = ?;",
        {comment_id});
    assert(metal::from_value<std::int64_t>(comment_fk.rows[0].at("user_id")) == other->id);

    loaded_comment->author.reset();
    assert(loaded_comment->author.dirty());
    assert(!loaded_comment->user_id.has_value());
    session.commit();
    assert(!loaded_comment->author.dirty());
    comment_fk = db->execute(
        "SELECT user_id FROM single_comments WHERE id = ?;",
        {comment_id});
    assert(std::holds_alternative<std::nullptr_t>(comment_fk.rows[0].at("user_id")));

    // Lazy has-one loading resolves the child by reflected local/foreign keys.
    session.clear();
    auto loaded_user = session.find<SingleUser>(user_id);
    assert(loaded_user);
    assert(!loaded_user->profile.loaded());
    const auto& lazy_profile = loaded_user->profile.load();
    assert(lazy_profile);
    assert(lazy_profile->id == profile_id);
    assert(!loaded_user->profile.dirty());

    // Replacing a has-one target detaches the old nullable FK and attaches the
    // new child. Cascade persist handles the new child automatically.
    auto replacement = std::make_shared<SingleProfile>();
    replacement->bio = "replacement";
    loaded_user->profile.set(replacement);
    assert(loaded_user->profile.dirty());
    session.commit();
    assert(replacement->id != 0);
    assert(replacement->user_id == user_id);
    assert(!loaded_user->profile.dirty());

    auto old_profile_fk = db->execute(
        "SELECT user_id FROM single_profiles WHERE id = ?;",
        {profile_id});
    assert(std::holds_alternative<std::nullptr_t>(old_profile_fk.rows[0].at("user_id")));
    auto new_profile_fk = db->execute(
        "SELECT user_id FROM single_profiles WHERE id = ?;",
        {replacement->id});
    assert(metal::from_value<std::int64_t>(new_profile_fk.rows[0].at("user_id")) == user_id);

    loaded_user->profile.reset();
    assert(loaded_user->profile.dirty());
    session.commit();
    assert(!loaded_user->profile.dirty());
    new_profile_fk = db->execute(
        "SELECT user_id FROM single_profiles WHERE id = ?;",
        {replacement->id});
    assert(std::holds_alternative<std::nullptr_t>(new_profile_fk.rows[0].at("user_id")));

    // Reference state participates in the generic 0.0.14 runtime checkpoint.
    loaded_user->profile.set(replacement);
    session.commit();
    assert(replacement->user_id == user_id);
    assert(!loaded_user->profile.dirty());

    bool rolled_back = false;
    try {
        session.transaction([&](metal::Session&) {
            loaded_user->profile.reset();
            assert(loaded_user->profile.dirty());
            throw std::runtime_error("rollback reference");
        });
    } catch (const std::runtime_error&) {
        rolled_back = true;
    }
    assert(rolled_back);
    assert(loaded_user->profile.get() == replacement);
    assert(!loaded_user->profile.dirty());
    assert(replacement->user_id == user_id);
}
