#include <metal/postgres_execution.hpp>

#include <cassert>
#include <stdexcept>
#include <string>

int main() {
    bool failed = false;
    try {
        metal::PostgresExecutor executor{
            "host=127.0.0.1 port=1 dbname=metal_orm_unreachable connect_timeout=1"};
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        failed = message.find("MetalORM: PostgreSQL connection failed:") != std::string::npos;
    }
    assert(failed);
}
