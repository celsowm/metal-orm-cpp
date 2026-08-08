#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

struct [[=metal::mapping::table{"tx_children"}]] TxChild {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::int64_t parent_id{};
    std::string name;
};

struct [[=metal::mapping::table{"tx_parents"}]] TxParent {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;

    [[=metal::mapping::has_many<
        ^^TxChild::parent_id,
        metal::mapping::cascade_mode::persist>{}]]
    metal::has_many_collection<TxChild> children;
};

static_assert(metal::reflect::validate_mapping<TxChild>());
static_assert(metal::reflect::validate_mapping<TxParent>());

static std::int64_t scalar_i64(
    metal::DbExecutor& db,
    const std::string& sql,
    const std::string& column = "value") {
    const auto result = db.execute(sql);
    assert(result.rows.size() == 1);
    return metal::from_value<std::int64_t>(result.rows.front().at(column));
}

class NoSavepointExecutor final : public metal::DbExecutor {
public:
    explicit NoSavepointExecutor(std::shared_ptr<metal::SQLiteExecutor> inner)
        : inner_(std::move(inner)) {}

    metal::QueryResult execute(
        const std::string& sql,
        const std::vector<metal::Value>& params = {}) override {
        return inner_->execute(sql, params);
    }

    [[nodiscard]] metal::ExecutorCapabilities capabilities() const noexcept override {
        return {true, false};
    }

    void begin_transaction() override { inner_->begin_transaction(); }
    void commit_transaction() override { inner_->commit_transaction(); }
    void rollback_transaction() override { inner_->rollback_transaction(); }

private:
    std::shared_ptr<metal::SQLiteExecutor> inner_;
};

int main() {
    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    metal::SQLiteDialect dialect;
    db->execute(metal::create_table_sql<TxParent>(dialect));
    db->execute(metal::create_table_sql<TxChild>(dialect));
    db->execute("INSERT INTO tx_parents(id, name) VALUES (1, 'base');");

    metal::Session session{db};
    auto parent = session.find<TxParent>(1);
    assert(parent);
    parent->children.load();
    assert(parent->children.empty());

    // UPDATE flushed inside a transaction must restore both SQLite and object state.
    bool update_failed = false;
    try {
        session.transaction([&](metal::Session& tx) {
            parent->name = "rolled-back-update";
            tx.flush();
            assert(parent->name == "rolled-back-update");
            throw std::runtime_error("force update rollback");
        });
    } catch (const std::runtime_error&) {
        update_failed = true;
    }
    assert(update_failed);
    assert(parent->name == "base");
    assert(session.find<TxParent>(1) == parent);
    assert(scalar_i64(*db, "SELECT COUNT(*) AS value FROM tx_parents WHERE id = 1 AND name = 'base';") == 1);

    // DELETE removes tracking during flush; rollback must resurrect it and its identity.
    bool delete_failed = false;
    try {
        session.transaction([&](metal::Session& tx) {
            tx.remove(parent);
            tx.flush();
            assert(scalar_i64(*db, "SELECT COUNT(*) AS value FROM tx_parents WHERE id = 1;") == 0);
            throw std::runtime_error("force delete rollback");
        });
    } catch (const std::runtime_error&) {
        delete_failed = true;
    }
    assert(delete_failed);
    assert(session.find<TxParent>(1) == parent);
    assert(scalar_i64(*db, "SELECT COUNT(*) AS value FROM tx_parents WHERE id = 1;") == 1);

    // A generated ID assigned by a flushed INSERT must return to its pre-transaction value.
    auto transient = std::make_shared<TxParent>();
    transient->name = "transient";
    bool insert_failed = false;
    try {
        session.transaction([&](metal::Session& tx) {
            tx.persist(transient);
            tx.flush();
            assert(transient->id != 0);
            assert(tx.unit_of_work().contains(transient.get()));
            throw std::runtime_error("force insert rollback");
        });
    } catch (const std::runtime_error&) {
        insert_failed = true;
    }
    assert(insert_failed);
    assert(transient->id == 0);
    assert(!session.unit_of_work().contains(transient.get()));
    assert(scalar_i64(*db, "SELECT COUNT(*) AS value FROM tx_parents WHERE name = 'transient';") == 0);

    // A successful nested transaction releases a savepoint and remains part of the outer transaction.
    session.transaction([&](metal::Session& outer) {
        parent->name = "outer";
        outer.transaction([&](metal::Session&) {
            parent->name = "inner";
        });
        assert(parent->name == "inner");
        parent->name = "nested-committed";
    });
    assert(parent->name == "nested-committed");
    assert(scalar_i64(*db, "SELECT COUNT(*) AS value FROM tx_parents WHERE id = 1 AND name = 'nested-committed';") == 1);

    // If an inner scope fails, it rolls back to its savepoint and poisons the outer scope.
    bool rollback_only_rejected = false;
    try {
        session.transaction([&](metal::Session& outer) {
            parent->name = "outer-before-inner";
            try {
                outer.transaction([&](metal::Session& inner) {
                    parent->name = "inner-failure";
                    inner.flush();
                    throw std::runtime_error("inner failed");
                });
            } catch (const std::runtime_error&) {
                assert(parent->name == "outer-before-inner");
                assert(outer.rollback_only());
            }
            parent->name = "must-not-commit";
        });
    } catch (const std::logic_error&) {
        rollback_only_rejected = true;
    }
    assert(rollback_only_rejected);
    assert(parent->name == "nested-committed");
    assert(session.transaction_depth() == 0);
    assert(!session.rollback_only());
    assert(scalar_i64(*db, "SELECT COUNT(*) AS value FROM tx_parents WHERE id = 1 AND name = 'nested-committed';") == 1);

    // Inner relation changes may be flushed and accepted at a savepoint; outer rollback restores them.
    std::shared_ptr<TxChild> rolled_back_child;
    bool graph_failed = false;
    try {
        session.transaction([&](metal::Session& outer) {
            outer.transaction([&](metal::Session&) {
                rolled_back_child = parent->children.add();
                rolled_back_child->name = "savepoint-child";
            });
            assert(rolled_back_child);
            assert(rolled_back_child->id != 0);
            assert(parent->children.size() == 1);
            assert(scalar_i64(*db, "SELECT COUNT(*) AS value FROM tx_children WHERE name = 'savepoint-child';") == 1);
            throw std::runtime_error("outer graph rollback");
        });
    } catch (const std::runtime_error&) {
        graph_failed = true;
    }
    assert(graph_failed);
    assert(rolled_back_child->id == 0);
    assert(parent->children.loaded());
    assert(parent->children.empty());
    assert(!session.unit_of_work().contains(rolled_back_child.get()));
    assert(scalar_i64(*db, "SELECT COUNT(*) AS value FROM tx_children WHERE name = 'savepoint-child';") == 0);

    // Nested transactions require explicit savepoint capability.
    auto raw_no_savepoint = std::make_shared<metal::SQLiteExecutor>(":memory:");
    raw_no_savepoint->execute(metal::create_table_sql<TxParent>(dialect));
    auto no_savepoint = std::make_shared<NoSavepointExecutor>(raw_no_savepoint);
    metal::Session limited{no_savepoint};
    bool savepoint_rejected = false;
    try {
        limited.transaction([&](metal::Session& outer) {
            outer.transaction([](metal::Session&) {});
        });
    } catch (const std::logic_error&) {
        savepoint_rejected = true;
    }
    assert(savepoint_rejected);
    assert(limited.transaction_depth() == 0);

    return 0;
}
