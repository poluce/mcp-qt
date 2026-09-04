# MCP Apps 模块（io.modelcontextprotocol/ui 扩展）
# 独立模块：协议层(McpAppSupport) + 渲染抽象 + WebView2 后端
# 渲染：WebView2（Windows 默认后端，动态加载 WebView2Loader.dll）

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_library(mcp_qt_apps STATIC
    include/mcp_qt_apps/McpAppSupport.h
    include/mcp_qt_apps/IMcpAppRenderer.h
    include/mcp_qt_apps/McpAppBridge.h
    include/mcp_qt_apps/McpAppContentAdapter.h
    include/mcp_qt_apps/McpAppCompatibility.h
    include/mcp_qt_apps/McpAppWebView2Renderer.h
    src/McpAppSupport.cpp
    src/McpAppBridge.cpp
    src/McpAppContentAdapter.cpp
    src/adapters/CesiumDownloadsAdapter.cpp
    src/adapters/StandardCompatibilityAdapters.cpp
    src/McpAppWebView2Renderer.cpp
    mcp_app_assets.qrc
)

target_include_directories(mcp_qt_apps PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# WebView2 SDK（动态加载 WebView2Loader.dll，仅需 COM 头文件）
# 定位策略：
# 1. 宿主可注入 MCP_QT_WEBVIEW2_INCLUDE_DIR 缓存变量（submodule 嵌入场景）；
# 2. 否则用 CMAKE_CURRENT_SOURCE_DIR 相对定位——不能用 CMAKE_SOURCE_DIR，
#    submodule 嵌入宿主时它是宿主根，路径错位导致 WebView2.h 找不到（issue #11）。
set(MCP_QT_WEBVIEW2_INCLUDE_DIR "" CACHE PATH "WebView2 SDK 头文件目录（宿主可注入）")
if(MCP_QT_WEBVIEW2_INCLUDE_DIR)
    target_include_directories(mcp_qt_apps PRIVATE ${MCP_QT_WEBVIEW2_INCLUDE_DIR})
else()
    # CMAKE_CURRENT_SOURCE_DIR = <mcp-qt>/src/apps，回仓库根需 ../../（→ <mcp-qt>/third_party）
    target_include_directories(mcp_qt_apps PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/webview2/sdk/build/native/include
    )
endif()

target_compile_options(mcp_qt_apps PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)
target_link_libraries(mcp_qt_apps PUBLIC
    mcp_qt_client
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
)

if(WIN32)
    # WebView2Loader 动态加载（运行时可执行文件旁需提供 WebView2Loader.dll）；
    # 仅需 COM/COMCTL 等系统库
    target_link_libraries(mcp_qt_apps PUBLIC user32 ole32 oleaut32 comctl32)
endif()
