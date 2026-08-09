cmake_minimum_required(VERSION 3.20)
project(metal-orm VERSION 0.0.23 LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "MetalORM 0.0.23 is intentionally C++26-native and currently requires GCC 16+ reflection support")
endif()

if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR "MetalORM 0.0.23 requires GCC 16 or newer")
endif()

find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

add_library(metal-orm STATIC
    src/sqlite_executor.cpp
)
add_library(metal::orm ALIAS metal-orm)

target_include_directories(metal-orm PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(metal-orm PUBLIC SQLite::SQLite3 Threads::Threads)
target_compile_options(metal-orm PUBLIC -std=c++26 -freflection)

option(METAL_ORM_BUILD_EXAMPLES "Build MetalORM examples" ON)
option(METAL_ORM_BUILD_TESTS "Build MetalORM tests" ON)

if(METAL_ORM_BUILD_EXAMPLES)
    add_executable(metal-orm-quickstart examples/quickstart.cpp)
    target_link_libraries(metal-orm-quickstart PRIVATE metal::orm)
endif()

if(METAL_ORM_BUILD_TESTS)
    function(metal_expect_compile_failure name source expected_message)
        try_compile(
            did_compile
            "${CMAKE_BINARY_DIR}/compile-fail-${name}"
            SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
            CMAKE_FLAGS
                "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                "-DCMAKE_CXX_FLAGS=-std=c++26 -freflection -I${CMAKE_CURRENT_SOURCE_DIR}/include"
            OUTPUT_VARIABLE compiler_output
        )

        if(did_compile)
            message(FATAL_ERROR "Expected compile failure '${name}' unexpectedly compiled")
        endif()

        string(FIND "${compiler_output}" "${expected_message}" diagnostic_position)
        if(diagnostic_position EQUAL -1)
            message(FATAL_ERROR
                "Compile failure '${name}' did not contain expected MetalORM diagnostic '${expected_message}'.\n${compiler_output}")
        endif()
    endfunction()

    metal_expect_compile_failure(
        incompatible-relation-keys
        tests/compile_fail/incompatible_relation_keys.cpp
        "has_many key types are incompatible"
    )
    metal_expect_compile_failure(
        wrong-relation-shape
        tests/compile_fail/wrong_relation_shape.cpp
        "has_many member must be metal::has_many_collection<T>"
    )
    metal_expect_compile_failure(
        raw-shared-ptr-single-relation
        tests/compile_fail/raw_shared_ptr_single_relation.cpp
        "belongs_to member must be metal::belongs_to_reference<T>"
    )
    metal_expect_compile_failure(
        raw-shared-ptr-has-one
        tests/compile_fail/raw_shared_ptr_has_one.cpp
        "has_one member must be metal::has_one_reference<T>"
    )
    metal_expect_compile_failure(
        wrong-pivot-collection
        tests/compile_fail/wrong_pivot_collection.cpp
        "many_to_many collection pivot type must match the reflected pivot annotation"
    )
    metal_expect_compile_failure(
        wrong-pivot-patch-member
        tests/compile_fail/wrong_pivot_patch_member.cpp
        "pivot_patch member must belong to the pivot type"
    )
    metal_expect_compile_failure(
        wrong-pivot-patch-value
        tests/compile_fail/wrong_pivot_patch_value.cpp
        "pivot_patch value is incompatible with pivot member type"
    )
    metal_expect_compile_failure(
        wrong-morph-shape
        tests/compile_fail/wrong_morph_shape.cpp
        "morph_one member must be metal::morph_one_reference<T>"
    )
    metal_expect_compile_failure(
        duplicate-morph-discriminator
        tests/compile_fail/duplicate_morph_discriminator.cpp
        "morph_to discriminator values must be non-empty and unique"
    )
    metal_expect_compile_failure(
        wrong-graph-value
        tests/compile_fail/wrong_graph_value.cpp
        "graph value is incompatible with the reflected member type"
    )
    metal_expect_compile_failure(
        wrong-schema-index-member
        tests/compile_fail/wrong_schema_index_member.cpp
        "schema index members must belong to the declared entity"
    )
    metal_expect_compile_failure(
        wrong-tree-parent
        tests/compile_fail/wrong_tree_parent.cpp
        "tree parent column must be std::optional<T> so root nodes can be NULL"
    )
    metal_expect_compile_failure(
        wrong-dto-member
        tests/compile_fail/wrong_dto_member.cpp
        "DTO members must belong to the declared entity"
    )
    metal_expect_compile_failure(
        duplicate-column-default
        tests/compile_fail/duplicate_column_default.cpp
        "a column can declare at most one default annotation"
    )

    enable_testing()

    add_executable(metal-orm-tests tests/test_main.cpp)
    target_link_libraries(metal-orm-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-tests COMMAND metal-orm-tests)

    add_executable(metal-orm-query-tests tests/test_query.cpp)
    target_link_libraries(metal-orm-query-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-query-tests COMMAND metal-orm-query-tests)

    add_executable(metal-orm-runtime-parity-tests tests/test_runtime_parity.cpp)
    target_link_libraries(metal-orm-runtime-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-runtime-parity-tests COMMAND metal-orm-runtime-parity-tests)

    add_executable(metal-orm-collection-parity-tests tests/test_collection_parity.cpp)
    target_link_libraries(metal-orm-collection-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-collection-parity-tests COMMAND metal-orm-collection-parity-tests)

    add_executable(metal-orm-target-key-parity-tests tests/test_target_key_parity.cpp)
    target_link_libraries(metal-orm-target-key-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-target-key-parity-tests COMMAND metal-orm-target-key-parity-tests)

    add_executable(metal-orm-morph-parity-tests tests/test_morph_parity.cpp)
    target_link_libraries(metal-orm-morph-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-morph-parity-tests COMMAND metal-orm-morph-parity-tests)

    add_executable(metal-orm-dml-parity-tests tests/test_dml_parity.cpp)
    target_link_libraries(metal-orm-dml-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-dml-parity-tests COMMAND metal-orm-dml-parity-tests)

    add_executable(metal-orm-advanced-query-parity-tests tests/test_advanced_query_parity.cpp)
    target_link_libraries(metal-orm-advanced-query-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-advanced-query-parity-tests COMMAND metal-orm-advanced-query-parity-tests)

    add_executable(metal-orm-computed-query-parity-tests tests/test_computed_query_parity.cpp)
    target_link_libraries(metal-orm-computed-query-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-computed-query-parity-tests COMMAND metal-orm-computed-query-parity-tests)

    add_executable(metal-orm-relation-pagination-parity-tests tests/test_relation_pagination_parity.cpp)
    target_link_libraries(metal-orm-relation-pagination-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-relation-pagination-parity-tests COMMAND metal-orm-relation-pagination-parity-tests)

    add_executable(metal-orm-transaction-parity-tests tests/test_transaction_parity.cpp)
    target_link_libraries(metal-orm-transaction-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-transaction-parity-tests COMMAND metal-orm-transaction-parity-tests)

    add_executable(metal-orm-lifecycle-events-parity-tests tests/test_lifecycle_events_parity.cpp)
    target_link_libraries(metal-orm-lifecycle-events-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-lifecycle-events-parity-tests COMMAND metal-orm-lifecycle-events-parity-tests)

    add_executable(metal-orm-graph-parity-tests tests/test_graph_parity.cpp)
    target_link_libraries(metal-orm-graph-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-graph-parity-tests COMMAND metal-orm-graph-parity-tests)

    add_executable(metal-orm-single-reference-parity-tests tests/test_single_reference_parity.cpp)
    target_link_libraries(metal-orm-single-reference-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-single-reference-parity-tests COMMAND metal-orm-single-reference-parity-tests)

    add_executable(metal-orm-schema-parity-tests tests/test_schema_parity.cpp)
    target_link_libraries(metal-orm-schema-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-schema-parity-tests COMMAND metal-orm-schema-parity-tests)

    add_executable(metal-orm-bulk-parity-tests tests/test_bulk_parity.cpp)
    target_link_libraries(metal-orm-bulk-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-bulk-parity-tests COMMAND metal-orm-bulk-parity-tests)

    add_executable(metal-orm-tree-parity-tests tests/test_tree_parity.cpp)
    target_link_libraries(metal-orm-tree-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-tree-parity-tests COMMAND metal-orm-tree-parity-tests)

    add_executable(metal-orm-tree-remove-parity-tests tests/test_tree_remove_parity.cpp)
    target_link_libraries(metal-orm-tree-remove-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-tree-remove-parity-tests COMMAND metal-orm-tree-remove-parity-tests)

    add_executable(metal-orm-dto-openapi-parity-tests tests/test_dto_openapi_parity.cpp)
    target_link_libraries(metal-orm-dto-openapi-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-dto-openapi-parity-tests COMMAND metal-orm-dto-openapi-parity-tests)

    add_executable(metal-orm-dto-filter-parity-tests tests/test_dto_filter_parity.cpp)
    target_link_libraries(metal-orm-dto-filter-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-dto-filter-parity-tests COMMAND metal-orm-dto-filter-parity-tests)

    add_executable(metal-orm-openapi-tree-parity-tests tests/test_openapi_tree_parity.cpp)
    target_link_libraries(metal-orm-openapi-tree-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-openapi-tree-parity-tests COMMAND metal-orm-openapi-tree-parity-tests)

    add_executable(metal-orm-dto-relation-filter-parity-tests tests/test_dto_relation_filter_parity.cpp)
    target_link_libraries(metal-orm-dto-relation-filter-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-dto-relation-filter-parity-tests COMMAND metal-orm-dto-relation-filter-parity-tests)

    add_executable(metal-orm-openapi-relations-parity-tests tests/test_openapi_relations_parity.cpp)
    target_link_libraries(metal-orm-openapi-relations-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-openapi-relations-parity-tests COMMAND metal-orm-openapi-relations-parity-tests)

    add_executable(metal-orm-column-default-parity-tests tests/test_column_defaults_parity.cpp)
    target_link_libraries(metal-orm-column-default-parity-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-column-default-parity-tests COMMAND metal-orm-column-default-parity-tests)
endif()
