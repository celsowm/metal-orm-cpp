#pragma once

#include "metal/execution.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace metal {

class PostgresExecutor final : public DbExecutor {
public:
    explicit PostgresExecutor(std::string connection_string);
    ~PostgresExecutor() override;

    PostgresExecutor(const PostgresExecutor&) = delete;
    PostgresExecutor& operator=(const PostgresExecutor&) = delete;
    PostgresExecutor(PostgresExecutor&&) noexcept;
    PostgresExecutor& operator=(PostgresExecutor&&) noexcept;

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
