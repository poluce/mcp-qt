// McpAppBridge.cpp — MCP Apps AppBridge 宿主侧代理实现
#include "mcp_qt_apps/McpAppBridge.h"
#include "mcp_qt_apps/McpAppSupport.h"
#include "mcp_qt_client/McpQtClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QDebug>

namespace mcp_qt {

// ========== 静态工具 ==========
namespace {
// 从工具 meta 提取 visibility 数组（_meta.ui.visibility）
QJsonArray visibilityOf(const QJsonObject& meta) {
    const QJsonObject ui = meta.value(QStringLiteral("ui")).toObject();
    return ui.value(QStringLiteral("visibility")).toArray();
}
} // namespace

McpAppBridge::McpAppBridge(QObject* parent) : QObject(parent) {}
McpAppBridge::~McpAppBridge() = default;

void McpAppBridge::attach(IMcpAppRenderer* renderer, std::shared_ptr<McpQtClient> client) {
    m_renderer = renderer;
    m_client = std::move(client);
}

void McpAppBridge::start() {
    if (!m_renderer) {
        qWarning() << "[McpAppBridge] start() called before attach()";
        return;
    }
    m_running = true;
    // 接管渲染器消息回调，统一按 AppBridge 协议分发
    m_renderer->setAppMessageHandler([this](const QJsonObject& message) {
        if (!m_running) return;
        handleMessage(message);
    });
    qInfo() << "[McpAppBridge] started, protocolVersion=" << m_protocolVersion;
}

void McpAppBridge::stop() {
    m_running = false;
    if (m_renderer) {
        m_renderer->setAppMessageHandler(nullptr);
    }
}

void McpAppBridge::setHostInfo(const QString& name, const QString& version) {
    m_hostName = name;
    m_hostVersion = version;
}

void McpAppBridge::setProtocolVersion(const QString& version) {
    m_protocolVersion = version;
}

void McpAppBridge::setPermissionPolicy(const std::vector<QString>& allowedTools,
                                       const std::vector<QString>& allowedCapabilities) {
    m_allowedTools = allowedTools;
    m_allowedCapabilities = allowedCapabilities;
}

void McpAppBridge::setOpenLinkHandler(std::function<void(const QJsonObject&, UiRequestRespond)> handler) {
    m_openLinkHandler = std::move(handler);
}
void McpAppBridge::setMessageHandler(std::function<void(const QJsonObject&, UiRequestRespond)> handler) {
    m_messageHandler = std::move(handler);
}
void McpAppBridge::setUpdateModelContextHandler(std::function<void(const QJsonObject&, UiRequestRespond)> handler) {
    m_updateModelContextHandler = std::move(handler);
}
void McpAppBridge::setRequestDisplayModeHandler(std::function<void(const QJsonObject&, UiRequestRespond)> handler) {
    m_requestDisplayModeHandler = std::move(handler);
}

// ========== 消息分发 ==========
void McpAppBridge::handleMessage(const QJsonObject& message) {
    QString method;
    qint64 id = 0;
    QJsonObject params;
    if (!McpAppSupport::parseAppMessage(message, &method, &id, &params)) {
        qWarning() << "[McpAppBridge] ignoring non-JSON-RPC message:" << message;
        return;
    }
    qDebug() << "[McpAppBridge] received method=" << method << "id=" << id;

    if (method == QStringLiteral("ui/initialize")) {
        handleInitialize(params, id);
    } else if (method == QStringLiteral("tools/call")) {
        handleToolsCall(params, id);
    } else if (method == QStringLiteral("tools/list")) {
        handleToolsList(id);
    } else if (method == QStringLiteral("resources/read")) {
        handleReadResource(params, id);
    } else if (method == QStringLiteral("ping")) {
        handlePing(id);
    } else if (method == QStringLiteral("ui/open-link")) {
        handleOpenLink(params, id);
    } else if (method == QStringLiteral("ui/message")) {
        handleMessageReq(params, id);
    } else if (method == QStringLiteral("ui/update-model-context")) {
        handleUpdateModelContext(params, id);
    } else if (method == QStringLiteral("ui/request-display-mode")) {
        handleRequestDisplayMode(params, id);
    } else if (method.startsWith(QStringLiteral("ui/notifications/"))) {
        // View 通知：记录或忽略（initialized / size-changed 等）
        qDebug() << "[McpAppBridge] notification ignored:" << method;
    } else if (method == QStringLiteral("notifications/message")) {
        qDebug() << "[McpAppBridge] app log:" << params;
    } else {
        qWarning() << "[McpAppBridge] unknown method:" << method;
        sendError(id, -32601, QStringLiteral("Method not found: ") + method);
    }
}

// ========== ui/initialize 握手 ==========
void McpAppBridge::handleInitialize(const QJsonObject& params, qint64 id) {
    // params.appCapabilities：App 声明的能力（tools / sendOpenLink / availableDisplayModes 等）
    const QJsonObject result{
        {QStringLiteral("protocolVersion"), m_protocolVersion},
        {QStringLiteral("hostCapabilities"), QJsonObject{
            {QStringLiteral("serverTools"), QJsonObject{{QStringLiteral("listChanged"), true}}},
            {QStringLiteral("serverResources"), QJsonObject{{QStringLiteral("listChanged"), true}}},
            {QStringLiteral("openLinks"), QJsonObject{}},
            {QStringLiteral("logging"), QJsonObject{}}
        }},
        {QStringLiteral("hostInfo"), QJsonObject{
            {QStringLiteral("name"), m_hostName},
            {QStringLiteral("version"), m_hostVersion}
        }},
        {QStringLiteral("hostContext"), QJsonObject{
            {QStringLiteral("displayMode"), QStringLiteral("inline")},
            {QStringLiteral("availableDisplayModes"), QJsonArray{QStringLiteral("inline")}},
            {QStringLiteral("platform"), QStringLiteral("desktop")}
        }}
    };
    sendResponse(id, result);
}

// ========== tools/call 代理（A1）==========
void McpAppBridge::handleToolsCall(const QJsonObject& params, qint64 id) {
    const QString name = params.value(QStringLiteral("name")).toString();
    if (name.isEmpty()) {
        sendError(id, -32602, QStringLiteral("Invalid params: missing tool name"));
        return;
    }

    // A2 权限校验：allowedTools 白名单 + visibility 含 "app"
    if (!isToolCallAllowed(name)) {
        sendError(id, -32000, QStringLiteral("Tool call denied: %1").arg(name));
        return;
    }

    if (!m_client) {
        sendError(id, -32603, QStringLiteral("No MCP server client attached"));
        return;
    }

    const QJsonObject arguments = params.value(QStringLiteral("arguments")).toObject();
    m_client->callToolAsync(name, arguments, [this, id](mcp_qt::McpResult result) {
        if (!m_running) return;
        // 标准 CallToolResult：result.data 即服务端返回的 result 对象
        QJsonObject callResult = result.data;
        if (result.isError && !callResult.contains(QStringLiteral("isError"))) {
            callResult[QStringLiteral("isError")] = true;
        }
        if (result.isError && !result.errorString.isEmpty()
            && !callResult.contains(QStringLiteral("content"))) {
            callResult[QStringLiteral("content")] = QJsonArray{
                QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                            {QStringLiteral("text"), result.errorString}}
            };
        }
        sendResponse(id, callResult);
    });
}

