#pragma once

#include "metal/value.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace metal {

using Row = std::unordered_map<std::string, Value>;

struct QueryResult {
    std::vector<Row> rows;
    std::int64_t affected_rows{0};
    std::int64_t last_insert_id{0};
};

struct ExecutorCapabilities {
    bool transactions{false};
    bool savepoints{false};
};

class DbExecutor {
public:
    virtual ~DbExecutor() = default;
    virtual QueryResult execute(const std::string& sql, const std::vector<Value>& params = {}) = 0;

    [[nodiscard]] virtual ExecutorCapabilities capabilities() const noexcept { return {}; }

    virtual void begin_transaction() {
        throw std::logic_error("MetalORM: executor does not support transactions");
    }
    virtual void commit_transaction() {
        throw std::logic_error("MetalORM: executor does not support transactions");
    }
    virtual void rollback_transaction() {
        throw std::logic_error("MetalORM: executor does not support transactions");
    }
    virtual void savepoint(std::string_view) {
        throw std::logic_error("MetalORM: executor does not support savepoints");
    }
    virtual void release_savepoint(std::string_view) {
        throw std::logic_error("MetalORM: executor does not support savepoints");
    }
    virtual void rollback_to_savepoint(std::string_view) {
        throw std::logic_error("MetalORM: executor does not support savepoints");
    }
};

class SQLiteExecutor final : public DbExecutor {
public:
    explicit SQLiteExecutor(std::string filename);
    ~SQLiteExecutor() override;

    SQLiteExecutor(const SQLiteExecutor&) = delete;
    SQLiteExecutor& operator=(const SQLiteExecutor&) = delete;
    SQLiteExecutor(SQLiteExecutor&&) noexcept;
    SQLiteExecutor& operator=(SQLiteExecutor&&) noexcept;

    QueryResult execute(const std::string& sql, const std::vector<Value>& params = {}) override;
    [[nodiscard]] ExecutorCapabilities capabilities() const noexcept override { return {true, true}; }
    void begin_transaction() override;
    void commit_transaction() override;
    void rollback_transaction() override;
    void savepoint(std::string_view name) override;
    void release_savepoint(std::string_view name) override;
    void rollback_to_savepoint(std::string_view name) override;

private:
    static void validate_savepoint_name(std::string_view name);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace metal
