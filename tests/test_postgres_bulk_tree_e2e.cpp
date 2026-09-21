#include <metal/metal.hpp>
#include <metal/postgres_execution.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct [[=metal::mapping::table{"pg_bulk_records"}]] BulkRecord {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string email;
    std::int64_t score{};
    bool active{};
};

struct [[=metal::mapping::table{"pg_tree_nodes"}]] TreeRecord {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;
    [[=metal::mapping::tree_parent]]
    std::optional<std::int64_t> parent_id;
    [[=metal::mapping::tree_left]]
    std::int64_t lft{};
    [[=metal::mapping::tree_right]]
    std::int64_t rght{};
    [[=metal::mapping::tree_depth]]
    std::optional<std::int64_t> depth;
    [[=metal::mapping::tree_scope]]
    std::int64_t tenant_id{};
};

static_assert(metal::reflect::validate_mapping<BulkRecord>());
static_assert(metal::reflect::validate_tree_mapping<TreeRecord>());

std::int64_t id_of(const metal::Value& value) {
    return metal::from_value<std::int64_t>(value);
}

std::int64_t count(metal::DbExecutor& db, const std::string& table) {
    const auto rows = db.execute("SELECT COUNT(*)::BIGINT AS n FROM " + table + ";").rows;
    assert(rows.size() == 1);
    return id_of(rows.front().at("n"));
}

metal::BulkRow bulk_row(std::string email, std::int64_t score) {
    return metal::bulk_row<BulkRecord>()
        .set<^^BulkRecord::email>(std::move(email))
        .set<^^BulkRecord::score>(score)
        .set<^^BulkRecord::active>(true)
        .build();
}

metal::BulkRow tree_row(std::string name) {
    return metal::tree_row<TreeRecord>()
        .set<^^TreeRecord::name>(std::move(name))
        .build();
}

void test_bulk(metal::Session& session) {
    auto& db = session.executor();
    db.execute(metal::create_table_sql<BulkRecord>(session.dialect()));
    db.execute("CREATE UNIQUE INDEX pg_bulk_email_uq ON pg_bulk_records(email);");

    metal::BulkInsertOptions insert_options;
    insert_options.chunk_size = 2;
    insert_options.returning = metal::bulk_columns<^^BulkRecord::id, ^^BulkRecord::email>();
    const auto inserted = metal::bulk_insert<BulkRecord>(
        session, {bulk_row("a@test", 1), bulk_row("b@test", 2), bulk_row("c@test", 3)}, insert_options);
    assert(inserted.processed_rows == 3 && inserted.chunks_executed == 2);
    assert(inserted.returning.size() == 3);
    assert(inserted.metadata && inserted.metadata->dialect == "postgresql");

    const auto first_id = id_of(inserted.returning[0].at("id"));
    const auto second_id = id_of(inserted.returning[1].at("id"));

    metal::BulkUpdateOptions<BulkRecord> update_options;
    update_options.returning = metal::bulk_columns<^^BulkRecord::id, ^^BulkRecord::score>();
    update_options.where = metal::field<^^BulkRecord::active> == true;
    const auto updated = metal::bulk_update<BulkRecord>(
        session,
        {metal::bulk_row<BulkRecord>()
            .set<^^BulkRecord::id>(first_id)
            .set<^^BulkRecord::score>(std::int64_t{11})
            .build()},
        update_options);
    assert(updated.returning.size() == 1);
    assert(id_of(updated.returning.front().at("score")) == 11);
    assert(updated.metadata && updated.metadata->dialect == "postgresql");

    update_options.chunk_size = 1;
    const auto updated_where = metal::bulk_update_where<BulkRecord>(
        session, std::vector<std::int64_t>{first_id, second_id},
        metal::bulk_row<BulkRecord>().set<^^BulkRecord::score>(std::int64_t{22}).build(),
        update_options);
    assert(updated_where.returning.size() == 2);
    assert(updated_where.metadata && updated_where.metadata->dialect == "postgresql");

    metal::BulkUpsertOptions upsert_options;
    upsert_options.conflict_columns = metal::bulk_columns<^^BulkRecord::email>();
    upsert_options.update_columns = metal::bulk_columns<^^BulkRecord::score>();
    upsert_options.returning = metal::bulk_columns<^^BulkRecord::id, ^^BulkRecord::score>();
    const auto upserted = metal::bulk_upsert<BulkRecord>(
        session, {bulk_row("a@test", 30), bulk_row("d@test", 40)}, upsert_options);
    assert(upserted.returning.size() == 2);
    assert(upserted.metadata && upserted.metadata->dialect == "postgresql");
    assert(count(db, "pg_bulk_records") == 4);

    metal::BulkDeleteOptions<BulkRecord> delete_options;
    delete_options.where = metal::field<^^BulkRecord::active> == true;
    const auto deleted = metal::bulk_delete<BulkRecord>(
        session, std::vector<std::int64_t>{second_id}, delete_options);
    assert(deleted.processed_rows == 1);
    assert(deleted.metadata && deleted.metadata->dialect == "postgresql");
    assert(count(db, "pg_bulk_records") == 3);

    const auto deleted_where = metal::bulk_delete_where<BulkRecord>(
        session, metal::field<^^BulkRecord::score> >= std::int64_t{40});
    assert(deleted_where.metadata && deleted_where.metadata->dialect == "postgresql");
    assert(count(db, "pg_bulk_records") == 2);

    bool rolled_back = false;
    metal::BulkInsertOptions rollback_options;
    rollback_options.chunk_size = 1;
    try {
        (void)metal::bulk_insert<BulkRecord>(
            session, {bulk_row("rollback@test", 1), bulk_row("a@test", 2)},
            rollback_options);
    } catch (const std::runtime_error&) {
        rolled_back = true;
    }
    assert(rolled_back);
    assert(count(db, "pg_bulk_records") == 2);
    assert(db.execute(
        "SELECT id FROM pg_bulk_records WHERE email = $1;", {std::string{"rollback@test"}}).rows.empty());
}

