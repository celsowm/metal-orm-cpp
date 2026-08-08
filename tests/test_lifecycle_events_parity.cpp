#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

struct UserCreated {
    std::int64_t id{};
    std::string name;
};

struct UserUpdated {
    std::int64_t id{};
    std::string name;
};

using UserEvents = metal::domain_event_queue<UserCreated, UserUpdated>;
static_assert(UserEvents::accepts<UserCreated>);
static_assert(UserEvents::accepts<UserUpdated>);
static_assert(!UserEvents::accepts<int>);

struct [[=metal::mapping::table{"lifecycle_users"}]] LifecycleUser {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;

    [[=metal::mapping::ignore]]
    UserEvents domain_events;
};

static_assert(metal::reflect::validate_mapping<LifecycleUser>());

static std::int64_t scalar_i64(
    metal::DbExecutor& db,
    const std::string& sql,
    const std::string& column = "value") {
    const auto result = db.execute(sql);
    assert(result.rows.size() == 1);
    return metal::from_value<std::int64_t>(result.rows.front().at(column));
}

int main() {
    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    metal::SQLiteDialect dialect;
    db->execute(metal::create_table_sql<LifecycleUser>(dialect));

    metal::Session session{db};
    std::vector<std::string> calls;
    std::size_t created_dispatches = 0;
    std::size_t updated_dispatches = 0;

    metal::TableHooks<LifecycleUser> hooks;
    hooks.before_insert = [&](metal::Session&, LifecycleUser& user) {
        calls.push_back("beforeInsert");
        user.name += "-prepared";
    };
    hooks.after_insert = [&](metal::Session&, LifecycleUser& user) {
        calls.push_back("afterInsert");
        user.domain_events.raise(UserCreated{user.id, user.name});
    };
    hooks.before_update = [&](metal::Session&, LifecycleUser&) {
        calls.push_back("beforeUpdate");
    };
    hooks.after_update = [&](metal::Session&, LifecycleUser& user) {
        calls.push_back("afterUpdate");
        user.domain_events.raise(UserUpdated{user.id, user.name});
    };
    hooks.before_delete = [&](metal::Session&, LifecycleUser&) {
        calls.push_back("beforeDelete");
    };
    hooks.after_delete = [&](metal::Session&, LifecycleUser&) {
        calls.push_back("afterDelete");
    };
    session.register_table_hooks<LifecycleUser>(std::move(hooks));

    session.register_interceptor(metal::SessionInterceptor{
        .before_flush = [&](metal::Session&) { calls.push_back("beforeFlush"); },
        .after_flush = [&](metal::Session&) { calls.push_back("afterFlush"); }
    });

    session.register_domain_event_handler<UserCreated>(
        [&](const UserCreated& event, metal::Session& ctx) {
            ++created_dispatches;
            calls.push_back("UserCreated");
            assert(event.id != 0);
            assert(scalar_i64(
                ctx.executor(),
                "SELECT COUNT(*) AS value FROM lifecycle_users WHERE id = " +
                    std::to_string(event.id) + ";") == 1);
        });

    session.register_domain_event_handler<UserUpdated>(
        [&](const UserUpdated& event, metal::Session& ctx) {
            ++updated_dispatches;
            calls.push_back("UserUpdated");
            assert(scalar_i64(
                ctx.executor(),
                "SELECT COUNT(*) AS value FROM lifecycle_users WHERE id = " +
                    std::to_string(event.id) + " AND name = '" + event.name + "';") == 1);
        });

    // INSERT: interceptor -> table hooks -> interceptor -> committed event dispatch.
    auto user = std::make_shared<LifecycleUser>();
    user->name = "alpha";
    session.persist(user);
    session.commit();
    assert(user->id != 0);
    assert(user->name == "alpha-prepared");
    assert(user->domain_events.empty());
    assert(created_dispatches == 1);
    assert((calls == std::vector<std::string>{
        "beforeFlush", "beforeInsert", "afterInsert", "afterFlush", "UserCreated"}));

    // UPDATE hooks run only for a dirty managed entity and dispatch after commit.
    calls.clear();
    user->name = "beta";
    session.commit();
    assert(updated_dispatches == 1);
    assert(user->domain_events.empty());
    assert((calls == std::vector<std::string>{
        "beforeFlush", "beforeUpdate", "afterUpdate", "afterFlush", "UserUpdated"}));

    // Raw flush is intentionally UoW-only: table hooks run, Session interceptors
    // and domain-event dispatch do not.
    calls.clear();
    user->name = "raw-flush";
    session.flush();
    assert((calls == std::vector<std::string>{"beforeUpdate", "afterUpdate"}));
    assert(user->domain_events.size() == 1);
    assert(updated_dispatches == 1);

    calls.clear();
    session.commit();
    assert((calls == std::vector<std::string>{"beforeFlush", "afterFlush", "UserUpdated"}));
    assert(updated_dispatches == 2);
    assert(user->domain_events.empty());

    // A nested transaction may flush and raise events, but RELEASE SAVEPOINT is
    // not a dispatch boundary. Events leave only after the outer COMMIT.
    calls.clear();
    const auto before_nested_dispatches = updated_dispatches;
    session.transaction([&](metal::Session& outer) {
        user->name = "outer-value";
        outer.transaction([&](metal::Session&) {
            user->name = "inner-value";
        });
        assert(updated_dispatches == before_nested_dispatches);
        assert(user->domain_events.size() == 1);
    });
    assert(updated_dispatches == before_nested_dispatches + 1);
    assert(user->name == "inner-value");
    assert(user->domain_events.empty());
    assert((calls == std::vector<std::string>{
        "beforeFlush", "beforeUpdate", "afterUpdate", "afterFlush",
        "beforeFlush", "afterFlush", "UserUpdated"}));

    // Events raised by hooks inside a failed transaction are checkpointed with
    // the entity and disappear together with the rolled-back mutation.
    calls.clear();
    const auto before_rollback_dispatches = updated_dispatches;
    bool rolled_back = false;
    try {
        session.transaction([&](metal::Session& tx) {
            user->name = "must-roll-back";
            tx.flush();
            assert(user->domain_events.size() == 1);
            throw std::runtime_error("rollback lifecycle event");
        });
    } catch (const std::runtime_error&) {
        rolled_back = true;
    }
    assert(rolled_back);
    assert(user->name == "inner-value");
    assert(user->domain_events.empty());
    assert(updated_dispatches == before_rollback_dispatches);
    assert((calls == std::vector<std::string>{"beforeUpdate", "afterUpdate"}));

    calls.clear();
    session.commit();
    assert(updated_dispatches == before_rollback_dispatches);
    assert((calls == std::vector<std::string>{"beforeFlush", "afterFlush"}));

    // DELETE hooks retain a live entity through afterDelete even though the UoW
    // removes tracking before the callback, matching the TypeScript lifecycle.
    calls.clear();
    session.remove(user);
    session.commit();
    assert((calls == std::vector<std::string>{
        "beforeFlush", "beforeDelete", "afterDelete", "afterFlush"}));
    assert(scalar_i64(*db, "SELECT COUNT(*) AS value FROM lifecycle_users;") == 0);

    // A hook failure participates in the transaction and restores both DB and
    // object/UoW state through the 0.0.14 checkpoint mechanism.
    auto failing = std::make_shared<LifecycleUser>();
    failing->name = "hook-failure";
    session.persist(failing);

    metal::TableHooks<LifecycleUser> throwing_hooks;
    throwing_hooks.before_insert = [](metal::Session&, LifecycleUser& entity) {
        entity.name = "changed-by-failing-hook";
        throw std::runtime_error("beforeInsert failed");
    };
    session.register_table_hooks<LifecycleUser>(std::move(throwing_hooks));

    bool hook_failed = false;
    try {
        session.commit();
    } catch (const std::runtime_error&) {
        hook_failed = true;
    }
    assert(hook_failed);
    assert(failing->id == 0);
    assert(failing->name == "hook-failure");
    assert(session.unit_of_work().contains(failing.get()));
    assert(session.unit_of_work().find(failing.get())->status == metal::EntityStatus::New);
    assert(scalar_i64(*db, "SELECT COUNT(*) AS value FROM lifecycle_users;") == 0);

    return 0;
}
