#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct [[=metal::mapping::table{"parity_roles"}]] ParityRole {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;
};

struct [[=metal::mapping::table{"parity_user_roles"}]] ParityUserRole {
    [[=metal::mapping::primary_key]] std::int64_t user_id{};
    [[=metal::mapping::primary_key]] std::int64_t role_id{};
    std::string label;
    std::int64_t weight{};
};

struct [[=metal::mapping::table{"parity_users"}]] ParityUser {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;

    [[=metal::mapping::many_to_many<
        ^^ParityUserRole,
        ^^ParityUserRole::user_id,
        ^^ParityUserRole::role_id,
        metal::mapping::cascade_mode::remove>{}]]
    metal::many_to_many_collection<ParityRole, ParityUserRole> roles;
};

struct [[=metal::mapping::table{"parity_link_users"}]] ParityLinkUser {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;

    [[=metal::mapping::many_to_many<
        ^^ParityUserRole,
        ^^ParityUserRole::user_id,
        ^^ParityUserRole::role_id,
        metal::mapping::cascade_mode::link>{}]]
    metal::many_to_many_collection<ParityRole, ParityUserRole> roles;
};

struct [[=metal::mapping::table{"parity_manual_keys"}]] ParityManualKey {
    [[=metal::mapping::primary_key]]
    std::int64_t id{};
    std::string name;
};

static_assert(metal::reflect::validate_mapping<ParityUser>());
static_assert(metal::reflect::validate_mapping<ParityLinkUser>());
static_assert(metal::reflect::validate_mapping<ParityManualKey>());
static_assert(metal::mapping::cascades_remove(metal::mapping::cascade_mode::remove));
static_assert(!metal::mapping::cascades_persist(metal::mapping::cascade_mode::link));
static_assert(!metal::mapping::cascades_remove(metal::mapping::cascade_mode::link));

