cmake_minimum_required(VERSION 3.16)
project(mcp-cpp-agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch nlohmann/json (single-header JSON library)
include(FetchContent)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# SDK 核心库（纯 C++17，零外部框架依赖）
add_subdirectory(mcp_core)

# 可选：Qt 扩展层（需要 Qt6）
option(MCP_ENABLE_QT "Build Qt transport extensions and debugger example" OFF)

if(MCP_ENABLE_QT)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)
    set(CMAKE_AUTORCC ON)

    find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

    add_subdirectory(extensions/qt)
    add_subdirectory(examples/debugger)

    # 协议一致性测试（依赖 Qt 的 http transport 测试）
    add_subdirectory(conformance_runner)
endif()
