set(CMAKE_AUTOMOC ON)

add_executable(tests_qt
    ${CMAKE_SOURCE_DIR}/tests/common.cpp
    ${CMAKE_SOURCE_DIR}/tests/features/test_typed_results.cpp
    main_qt.cpp
    test_qt_sse_parser.cpp
    test_qt_http_transport.cpp
    test_qt_transport_shutdown.cpp
    test_qt_session_integration.cpp
    test_qt_http_request_config.cpp
    test_qt_resource_router.cpp
    test_qt_tools_model.cpp
    test_qt_transport_recovery.cpp
    test_qt_stateless_http_transport.cpp
)

target_include_directories(tests_qt PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/mcp_qt_transport/include
    ${CMAKE_SOURCE_DIR}/mcp_qt_transport/src
    ${CMAKE_SOURCE_DIR}/mcp_qt_client/include
)

target_link_libraries(tests_qt PRIVATE
    mcp_qt_transport
    mcp_qt_client
    Qt6::Core
    Qt6::Network
    Qt6::Test
)

# 独立的 HTTPS 运行时测试目标，防止 TLS 动态装载冲突挂掉主测试
add_executable(tests_qt_https_runtime EXCLUDE_FROM_ALL
    test_qt_https_runtime.cpp
    ../tests/common.cpp
)

target_include_directories(tests_qt_https_runtime PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/mcp_qt_transport/include
    ${CMAKE_SOURCE_DIR}/mcp_qt_transport/src
)

target_link_libraries(tests_qt_https_runtime PRIVATE
    mcp_qt_transport
    Qt6::Core
    Qt6::Network
)
