#include "mcp_qt_client/McpPromptRouter.h"
#include "mcp_qt_client/McpNamespace.h"
#include "mcp_qt_client/McpServerManager.h"
#include <QJsonArray>
#include <QJsonObject>

namespace mcp_qt {

McpPromptRouter::McpPromptRouter(McpServerManager* manager, QObject* parent)
    : QObject(parent), m_manager(manager) 
{
    if (m_manager) {
        connect(m_manager, &McpServerManager::clientConnected, this, &McpPromptRouter::promptsChanged);
        connect(m_manager, &McpServerManager::clientDisconnected, this, &McpPromptRouter::promptsChanged);
        connect(m_manager, &McpServerManager::clientPromptsChanged, this, &McpPromptRouter::promptsChanged);
    }
}

QJsonArray McpPromptRouter::fetchAllPrompts(int timeoutMs) const {
    QJsonArray allPrompts;
    if (!m_manager) return allPrompts;

    for (const QString& serverName : m_manager->serverNames()) {
        auto client = m_manager->client(serverName);
        if (!client || !client->hasPromptsCapability()) continue;
        
        QJsonObject result = client->fetchAllPrompts(timeoutMs);
        QJsonArray prompts = result["prompts"].toArray();
        for (int i = 0; i < prompts.size(); ++i) {
            QJsonObject p = prompts[i].toObject();
            p["name"] = serverName + "_" + p["name"].toString();
            allPrompts.append(p);
        }
    }
    return allPrompts;
}

QPair<QString, QString> McpPromptRouter::parsePromptName(const QString& nameSpacedPromptName) const {
    if (!m_manager) return {};
    // 前缀解析收敛（终态架构 §4）：走 McpNamespace 单一实现
    return McpNamespace::parseNamespacedName(m_manager->serverNames(), nameSpacedPromptName);
}

QJsonObject McpPromptRouter::getPrompt(const QString& nameSpacedPromptName, const QJsonObject& arguments, int timeoutMs) {
    auto parsed = parsePromptName(nameSpacedPromptName);
    if (parsed.first.isEmpty()) {
        return QJsonObject{{"error", "Unknown prompt namespace"}};
    }
    auto client = m_manager->client(parsed.first);
    if (!client) {
        return QJsonObject{{"error", "Client disconnected"}};
    }
    return client->getPrompt(parsed.second, arguments, timeoutMs);
}

void McpPromptRouter::getPromptAsync(const QString& nameSpacedPromptName, const QJsonObject& arguments,
                                     std::function<void(const QJsonObject&, const QString&)> callback) {
    auto parsed = parsePromptName(nameSpacedPromptName);
    if (parsed.first.isEmpty()) {
        callback(QJsonObject(), "Unknown prompt namespace");
        return;
    }
    auto client = m_manager->client(parsed.first);
    if (!client) {
        callback(QJsonObject(), "Client disconnected");
        return;
    }
    client->getPromptAsync(parsed.second, arguments, callback);
}

} // namespace mcp_qt
