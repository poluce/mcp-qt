cmake_minimum_required(VERSION 3.16)
project(mcp-cpp-agent LANGUAGES CXX)

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

# HTTP 传输层开关：关闭后无需编译 libcurl 和 cpp-httplib（约 8 分钟）
option(MCP_ENABLE_HTTP "Build HTTP/SSE transport and OAuth client (requires libcurl)" ON)
if(MCP_ENABLE_HTTP)
    # Fetch libcurl
    set(CURL_DISABLE_TESTS ON CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(HTTP_ONLY OFF CACHE BOOL "" FORCE)
    set(ENABLE_IPV6 OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
    set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
    set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBSSH OFF CACHE BOOL "" FORCE)
    set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
    if(WIN32)
        set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
    else()
        set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(
        curl
        URL https://github.com/curl/curl/releases/download/curl-8_12_1/curl-8.12.1.tar.gz
    )
    FetchContent_MakeAvailable(curl)

    # Fetch cpp-httplib (用于 SSE GET，开启 SSL 支持 HTTPS)
    # 先查找 OpenSSL：Qt MinGW 工具链自带 OpenSSL 开发库
    if(WIN32 AND NOT OpenSSL_FOUND)
        # Qt MinGW 的 opt 目录包含 OpenSSL
        foreach(_qt_mingw_path
                "E:/Qt6/Tools/mingw1310_64/opt"
                "C:/Qt/Tools/mingw1310_64/opt"
                "$ENV{Qt6_DIR}/../../Tools/mingw1310_64/opt")
            if(EXISTS "${_qt_mingw_path}/include/openssl/ssl.h")
                set(OPENSSL_ROOT_DIR "${_qt_mingw_path}" CACHE PATH "OpenSSL root directory")
                message(STATUS "Found OpenSSL at: ${OPENSSL_ROOT_DIR}")
                break()
            endif()
        endforeach()
    endif()
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        message(STATUS "OpenSSL enabled for cpp-httplib SSE HTTPS support")
    else()
        message(STATUS "OpenSSL not found — cpp-httplib SSE GET will not support HTTPS")
    endif()
    FetchContent_Declare(
        cpp-httplib
        URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.1.tar.gz
    )
    set(CPPHTTPLIB_OPENSSL_SUPPORT ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(cpp-httplib)
endif()

# SDK 核心库（纯 C++17，零外部框架依赖）
add_subdirectory(mcp_core)

# 独立测试（纯 C++17，不需要 Qt）
add_subdirectory(tests)

# 官方一致性测试客户端（纯 C++17，不需要 Qt）
add_subdirectory(conformance_runner)

option(MCP_ENABLE_QT_TRANSPORT "Build Qt-based HTTP/SSE transport for Qt agents" OFF)

if(MCP_ENABLE_QT_TRANSPORT)
    find_package(Qt6 REQUIRED COMPONENTS Core Network)
    add_subdirectory(mcp_qt_transport)
    add_subdirectory(mcp_qt_client)
    add_subdirectory(conformance_runner_qt)
endif()
