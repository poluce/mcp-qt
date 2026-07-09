#include <mcp_qt_client/McpJsonConfigLoader.h>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDebug>
#include <QRegularExpression>
#include <QProcessEnvironment>

namespace mcp_qt {

McpJsonConfigLoader::McpJsonConfigLoader(const QJsonObject& configObj)
    : m_configObj(configObj) {}

McpJsonConfigLoader McpJsonConfigLoader::fromFile(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Failed to open config file:" << filePath;
        return McpJsonConfigLoader(QJsonObject{});
    }
    QByteArray data = file.readAll();
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "Failed to parse JSON config or not an object:" << parseError.errorString();
        return McpJsonConfigLoader(QJsonObject{});
    }
    return McpJsonConfigLoader(doc.object());
}

QString McpJsonConfigLoader::interpolateEnv(const QString& value) const {
    QString result = value;
    static QRegularExpression re(QStringLiteral("\\$\\{([A-Za-z0-9_]+)\\}"));
    auto env = QProcessEnvironment::systemEnvironment();
    
    QRegularExpressionMatchIterator i = re.globalMatch(value);
    int offset = 0;
    while (i.hasNext()) {
        QRegularExpressionMatch match = i.next();
        QString varName = match.captured(1);
        if (env.contains(varName)) {
            QString replacement = env.value(varName);
            result.replace(match.capturedStart(0) + offset, match.capturedLength(0), replacement);
            offset += replacement.length() - match.capturedLength(0);
        }
    }
    return result;
}

QList<McpServerConfig> McpJsonConfigLoader::load() {
    QList<McpServerConfig> configs;
    QJsonObject serversObj;
    if (m_configObj.contains(QStringLiteral("mcpServers")) && m_configObj.value(QStringLiteral("mcpServers")).isObject()) {
        serversObj = m_configObj.value(QStringLiteral("mcpServers")).toObject();
    } else {
        serversObj = m_configObj;
    }

    for (auto it = serversObj.constBegin(); it != serversObj.constEnd(); ++it) {
        if (!it.value().isObject()) continue;
        QJsonObject serverCfg = it.value().toObject();
        
        McpServerConfig cfg;
        cfg.serverName = it.key();
        cfg.disabled = serverCfg.value(QStringLiteral("disabled")).toBool(false);

        if (serverCfg.contains(QStringLiteral("command"))) {
            cfg.command = interpolateEnv(serverCfg.value(QStringLiteral("command")).toString());
        }
        if (serverCfg.contains(QStringLiteral("args")) && serverCfg.value(QStringLiteral("args")).isArray()) {
            QJsonArray argsArr = serverCfg.value(QStringLiteral("args")).toArray();
            for (const auto& argVal : argsArr) {
                cfg.args.append(interpolateEnv(argVal.toString()));
            }
        }
        if (serverCfg.contains(QStringLiteral("url"))) {
            cfg.url = interpolateEnv(serverCfg.value(QStringLiteral("url")).toString());
        }
        if (serverCfg.contains(QStringLiteral("type"))) {
            cfg.type = serverCfg.value(QStringLiteral("type")).toString();
        }

        if (serverCfg.contains(QStringLiteral("namespace"))) {
            cfg.nameSpace = interpolateEnv(serverCfg.value(QStringLiteral("namespace")).toString());
        }

        if (serverCfg.contains(QStringLiteral("env")) && serverCfg.value(QStringLiteral("env")).isObject()) {
            QJsonObject envs = serverCfg.value(QStringLiteral("env")).toObject();
            for (auto envIt = envs.constBegin(); envIt != envs.constEnd(); ++envIt) {
                QString envVal;
                if (envIt.value().isBool()) {
                    envVal = envIt.value().toBool() ? QStringLiteral("true") : QStringLiteral("false");
                } else {
                    envVal = envIt.value().toVariant().toString();
                }
                cfg.env.insert(envIt.key(), interpolateEnv(envVal));
            }
        }
        
        if (serverCfg.contains(QStringLiteral("headers")) && serverCfg.value(QStringLiteral("headers")).isObject()) {
            QJsonObject hdrs = serverCfg.value(QStringLiteral("headers")).toObject();
            for (auto hdrIt = hdrs.constBegin(); hdrIt != hdrs.constEnd(); ++hdrIt) {
                if (hdrIt.value().isString()) {
                    cfg.headers.insert(hdrIt.key(), interpolateEnv(hdrIt.value().toString()));
                }
            }
        }
        configs.append(cfg);
    }
    return configs;
}

} // namespace mcp_qt
