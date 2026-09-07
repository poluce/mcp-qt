#include "mcp_qt_client/McpServerView.h"
#include "mcp_qt_client/McpHost.h"
#include "mcp_qt_client/McpServerManager.h"
#include "mcp_qt_client/McpToolRouter.h"
#include "mcp_qt_client/McpPromptRouter.h"
#include "mcp_qt_client/McpResourceRouter.h"

namespace mcp_qt {

McpServerView::McpServerView(McpHost* host, QObject* parent)
    : QObject(parent), m_host(host) {}

void McpServerView::setVisibleServers(const QStringList& servers) {
    m_visibleServers = servers;
}

QStringList McpServerView::visibleServers() const {
    return m_visibleServers;
}

QStringList McpServerView::serverNames() const {
    return m_visibleServers;
}

bool McpServerView::isServerVisible(const QString& serverName) const {
    // 空列表 = 全部可见
    return m_visibleServers.isEmpty() || m_visibleServers.contains(serverName);
}

// ========== 工具 ==========

QJsonArray McpServerView::exportAllToolsToLlmFormat(McpQtClient::LlmFormat format) const {
    QJsonArray result;
    if (!m_host) return result;
    auto clients = m_host->manager()->clients();
    for (auto it = clients.begin(); it != clients.end(); ++it) {
        const QString& serverName = it.key();
        if (!isServerVisible(serverName)) continue;
        auto client = it.value();
        if (!client) continue;
        // 直接按可见服务器取缓存工具，避免"全量导出再按前缀反查"的边界坑
        for (const auto& tool : client->cachedTools()) {
            McpQtTool modified = tool;
            modified.name = serverName + QStringLiteral("_") + tool.name;
            result.append(McpQtClient::exportToolToLlmFormat(modified, format));
        }
    }
    return result;
}

QJsonArray McpServerView::exportAllToolsAsMcpSchema() const {
    QJsonArray result;
    if (!m_host) return result;
    auto clients = m_host->manager()->clients();
    for (auto it = clients.begin(); it != clients.end(); ++it) {
        const QString& serverName = it.key();
        if (!isServerVisible(serverName)) continue;
        auto client = it.value();
        if (!client) continue;
        for (const auto& tool : client->cachedTools()) {
            QJsonObject t;
            t[QStringLiteral("name")] = serverName + QStringLiteral("_") + tool.name;
            t[QStringLiteral("description")] = tool.description;
            t[QStringLiteral("inputSchema")] = tool.inputSchema;
            result.append(t);
        }
    }
    return result;
}

QList<McpQtTool> McpServerView::toolsForServer(const QString& serverName) const {
    QList<McpQtTool> result;
    if (!m_host || !isServerVisible(serverName)) return result;
    auto client = m_host->manager()->client(serverName);
    if (!client) return result;
    const auto tools = client->cachedTools();
    result.reserve(static_cast<int>(tools.size()));
    for (const auto& t : tools) result.append(t);
    return result;
}

void McpServerView::callToolAsync(const QString& nameSpacedToolName, const QJsonObject& arguments,
                                  std::function<void(McpResult)> callback,
                                  McpQtClient::ProgressCallback onProgress) {
    if (!m_host || !callback) return;

    // 归属校验：解析 serverName_ 前缀，不在可见列表直接拒绝，不发请求
    auto parsed = m_host->toolRouter()->parseToolName(nameSpacedToolName);
    if (parsed.first.isEmpty()) {
        McpResult err;
        err.isError = true;
        err.errorString = QStringLiteral("Unknown tool namespace: ") + nameSpacedToolName;
        callback(err);
        return;
    }
    if (!isServerVisible(parsed.first)) {
        McpResult err;
        err.isError = true;
        err.errorString = QStringLiteral("Server not visible to this view: ") + parsed.first;
        callback(err);
        return;
    }

    m_host->toolRouter()->callToolAsync(nameSpacedToolName, arguments, callback, onProgress);
}

// ========== 提示词 ==========

QJsonArray McpServerView::exportAllPrompts() const {
    QJsonArray result;
    if (!m_host) return result;
    auto clients = m_host->manager()->clients();
    for (auto it = clients.begin(); it != clients.end(); ++it) {
        const QString& serverName = it.key();
        if (!isServerVisible(serverName)) continue;
        auto client = it.value();
        if (!client) continue;
        const QJsonObject promptsObj = client->listPrompts();
        const QJsonArray prompts = promptsObj.value(QStringLiteral("prompts")).toArray();
        for (const auto& p : prompts) {
            QJsonObject po = p.toObject();
            po[QStringLiteral("name")] = serverName + QStringLiteral("_") + po.value(QStringLiteral("name")).toString();
            result.append(po);
        }
    }
    return result;
}

QJsonObject McpServerView::getPrompt(const QString& nameSpacedPromptName, const QJsonObject& arguments, int timeoutMs) {
    if (!m_host) return {};

    // 归属校验：前缀服务器不在可见列表直接拒绝，不发请求
    auto parsed = m_host->promptRouter()->parsePromptName(nameSpacedPromptName);
    if (parsed.first.isEmpty()) {
        return QJsonObject{{"error", "Unknown prompt namespace"}};
    }
    if (!isServerVisible(parsed.first)) {
        return QJsonObject{{"error", QStringLiteral("Server not visible to this view: ") + parsed.first}};
    }

    return m_host->promptRouter()->getPrompt(nameSpacedPromptName, arguments, timeoutMs);
}

void McpServerView::getPromptAsync(const QString& nameSpacedPromptName, const QJsonObject& arguments,
                                   std::function<void(const QJsonObject&, const QString&)> callback) {
    if (!m_host || !callback) return;

    // 归属校验：前缀服务器不在可见列表直接拒绝，不发请求
    auto parsed = m_host->promptRouter()->parsePromptName(nameSpacedPromptName);
    if (parsed.first.isEmpty()) {
        callback(QJsonObject(), QStringLiteral("Unknown prompt namespace"));
        return;
    }
    if (!isServerVisible(parsed.first)) {
        callback(QJsonObject(), QStringLiteral("Server not visible to this view: ") + parsed.first);
        return;
    }

    m_host->promptRouter()->getPromptAsync(nameSpacedPromptName, arguments, callback);
}

// ========== 资源 ==========

QJsonArray McpServerView::exportAllResources() const {
    QJsonArray result;
    if (!m_host) return result;
    auto clients = m_host->manager()->clients();
    for (auto it = clients.begin(); it != clients.end(); ++it) {
        const QString& serverName = it.key();
        if (!isServerVisible(serverName)) continue;
        auto client = it.value();
        if (!client) continue;
        const QJsonObject resourcesObj = client->listResources();
        const QJsonArray resources = resourcesObj.value(QStringLiteral("resources")).toArray();
        for (const auto& r : resources) {
            QJsonObject ro = r.toObject();
            // 与 McpResourceRouter 一致的 URI 重写：mcp-{serverName}-{uri}
            ro[QStringLiteral("uri")] = QStringLiteral("mcp-") + serverName + QStringLiteral("-")
                                        + ro.value(QStringLiteral("uri")).toString();
            result.append(ro);
        }
    }
    return result;
}

QJsonObject McpServerView::readResource(const QString& nameSpacedUri, int timeoutMs) {
    if (!m_host) return {};

    // 归属校验：前缀服务器不在可见列表直接拒绝，不发请求
    auto parsed = m_host->resourceRouter()->parseResourceUri(nameSpacedUri);
    if (parsed.first.isEmpty()) {
        return QJsonObject{{"error", "Unknown resource namespace"}};
    }
    if (!isServerVisible(parsed.first)) {
        return QJsonObject{{"error", QStringLiteral("Server not visible to this view: ") + parsed.first}};
    }

    return m_host->resourceRouter()->readResource(nameSpacedUri, timeoutMs);
}

void McpServerView::readResourceAsync(const QString& nameSpacedUri,
                                      std::function<void(const QJsonObject&, const QString&)> callback) {
    if (!m_host || !callback) return;

    // 归属校验：前缀服务器不在可见列表直接拒绝，不发请求
    auto parsed = m_host->resourceRouter()->parseResourceUri(nameSpacedUri);
    if (parsed.first.isEmpty()) {
        callback(QJsonObject(), QStringLiteral("Unknown resource namespace"));
        return;
    }
    if (!isServerVisible(parsed.first)) {
        callback(QJsonObject(), QStringLiteral("Server not visible to this view: ") + parsed.first);
        return;
    }

    m_host->resourceRouter()->readResourceAsync(nameSpacedUri, callback);
}

} // namespace mcp_qt
