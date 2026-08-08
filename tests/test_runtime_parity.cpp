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

static_assert(metal::reflect::validate_mapping<ParityUser>());
static_assert(metal::reflect::validate_mapping<ParityLinkUser>());
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
}