void test_tree(metal::Session& session) {
    auto& db = session.executor();
    db.execute(metal::create_table_sql<TreeRecord>(session.dialect()));

    const auto manager = metal::create_tree_manager<TreeRecord>(session);
    auto tenant1 = manager.with_scope<^^TreeRecord::tenant_id>(std::int64_t{1});
    auto tenant2 = manager.with_scope<^^TreeRecord::tenant_id>(std::int64_t{2});

    const auto other_id = id_of(tenant2.insert_as_child(nullptr, tree_row("Other")));
    const auto root_id = id_of(tenant1.insert_as_child(nullptr, tree_row("Root")));
    const auto a_id = id_of(tenant1.insert_as_child(root_id, tree_row("A")));
    const auto b_id = id_of(tenant1.insert_as_child(root_id, tree_row("B")));
    const auto grand_id = id_of(tenant1.insert_as_child(a_id, tree_row("Grand")));

    assert(tenant1.validate().empty());
    assert(tenant1.get_descendants({1, 8}).size() == 3);
    assert(tenant1.get_leaves().size() == 2);
    assert(tenant1.get_path({4, 5}).size() == 3);

    auto a = tenant1.get_node(a_id);
    auto b = tenant1.get_node(b_id);
    assert(a && b);
    tenant1.move_to(*a, b_id);
    assert(tenant1.validate().empty());
    a = tenant1.get_node(a_id);
    b = tenant1.get_node(b_id);
    assert(a && b && id_of(a->parent_id) == b_id);
    assert(a->depth && *a->depth == 2);

    tenant1.remove_from_tree(*a);
    assert(tenant1.validate().empty());
    a = tenant1.get_node(a_id);
    const auto grand = tenant1.get_node(grand_id);
    assert(a && a->is_root);
    assert(grand && id_of(grand->parent_id) == b_id);

    b = tenant1.get_node(b_id);
    assert(b && tenant1.delete_subtree(*b) == 2);
    assert(!tenant1.get_node(b_id));
    assert(!tenant1.get_node(grand_id));
    assert(tenant1.validate().empty());

    db.execute("UPDATE pg_tree_nodes SET lft = 0, rght = 0, depth = NULL WHERE tenant_id = $1;",
               {std::int64_t{1}});
    const auto recovery = tenant1.recover();
    assert(recovery.success && recovery.processed == 2);
    assert(tenant1.validate().empty());

    const auto other = tenant2.get_node(other_id);
    assert(other && other->lft == 1 && other->rght == 2);
    assert(tenant2.validate().empty());
    assert(tenant1.get_node(root_id));
}

} // namespace

int main() {
    const char* connection_string = std::getenv("METAL_ORM_POSTGRES_E2E_URL");
    if (!connection_string || !*connection_string) return 0;

    auto executor = std::make_shared<metal::PostgresExecutor>(connection_string);
    auto dialect = std::make_shared<metal::PostgresDialect>();
    executor->execute("DROP SCHEMA IF EXISTS metalorm_bulk_tree_e2e CASCADE;");
    executor->execute("CREATE SCHEMA metalorm_bulk_tree_e2e;");
    executor->execute("SET search_path TO metalorm_bulk_tree_e2e;");
    metal::Session session{executor, dialect};
    test_bulk(session);
    test_tree(session);
    executor->execute("DROP SCHEMA metalorm_bulk_tree_e2e CASCADE;");
}
