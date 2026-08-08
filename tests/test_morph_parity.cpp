#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct [[=metal::mapping::table{"morph_covers"}]] MorphCover {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::optional<std::int64_t> imageable_id;
    std::optional<std::string> imageable_type;
    std::string name;
};

struct [[=metal::mapping::table{"morph_attachments"}]] MorphAttachment {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::optional<std::int64_t> attachable_id;
    std::optional<std::string> attachable_type;
    std::string name;
};

struct [[=metal::mapping::table{"morph_posts"}]] MorphPost {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string title;

    [[=metal::mapping::morph_one<
        ^^MorphCover::imageable_type,
        ^^MorphCover::imageable_id,
        "post",
        metal::mapping::cascade_mode::all>{}]]
    metal::morph_one_reference<MorphCover> cover;

    [[=metal::mapping::morph_many<
        ^^MorphAttachment::attachable_type,
        ^^MorphAttachment::attachable_id,
        "post",
        metal::mapping::cascade_mode::all>{}]]
    metal::morph_many_collection<MorphAttachment> attachments;
};

struct [[=metal::mapping::table{"morph_subject_posts"}]] MorphSubjectPost {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string title;
};

struct [[=metal::mapping::table{"morph_subject_videos"}]] MorphSubjectVideo {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string title;
};

struct [[=metal::mapping::table{"morph_activities"}]] MorphActivity {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string action;
    std::optional<std::int64_t> subject_id;
    std::optional<std::string> subject_type;

    [[=metal::mapping::morph_to<
        ^^MorphActivity::subject_type,
        ^^MorphActivity::subject_id,
        metal::mapping::cascade_mode::persist,
        metal::mapping::morph_target<"post", ^^MorphSubjectPost>,
        metal::mapping::morph_target<"video", ^^MorphSubjectVideo>>{}]]
    metal::morph_to_reference<MorphSubjectPost, MorphSubjectVideo> subject;
};

static_assert(metal::reflect::validate_mapping<MorphCover>());
static_assert(metal::reflect::validate_mapping<MorphAttachment>());
static_assert(metal::reflect::validate_mapping<MorphPost>());
static_assert(metal::reflect::validate_mapping<MorphActivity>());

