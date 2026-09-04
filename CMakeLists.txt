cmake_minimum_required(VERSION 3.16)
project(mcp-qt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/Zc:__cplusplus)
endif()

# Fetch nlohmann/json (single-header JSON library)
include(FetchContent)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Network Test)

# Add SDK Core and Qt components
add_subdirectory(src/core)
add_subdirectory(src/transport)
add_subdirectory(src/client)
add_subdirectory(src/apps)

# If this is the top-level project, build tests and examples
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    add_subdirectory(conformance_runner_qt)
    add_subdirectory(examples)
    add_subdirectory(test build_test)
endif()