int main() {
    metal::SQLiteDialect dialect;

    const auto insert = metal::InsertQueryBuilder{"parity_roles"}
        .values({metal::DmlAssignment{"name", std::string{"admin"}}})
        .compile(dialect);
    assert(insert.sql == "INSERT INTO \"parity_roles\" (\"name\") VALUES (?);");

    const auto update = metal::UpdateQueryBuilder{"parity_roles"}
        .set({metal::DmlAssignment{"name", std::string{"developer"}}})
        .where_eq("id", std::int64_t{1})
        .compile(dialect);
    assert(update.sql == "UPDATE \"parity_roles\" SET \"name\" = ? WHERE \"id\" = ?;");

    const auto erase = metal::DeleteQueryBuilder{"parity_roles"}
        .where_eq("id", std::int64_t{1})
        .compile(dialect);
    assert(erase.sql == "DELETE FROM \"parity_roles\" WHERE \"id\" = ?;");

    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    auto dialect_ptr = std::make_shared<metal::SQLiteDialect>();
    db->execute(metal::create_table_sql<ParityRole>(*dialect_ptr));
    db->execute(metal::create_table_sql<ParityUserRole>(*dialect_ptr));
    db->execute(metal::create_table_sql<ParityUser>(*dialect_ptr));
    db->execute(metal::create_table_sql<ParityManualKey>(*dialect_ptr));

    metal::Session session{db, dialect_ptr};

    auto admin = std::make_shared<ParityRole>();
    admin->name = "admin";
    auto developer = std::make_shared<ParityRole>();
    developer->name = "developer";
    auto user = std::make_shared<ParityUser>();
    user->name = "Celso";

    session.persist(admin);
    session.persist(developer);
    session.persist(user);

    metal::pivot_patch<ParityUserRole> owner_patch;
    owner_patch
        .set<^^ParityUserRole::label>(std::string{"owner"})
        .set<^^ParityUserRole::weight>(std::int64_t{10});
    user->roles.attach(admin, owner_patch);
    session.commit();

    assert(admin->id != 0);
    assert(developer->id != 0);
    assert(user->id != 0);

    auto pivot_row = db->execute(
        "SELECT label, weight FROM parity_user_roles WHERE user_id = ? AND role_id = ?;",
        {user->id, admin->id});
    assert(pivot_row.rows.size() == 1);
    assert(metal::from_value<std::string>(pivot_row.rows[0].at("label")) == "owner");
    assert(metal::from_value<std::int64_t>(pivot_row.rows[0].at("weight")) == 10);

    session.clear();
    auto loaded = session.query<ParityUser>().first();
    assert(loaded);
    assert(!loaded->roles.loaded());
    const auto& lazy_items = loaded->roles.load();
    assert(loaded->roles.loaded());
    assert(lazy_items.size() == 1);
    assert(loaded->roles.get_items().size() == 1);

    auto loaded_admin = loaded->roles[0];
    const auto* hydrated_pivot = loaded->roles.pivot(loaded_admin);
    assert(hydrated_pivot);
    assert(hydrated_pivot->label == "owner");
    assert(hydrated_pivot->weight == 10);

    // Partial<TPivot> parity: patch only label. Weight must remain untouched.
    metal::pivot_patch<ParityUserRole> label_only;
    label_only.set<^^ParityUserRole::label>(std::string{"primary"});
    loaded->roles.attach(loaded_admin, label_only);

    const auto* locally_patched = loaded->roles.pivot(loaded_admin);
    assert(locally_patched);
    assert(locally_patched->label == "primary");
    assert(locally_patched->weight == 10);
    session.commit();

    pivot_row = db->execute(
        "SELECT label, weight FROM parity_user_roles WHERE user_id = ? AND role_id = ?;",
        {loaded->id, loaded_admin->id});
    assert(pivot_row.rows.size() == 1);
    assert(metal::from_value<std::string>(pivot_row.rows[0].at("label")) == "primary");
    assert(metal::from_value<std::int64_t>(pivot_row.rows[0].at("weight")) == 10);

    metal::pivot_patch<ParityUserRole> secondary_patch;
    secondary_patch
        .set<^^ParityUserRole::label>(std::string{"secondary"})
        .set<^^ParityUserRole::weight>(std::int64_t{5});
    loaded->roles.attach(developer->id, secondary_patch);
    session.commit();

    auto pivot_count = db->execute(
        "SELECT COUNT(*) AS c FROM parity_user_roles WHERE user_id = ?;",
        {loaded->id});
    assert(metal::from_value<std::int64_t>(pivot_count.rows.at(0).at("c")) == 2);

    loaded->roles.sync_by_ids(std::vector<std::int64_t>{developer->id});
    session.commit();

    pivot_count = db->execute(
        "SELECT COUNT(*) AS c FROM parity_user_roles WHERE user_id = ?;",
        {loaded->id});
    auto admin_count = db->execute(
        "SELECT COUNT(*) AS c FROM parity_roles WHERE id = ?;",
        {admin->id});
    auto developer_count = db->execute(
        "SELECT COUNT(*) AS c FROM parity_roles WHERE id = ?;",
        {developer->id});
    assert(metal::from_value<std::int64_t>(pivot_count.rows.at(0).at("c")) == 1);
    assert(metal::from_value<std::int64_t>(admin_count.rows.at(0).at("c")) == 0);
    assert(metal::from_value<std::int64_t>(developer_count.rows.at(0).at("c")) == 1);

    db->execute(
        "INSERT INTO parity_users(id, name) VALUES (?, ?);",
        {std::int64_t{100}, std::string{"seed"}});
    session.clear();
    auto managed = std::make_shared<ParityUser>();
    managed->id = 100;
    managed->name = "seed";
    session.persist(managed);
    managed->name = "managed";
    session.commit();

    auto managed_row = db->execute(
        "SELECT name FROM parity_users WHERE id = ?;",
        {std::int64_t{100}});
    assert(managed_row.rows.size() == 1);
    assert(metal::from_value<std::string>(managed_row.rows[0].at("name")) == "managed");

    db->execute(
        "INSERT INTO parity_manual_keys(id, name) VALUES (?, ?);",
        {std::int64_t{0}, std::string{"zero"}});
    session.clear();
    auto zero = std::make_shared<ParityManualKey>();
    zero->id = 0;
    zero->name = "zero";
    session.persist(zero);
    assert(session.find<ParityManualKey>(0) == zero);
    zero->name = "zero-managed";
    session.commit();

    auto zero_row = db->execute(
        "SELECT name FROM parity_manual_keys WHERE id = ?;",
        {std::int64_t{0}});
    assert(zero_row.rows.size() == 1);
    assert(metal::from_value<std::string>(zero_row.rows[0].at("name")) == "zero-managed");

    session.clear();
    auto detached = std::make_shared<ParityUser>();
    detached->id = user->id;
    session.remove(detached);
    session.commit();
    auto user_count = db->execute(
        "SELECT COUNT(*) AS c FROM parity_users WHERE id = ?;",
        {user->id});
    assert(metal::from_value<std::int64_t>(user_count.rows.at(0).at("c")) == 1);
}
