#include <metal/metal.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct [[=metal::mapping::view{"active_users"}]] ActiveUsersView {
    std::int64_t id{};
    std::string name;
};

template <typename T>
concept SessionPersistable = requires(metal::Session& session, std::shared_ptr<T> value) {
    session.persist(value);
};

static bool contains(const std::string& value, const std::string& needle) {
    return value.find(needle) != std::string::npos;
}

static_assert(metal::reflect::ViewMapped<ActiveUsersView>);
static_assert(metal::reflect::validate_view_mapping<ActiveUsersView>());
static_assert(!metal::reflect::Mapped<ActiveUsersView>);
static_assert(!metal::reflect::Entity<ActiveUsersView>);
static_assert(!SessionPersistable<ActiveUsersView>);

int main() {
    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    auto dialect = std::make_shared<metal::SQLiteDialect>();

    db->execute(
        "CREATE TABLE users ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "name TEXT NOT NULL, "
        "active INTEGER NOT NULL"
        ");");
    db->execute(
        "CREATE TABLE orders ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "user_id INTEGER NOT NULL, "
        "amount REAL NOT NULL"
        ");");
    db->execute(
        "INSERT INTO users(name, active) VALUES ('Ada', 1), ('Grace', 1), ('Linus', 0);");
    db->execute(
        "INSERT INTO orders(user_id, amount) VALUES (1, 10.0), (1, 20.0), (2, 7.0);");
    db->execute(
        "CREATE VIEW active_users AS "
        "SELECT id, name FROM users WHERE active = 1;");
    db->execute(
        "CREATE VIEW user_order_summary AS "
        "SELECT u.id AS user_id, u.name AS name, COUNT(o.id) AS order_count "
        "FROM users u LEFT JOIN orders o ON o.user_id = u.id "
        "GROUP BY u.id, u.name;");

    metal::Session session{db, dialect};
    const auto compiled = metal::view_query<ActiveUsersView>(session)
        .where(metal::field<^^ActiveUsersView::name> == std::string{"Ada"})
        .order_by<^^ActiveUsersView::id>()
        .compile();
    assert(contains(compiled.sql, "FROM \"active_users\" AS \"v\""));
    assert(compiled.params.size() == 1);

    const auto rows = metal::view_query<ActiveUsersView>(session)
        .where(metal::field<^^ActiveUsersView::name> != std::string{"Nobody"})
        .order_by<^^ActiveUsersView::id>()
        .all();
    assert(rows.size() == 2);
    assert(rows[0].id == 1);
    assert(rows[0].name == "Ada");
    assert(rows[1].id == 2);
    assert(rows[1].name == "Grace");

    auto detached = rows.front();
    detached.name = "changed only in memory";
    const auto first = metal::view_query<ActiveUsersView>(session)
        .order_by<^^ActiveUsersView::id>()
        .first();
    assert(first);
    assert(first->name == "Ada");

    metal::IntrospectOptions introspection;
    introspection.include_views = true;
    const auto schema = metal::introspect_sqlite(*db, introspection);
    assert(schema.views.size() == 2);

    const auto generated_views = metal::generate_view_header(schema);
    assert(contains(generated_views.code, "=metal::mapping::view{\"active_users\"}"));
    assert(contains(generated_views.code, "struct [[=metal::mapping::view{\"user_order_summary\"}]] UserOrderSummaryView"));
    assert(contains(generated_views.code, "metal::Value order_count{nullptr};"));

    const auto generated_model = metal::generate_model_header(schema);
    assert(contains(generated_model.code, "=metal::mapping::table{\"users\"}"));
    assert(contains(generated_model.code, "=metal::mapping::view{\"active_users\"}"));
    assert(std::none_of(
        generated_model.warnings.begin(), generated_model.warnings.end(),
        [](const std::string& warning) {
            return warning.find("was introspected but not emitted as an ORM entity") != std::string::npos;
        }));

    const auto sqlite_generated = metal::generate_sqlite_model_header(
        *db,
        {},
        metal::IntrospectOptions{.include_views = true});
    assert(contains(sqlite_generated.code, "=metal::mapping::view{\"active_users\"}"));
}
