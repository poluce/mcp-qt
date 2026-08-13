set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_library(multi_server_agent_core STATIC
    AgentSession.cpp
    AgentRegistry.cpp
    AgentReconciler.cpp
    LlmAgentExecutor.cpp
    LlmBackends.cpp
    LlmCredentialResolver.cpp
    LlmCredentialResolver.h
    AgentMainWindow.cpp
    AgentMainWindow.h
)

target_include_directories(multi_server_agent_core PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/client/include
    ${CMAKE_SOURCE_DIR}/src/core/include
    ${CMAKE_SOURCE_DIR}/src/apps/include
    ${CMAKE_SOURCE_DIR}/src/transport/include
)

target_link_libraries(multi_server_agent_core PUBLIC
    mcp_qt_client
    mcp_qt_apps
    mcp_qt_transport
    Qt6::Core
    Qt6::Network
    Qt6::Widgets
)

target_compile_features(multi_server_agent_core PUBLIC cxx_std_17)

add_executable(multi_server_agent main.cpp)

target_include_directories(multi_server_agent PRIVATE
    ${CMAKE_SOURCE_DIR}/src/apps/include
    ${CMAKE_SOURCE_DIR}/src/transport/include
)

target_link_libraries(multi_server_agent PRIVATE
    multi_server_agent_core
    mcp_qt_apps
    mcp_qt_transport
    Qt6::Core
    Qt6::Network
    Qt6::Widgets
)

target_compile_features(multi_server_agent PRIVATE cxx_std_17)

# MCP Apps 渲染（WebView2）：运行时可执行文件旁需提供 WebView2Loader.dll（动态加载）
if(WIN32)
    add_custom_command(TARGET multi_server_agent POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/webview2/sdk/runtimes/win-x64/native/WebView2Loader.dll"
            "$<TARGET_FILE_DIR:multi_server_agent>/WebView2Loader.dll"
        COMMENT "Copy WebView2Loader.dll"
    )
    target_link_libraries(multi_server_agent PRIVATE user32 ole32 oleaut32)
endif()
