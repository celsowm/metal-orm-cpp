#pragma once

#include "metal/execution.hpp"
#include "metal/schema_types.hpp"

#include <cstdlib>
#include <string>
#include <vector>

namespace metal {

struct EntityGeneratorOptions {
    std::string namespace_name{"entities"};
    bool emit_relations{true};
    bool emit_comments{true};
};

struct GeneratedEntityHeader {
    std::string code;
    std::vector<std::string> warnings;
};

/** Generate one self-contained C++26 header from already-introspected schema metadata. */
GeneratedEntityHeader generate_entity_header(
    const DatabaseSchema& schema,
    const EntityGeneratorOptions& options = {});

/** Introspect SQLite and generate a C++26 reflection-native entity header. */
GeneratedEntityHeader generate_sqlite_entity_header(
    DbExecutor& executor,
    const EntityGeneratorOptions& options = {},
    const IntrospectOptions& introspect_options = {});

} // namespace metal
