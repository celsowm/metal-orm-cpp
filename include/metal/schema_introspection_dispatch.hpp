#pragma once

#include "metal/postgres_schema_introspection.hpp"
#include "metal/query/core_types.hpp"
#include "metal/schema_introspection.hpp"

#include <stdexcept>

namespace metal {

inline DatabaseSchema introspect_schema(
    DbExecutor& executor,
    const Dialect& dialect,
    const IntrospectOptions& options = {}) {
    switch (dialect.family()) {
        case DialectFamily::SQLite:
            return introspect_sqlite(executor, options);
        case DialectFamily::PostgreSQL:
            return introspect_postgres(executor, options);
        case DialectFamily::Generic:
            throw std::logic_error(
                "MetalORM: schema introspection is not available for the generic dialect");
    }
    throw std::logic_error("MetalORM: unknown dialect family");
}

} // namespace metal
