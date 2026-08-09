#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

struct [[=metal::mapping::table{"drf_comments"}]] DrfComment {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::int64_t post_id{};
    std::string body;
};

struct [[=metal::mapping::table{"drf_posts"}]] DrfPost {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::int64_t user_id{};
    std::string title;

    [[=metal::mapping::has_many<^^DrfComment::post_id>{}]]
    metal::has_many_collection<DrfComment> comments;
};

struct [[=metal::mapping::table{"drf_roles"}]] DrfRole {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::string name;
};

struct [[=metal::mapping::table{"drf_user_roles"}]] DrfUserRole {
    [[=metal::mapping::primary_key]] std::int64_t user_id{};
    [[=metal::mapping::primary_key]] std::int64_t role_id{};
};

struct [[=metal::mapping::table{"drf_users"}]] DrfUser {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::string name;

    [[=metal::mapping::has_many<^^DrfPost::user_id>{}]]
    metal::has_many_collection<DrfPost> posts;

    [[=metal::mapping::many_to_many<
        ^^DrfUserRole,
        ^^DrfUserRole::user_id,
        ^^DrfUserRole::role_id>{}]]
    metal::many_to_many_collection<DrfRole, DrfUserRole> roles;
};

static_assert(metal::reflect::validate_mapping<DrfComment>());
static_assert(metal::reflect::validate_mapping<DrfPost>());
static_assert(metal::reflect::validate_mapping<DrfRole>());
static_assert(metal::reflect::validate_mapping<DrfUserRole>());
static_assert(metal::reflect::validate_mapping<DrfUser>());

static std::vector<std::int64_t> ids(const std::vector<metal::Row>& rows) {
    std::vector<std::int64_t> out;
    for (const auto& row : rows) {
        out.push_back(metal::from_value<std::int64_t>(row.at("id")));
    }
    return out;
}

static metal::WhereInput scalar_where(
    std::string field,
    metal::FilterOperator op,
    metal::Value value,
    metal::StringFilterMode mode = metal::StringFilterMode::default_mode) {
    return metal::where_input(metal::FilterInput{{
        metal::filter_clause(std::move(field), op, std::move(value), mode)
    }});
}

