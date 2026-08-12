add_executable(mcp_client_conformance_qt
    RunnerConfig.cpp
    ScenarioRegistry.cpp
    ScenarioHandlersAuthQt.cpp
    ScenarioHandlersStateless.cpp
    ScenarioHandlersStatelessHttp.cpp
    ScenarioHandlersStateless2026.cpp
    ConsoleStdioTransport.cpp
    main_qt.cpp
)

target_include_directories(mcp_client_conformance_qt PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/client/include
    ${CMAKE_SOURCE_DIR}/src/core/include
    ${CMAKE_SOURCE_DIR}/src/transport/include
)

target_compile_options(mcp_client_conformance_qt PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_link_libraries(mcp_client_conformance_qt PRIVATE
    mcp_qt_client
    mcp_core
    nlohmann_json::nlohmann_json
    Qt6::Core
)
