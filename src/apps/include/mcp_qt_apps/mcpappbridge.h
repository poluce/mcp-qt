#pragma once
// McpAppBridge.h — MCP Apps 宿主侧 AppBridge（SEP-1865, io.modelcontextprotocol/ui）
//
// 作为宿主(Host)与 View(iframe) 之间的双向代理，自动转发 MCP 服务器能力：
//   - ui/initialize 初始化握手与能力协商（hostCapabilities/hostInfo/hostContext）
//   - tools/call / tools/list / resources/read 服务器代理（含权限与可见性校验）
//   - ui/open-link / ui/message / ui/update-model-context / ui/request-display-mode 方言
//   - Host → View 通知（tool-input / tool-result / host-context-changed / teardown）
//
// 用法：
//   McpAppBridge bridge;
//   bridge.attach(renderer.get(), mcpClient);
//   bridge.setPermissionPolicy({}, {});            // 可选权限限制
//   bridge.setOpenLinkHandler([](const QJsonObject& p, auto respond){ respond(QJsonObject{}, 0, {}); });
//   bridge.start();

#include "mcp_qt_apps/IMcpAppRenderer.h"
#include <QJsonObject>
#include <QObject>
#include <functional>
#include <memory>
#include <vector>

namespace mcp_qt {

class McpQtClient;

/**
 * @brief MCP Apps AppBridge：View ↔ Host ↔ MCP 服务器的代理。
 *
 * 自动接管渲染器的 setAppMessageHandler，解析 App 的 JSON-RPC 方言消息，
 * 按方法路由（ui/ 方言、tools/、resources/），并代理到 McpQtClient。
 */
class McpAppBridge : public QObject {
    Q_OBJECT
public:
    explicit McpAppBridge(QObject* parent = nullptr);
    ~McpAppBridge() override;

    // ---- 装配 ----
    /// 绑定渲染器与 MCP 服务器客户端（必须设置后才可 start）。
    void attach(IMcpAppRenderer* renderer, std::shared_ptr<McpQtClient> client);

    /// 启动服务：接管渲染器消息回调，开始响应 View。
    void start();
    /// 停止服务：不再处理 View 消息。
    void stop();

    /// 是否已启动。
    bool isRunning() const { return m_running; }

    /// 设置宿主标识（默认 name="mcp-qt-client" version="1.0.0"）。
    void setHostInfo(const QString& name, const QString& version);
    /// 设置 AppBridge 协议版本（默认 "2026-01-26"，MCP Apps stable 版本）。
    void setProtocolVersion(const QString& version);

    // ---- A2 权限策略 ----
    /**
     * @brief 设置 App 权限策略。
     * @param allowedTools  允许 App 调用的工具名集合；空 = 全部放行（仍受 visibility 约束）。
     * @param allowedCapabilities 允许 App 请求的宿主能力（openLink / message 等）；空 = 全部放行。
     */
    void setPermissionPolicy(const std::vector<QString>& allowedTools,
                             const std::vector<QString>& allowedCapabilities);

    // ---- A3 ui/ 方言处理器（默认拒绝/空响应，可覆盖）----
    using UiRequestRespond = std::function<void(const QJsonObject& result, int errorCode, const QString& errorMessage)>;
    /// ui/open-link：打开外部 URL。默认拒绝（-32000 Link opening denied）。
    void setOpenLinkHandler(std::function<void(const QJsonObject& params, UiRequestRespond respond)> handler);
    /// ui/message：向会话发消息。默认拒绝。
    void setMessageHandler(std::function<void(const QJsonObject& params, UiRequestRespond respond)> handler);
    /// ui/update-model-context：更新模型上下文。默认接受。
    void setUpdateModelContextHandler(std::function<void(const QJsonObject& params, UiRequestRespond respond)> handler);
    /// ui/request-display-mode：请求切换显示模式。默认返回 inline。
    void setRequestDisplayModeHandler(std::function<void(const QJsonObject& params, UiRequestRespond respond)> handler);

    // ---- Host → View 通知 ----
    /// ui/notifications/tool-input：发送完整工具参数。
    void sendToolInput(const QJsonObject& arguments);
    /// ui/notifications/tool-input-partial：发送流式部分参数。
    void sendToolInputPartial(const QJsonObject& arguments);
    /// ui/notifications/tool-result：发送工具执行结果（标准 CallToolResult）。
    void sendToolResult(const QJsonObject& callToolResult);
    /// ui/notifications/tool-cancelled：通知工具取消。
    void sendToolCancelled(const QString& reason);
    /// ui/notifications/host-context-changed：通知宿主上下文变化。
    void sendHostContextChanged(const QJsonObject& partialContext);
    /// ui/resource-teardown：销毁资源前通知（并等待响应）。
    void teardownResource(const QString& reason = QString());

    /// 触发 tools/list_changed 通知（服务器工具列表变化时）。
    void sendToolListChanged();

private:
    void handleMessage(const QJsonObject& message);
    void handleInitialize(const QJsonObject& params, qint64 id);
    void handleToolsCall(const QJsonObject& params, qint64 id);
    void handleToolsList(qint64 id);
    void handleReadResource(const QJsonObject& params, qint64 id);
    void handlePing(qint64 id);
    void handleOpenLink(const QJsonObject& params, qint64 id);
    void handleMessageReq(const QJsonObject& params, qint64 id);
    void handleUpdateModelContext(const QJsonObject& params, qint64 id);
    void handleRequestDisplayMode(const QJsonObject& params, qint64 id);

    /// 工具是否允许 App 调用：allowedTools 白名单 + _meta.ui.visibility 含 "app"。
    bool isToolCallAllowed(const QString& name) const;
    /// 工具对指定主体可见（visibility 缺省 = model+app 均可）。
    static bool hasVisibility(const QJsonObject& meta, const QString& which);
    /// 从客户端缓存查工具（返回其 meta）。
    QJsonObject toolMetaByName(const QString& name) const;

    void sendResponse(qint64 id, const QJsonObject& result);
    void sendError(qint64 id, int code, const QString& message);

    IMcpAppRenderer* m_renderer{nullptr};
    std::shared_ptr<McpQtClient> m_client;
    bool m_running{false};

    QString m_hostName{QStringLiteral("mcp-qt-client")};
    QString m_hostVersion{QStringLiteral("1.0.0")};
    QString m_protocolVersion{QStringLiteral("2026-01-26")};

    std::vector<QString> m_allowedTools;
    std::vector<QString> m_allowedCapabilities;

    std::function<void(const QJsonObject&, UiRequestRespond)> m_openLinkHandler;
    std::function<void(const QJsonObject&, UiRequestRespond)> m_messageHandler;
    std::function<void(const QJsonObject&, UiRequestRespond)> m_updateModelContextHandler;
    std::function<void(const QJsonObject&, UiRequestRespond)> m_requestDisplayModeHandler;
};

} // namespace mcp_qt
