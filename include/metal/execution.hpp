#pragma once

#include "metal/value.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace metal {

using Row = std::unordered_map<std::string, Value>;

struct QueryResult {
    std::vector<Row> rows;
    std::int64_t affected_rows{0};
    std::int64_t last_insert_id{0};
};

class DbExecutor {
public:
    virtual ~DbExecutor() = default;
    virtual QueryResult execute(const std::string& sql, const std::vector<Value>& params = {}) = 0;
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace metal
