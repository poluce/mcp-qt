add_library(mcp_core STATIC
    include/mcp_core/ConsoleStdioTransport.h
    include/mcp_core/SubprocessStdioTransport.h
    include/mcp_core/HttpSseTransport.h
    include/mcp_core/mcp_core.h
    include/mcp_core/McpResource.h
    include/mcp_core/McpPrompt.h
    include/mcp_core/IMcpTransport.h
    include/mcp_core/McpMessage.h
    include/mcp_core/McpTool.h
    include/mcp_core/McpClientSession.h
    include/mcp_core/JsonRpcDispatcher.h
    include/mcp_core/McpOAuthClient.h
    src/ConsoleStdioTransport.cpp
    src/SubprocessStdioTransport.cpp
    src/HttpSseTransport.cpp
    src/JsonRpcDispatcher.cpp
    src/McpClientSession.cpp
    src/McpOAuthClient.cpp
)

target_compile_options(mcp_core PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_include_directories(mcp_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(mcp_core PUBLIC
    $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>
    CURL::libcurl
    httplib::httplib
)

install(TARGETS mcp_core
    EXPORT mcp_coreTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/mcp_core DESTINATION include)

install(EXPORT mcp_coreTargets
    FILE mcp_coreTargets.cmake
    NAMESPACE mcp::
    DESTINATION lib/cmake/mcp_core
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/mcp_coreConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/mcp_coreConfig.cmake
    INSTALL_DESTINATION lib/cmake/mcp_core
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/mcp_coreConfigVersion.cmake
    VERSION 1.0.0
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/mcp_coreConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/mcp_coreConfigVersion.cmake
    DESTINATION lib/cmake/mcp_core
)
