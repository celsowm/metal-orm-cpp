cmake_minimum_required(VERSION 3.20)
project(metal-orm VERSION 0.0.10 LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "MetalORM 0.0.10 is intentionally C++26-native and currently requires GCC 16+ reflection support")
endif()

if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR "MetalORM 0.0.10 requires GCC 16 or newer")
endif()

find_package(SQLite3 REQUIRED)

add_library(metal-orm STATIC
    src/sqlite_executor.cpp
)
add_library(metal::orm ALIAS metal-orm)

target_include_directories(metal-orm PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(metal-orm PUBLIC SQLite::SQLite3)
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
endif()
