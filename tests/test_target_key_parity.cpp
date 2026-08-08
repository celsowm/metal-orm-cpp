#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct [[=metal::mapping::table{"alt_roles"}]] AltRole {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string code;
    std::string name;
};

struct [[=metal::mapping::table{"alt_user_roles"}]] AltUserRole {
    [[=metal::mapping::primary_key]] std::int64_t user_id{};
    [[=metal::mapping::primary_key]] std::string role_code;
};

struct [[=metal::mapping::table{"alt_users"}]] AltUser {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;

    [[=metal::mapping::many_to_many<
        ^^AltUserRole,
        ^^AltUserRole::user_id,
        ^^AltUserRole::role_code,
        metal::mapping::cascade_mode::remove,
        std::meta::info{},
        ^^AltRole::code>{}]]
    metal::many_to_many_collection<AltRole, AltUserRole> roles;
};

static_assert(metal::reflect::validate_mapping<AltRole>());
static_assert(metal::reflect::validate_mapping<AltUserRole>());
static_assert(metal::reflect::validate_mapping<AltUser>());

int main() {
    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    auto dialect = std::make_shared<metal::SQLiteDialect>();

    db->execute(metal::create_table_sql<AltRole>(*dialect));
    db->execute(metal::create_table_sql<AltUserRole>(*dialect));
    db->execute(metal::create_table_sql<AltUser>(*dialect));

    db->execute(
        "INSERT INTO alt_roles(id, code, name) VALUES (?, ?, ?), (?, ?, ?);",
        {std::int64_t{1}, std::string{"DEV"}, std::string{"developer"},
         std::int64_t{2}, std::string{"ADMIN"}, std::string{"administrator"}});

    metal::Session session{db, dialect};
    auto user = std::make_shared<AltUser>();
    user->name = "Celso";
    session.persist(user);
    session.commit();

    // ID-based collection APIs use the declared targetKey, not the target PK.
    user->roles.attach(std::string{"DEV"});
    session.commit();

    auto pivot = db->execute(
        "SELECT role_code FROM alt_user_roles WHERE user_id = ?;",
        {user->id});
    assert(pivot.rows.size() == 1);
    assert(metal::from_value<std::string>(pivot.rows[0].at("role_code")) == "DEV");

    const auto user_id = user->id;
    session.clear();

    auto loaded = session.find<AltUser>(user_id);
    assert(loaded);
    assert(!loaded->roles.loaded());
    loaded->roles.load();
    assert(loaded->roles.size() == 1);
    assert(loaded->roles[0]->id == 1);
    assert(loaded->roles[0]->code == "DEV");

    // Hydration still participates in the normal PK-based Session IdentityMap.
    assert(session.find<AltRole>(1) == loaded->roles[0]);

    // syncByIds is keyed by AltRole::code. DEV is detached/deleted while
    // ADMIN is attached through an alternate-key stub.
    loaded->roles.sync_by_ids(std::vector<std::string>{"ADMIN"});
    session.commit();

    auto dev_count = db->execute(
        "SELECT COUNT(*) AS c FROM alt_roles WHERE code = ?;",
        {std::string{"DEV"}});
    auto admin_count = db->execute(
        "SELECT COUNT(*) AS c FROM alt_roles WHERE code = ?;",
        {std::string{"ADMIN"}});
    pivot = db->execute(
        "SELECT role_code FROM alt_user_roles WHERE user_id = ?;",
        {user_id});

    assert(metal::from_value<std::int64_t>(dev_count.rows[0].at("c")) == 0);
    assert(metal::from_value<std::int64_t>(admin_count.rows[0].at("c")) == 1);
    assert(pivot.rows.size() == 1);
    assert(metal::from_value<std::string>(pivot.rows[0].at("role_code")) == "ADMIN");

    // ADMIN is currently represented by the attach-by-targetKey stub. Cascade
    // remove must therefore delete by targetKey even though that stub has no PK.
    assert(loaded->roles.detach(std::string{"ADMIN"}));
    session.commit();

    admin_count = db->execute(
        "SELECT COUNT(*) AS c FROM alt_roles WHERE code = ?;",
        {std::string{"ADMIN"}});
    auto pivot_count = db->execute(
        "SELECT COUNT(*) AS c FROM alt_user_roles WHERE user_id = ?;",
        {user_id});
    assert(metal::from_value<std::int64_t>(admin_count.rows[0].at("c")) == 0);
    assert(metal::from_value<std::int64_t>(pivot_count.rows[0].at("c")) == 0);
}
