#include "mcp_qt_client/McpConfigStore.h"

#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonArray>

namespace mcp_qt {

bool McpConfigStore::readWrite(bool allowMissing, const std::function<bool(QJsonObject&)>& mutate) {
    if (m_configPath.isEmpty()) return false;
    QJsonObject root;
    QFile file(m_configPath);
    if (file.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
    } else if (!allowMissing) {
        return false;
    }

    if (!mutate(root)) return false;

    QSaveFile saveFile(m_configPath);
    if (saveFile.open(QIODevice::WriteOnly)) {
        saveFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        return saveFile.commit();
    }
    return false;
}

bool McpConfigStore::setServerProperty(const QString& serverName, const QString& key, const QJsonValue& value) {
    return readWrite(false, [&](QJsonObject& root) {
        QJsonObject serversObj = root.contains(QStringLiteral("mcpServers")) ? root[QStringLiteral("mcpServers")].toObject() : root;
        if (!serversObj.contains(serverName)) return false;
        QJsonObject srvObj = serversObj[serverName].toObject();
        srvObj[key] = value;
        serversObj[serverName] = srvObj;
        if (root.contains(QStringLiteral("mcpServers"))) root[QStringLiteral("mcpServers")] = serversObj;
        else root = serversObj;
        return true;
    });
}

bool McpConfigStore::setServerObject(const QString& serverName, const QJsonObject& obj) {
    return readWrite(true, [&](QJsonObject& root) {
        QJsonObject serversObj = root.contains(QStringLiteral("mcpServers")) ? root[QStringLiteral("mcpServers")].toObject() : root;
        serversObj[serverName] = obj;
        if (root.contains(QStringLiteral("mcpServers"))) root[QStringLiteral("mcpServers")] = serversObj;
        else root = serversObj;
        return true;
    });
}

bool McpConfigStore::removeServer(const QString& serverName) {
    return readWrite(false, [&](QJsonObject& root) {
        QJsonObject serversObj = root.contains(QStringLiteral("mcpServers")) ? root[QStringLiteral("mcpServers")].toObject() : root;
        serversObj.remove(serverName);
        if (root.contains(QStringLiteral("mcpServers"))) root[QStringLiteral("mcpServers")] = serversObj;
        else root = serversObj;
        return true;
    });
}

QJsonObject McpConfigStore::serializeServerConfig(const McpServerConfig& cfg) {
    QJsonObject obj;
    obj[QStringLiteral("disabled")] = cfg.disabled;
    if (!cfg.command.isEmpty()) obj[QStringLiteral("command")] = cfg.command;
    if (!cfg.args.isEmpty()) {
        QJsonArray argsArr;
        for (const auto& arg : cfg.args) argsArr.append(arg);
        obj[QStringLiteral("args")] = argsArr;
    }
    if (!cfg.url.isEmpty()) obj[QStringLiteral("url")] = cfg.url;
    if (!cfg.type.isEmpty()) obj[QStringLiteral("type")] = cfg.type;
    if (!cfg.nameSpace.isEmpty()) obj[QStringLiteral("namespace")] = cfg.nameSpace;

    if (!cfg.env.isEmpty()) {
        QJsonObject envs;
        for (auto it = cfg.env.constBegin(); it != cfg.env.constEnd(); ++it) {
            envs[it.key()] = it.value();
        }
        obj[QStringLiteral("env")] = envs;
    }

    if (!cfg.headers.isEmpty()) {
        QJsonObject hdrs;
        for (auto it = cfg.headers.constBegin(); it != cfg.headers.constEnd(); ++it) {
            hdrs[it.key()] = it.value();
        }
        obj[QStringLiteral("headers")] = hdrs;
    }
    return obj;
}

} // namespace mcp_qt
