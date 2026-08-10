#pragma once

#include "metal/entity_generator.hpp"

namespace metal {

GeneratedEntityHeader generate_view_header(
    const DatabaseSchema& schema,
    const EntityGeneratorOptions& options = {});

GeneratedEntityHeader generate_sqlite_view_header(
    DbExecutor& executor,
    const EntityGeneratorOptions& options = {},
    const IntrospectOptions& introspect_options = {});

GeneratedEntityHeader generate_model_header(
    const DatabaseSchema& schema,
    const EntityGeneratorOptions& options = {});

GeneratedEntityHeader generate_sqlite_model_header(
    DbExecutor& executor,
    const EntityGeneratorOptions& options = {},
    const IntrospectOptions& introspect_options = {});

} // namespace metal
