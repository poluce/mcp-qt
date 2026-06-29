add_executable(mcp_client_conformance_qt
    ${CMAKE_SOURCE_DIR}/conformance_runner/RunnerConfig.cpp
    ${CMAKE_SOURCE_DIR}/conformance_runner/ScenarioRegistry.cpp
    ${CMAKE_SOURCE_DIR}/conformance_runner/ScenarioHandlersList.cpp
    main_qt.cpp
    ScenarioHandlersAuthQt.cpp
)

target_include_directories(mcp_client_conformance_qt PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/conformance_runner
    ${CMAKE_SOURCE_DIR}/mcp_qt_client/include
    ${CMAKE_SOURCE_DIR}/mcp_core/include
)

target_compile_options(mcp_client_conformance_qt PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_link_libraries(mcp_client_conformance_qt PRIVATE
    mcp_qt_client
    mcp_core
    nlohmann_json::nlohmann_json
    CURL::libcurl
    Qt6::Core
)