int main() {
    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    auto dialect = std::make_shared<metal::SQLiteDialect>();

    db->execute(metal::create_table_sql<MorphCover>(*dialect));
    db->execute(metal::create_table_sql<MorphAttachment>(*dialect));
    db->execute(metal::create_table_sql<MorphPost>(*dialect));
    db->execute(metal::create_table_sql<MorphSubjectPost>(*dialect));
    db->execute(metal::create_table_sql<MorphSubjectVideo>(*dialect));
    db->execute(metal::create_table_sql<MorphActivity>(*dialect));

    metal::Session session{db, dialect};

    // Parent and children are all new in the same commit. The first UoW flush
    // generates the parent key; the relation processor then rebinds morph keys
    // and the second UoW flush persists the final discriminator/id pair.
    auto post = std::make_shared<MorphPost>();
    post->title = "C++26";
    auto cover = std::make_shared<MorphCover>();
    cover->name = "hero";
    post->cover.set(cover);
    auto attachment = post->attachments.add();
    attachment->name = "spec.pdf";
    session.persist(post);
    session.commit();

    assert(post->id != 0);
    assert(cover->id != 0);
    assert(cover->imageable_id == post->id);
    assert(cover->imageable_type == "post");
    assert(attachment->id != 0);
    assert(attachment->attachable_id == post->id);
    assert(attachment->attachable_type == "post");

    auto cover_row = db->execute(
        "SELECT imageable_id, imageable_type FROM morph_covers WHERE id = ?;",
        {cover->id});
    auto attachment_row = db->execute(
        "SELECT attachable_id, attachable_type FROM morph_attachments WHERE id = ?;",
        {attachment->id});
    assert(metal::from_value<std::int64_t>(cover_row.rows[0].at("imageable_id")) == post->id);
    assert(metal::from_value<std::string>(cover_row.rows[0].at("imageable_type")) == "post");
    assert(metal::from_value<std::int64_t>(attachment_row.rows[0].at("attachable_id")) == post->id);
    assert(metal::from_value<std::string>(attachment_row.rows[0].at("attachable_type")) == "post");

    const auto post_id = post->id;
    const auto cover_id = cover->id;
    const auto attachment_id = attachment->id;

    session.clear();
    auto loaded_post = session.find<MorphPost>(post_id);
    assert(loaded_post);
    assert(!loaded_post->cover.loaded());
    assert(!loaded_post->attachments.loaded());

    auto loaded_cover = loaded_post->cover.load();
    const auto& loaded_attachments = loaded_post->attachments.load();
    assert(loaded_cover && loaded_cover->name == "hero");
    assert(loaded_attachments.size() == 1);
    assert(loaded_attachments[0]->name == "spec.pdf");
    assert(session.find<MorphCover>(cover_id) == loaded_cover);
    assert(session.find<MorphAttachment>(attachment_id) == loaded_attachments[0]);

    session.clear();
    auto eager_posts = session.query<MorphPost>()
        .include<^^MorphPost::cover>()
        .include<^^MorphPost::attachments>()
        .all();
    assert(eager_posts.size() == 1);
    assert(eager_posts[0]->cover.loaded());
    assert(eager_posts[0]->attachments.loaded());

    auto eager_cover = eager_posts[0]->cover.get();
    auto eager_attachment = eager_posts[0]->attachments[0];
    eager_posts[0]->cover.reset();
    assert(eager_posts[0]->attachments.remove(eager_attachment));
    session.commit();
    assert(!session.find<MorphCover>(eager_cover->id));
    assert(!session.find<MorphAttachment>(eager_attachment->id));

    auto activity = std::make_shared<MorphActivity>();
    activity->action = "created";
    session.persist(activity);
    session.commit();

    auto subject_post = std::make_shared<MorphSubjectPost>();
    subject_post->title = "static reflection";
    activity->subject.set(subject_post);
    session.commit();

    assert(subject_post->id != 0);
    assert(activity->subject_type == "post");
    assert(activity->subject_id == subject_post->id);

    const auto activity_id = activity->id;
    const auto subject_post_id = subject_post->id;
    session.clear();

    auto loaded_activity = session.find<MorphActivity>(activity_id);
    assert(loaded_activity && !loaded_activity->subject.loaded());
    loaded_activity->subject.load();
    auto loaded_subject_post = loaded_activity->subject.get_as<MorphSubjectPost>();
    assert(loaded_subject_post);
    assert(loaded_subject_post->id == subject_post_id);
    assert(session.find<MorphSubjectPost>(subject_post_id) == loaded_subject_post);

    auto video = std::make_shared<MorphSubjectVideo>();
    video->title = "C++26 demo";
    loaded_activity->subject.set(video);
    session.commit();
    assert(video->id != 0);
    assert(loaded_activity->subject_type == "video");
    assert(loaded_activity->subject_id == video->id);

    session.clear();
    loaded_activity = session.query<MorphActivity>()
        .include<^^MorphActivity::subject>()
        .first();
    assert(loaded_activity);
    assert(loaded_activity->subject.loaded());
    auto loaded_video = loaded_activity->subject.get_as<MorphSubjectVideo>();
    assert(loaded_video && loaded_video->title == "C++26 demo");

    loaded_activity->subject.reset();
    assert(!loaded_activity->subject_type.has_value());
    assert(!loaded_activity->subject_id.has_value());
    session.commit();

    auto row = db->execute(
        "SELECT subject_type, subject_id FROM morph_activities WHERE id = ?;",
        {activity_id});
    assert(row.rows.size() == 1);
    assert(std::holds_alternative<std::nullptr_t>(row.rows[0].at("subject_type")));
    assert(std::holds_alternative<std::nullptr_t>(row.rows[0].at("subject_id")));
}
