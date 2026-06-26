add_executable(mcp_core_tests
    main_standalone.cpp
    common.cpp
    protocol/test_initialize.cpp
    protocol/test_json_rpc.cpp
    protocol/test_capabilities.cpp
    protocol/test_error_response.cpp
    transport/test_stdio_transport.cpp
    transport/test_process_lifecycle.cpp
    features/test_tools.cpp
    features/test_resources.cpp
    features/test_prompts.cpp
    features/test_notifications.cpp
    features/test_new_capabilities.cpp
)

# Windows 控制台应用
if(WIN32)
    set_target_properties(mcp_core_tests PROPERTIES
        WIN32_EXECUTABLE FALSE
    )
endif()

target_compile_options(mcp_core_tests PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_include_directories(mcp_core_tests PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(mcp_core_tests PRIVATE
    mcp_core
    nlohmann_json::nlohmann_json
)
