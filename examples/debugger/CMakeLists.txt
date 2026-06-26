cmake_minimum_required(VERSION 3.16)
project(app LANGUAGES CXX)

add_executable(app
    include/AgentController.h
    include/ToolManager.h
    include/ChatWindow.h
    src/main.cpp
    src/AgentController.cpp
    src/ToolManager.cpp
    src/ChatWindow.cpp
)

target_include_directories(app PRIVATE
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../mcp_core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../extensions/qt/include
)

target_link_libraries(app PRIVATE
    mcp_qt
    mcp_core
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
)
