cmake_minimum_required(VERSION 3.16)
project(mcp_client_playwright_qt)

find_package(Qt6 COMPONENTS Core Network REQUIRED)

add_executable(mcp_client_playwright_qt main.cpp)

target_link_libraries(mcp_client_playwright_qt PRIVATE
    Qt6::Core
    Qt6::Network
    mcp_qt_client
)
