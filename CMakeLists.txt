cmake_minimum_required(VERSION 3.20)
project(metal-orm VERSION 0.0.1 LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "MetalORM 0.0.1 is intentionally C++26-native and currently requires GCC 16+ reflection support")
endif()

if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR "MetalORM 0.0.1 requires GCC 16 or newer")
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
target_compile_features(metal-orm PUBLIC cxx_std_23)
target_compile_options(metal-orm PUBLIC -std=c++26 -freflection)

option(METAL_ORM_BUILD_EXAMPLES "Build MetalORM examples" ON)
option(METAL_ORM_BUILD_TESTS "Build MetalORM tests" ON)

if(METAL_ORM_BUILD_EXAMPLES)
    add_executable(metal-orm-quickstart examples/quickstart.cpp)
    target_link_libraries(metal-orm-quickstart PRIVATE metal::orm)
endif()

if(METAL_ORM_BUILD_TESTS)
    enable_testing()
    add_executable(metal-orm-tests tests/test_main.cpp)
    target_link_libraries(metal-orm-tests PRIVATE metal::orm)
    add_test(NAME metal-orm-tests COMMAND metal-orm-tests)
endif()
