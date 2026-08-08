#pragma once
// McpAppWebView2Renderer.h — MCP Apps WebView2 渲染后端（Windows）
//
// 基于 Microsoft WebView2（Edge WebView2 Runtime，Windows 10/11 系统预装）。
// 通过动态加载 WebView2Loader.dll 接入，避免对 MSVC 构建的静态库 ABI 依赖，
// 使 MinGW 工具链可直接使用。
//
// 特性：
//   - QWidget 宿主（内含 HWND，WebView2 控制器绑定为子窗口）
//   - 注入 AppBridge 桥接脚本：window.mcpAppHost.postMessage(json) / 监听宿主消息
//   - JS → C++：window.chrome.webview.postMessage → WebMessageReceived → 回调
//   - C++ → JS：PostWebMessageAsJson
//   - 隔离进程渲染（WebView2 原生沙箱语义）

#include "IMcpAppRenderer.h"
#include <QJsonObject>
#include <QPointer>
#include <QUrl>
#include <QWidget>
#include <functional>
#include <memory>
#include <vector>

// 前置声明 WebView2 COM 接口（避免污染头文件）
struct ICoreWebView2;
struct ICoreWebView2Controller;
struct ICoreWebView2Environment;

namespace mcp_qt {

/**
 * @brief WebView2 渲染后端。
 *
 * 用法：
 *   auto renderer = std::make_shared<McpAppWebView2Renderer>();
 *   auto w = renderer->hostWidget();   // 嵌入你的布局
 *   renderer->setAppMessageHandler([this](const QJsonObject& m){ ... });
 *   renderer->loadHtml(html, baseUrl);
 */
class McpAppWebView2Renderer : public QWidget, public IMcpAppRenderer {
    Q_OBJECT
public:
    explicit McpAppWebView2Renderer(QWidget* parent = nullptr);
    ~McpAppWebView2Renderer() override;

    // IMcpAppRenderer
    QWidget* hostWidget() override { return this; }
    void loadHtml(const QString& html, const QUrl& baseUrl) override;
    void sendMessageToApp(const QJsonObject& message) override;
    void setAppMessageHandler(std::function<void(const QJsonObject&)> handler) override;
    void setPermissionPolicy(const std::vector<QString>& allowedTools,
                             const std::vector<QString>& allowedCapabilities) override;

    /**
     * @brief 异步初始化 WebView2（首次调用自动创建环境/控制器）。
     * @param onReady 初始化完成回调（成功 true）
     */
    void initializeAsync(std::function<void(bool ok, const QString& error)> onReady = nullptr);

    /**
     * @brief 当前是否已初始化（可导航）。
     */
    bool isReady() const { return m_ready; }

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void ensureInitialized();
    void injectBridgeScript();
    void handleWebMessage(const QString& json);
    void postToJs(const QString& jsonString);

    // WebView2 COM 状态
    void* m_environment{nullptr};     // ICoreWebView2Environment*
    void* m_controller{nullptr};      // ICoreWebView2Controller*
    void* m_webview{nullptr};         // ICoreWebView2*
    bool m_ready{false};
    bool m_initStarted{false};
    QString m_pendingHtml;
    QUrl m_pendingBaseUrl;

    std::function<void(const QJsonObject&)> m_messageHandler;
    std::function<void(bool, const QString&)> m_onReady;

    QString m_bridgeScript;
    QJsonObject m_pendingQueue; // 待初始化的消息队列（简化：初始化前消息丢弃）
};

} // namespace mcp_qt
