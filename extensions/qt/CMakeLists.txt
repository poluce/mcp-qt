add_library(mcp_qt STATIC
    include/mcp_qt/QtStdioTransport.h
    include/mcp_qt/QtHttpTransport.h
    include/mcp_qt/QtMcpClient.h
    include/mcp_qt/QtMcpToolBridge.h
    src/QtStdioTransport.cpp
    src/QtHttpTransport.cpp
    src/QtMcpClient.cpp
    src/QtMcpToolBridge.cpp
)

target_include_directories(mcp_qt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(mcp_qt PUBLIC
    mcp_core
    Qt6::Core
    Qt6::Network
)

add_executable(test_http_transport
    src/test_http_transport.cpp
)
target_link_libraries(test_http_transport PRIVATE
    mcp_qt
    mcp_core
    Qt6::Core
    Qt6::Network
)