// ========== tools/list 代理 ==========
void McpAppBridge::handleToolsList(qint64 id) {
    if (!m_client) {
        sendError(id, -32603, QStringLiteral("No MCP server client attached"));
        return;
    }
    QJsonArray tools;
    for (const auto& t : m_client->cachedTools()) {
        // 按 visibility 过滤：只暴露 App 可见的工具
        if (!hasVisibility(t.meta, QStringLiteral("app"))) {
            continue;
        }
        // A2 权限：如果配置了 allowedTools 白名单，只暴露白名单内工具
        if (!m_allowedTools.empty()) {
            bool found = false;
            for (const auto& allowed : m_allowedTools) {
                if (allowed == t.name) { found = true; break; }
            }
            if (!found) continue;
        }
        QJsonObject toolJson{
            {QStringLiteral("name"), t.name},
            {QStringLiteral("description"), t.description},
            {QStringLiteral("inputSchema"), t.inputSchema}
        };
        if (!t.meta.isEmpty()) {
            toolJson[QStringLiteral("_meta")] = t.meta;
        }
        tools.append(toolJson);
    }
    sendResponse(id, QJsonObject{{QStringLiteral("tools"), tools}});
}

// ========== resources/read 代理 ==========
void McpAppBridge::handleReadResource(const QJsonObject& params, qint64 id) {
    if (!m_client) {
        sendError(id, -32603, QStringLiteral("No MCP server client attached"));
        return;
    }
    const QString uri = params.value(QStringLiteral("uri")).toString();
    if (uri.isEmpty()) {
        sendError(id, -32602, QStringLiteral("Invalid params: missing uri"));
        return;
    }
    m_client->readResourceAsync(uri, [this, id](const QJsonObject& result, const QString& error) {
        if (!m_running) return;
        if (!error.isEmpty()) {
            sendError(id, -32000, error);
            return;
        }
        sendResponse(id, result);
    });
}

void McpAppBridge::handlePing(qint64 id) {
    sendResponse(id, QJsonObject{});
}

// ========== A3 ui/ 方言 ==========
void McpAppBridge::handleOpenLink(const QJsonObject& params, qint64 id) {
    if (m_openLinkHandler) {
        m_openLinkHandler(params, [this, id](const QJsonObject& result, int code, const QString& msg) {
            if (code == 0) sendResponse(id, result);
            else sendError(id, code, msg);
        });
        return;
    }
    // 默认拒绝（安全默认）
    sendError(id, -32000, QStringLiteral("Link opening denied"));
}

