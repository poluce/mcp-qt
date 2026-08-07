add_library(mcp_core STATIC
    include/mcp_core/mcp_core.h
    include/mcp_core/McpResource.h
    include/mcp_core/McpPrompt.h
    include/mcp_core/IMcpTransport.h
    include/mcp_core/McpMessage.h
    include/mcp_core/McpTool.h
    include/mcp_core/McpClientSession.h
    include/mcp_core/McpHeaderEncoding.h
    include/mcp_core/JsonRpcDispatcher.h
    include/mcp_core/McpOAuthClient.h
    src/JsonRpcDispatcher.cpp
    src/McpClientSession.cpp
    src/McpOAuthClient.cpp
    src/McpHeaderEncoding.cpp
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

if(WIN32)
    target_link_libraries(mcp_core PUBLIC bcrypt)
endif()
