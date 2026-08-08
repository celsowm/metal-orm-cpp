#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct [[=metal::mapping::table{"q_posts"}]] QueryPost {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::int64_t user_id{};
    std::string title;
};

struct [[=metal::mapping::table{"q_roles"}]] QueryRole {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;
};

struct [[=metal::mapping::table{"q_user_roles"}]] QueryUserRole {
    [[=metal::mapping::primary_key]] std::int64_t user_id{};
    [[=metal::mapping::primary_key]] std::int64_t role_id{};
};

struct [[=metal::mapping::table{"q_users"}]] QueryUser {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;
    std::optional<std::string> nickname;

    [[=metal::mapping::has_many<^^QueryPost::user_id>{}]]
    metal::collection<QueryPost> posts;

    [[=metal::mapping::many_to_many<
        ^^QueryUserRole,
        ^^QueryUserRole::user_id,
        ^^QueryUserRole::role_id>{}]]
    metal::collection<QueryRole> roles;
};

template <typename Q>
concept CanFilterByPost = requires(Q query) {
    query.where(metal::field<^^QueryPost::title> == "reflection");
};

using RootQuery = metal::SelectQuery<QueryUser>;
using JoinedPostQuery = decltype(metal::select<QueryUser>().join<^^QueryUser::posts>());

static_assert(!CanFilterByPost<RootQuery>);
static_assert(CanFilterByPost<JoinedPostQuery>);

int main() {
    metal::SQLiteDialect dialect;

    const std::vector<std::int64_t> ids{1, 2, 3};
    auto grouped = metal::select<QueryUser>()
        .join<^^QueryUser::posts>()
        .project(metal::field<^^QueryUser::name>)
        .project(metal::count(metal::field<^^QueryPost::id>).as("post_count"))
        .where(
            metal::like(metal::field<^^QueryUser::name>, "C%") &&
            metal::in(metal::field<^^QueryPost::id>, ids))
        .group_by(metal::field<^^QueryUser::name>)
        .having(metal::count(metal::field<^^QueryPost::id>) > 1)
        .order_by(metal::field<^^QueryUser::name>, false)
        .limit(5)
        .offset(0)
        .compile(dialect);

    assert(grouped.sql ==
        "SELECT \"t0\".\"name\", COUNT(\"t1\".\"id\") AS \"post_count\" "
        "FROM \"q_users\" AS \"t0\" "
        "INNER JOIN \"q_posts\" AS \"t1\" ON \"t0\".\"id\" = \"t1\".\"user_id\" "
        "WHERE (\"t0\".\"name\" LIKE ? AND \"t1\".\"id\" IN (?, ?, ?)) "
        "GROUP BY \"t0\".\"name\" "
        "HAVING COUNT(\"t1\".\"id\") > ? "
        "ORDER BY \"t0\".\"name\" DESC LIMIT 5 OFFSET 0;");
    assert(grouped.params.size() == 5);

    auto many_to_many = metal::select<QueryUser>()
        .left_join<^^QueryUser::roles>()
        .project(metal::field<^^QueryUser::name>)
        .project_as(metal::field<^^QueryRole::name>, "role_name")
        .where(metal::is_not_null(metal::field<^^QueryUser::nickname>))
        .compile(dialect);

    assert(many_to_many.sql ==
        "SELECT \"t0\".\"name\", \"t1\".\"name\" AS \"role_name\" "
        "FROM \"q_users\" AS \"t0\" "
        "LEFT JOIN \"q_user_roles\" AS \"p0\" ON \"p0\".\"user_id\" = \"t0\".\"id\" "
        "LEFT JOIN \"q_roles\" AS \"t1\" ON \"t1\".\"id\" = \"p0\".\"role_id\" "
        "WHERE \"t0\".\"nickname\" IS NOT NULL;");

    auto post_users = metal::select<QueryPost>()
        .project(metal::field<^^QueryPost::user_id>)
        .where(metal::like(metal::field<^^QueryPost::title>, "%C++%"));

    auto subquery = metal::select<QueryUser>()
        .where(metal::in(metal::field<^^QueryUser::id>, post_users))
        .compile(dialect);

    assert(subquery.sql ==
        "SELECT \"id\", \"name\", \"nickname\" FROM \"q_users\" "
        "WHERE \"id\" IN (SELECT \"user_id\" FROM \"q_posts\" WHERE \"title\" LIKE ?);");
    assert(subquery.params.size() == 1);

    auto empty_in = metal::select<QueryUser>()
        .where(metal::in(metal::field<^^QueryUser::id>, std::vector<std::int64_t>{}))
        .compile(dialect);
    assert(empty_in.sql.find("WHERE 0 = 1") != std::string::npos);

    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    db->execute(metal::create_table_sql<QueryUser>(dialect));
    db->execute(metal::create_table_sql<QueryPost>(dialect));
    db->execute(metal::create_table_sql<QueryRole>(dialect));
    db->execute(metal::create_table_sql<QueryUserRole>(dialect));

    db->execute(
        "INSERT INTO q_users(id, name, nickname) VALUES (?, ?, ?), (?, ?, ?);",
        {std::int64_t{1}, std::string{"Celso"}, std::string{"celsowm"},
         std::int64_t{2}, std::string{"Levi"}, nullptr});
    db->execute(
        "INSERT INTO q_posts(id, user_id, title) VALUES (?, ?, ?), (?, ?, ?), (?, ?, ?);",
        {std::int64_t{1}, std::int64_t{1}, std::string{"C++26 reflection"},
         std::int64_t{2}, std::int64_t{1}, std::string{"C++ ORM"},
         std::int64_t{3}, std::int64_t{2}, std::string{"SQLite"}});
    db->execute(
        "INSERT INTO q_roles(id, name) VALUES (?, ?), (?, ?);",
        {std::int64_t{10}, std::string{"admin"}, std::int64_t{20}, std::string{"developer"}});
    db->execute(
        "INSERT INTO q_user_roles(user_id, role_id) VALUES (?, ?), (?, ?);",
        {std::int64_t{1}, std::int64_t{10}, std::int64_t{1}, std::int64_t{20}});

    const auto grouped_result = db->execute(grouped.sql, grouped.params);
    assert(grouped_result.rows.size() == 1);
    assert(metal::from_value<std::string>(grouped_result.rows[0].at("name")) == "Celso");
    assert(metal::from_value<std::int64_t>(grouped_result.rows[0].at("post_count")) == 2);

    const auto relation_result = db->execute(many_to_many.sql, many_to_many.params);
    assert(relation_result.rows.size() == 2);
    assert(metal::from_value<std::string>(relation_result.rows[0].at("name")) == "Celso");

    const auto subquery_result = db->execute(subquery.sql, subquery.params);
    assert(subquery_result.rows.size() == 1);
    assert(metal::from_value<std::string>(subquery_result.rows[0].at("name")) == "Celso");

    const auto empty_result = db->execute(empty_in.sql, empty_in.params);
    assert(empty_result.rows.empty());
}
