set(CMAKE_AUTOMOC ON)

add_library(mcp_qt_client STATIC
    include/mcp_qt_client/McpQtClient.h
    src/McpQtClient.cpp
    include/mcp_qt_client/McpToolsModel.h
    src/McpToolsModel.cpp
    include/mcp_qt_client/McpPromptsModel.h
    src/McpPromptsModel.cpp
    include/mcp_qt_client/McpResourcesModel.h
    src/McpResourcesModel.cpp
    include/mcp_qt_client/McpResourceTemplatesModel.h
    src/McpResourceTemplatesModel.cpp
)

target_include_directories(mcp_qt_client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/mcp_qt_transport/include
    ${CMAKE_SOURCE_DIR}/mcp_core/include
)

target_link_libraries(mcp_qt_client PUBLIC
    mcp_qt_transport
    mcp_core
    nlohmann_json::nlohmann_json
    Qt6::Core
    Qt6::Network
)

target_compile_features(mcp_qt_client PUBLIC cxx_std_17)
target_compile_options(mcp_qt_client PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
