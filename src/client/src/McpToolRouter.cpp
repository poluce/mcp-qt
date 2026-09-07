#include <mcp_qt_client/McpToolRouter.h>
#include <mcp_qt_client/McpNamespace.h>
#include <mcp_qt_client/McpServerManager.h>
#include <QDebug>

#include <QPromise>

namespace mcp_qt {

namespace {
    QFuture<McpResult> makeErrorFuture(const QString& errorMsg) {
        QPromise<McpResult> promise;
        promise.start();
        McpResult errRes;
        errRes.isError = true;
        errRes.errorString = errorMsg;
        promise.addResult(errRes);
        promise.finish();
        return promise.future();
    }
}

McpToolRouter::McpToolRouter(McpServerManager* manager, QObject* parent)
    : QObject(parent), m_manager(manager) {}

QPair<QString, QString> McpToolRouter::parseToolName(const QString& nameSpacedToolName) const {
    if (!m_manager) {
        return {};
    }

    // 前缀解析收敛（终态架构 §4）：主路径走 McpNamespace 单一实现
    auto parsed = McpNamespace::parseNamespacedName(m_manager->serverNames(), nameSpacedToolName);
    if (!parsed.first.isEmpty()) {
        return parsed;
    }

    // 后备方案：以第一个 "_" 为界进行分割（legacy 兼容）
    int index = nameSpacedToolName.indexOf(QStringLiteral("_"));
    if (index > 0) {
        QString serverName = nameSpacedToolName.left(index);
        QString originalToolName = nameSpacedToolName.mid(index + 1);
        if (m_manager->client(serverName)) {
            return {serverName, originalToolName};
        }
    }

    return {};
}

QJsonArray McpToolRouter::exportAllToolsToLlmFormat(McpQtClient::LlmFormat format) const {
    QJsonArray result;
    if (!m_manager) return result;

    auto clientsMap = m_manager->clients();
    for (auto it = clientsMap.begin(); it != clientsMap.end(); ++it) {
        QString serverName = it.key();
        auto client = it.value();
        if (!client) continue;

        // 获取缓存在该客户端中的工具，而无需重复发送网络 RPC 获取列表
        std::vector<McpQtTool> tools = client->cachedTools();
        for (const auto& tool : tools) {
            McpQtTool modifiedTool = tool;
            modifiedTool.name = serverName + QStringLiteral("_") + tool.name;
            result.append(McpQtClient::exportToolToLlmFormat(modifiedTool, format));
        }
    }
    return result;
}

QJsonArray McpToolRouter::exportAllToolsAsMcpSchema() const {
    QJsonArray result;
    if (!m_manager) return result;

    auto clientsMap = m_manager->clients();
    for (auto it = clientsMap.begin(); it != clientsMap.end(); ++it) {
        QString serverName = it.key();
        auto client = it.value();
        if (!client) continue;

        // 直接复用底层 Client 已封装好的 Schema 数组，保留所有合规性校验（ensureValidSchema）
        QJsonArray clientTools = client->exportAllToolsAsMcpSchema();
        for (int i = 0; i < clientTools.size(); ++i) {
            QJsonObject obj = clientTools[i].toObject();
            obj[QStringLiteral("name")] = serverName + QStringLiteral("_") + obj[QStringLiteral("name")].toString();
            result.append(obj);
        }
    }
    return result;
}

QFuture<McpResult> McpToolRouter::callToolFuture(const QString& nameSpacedToolName, const QJsonObject& arguments) {
    auto parsed = parseToolName(nameSpacedToolName);
    if (parsed.first.isEmpty()) {
        return makeErrorFuture(QStringLiteral("Failed to resolve server for namespaced tool: ") + nameSpacedToolName);
    }

    auto client = m_manager->client(parsed.first);
    if (!client) {
        return makeErrorFuture(QStringLiteral("Client not found for server: ") + parsed.first);
    }

    return client->callToolFuture(parsed.second, arguments);
}

void McpToolRouter::callToolAsync(const QString& nameSpacedToolName, const QJsonObject& arguments,
                                  std::function<void(McpResult)> callback,
                                  McpQtClient::ProgressCallback onProgress) {
    auto invokeError = [this, callback](const QString& msg) {
        if (callback) {
            McpResult errRes;
            errRes.isError = true;
            errRes.errorString = msg;
            QMetaObject::invokeMethod(this, [=]() { callback(errRes); }, Qt::QueuedConnection);
        }
    };

    auto parsed = parseToolName(nameSpacedToolName);
    if (parsed.first.isEmpty()) {
        invokeError(QStringLiteral("Failed to resolve server for namespaced tool: ") + nameSpacedToolName);
        return;
    }

    auto client = m_manager->client(parsed.first);
    if (!client) {
        invokeError(QStringLiteral("Client not found for server: ") + parsed.first);
        return;
    }

    client->callToolAsync(parsed.second, arguments, callback, onProgress);
}

} // namespace mcp_qt
