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
    src/JsonRpcDispatcher.cpp
    src/McpClientSession.cpp
)

target_compile_definitions(mcp_core PUBLIC
    $<$<BOOL:${MCP_ENABLE_HTTP}>:MCP_ENABLE_HTTP>
)

target_include_directories(mcp_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(mcp_core PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_link_libraries(mcp_core PUBLIC
    nlohmann_json::nlohmann_json
)

if(MCP_ENABLE_HTTP)
    target_sources(mcp_core PRIVATE
        src/HttpSseTransport.cpp
        src/McpOAuthClient.cpp
    )
    target_link_libraries(mcp_core PUBLIC
        CURL::libcurl
        httplib::httplib
        OpenSSL::SSL
        OpenSSL::Crypto
    )
endif()
