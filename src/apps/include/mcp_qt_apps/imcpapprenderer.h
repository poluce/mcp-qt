#pragma once
// IMcpAppRenderer.h — MCP Apps 渲染抽象接口
//
// 渲染层可插拔：默认提供 WebView2 后端（McpAppWebView2Renderer），
// 未来可替换为 QCefView / QtWebEngine / WebKitGTK。
// 协议层（McpAppSupport）只与抽象接口交互。

#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <QWidget>
#include <functional>

namespace mcp_qt {

/**
 * @brief MCP Apps 渲染器抽象。
 *
 * 职责：
 *   - 提供一个可嵌入的 QWidget（沙箱隔离的 WebView 宿主）
 *   - 加载服务器返回的 HTML（text/html;profile=mcp-app）
 *   - 与 App 双向通信（postMessage JSON-RPC 方言）
 *   - 权限策略（App 声明的 capabilities 约束）
 */
class IMcpAppRenderer {
public:
    virtual ~IMcpAppRenderer() = default;

    /**
     * @brief 渲染器承载的 QWidget（调用方负责 addWidget / 布局 / 生命周期）
     */
    virtual QWidget* hostWidget() = 0;

    /**
     * @brief 加载 App HTML 内容。
     * @param html   服务器 ui:// 资源返回的 HTML
     * @param baseUrl 用于解析相对资源的基础地址
     */
    virtual void loadHtml(const QString& html, const QUrl& baseUrl) = 0;

    /**
     * @brief 宿主 → App：发送一条 AppBridge 消息（JSON-RPC）。
     *        经 postMessage 通道送达 App 的 window.chrome.webview message 事件。
     */
    virtual void sendMessageToApp(const QJsonObject& message) = 0;

    /**
     * @brief 注册 App → 宿主 消息回调（渲染器解析 postMessage 后调用）。
     */
    virtual void setAppMessageHandler(std::function<void(const QJsonObject& message)> handler) = 0;

    /**
     * @brief 设置权限策略（沙箱允许的能力子集）。空实现为默认放行指定工具调用。
     * @param allowedTools  允许 App 调用的 MCP 工具名集合
     * @param allowedCapabilities 允许 App 请求的宿主能力（sendOpenLink 等）
     */
    virtual void setPermissionPolicy(const std::vector<QString>& allowedTools,
                                     const std::vector<QString>& allowedCapabilities) = 0;
};

} // namespace mcp_qt
