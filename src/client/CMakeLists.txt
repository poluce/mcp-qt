set(CMAKE_AUTOMOC ON)

add_library(mcp_qt_client STATIC
    include/mcp_qt_client/McpServerConfig.h
    include/mcp_qt_client/IMcpConfigLoader.h
    include/mcp_qt_client/McpJsonConfigLoader.h
    src/McpJsonConfigLoader.cpp
    include/mcp_qt_client/McpError.h
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
    include/mcp_qt_client/McpServerManager.h
    src/McpServerManager.cpp
    include/mcp_qt_client/McpToolRouter.h
    src/McpToolRouter.cpp
    include/mcp_qt_client/McpPromptRouter.h
    src/McpPromptRouter.cpp
    include/mcp_qt_client/McpResourceRouter.h
    src/McpResourceRouter.cpp
    include/mcp_qt_client/McpHost.h
    src/McpHost.cpp
    include/mcp_qt_client/McpServerView.h
    src/McpServerView.cpp
    include/mcp_qt_client/McpDiagnosticReporter.h
    src/McpDiagnosticReporter.cpp
    include/mcp_qt_client/es256jwt.h
    src/es256jwt.cpp
)

target_include_directories(mcp_qt_client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/transport/include
    ${CMAKE_SOURCE_DIR}/src/core/include
)

target_link_libraries(mcp_qt_client PUBLIC
    mcp_qt_transport
    mcp_core
    nlohmann_json::nlohmann_json
    Qt6::Core
    Qt6::Network
)

# ES256 JWT assertion 签名后端：Windows 用系统 CNG（BCrypt，零新增依赖），
# 非 Windows 沿用仓库既有 OpenSSL 依赖（src/core/mcpoauthclient.cpp 已 include <openssl/sha.h>）。
if(WIN32)
    target_link_libraries(mcp_qt_client PRIVATE bcrypt)
else()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(mcp_qt_client PRIVATE OpenSSL::Crypto)
endif()

target_compile_features(mcp_qt_client PUBLIC cxx_std_17)
target_compile_options(mcp_qt_client PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
