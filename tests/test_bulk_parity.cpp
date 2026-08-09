#include <metal/metal.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

struct [[=metal::mapping::table{"bulk_users"}]] BulkUser {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string email;
    std::string name;
    std::int64_t score{};
    bool active{};
};

static_assert(metal::reflect::validate_mapping<BulkUser>());

static metal::BulkRow make_user(
    std::string email,
    std::string name,
    std::int64_t score,
    bool active = true) {
    return metal::bulk_row<BulkUser>()
        .set<^^BulkUser::email>(std::move(email))
        .set<^^BulkUser::name>(std::move(name))
        .set<^^BulkUser::score>(score)
        .set<^^BulkUser::active>(active)
        .build();
}

static std::vector<std::int64_t> current_ids(metal::DbExecutor& db) {
    const auto rows = db.execute("SELECT id FROM bulk_users ORDER BY id;");
    std::vector<std::int64_t> ids;
    ids.reserve(rows.rows.size());
    for (const auto& row : rows.rows) {
        ids.push_back(metal::from_value<std::int64_t>(row.at("id")));
    }
    return ids;
}

int main() {
    metal::SQLiteDialect dialect;
    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    db->execute(metal::create_table_sql<BulkUser>(dialect));
    db->execute("CREATE UNIQUE INDEX bulk_users_email_uq ON bulk_users(email);");
    metal::Session session{db};

    std::vector<metal::BulkRow> inserts{
        make_user("a@example.test", "A", 10),
        make_user("b@example.test", "B", 20),
        make_user("c@example.test", "C", 30),
        make_user("d@example.test", "D", 40),
        make_user("e@example.test", "E", 50)
    };

    std::atomic<std::size_t> completed_chunks{0};
    metal::BulkInsertOptions insert_options;
    insert_options.chunk_size = 2;
    insert_options.concurrency = 2;
    insert_options.timing = true;
    insert_options.returning = metal::bulk_columns<
        ^^BulkUser::id,
        ^^BulkUser::email,
        ^^BulkUser::score>();
    insert_options.on_chunk_complete = [&](const metal::ChunkCompleteInfo& info) {
        assert(info.total_chunks == 3);
        assert(info.rows_in_chunk >= 1 && info.rows_in_chunk <= 2);
        ++completed_chunks;
    };

    const auto inserted = metal::bulk_insert<BulkUser>(session, inserts, insert_options);
    assert(inserted.processed_rows == 5);
    assert(inserted.chunks_executed == 3);
    assert(inserted.returning.size() == 5);
    assert(inserted.chunk_timings_ms && inserted.chunk_timings_ms->size() == 3);
    assert(inserted.metadata && inserted.metadata->strategy == metal::BulkStrategy::Batch);
    assert(completed_chunks == 3);

    auto ids = current_ids(*db);
    assert(ids.size() == 5);

    std::vector<metal::BulkRow> updates;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        updates.push_back(
            metal::bulk_row<BulkUser>()
                .set<^^BulkUser::id>(ids[i])
                .set<^^BulkUser::score>(static_cast<std::int64_t>(100 + i))
                .build());
    }

    metal::BulkUpdateOptions<BulkUser> update_options;
    update_options.chunk_size = 2;
    update_options.returning = metal::bulk_columns<^^BulkUser::id, ^^BulkUser::score>();
    update_options.where = metal::field<^^BulkUser::active> == true;

    const auto updated = metal::bulk_update<BulkUser>(session, updates, update_options);
    assert(updated.processed_rows == 5);
    assert(updated.chunks_executed == 3);
    assert(updated.returning.size() == 5);
    assert(updated.metadata && updated.metadata->strategy == metal::BulkStrategy::Individual);

    db->execute("UPDATE bulk_users SET active = 0 WHERE id = ?;", {ids.front()});
    const auto guarded = metal::bulk_update<BulkUser>(
        session,
        {metal::bulk_row<BulkUser>()
             .set<^^BulkUser::id>(ids.front())
             .set<^^BulkUser::score>(999)
             .build()},
        update_options);
    assert(guarded.processed_rows == 1);
    assert(guarded.returning.empty());
    const auto guarded_row = db->execute(
        "SELECT score FROM bulk_users WHERE id = ?;",
        {ids.front()});
    assert(metal::from_value<std::int64_t>(guarded_row.rows.front().at("score")) == 100);

    metal::BulkUpdateOptions<BulkUser> update_where_options;
    update_where_options.chunk_size = 2;
    update_where_options.returning = metal::bulk_columns<^^BulkUser::id, ^^BulkUser::score>();
    update_where_options.where = metal::field<^^BulkUser::active> == true;
    const std::vector<std::int64_t> first_three{ids[0], ids[1], ids[2]};
    const auto update_where = metal::bulk_update_where<BulkUser>(
        session,
        first_three,
        metal::bulk_row<BulkUser>()
            .set<^^BulkUser::score>(777)
            .build(),
        update_where_options);
    assert(update_where.processed_rows == 3);
    assert(update_where.chunks_executed == 2);
    assert(update_where.returning.size() == 2);
    assert(update_where.metadata && update_where.metadata->strategy == metal::BulkStrategy::WhereIn);

    metal::BulkUpsertOptions upsert_options;
    upsert_options.chunk_size = 2;
    upsert_options.conflict_columns = metal::bulk_columns<^^BulkUser::email>();
    upsert_options.update_columns = metal::bulk_columns<^^BulkUser::name, ^^BulkUser::score>();
    upsert_options.returning = metal::bulk_all_columns<BulkUser>();

    const auto upserted = metal::bulk_upsert<BulkUser>(
        session,
        {
            make_user("b@example.test", "B2", 901),
            make_user("c@example.test", "C2", 902),
            make_user("f@example.test", "F", 903)
        },
        upsert_options);
    assert(upserted.processed_rows == 3);
    assert(upserted.chunks_executed == 2);
    assert(upserted.returning.size() == 3);
    assert(upserted.metadata && upserted.metadata->strategy == metal::BulkStrategy::Batch);

    metal::BulkUpsertOptions do_nothing_options;
    do_nothing_options.conflict_columns = metal::bulk_columns<^^BulkUser::email>();
    do_nothing_options.update_columns = metal::bulk_no_columns();
    do_nothing_options.returning = metal::bulk_columns<^^BulkUser::id>();
    const auto ignored = metal::bulk_upsert<BulkUser>(
        session,
        {make_user("b@example.test", "ignored", 9999)},
        do_nothing_options);
    assert(ignored.processed_rows == 1);
    assert(ignored.returning.empty());

    ids = current_ids(*db);
    assert(ids.size() == 6);

    metal::BulkDeleteOptions<BulkUser> delete_options;
    delete_options.chunk_size = 1;
    delete_options.where = metal::field<^^BulkUser::active> == true;
    const std::vector<std::int64_t> delete_ids{ids[1], ids[2]};
    const auto deleted = metal::bulk_delete<BulkUser>(session, delete_ids, delete_options);
    assert(deleted.processed_rows == 2);
    assert(deleted.chunks_executed == 2);
    assert(deleted.metadata && deleted.metadata->strategy == metal::BulkStrategy::WhereIn);

    const auto deleted_where = metal::bulk_delete_where<BulkUser>(
        session,
        metal::field<^^BulkUser::score> >= std::int64_t{900});
    assert(deleted_where.processed_rows == 0);
    assert(deleted_where.chunks_executed == 1);

    const auto before_rollback = db->execute("SELECT COUNT(*) AS c FROM bulk_users;");
    const auto count_before = metal::from_value<std::int64_t>(before_rollback.rows.front().at("c"));

    metal::BulkInsertOptions rollback_options;
    rollback_options.chunk_size = 1;
    bool rolled_back = false;
    try {
        (void)metal::bulk_insert<BulkUser>(
            session,
            {
                make_user("rollback-ok@example.test", "ok", 1),
                make_user("a@example.test", "duplicate", 2)
            },
            rollback_options);
    } catch (const std::runtime_error&) {
        rolled_back = true;
    }
    assert(rolled_back);
    const auto after_rollback = db->execute("SELECT COUNT(*) AS c FROM bulk_users;");
    assert(metal::from_value<std::int64_t>(after_rollback.rows.front().at("c")) == count_before);
    const auto rolled_back_row = db->execute(
        "SELECT COUNT(*) AS c FROM bulk_users WHERE email = 'rollback-ok@example.test';");
    assert(metal::from_value<std::int64_t>(rolled_back_row.rows.front().at("c")) == 0);

    metal::BulkInsertOptions non_transactional;
    non_transactional.chunk_size = 1;
    non_transactional.transactional = false;
    bool partially_failed = false;
    try {
        (void)metal::bulk_insert<BulkUser>(
            session,
            {
                make_user("partial@example.test", "partial", 1),
                make_user("a@example.test", "duplicate", 2)
            },
            non_transactional);
    } catch (const std::runtime_error&) {
        partially_failed = true;
    }
    assert(partially_failed);
    const auto partial_row = db->execute(
        "SELECT COUNT(*) AS c FROM bulk_users WHERE email = 'partial@example.test';");
    assert(metal::from_value<std::int64_t>(partial_row.rows.front().at("c")) == 1);

    bool invalid_chunk_rejected = false;
    try {
        metal::BulkInsertOptions invalid;
        invalid.chunk_size = 0;
        (void)metal::bulk_insert<BulkUser>(session, {make_user("x@example.test", "X", 1)}, invalid);
    } catch (const std::invalid_argument&) {
        invalid_chunk_rejected = true;
    }
    assert(invalid_chunk_rejected);
}
