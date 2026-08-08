#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

struct [[=metal::mapping::table{"rp_posts"}]] RpPost {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::int64_t user_id{};
    std::string title;
};

struct [[=metal::mapping::table{"rp_roles"}]] RpRole {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::string name;
};

struct [[=metal::mapping::table{"rp_user_roles"}]] RpUserRole {
    [[=metal::mapping::primary_key]] std::int64_t user_id{};
    [[=metal::mapping::primary_key]] std::int64_t role_id{};
};

struct [[=metal::mapping::table{"rp_users"}]] RpUser {
    [[=metal::mapping::primary_key]] std::int64_t id{};
    std::string name;
    std::int64_t score{};

    [[=metal::mapping::has_many<^^RpPost::user_id>{}]]
    metal::has_many_collection<RpPost> posts;

    [[=metal::mapping::many_to_many<
        ^^RpUserRole,
        ^^RpUserRole::user_id,
        ^^RpUserRole::role_id>{}]]
    metal::many_to_many_collection<RpRole, RpUserRole> roles;
};

static_assert(metal::reflect::validate_mapping<RpUser>());
static_assert(metal::reflect::validate_mapping<RpPost>());
static_assert(metal::reflect::validate_mapping<RpRole>());
static_assert(metal::reflect::validate_mapping<RpUserRole>());

static std::vector<std::int64_t> row_ids(const std::vector<metal::Row>& rows) {
    std::vector<std::int64_t> out;
    for (const auto& row : rows) out.push_back(metal::from_value<std::int64_t>(row.at("id")));
    return out;
}

int main() {
    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    metal::SQLiteDialect dialect;

    db->execute(metal::create_table_sql<RpUser>(dialect));
    db->execute(metal::create_table_sql<RpPost>(dialect));
    db->execute(metal::create_table_sql<RpRole>(dialect));
    db->execute(metal::create_table_sql<RpUserRole>(dialect));

    db->execute(
        "INSERT INTO rp_users(id, name, score) VALUES "
        "(1, 'A', 10), (2, 'B', 20), (3, 'C', 20), (4, 'D', 30), (5, 'E', 40);");
    db->execute(
        "INSERT INTO rp_posts(id, user_id, title) VALUES "
        "(10, 1, 'C++26'), (11, 1, 'SQLite'), (12, 3, 'C++ ORM'), (13, 4, 'Rust');");
    db->execute("INSERT INTO rp_roles(id, name) VALUES (100, 'admin'), (200, 'developer');");
    db->execute("INSERT INTO rp_user_roles(user_id, role_id) VALUES (1, 100), (3, 200), (4, 100);");

    auto has_cpp = metal::where_has<^^RpUser::posts>(
        metal::select<RpUser>(),
        [](auto& posts) {
            posts.where(metal::like(metal::field<^^RpPost::title>, "C++%"));
        });
    const auto has_cpp_sql = has_cpp.compile(dialect);
    assert(has_cpp_sql.sql.find("EXISTS") != std::string::npos);
    assert((row_ids(db->execute(has_cpp_sql.sql, has_cpp_sql.params).rows) ==
            std::vector<std::int64_t>{1, 3}));

    auto no_posts = metal::where_has_not<^^RpUser::posts>(metal::select<RpUser>());
    const auto no_posts_sql = no_posts.compile(dialect);
    assert(no_posts_sql.sql.find("NOT EXISTS") != std::string::npos);
    assert((row_ids(db->execute(no_posts_sql.sql, no_posts_sql.params).rows) ==
            std::vector<std::int64_t>{2, 5}));

    auto admins = metal::where_relation<^^RpUser::roles>(
        metal::select<RpUser>(),
        metal::field<^^RpRole::name> == "admin");
    const auto admins_sql = admins.compile(dialect);
    assert(admins_sql.sql.find("rp_user_roles") != std::string::npos);
    assert((row_ids(db->execute(admins_sql.sql, admins_sql.params).rows) ==
            std::vector<std::int64_t>{1, 4}));

    auto base = metal::select<RpUser>()
        .where(metal::field<^^RpUser::score> >= 20)
        .order_by(metal::field<^^RpUser::id>);

    const auto page = metal::execute_paged(
        base, *db, dialect, metal::PageOptions{.page = 2, .page_size = 2});
    assert(page.total_items == 4);
    assert(page.page == 2);
    assert(page.page_size == 2);
    assert((row_ids(page.items) == std::vector<std::int64_t>{4, 5}));

    const std::vector<metal::CursorOrderTerm> id_order{
        metal::cursor_order(metal::field<^^RpUser::id>)};

    auto first = metal::execute_cursor(
        metal::select<RpUser>(),
        *db,
        dialect,
        id_order,
        metal::CursorPageOptions{.first = 2});
    assert((row_ids(first.items) == std::vector<std::int64_t>{1, 2}));
    assert(first.page_info.has_next_page);
    assert(!first.page_info.has_previous_page);
    assert(first.page_info.start_cursor.has_value());
    assert(first.page_info.end_cursor.has_value());

    auto second = metal::execute_cursor(
        metal::select<RpUser>(),
        *db,
        dialect,
        id_order,
        metal::CursorPageOptions{.first = 2, .after = first.page_info.end_cursor});
    assert((row_ids(second.items) == std::vector<std::int64_t>{3, 4}));
    assert(second.page_info.has_next_page);
    assert(second.page_info.has_previous_page);

    auto backwards = metal::execute_cursor(
        metal::select<RpUser>(),
        *db,
        dialect,
        id_order,
        metal::CursorPageOptions{.last = 2, .before = second.page_info.start_cursor});
    assert((row_ids(backwards.items) == std::vector<std::int64_t>{1, 2}));
    assert(backwards.page_info.has_next_page);

    const std::vector<metal::CursorOrderTerm> score_order{
        metal::cursor_order(metal::field<^^RpUser::score>, false),
        metal::cursor_order(metal::field<^^RpUser::id>)};
    auto scored = metal::execute_cursor(
        metal::select<RpUser>(),
        *db,
        dialect,
        score_order,
        metal::CursorPageOptions{.first = 3});
    assert((row_ids(scored.items) == std::vector<std::int64_t>{5, 4, 2}));

    bool signature_rejected = false;
    try {
        (void)metal::execute_cursor(
            metal::select<RpUser>(),
            *db,
            dialect,
            id_order,
            metal::CursorPageOptions{.first = 2, .after = scored.page_info.end_cursor});
    } catch (const std::invalid_argument&) {
        signature_rejected = true;
    }
    assert(signature_rejected);

    return 0;
}
