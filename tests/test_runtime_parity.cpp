#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>

struct [[=metal::mapping::table{"parity_roles"}]] ParityRole {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;
};

struct [[=metal::mapping::table{"parity_user_roles"}]] ParityUserRole {
    [[=metal::mapping::primary_key]] std::int64_t user_id{};
    [[=metal::mapping::primary_key]] std::int64_t role_id{};
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
    metal::collection<ParityRole> roles;
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
    metal::collection<ParityRole> roles;
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

    auto role = std::make_shared<ParityRole>();
    role->name = "admin";
    auto user = std::make_shared<ParityUser>();
    user->name = "Celso";

    // cascade::remove does not imply persist: mirror MetalORM and persist the
    // target explicitly before linking it.
    session.persist(role);
    session.persist(user);
    user->roles.attach(role);
    session.commit();

    assert(role->id != 0);
    assert(user->id != 0);

    auto pivot_count = db->execute("SELECT COUNT(*) AS c FROM parity_user_roles;");
    assert(metal::from_value<std::int64_t>(pivot_count.rows.at(0).at("c")) == 1);

    session.clear();
    auto loaded = session.query<ParityUser>()
        .include<^^ParityUser::roles>()
        .first();
    assert(loaded);
    assert(loaded->roles.size() == 1);

    auto loaded_role = loaded->roles[0];
    loaded->roles.detach(loaded_role);
    session.commit();

    // MetalORM semantics: relation processing deletes the pivot first, marks
    // the target Removed, then the second UoW flush deletes the target row.
    pivot_count = db->execute("SELECT COUNT(*) AS c FROM parity_user_roles;");
    auto role_count = db->execute("SELECT COUNT(*) AS c FROM parity_roles;");
    assert(metal::from_value<std::int64_t>(pivot_count.rows.at(0).at("c")) == 0);
    assert(metal::from_value<std::int64_t>(role_count.rows.at(0).at("c")) == 0);

    // A non-empty generated PK means persist() attaches as Managed rather
    // than issuing an INSERT, matching OrmSession.persist in MetalORM TS.
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

    // A manually assigned non-generated key of zero is a real identity, not
    // an empty generated key. It must participate in the Identity Map.
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

    // remove() mirrors the original runtime: an untracked detached object is
    // not implicitly attached just to be deleted.
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
