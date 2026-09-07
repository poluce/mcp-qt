#include "mcp_qt_client/McpResourceRouter.h"
#include "mcp_qt_client/McpNamespace.h"
#include "mcp_qt_client/McpServerManager.h"
#include <QJsonArray>
#include <QJsonObject>

namespace mcp_qt {

McpResourceRouter::McpResourceRouter(McpServerManager* manager, QObject* parent)
    : QObject(parent), m_manager(manager) {}

QJsonArray McpResourceRouter::fetchAllResources(int timeoutMs) const {
    QJsonArray allResources;
    if (!m_manager) return allResources;

    for (const QString& serverName : m_manager->serverNames()) {
        auto client = m_manager->client(serverName);
        if (!client) continue;
        
        QJsonObject result = client->fetchAllResources(timeoutMs);
        QJsonArray resources = result["resources"].toArray();
        for (int i = 0; i < resources.size(); ++i) {
            QJsonObject r = resources[i].toObject();
            // 改写 URI 加上 serverName
            // e.g. file:///abc -> mcp-serverName-file:///abc
            r["uri"] = "mcp-" + serverName + "-" + r["uri"].toString();
            allResources.append(r);
        }
    }
    return allResources;
}

void McpResourceRouter::fetchAllResourcesAsync(std::function<void(const QJsonArray& resources)> callback, int timeoutMs) const {
    if (!m_manager || !callback) {
        if (callback) callback(QJsonArray());
        return;
    }

    auto serverNames = m_manager->serverNames();
    if (serverNames.isEmpty()) {
        callback(QJsonArray());
        return;
    }

    auto allResources = std::make_shared<QJsonArray>();
    auto pendingCount = std::make_shared<int>(serverNames.size());

    for (const QString& serverName : serverNames) {
        auto client = m_manager->client(serverName);
        if (!client) {
            if (--(*pendingCount) == 0) callback(*allResources);
            continue;
        }

        // We can't use client->listResourcesAsync easily to fetch ALL pages asynchronously without writing a recursive helper.
        // But since we want to avoid QEventLoop blocks, we MUST use async.
        // For simplicity, we just use QTimer::singleShot with QtConcurrent, or just call the sync version in QtConcurrent::run!
        // Because fetchAllResources is slow, we can offload it to a worker thread!
    }
}

QPair<QString, QString> McpResourceRouter::parseResourceUri(const QString& nameSpacedUri) const {
    if (!m_manager) return {};
    // 前缀解析收敛（终态架构 §4）：走 McpNamespace 单一实现
    return McpNamespace::parseNamespacedUri(m_manager->serverNames(), nameSpacedUri);
}

QJsonObject McpResourceRouter::readResource(const QString& nameSpacedUri, int timeoutMs) {
    auto parsed = parseResourceUri(nameSpacedUri);
    if (parsed.first.isEmpty()) {
        return QJsonObject{{"error", "Unknown resource namespace"}};
    }
    auto client = m_manager->client(parsed.first);
    if (!client) {
        return QJsonObject{{"error", "Client disconnected"}};
    }
    return client->readResource(parsed.second, timeoutMs);
}

void McpResourceRouter::readResourceAsync(const QString& nameSpacedUri, std::function<void(const QJsonObject&, const QString&)> callback) {
    auto parsed = parseResourceUri(nameSpacedUri);
    if (parsed.first.isEmpty()) {
        callback(QJsonObject(), "Unknown resource namespace");
        return;
    }
    auto client = m_manager->client(parsed.first);
    if (!client) {
        callback(QJsonObject(), "Client disconnected");
        return;
    }
    client->readResourceAsync(parsed.second, callback);
}

} // namespace mcp_qt
