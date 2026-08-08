#include "metal/execution.hpp"

#include <sqlite3.h>

#include <stdexcept>
#include <utility>

namespace metal {

struct SQLiteExecutor::Impl {
    sqlite3* db{};
};

static void bind_value(sqlite3_stmt* stmt, int index, const Value& value) {
    std::visit([&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        int rc = SQLITE_ERROR;
        if constexpr (std::is_same_v<T, std::nullptr_t>) {
            rc = sqlite3_bind_null(stmt, index);
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            rc = sqlite3_bind_int64(stmt, index, v);
        } else if constexpr (std::is_same_v<T, double>) {
            rc = sqlite3_bind_double(stmt, index, v);
        } else if constexpr (std::is_same_v<T, std::string>) {
            rc = sqlite3_bind_text(stmt, index, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        } else if constexpr (std::is_same_v<T, bool>) {
            rc = sqlite3_bind_int(stmt, index, v ? 1 : 0);
        }
        if (rc != SQLITE_OK) throw std::runtime_error("MetalORM: failed to bind SQLite parameter");
    }, value);
}

static Value column_value(sqlite3_stmt* stmt, int column) {
    switch (sqlite3_column_type(stmt, column)) {
        case SQLITE_NULL:
            return nullptr;
        case SQLITE_INTEGER:
            return static_cast<std::int64_t>(sqlite3_column_int64(stmt, column));
        case SQLITE_FLOAT:
            return sqlite3_column_double(stmt, column);
        case SQLITE_TEXT: {
            const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
            const int bytes = sqlite3_column_bytes(stmt, column);
            return std::string(text ? text : "", static_cast<std::size_t>(bytes));
        }
        case SQLITE_BLOB:
            throw std::runtime_error("MetalORM 0.0.1: BLOB columns are not supported yet");
        default:
            throw std::runtime_error("MetalORM: unsupported SQLite column type");
    }
}

SQLiteExecutor::SQLiteExecutor(std::string filename) : impl_(std::make_unique<Impl>()) {
    if (sqlite3_open(filename.c_str(), &impl_->db) != SQLITE_OK) {
        const std::string message = impl_->db ? sqlite3_errmsg(impl_->db) : "unknown error";
        if (impl_->db) sqlite3_close(impl_->db);
        impl_->db = nullptr;
        throw std::runtime_error("MetalORM: sqlite3_open failed: " + message);
    }
    sqlite3_exec(impl_->db, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
}

SQLiteExecutor::~SQLiteExecutor() {
    if (impl_ && impl_->db) sqlite3_close(impl_->db);
}

SQLiteExecutor::SQLiteExecutor(SQLiteExecutor&& other) noexcept = default;
SQLiteExecutor& SQLiteExecutor::operator=(SQLiteExecutor&& other) noexcept = default;

QueryResult SQLiteExecutor::execute(const std::string& sql, const std::vector<Value>& params) {
    sqlite3_stmt* raw_stmt{};
    const int prepare_rc = sqlite3_prepare_v2(impl_->db, sql.c_str(), -1, &raw_stmt, nullptr);
    if (prepare_rc != SQLITE_OK) {
        throw std::runtime_error("MetalORM: SQLite prepare failed: " + std::string(sqlite3_errmsg(impl_->db)) + "\nSQL: " + sql);
    }

    struct Finalizer {
        void operator()(sqlite3_stmt* stmt) const noexcept { if (stmt) sqlite3_finalize(stmt); }
    };
    std::unique_ptr<sqlite3_stmt, Finalizer> stmt(raw_stmt);

    for (std::size_t i = 0; i < params.size(); ++i) {
        bind_value(stmt.get(), static_cast<int>(i + 1), params[i]);
    }

    QueryResult result;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_ROW) {
            Row row;
            const int count = sqlite3_column_count(stmt.get());
            for (int i = 0; i < count; ++i) {
                const char* name = sqlite3_column_name(stmt.get(), i);
                row.emplace(name ? name : "", column_value(stmt.get(), i));
            }
            result.rows.push_back(std::move(row));
            continue;
        }
        if (rc == SQLITE_DONE) break;
        throw std::runtime_error("MetalORM: SQLite execute failed: " + std::string(sqlite3_errmsg(impl_->db)) + "\nSQL: " + sql);
    }

    result.affected_rows = sqlite3_changes64(impl_->db);
    result.last_insert_id = sqlite3_last_insert_rowid(impl_->db);
    return result;
}

} // namespace metal
