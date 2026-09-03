#include "metal/postgres_execution.hpp"

#include <libpq-fe.h>

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace metal {

namespace {

constexpr Oid postgres_bool_oid = 16;
constexpr Oid postgres_bytea_oid = 17;
constexpr Oid postgres_int8_oid = 20;
constexpr Oid postgres_int2_oid = 21;
constexpr Oid postgres_int4_oid = 23;
constexpr Oid postgres_float4_oid = 700;
constexpr Oid postgres_float8_oid = 701;

struct ResultDeleter {
    void operator()(PGresult* result) const noexcept {
        if (result) PQclear(result);
    }
};

using ResultPtr = std::unique_ptr<PGresult, ResultDeleter>;

std::string double_to_text(double value) {
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return out.str();
}

Value postgres_value(PGresult* result, int row, int column) {
    if (PQgetisnull(result, row, column)) return nullptr;

    const char* raw = PQgetvalue(result, row, column);
    const int length = PQgetlength(result, row, column);
    const Oid type = PQftype(result, column);

    switch (type) {
        case postgres_bool_oid:
            return raw && (raw[0] == 't' || raw[0] == '1');
        case postgres_int2_oid:
        case postgres_int4_oid:
        case postgres_int8_oid:
            return static_cast<std::int64_t>(std::stoll(std::string(raw, static_cast<std::size_t>(length))));
        case postgres_float4_oid:
        case postgres_float8_oid:
            return std::stod(std::string(raw, static_cast<std::size_t>(length)));
        case postgres_bytea_oid: {
            std::size_t decoded_length = 0;
            unsigned char* decoded = PQunescapeBytea(
                reinterpret_cast<const unsigned char*>(raw),
                &decoded_length);
            if (!decoded && decoded_length != 0) {
                throw std::runtime_error("MetalORM: failed to decode PostgreSQL bytea result");
            }
            Blob blob(decoded_length);
            if (decoded_length > 0) {
                std::memcpy(blob.data(), decoded, decoded_length);
            }
            if (decoded) PQfreemem(decoded);
            return blob;
        }
        default:
            return std::string(raw ? raw : "", static_cast<std::size_t>(length));
    }
}

} // namespace

struct PostgresExecutor::Impl {
    PGconn* connection{};
    std::mutex mutex;
};

PostgresExecutor::PostgresExecutor(std::string connection_string)
    : impl_(std::make_unique<Impl>()) {
    impl_->connection = PQconnectdb(connection_string.c_str());
    if (!impl_->connection || PQstatus(impl_->connection) != CONNECTION_OK) {
        const std::string message = impl_->connection
            ? PQerrorMessage(impl_->connection)
            : "libpq did not return a connection handle";
        if (impl_->connection) PQfinish(impl_->connection);
        impl_->connection = nullptr;
        throw std::runtime_error("MetalORM: PostgreSQL connection failed: " + message);
    }
}

PostgresExecutor::~PostgresExecutor() {
    if (impl_ && impl_->connection) PQfinish(impl_->connection);
}

PostgresExecutor::PostgresExecutor(PostgresExecutor&& other) noexcept = default;
PostgresExecutor& PostgresExecutor::operator=(PostgresExecutor&& other) noexcept = default;

QueryResult PostgresExecutor::execute(const std::string& sql, const std::vector<Value>& params) {
    std::lock_guard lock(impl_->mutex);

    std::vector<std::string> text_storage(params.size());
    std::vector<Oid> types(params.size(), 0);
    std::vector<const char*> values(params.size(), nullptr);
    std::vector<int> lengths(params.size(), 0);
    std::vector<int> formats(params.size(), 0);

    for (std::size_t i = 0; i < params.size(); ++i) {
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) {
                values[i] = nullptr;
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                text_storage[i] = std::to_string(value);
                values[i] = text_storage[i].c_str();
            } else if constexpr (std::is_same_v<T, double>) {
                text_storage[i] = double_to_text(value);
                values[i] = text_storage[i].c_str();
            } else if constexpr (std::is_same_v<T, std::string>) {
                text_storage[i] = value;
                values[i] = text_storage[i].c_str();
            } else if constexpr (std::is_same_v<T, bool>) {
                text_storage[i] = value ? "true" : "false";
                values[i] = text_storage[i].c_str();
                types[i] = postgres_bool_oid;
            } else if constexpr (std::is_same_v<T, Blob>) {
                values[i] = value.empty() ? "" : reinterpret_cast<const char*>(value.data());
                lengths[i] = static_cast<int>(value.size());
                formats[i] = 1;
                types[i] = postgres_bytea_oid;
            }
        }, params[i]);
    }

    ResultPtr result(PQexecParams(
        impl_->connection,
        sql.c_str(),
        static_cast<int>(params.size()),
        types.empty() ? nullptr : types.data(),
        values.empty() ? nullptr : values.data(),
        lengths.empty() ? nullptr : lengths.data(),
        formats.empty() ? nullptr : formats.data(),
        0));

    if (!result) {
        throw std::runtime_error("MetalORM: PostgreSQL execute failed: " + std::string(PQerrorMessage(impl_->connection)) + "\nSQL: " + sql);
    }

    const auto status = PQresultStatus(result.get());
    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
        throw std::runtime_error("MetalORM: PostgreSQL execute failed: " + std::string(PQresultErrorMessage(result.get())) + "\nSQL: " + sql);
    }

    QueryResult output;
    const int row_count = PQntuples(result.get());
    const int column_count = PQnfields(result.get());
    output.rows.reserve(static_cast<std::size_t>(row_count));

    for (int row_index = 0; row_index < row_count; ++row_index) {
        Row row;
        for (int column = 0; column < column_count; ++column) {
            const char* name = PQfname(result.get(), column);
            row.emplace(name ? name : "", postgres_value(result.get(), row_index, column));
        }
        output.rows.push_back(std::move(row));
    }

    if (const char* affected = PQcmdTuples(result.get()); affected && *affected) {
        output.affected_rows = static_cast<std::int64_t>(std::stoll(affected));
    }
    return output;
}

void PostgresExecutor::begin_transaction() { (void)execute("BEGIN;"); }
void PostgresExecutor::commit_transaction() { (void)execute("COMMIT;"); }
void PostgresExecutor::rollback_transaction() { (void)execute("ROLLBACK;"); }

void PostgresExecutor::validate_savepoint_name(std::string_view name) {
    if (name.empty()) throw std::invalid_argument("MetalORM: savepoint name cannot be empty");
    const auto first = static_cast<unsigned char>(name.front());
    if (!std::isalpha(first) && first != '_') {
        throw std::invalid_argument("MetalORM: savepoint name must be a simple SQL identifier");
    }
    for (const unsigned char c : name.substr(1)) {
        if (!std::isalnum(c) && c != '_') {
            throw std::invalid_argument("MetalORM: savepoint name must be a simple SQL identifier");
        }
    }
}

void PostgresExecutor::savepoint(std::string_view name) {
    validate_savepoint_name(name);
    (void)execute("SAVEPOINT " + std::string(name) + ";");
}

void PostgresExecutor::release_savepoint(std::string_view name) {
    validate_savepoint_name(name);
    (void)execute("RELEASE SAVEPOINT " + std::string(name) + ";");
}

void PostgresExecutor::rollback_to_savepoint(std::string_view name) {
    validate_savepoint_name(name);
    (void)execute("ROLLBACK TO SAVEPOINT " + std::string(name) + ";");
}

} // namespace metal
