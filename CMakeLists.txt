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

# Fetch libcurl (HTTP client with built-in platform SSL support)
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

# Fetch cpp-httplib (header-only HTTP client, used for SSE GET on Windows)
FetchContent_Declare(
    cpp-httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.1.tar.gz
)
set(CPPHTTPLIB_OPENSSL_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cpp-httplib)

# SDK 核心库（纯 C++17，零外部框架依赖）
add_subdirectory(mcp_core)

# 独立测试（纯 C++17，不需要 Qt）
add_subdirectory(tests)

# 官方一致性测试客户端（纯 C++17，不需要 Qt）
add_subdirectory(conformance_runner)