int main() {
    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    metal::SQLiteDialect dialect;
    metal::Session session{db};

    db->execute(metal::create_table_sql<DrfUser>(dialect));
    db->execute(metal::create_table_sql<DrfPost>(dialect));
    db->execute(metal::create_table_sql<DrfComment>(dialect));
    db->execute(metal::create_table_sql<DrfRole>(dialect));
    db->execute(metal::create_table_sql<DrfUserRole>(dialect));

    db->execute(
        "INSERT INTO drf_users(id, name) VALUES "
        "(1, 'A'), (2, 'B'), (3, 'C'), (4, 'D');");
    db->execute(
        "INSERT INTO drf_posts(id, user_id, title) VALUES "
        "(10, 1, 'C++26'), (11, 1, 'SQLite'), "
        "(12, 3, 'C++ ORM'), (13, 4, 'Rust');");
    db->execute(
        "INSERT INTO drf_comments(id, post_id, body) VALUES "
        "(100, 10, 'needle inside'), (101, 11, 'other'), "
        "(102, 12, 'different');");
    db->execute(
        "INSERT INTO drf_roles(id, name) VALUES (200, 'admin'), (201, 'developer');");
    db->execute(
        "INSERT INTO drf_user_roles(user_id, role_id) VALUES "
        "(1, 200), (3, 201), (4, 200);");

    metal::WhereInput some_cpp;
    some_cpp.relations.push_back(
        metal::relation_filter("posts").some(
            scalar_where(
                "title",
                metal::FilterOperator::contains,
                metal::Value{std::string{"C++"}})));
    auto some_query = metal::apply_where(metal::select<DrfUser>(), some_cpp);
    const auto some_sql = some_query.compile(dialect);
    assert(some_sql.sql.find("EXISTS") != std::string::npos);
    assert((ids(db->execute(some_sql.sql, some_sql.params).rows) ==
            std::vector<std::int64_t>{1, 3}));

    metal::WhereInput none_rust;
    none_rust.relations.push_back(
        metal::relation_filter("posts").none(
            scalar_where(
                "title",
                metal::FilterOperator::contains,
                metal::Value{std::string{"Rust"}})));
    auto none_query = metal::apply_where(metal::select<DrfUser>(), none_rust);
    const auto none_sql = none_query.compile(dialect);
    assert(none_sql.sql.find("NOT EXISTS") != std::string::npos);
    assert((ids(db->execute(none_sql.sql, none_sql.params).rows) ==
            std::vector<std::int64_t>{1, 2, 3}));

    metal::WhereInput empty_posts;
    empty_posts.relations.push_back(metal::relation_filter("posts").empty());
    auto empty_query = metal::apply_where(metal::select<DrfUser>(), empty_posts);
    const auto empty_sql = empty_query.compile(dialect);
    assert((ids(db->execute(empty_sql.sql, empty_sql.params).rows) ==
            std::vector<std::int64_t>{2}));

    metal::WhereInput nonempty_posts;
    nonempty_posts.relations.push_back(metal::relation_filter("posts").not_empty());
    auto nonempty_query = metal::apply_where(metal::select<DrfUser>(), nonempty_posts);
    const auto nonempty_sql = nonempty_query.compile(dialect);
    assert((ids(db->execute(nonempty_sql.sql, nonempty_sql.params).rows) ==
            std::vector<std::int64_t>{1, 3, 4}));

    // Universal semantics: every(P) == NOT EXISTS(related row that does not satisfy P).
    // Empty collections therefore satisfy every(P) vacuously, matching logical quantification.
    metal::WhereInput every_cpp;
    every_cpp.relations.push_back(
        metal::relation_filter("posts").every(
            scalar_where(
                "title",
                metal::FilterOperator::contains,
                metal::Value{std::string{"C++"}})));
    auto every_query = metal::apply_where(metal::select<DrfUser>(), every_cpp);
    const auto every_sql = every_query.compile(dialect);
    assert(every_sql.sql.find("NOT EXISTS") != std::string::npos);
    assert((ids(db->execute(every_sql.sql, every_sql.params).rows) ==
            std::vector<std::int64_t>{2, 3}));

    // Recursive relation filtering: users with some post having some matching comment.
    metal::WhereInput comment_predicate = scalar_where(
        "body",
        metal::FilterOperator::contains,
        metal::Value{std::string{"NEEDLE"}},
        metal::StringFilterMode::insensitive);
    metal::WhereInput post_predicate;
    post_predicate.relations.push_back(
        metal::relation_filter("comments").some(std::move(comment_predicate)));
    metal::WhereInput nested;
    nested.relations.push_back(
        metal::relation_filter("posts").some(std::move(post_predicate)));

    auto nested_query = metal::apply_where(metal::select<DrfUser>(), nested);
    const auto nested_sql = nested_query.compile(dialect);
    assert(nested_sql.sql.find("LOWER") != std::string::npos);
    assert((ids(db->execute(nested_sql.sql, nested_sql.params).rows) ==
            std::vector<std::int64_t>{1}));

    metal::WhereInput admins;
    admins.relations.push_back(
        metal::relation_filter("roles").some(
            scalar_where(
                "name",
                metal::FilterOperator::equals,
                metal::Value{std::string{"admin"}})));
    auto admin_query = metal::apply_where(metal::select<DrfUser>(), admins);
    const auto admin_sql = admin_query.compile(dialect);
    assert(admin_sql.sql.find("drf_user_roles") != std::string::npos);
    assert((ids(db->execute(admin_sql.sql, admin_sql.params).rows) ==
            std::vector<std::int64_t>{1, 4}));

    bool disallowed = false;
    try {
        (void)metal::apply_where(
            metal::select<DrfUser>(),
            admins,
            metal::DtoMemberPolicy<>{},
            metal::DtoRelationPolicy<^^DrfUser::posts>{});
    } catch (const std::invalid_argument&) {
        disallowed = true;
    }
    assert(disallowed);

    bool missing_operator = false;
    try {
        metal::WhereInput invalid;
        invalid.relations.push_back(metal::relation_filter("posts"));
        (void)metal::apply_where(metal::select<DrfUser>(), invalid);
    } catch (const std::invalid_argument&) {
        missing_operator = true;
    }
    assert(missing_operator);

    metal::WhereInput paged_where;
    paged_where.relations.push_back(metal::relation_filter("posts").not_empty());
    const auto page = metal::execute_filtered_paged(
        metal::select<DrfUser>(),
        session,
        paged_where,
        metal::SortInput{.field = std::string{"name"}, .ascending = false},
        metal::PageOptions{.page = 1, .page_size = 2},
        metal::DtoMemberPolicy<>{},
        metal::DtoMemberPolicy<^^DrfUser::name>{},
        metal::DtoRelationPolicy<^^DrfUser::posts>{});
    assert(page.total_items == 3);
    assert(page.items.size() == 2);
    assert(page.items[0]->id == 4);
    assert(page.items[1]->id == 3);
    assert(page.total_pages == 2);
    assert(page.has_next_page);
}