void McpAppBridge::handleMessageReq(const QJsonObject& params, qint64 id) {
    if (m_messageHandler) {
        m_messageHandler(params, [this, id](const QJsonObject& result, int code, const QString& msg) {
            if (code == 0) sendResponse(id, result);
            else sendError(id, code, msg);
        });
        return;
    }
    sendError(id, -32000, QStringLiteral("Message sending denied"));
}

void McpAppBridge::handleUpdateModelContext(const QJsonObject& params, qint64 id) {
    if (m_updateModelContextHandler) {
        m_updateModelContextHandler(params, [this, id](const QJsonObject& result, int code, const QString& msg) {
            if (code == 0) sendResponse(id, result);
            else sendError(id, code, msg);
        });
        return;
    }
    // 默认接受：存储最新模型上下文
    qDebug() << "[McpAppBridge] update-model-context accepted";
    sendResponse(id, QJsonObject{});
}

void McpAppBridge::handleRequestDisplayMode(const QJsonObject& params, qint64 id) {
    if (m_requestDisplayModeHandler) {
        m_requestDisplayModeHandler(params, [this, id](const QJsonObject& result, int code, const QString& msg) {
            if (code == 0) sendResponse(id, result);
            else sendError(id, code, msg);
        });
        return;
    }
    // 默认只支持 inline
    sendResponse(id, QJsonObject{{QStringLiteral("mode"), QStringLiteral("inline")}});
}

// ========== Host → View 通知 ==========
void McpAppBridge::sendToolInput(const QJsonObject& arguments) {
    if (!m_running || !m_renderer) return;
    m_renderer->sendMessageToApp(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), QStringLiteral("ui/notifications/tool-input")},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("arguments"), arguments}}}
    });
}

void McpAppBridge::sendToolInputPartial(const QJsonObject& arguments) {
    if (!m_running || !m_renderer) return;
    m_renderer->sendMessageToApp(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), QStringLiteral("ui/notifications/tool-input-partial")},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("arguments"), arguments}}}
    });
}

void McpAppBridge::sendToolResult(const QJsonObject& callToolResult) {
    if (!m_running || !m_renderer) return;
    m_renderer->sendMessageToApp(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), QStringLiteral("ui/notifications/tool-result")},
        {QStringLiteral("params"), callToolResult}
    });
}

void McpAppBridge::sendToolCancelled(const QString& reason) {
    if (!m_running || !m_renderer) return;
    m_renderer->sendMessageToApp(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), QStringLiteral("ui/notifications/tool-cancelled")},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("reason"), reason}}}
    });
}

void McpAppBridge::sendHostContextChanged(const QJsonObject& partialContext) {
    if (!m_running || !m_renderer) return;
    m_renderer->sendMessageToApp(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), QStringLiteral("ui/notifications/host-context-changed")},
        {QStringLiteral("params"), partialContext}
    });
}

void McpAppBridge::teardownResource(const QString& reason) {
    if (!m_running || !m_renderer) return;
    m_renderer->sendMessageToApp(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), QStringLiteral("ui/resource-teardown")},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("reason"), reason}}}
    });
}

void McpAppBridge::sendToolListChanged() {
    if (!m_running || !m_renderer) return;
    m_renderer->sendMessageToApp(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), QStringLiteral("ui/notifications/tool-list-changed")},
        {QStringLiteral("params"), QJsonObject{}}
    });
}

// ========== 权限与可见性 ==========
bool McpAppBridge::hasVisibility(const QJsonObject& meta, const QString& which) {
    const QJsonArray vis = visibilityOf(meta);
    // 缺省 = ["model", "app"]，即全部可见
    if (vis.isEmpty()) return true;
    for (const auto& v : vis) {
        if (v.toString() == which) return true;
    }
    return false;
}

QJsonObject McpAppBridge::toolMetaByName(const QString& name) const {
    if (!m_client) return {};
    for (const auto& t : m_client->cachedTools()) {
        if (t.name == name) return t.meta;
    }
    return {};
}

bool McpAppBridge::isToolCallAllowed(const QString& name) const {
    // 1. 客户端缓存中必须有该工具
    if (!m_client) return false;
    bool exists = false;
    for (const auto& t : m_client->cachedTools()) {
        if (t.name == name) { exists = true; break; }
    }
    if (!exists) return false;

    // 2. allowedTools 白名单（空 = 全部放行）
    if (!m_allowedTools.empty()) {
        bool found = false;
        for (const auto& allowed : m_allowedTools) {
            if (allowed == name) { found = true; break; }
        }
        if (!found) return false;
    }

    // 3. visibility 必须含 "app"
    const QJsonObject meta = toolMetaByName(name);
    if (!hasVisibility(meta, QStringLiteral("app"))) return false;

    return true;
}

// ========== 响应发送 ==========
void McpAppBridge::sendResponse(qint64 id, const QJsonObject& result) {
    if (!m_running || !m_renderer) return;
    m_renderer->sendMessageToApp(McpAppSupport::buildResponse(id, result));
}

void McpAppBridge::sendError(qint64 id, int code, const QString& message) {
    if (!m_running || !m_renderer) return;
    m_renderer->sendMessageToApp(McpAppSupport::buildError(id, code, message));
}

} // namespace mcp_qt
