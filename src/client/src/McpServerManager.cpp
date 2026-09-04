#include <mcp_qt_client/McpServerManager.h>
#include <mcp_qt_client/McpLogger.h>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDebug>
#include <QPointer>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QProcessEnvironment>
#include <QRegularExpression>

namespace mcp_qt {

McpServerManager::McpServerManager(QObject* parent)
    : QObject(parent) {}

McpServerManager::~McpServerManager() {
    closeAll(1000);
}

bool McpServerManager::loadServers(std::shared_ptr<IMcpConfigLoader> loader) {
    if (!loader) return false;
    return loadServers(loader->load());
}

QString McpServerManager::interpolateEnv(const QString& value) {
    return interpolateEnv(value, QProcessEnvironment::systemEnvironment());
}

QString McpServerManager::interpolateEnv(const QString& value, const QProcessEnvironment& env) {
    QString result = value;
    static QRegularExpression re(QStringLiteral("\\$\\{([A-Za-z0-9_]+)\\}"));

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

void McpServerManager::configureBuilder(McpQtClientBuilder& builder, const McpServerConfig& cfg) {
    if (!cfg.env.isEmpty()) builder.setEnvironment(cfg.env);
    if (!cfg.headers.isEmpty()) builder.setHttpHeaders(cfg.headers);
    builder.setClientInfo(cfg.serverName, QStringLiteral("1.0.0"));
    if (!cfg.nameSpace.isEmpty()) builder.setNamespace(cfg.nameSpace);
    if (!cfg.protocolVersion.isEmpty()) builder.setProtocolVersion(cfg.protocolVersion);
    if (cfg.type == QStringLiteral("stateless_http")) builder.setStatelessMode(true);

    // 配置校验：type 与 protocolVersion 矛盾时警告，避免用户配错协议组合
    if (cfg.type == QStringLiteral("stateless_http") && !cfg.protocolVersion.isEmpty()
        && cfg.protocolVersion != QStringLiteral("2026-07-28")) {
        McpLogger::warning(QStringLiteral("type=stateless_http 与 protocolVersion=%1 组合矛盾, 将按 2026-07-28 无状态处理")
                               .arg(cfg.protocolVersion),
                           QStringLiteral("McpServerManager"));
    }
    if ((cfg.type == QStringLiteral("sse") || cfg.type == QStringLiteral("http"))
        && cfg.protocolVersion == QStringLiteral("2026-07-28")) {
        McpLogger::warning(QStringLiteral("type=%1 与 protocolVersion=2026-07-28 组合: 无状态协议建议使用 type=stateless_http")
                               .arg(cfg.type),
                           QStringLiteral("McpServerManager"));
    }
}

bool McpServerManager::loadServers(const QList<McpServerConfig>& configs) {
    QSet<QString> loadedServers;

    for (const auto& cfg : configs) {
        if (cfg.disabled) continue;
        loadedServers.insert(cfg.serverName);
        startServer(cfg);
    }

    QStringList currentServers = serverNames();
    for (const QString& existing : currentServers) {
        if (!loadedServers.contains(existing)) {
            stopServer(existing);
        }
    }

    return true;
}

bool McpServerManager::startServer(const McpServerConfig& rawCfg) {
    if (rawCfg.disabled) return false;
    stopServer(rawCfg.serverName);

    McpServerConfig cfg = rawCfg;
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    cfg.command = interpolateEnv(cfg.command, env);
    for (int i = 0; i < cfg.args.size(); ++i) cfg.args[i] = interpolateEnv(cfg.args[i], env);
    cfg.url = interpolateEnv(cfg.url, env);
    cfg.nameSpace = interpolateEnv(cfg.nameSpace, env);
    for (auto it = cfg.env.begin(); it != cfg.env.end(); ++it) *it = interpolateEnv(*it, env);
    for (auto it = cfg.headers.begin(); it != cfg.headers.end(); ++it) *it = interpolateEnv(*it, env);

    if (!cfg.url.isEmpty()) {
        processHttpServerConfig(cfg);
    } else if (!cfg.command.isEmpty()) {
        McpQtClientBuilder builder;
        configureBuilder(builder, cfg);
        builder.setTransportStdio(cfg.command, cfg.args);

        auto clientPtr = builder.buildAndConnectAsync();
        if (clientPtr) registerClient(cfg.serverName, clientPtr);
    } else {
        McpLogger::warning(QStringLiteral("Server config for %1 does not contain url or command").arg(cfg.serverName),
                           QStringLiteral("McpServerManager"));
        return false;
    }
    return true;
}

void McpServerManager::probeProtocolVersion(const McpServerConfig& cfg,
                                            std::function<void(const McpServerConfig&)> done) {
    QPointer<McpServerManager> safeThis(this);
    auto* nam = new QNetworkAccessManager(this);
    // 探测是本地协议试探，必须绕过系统代理（代理对 localhost 可能挂起导致探测永不完成）
    nam->setProxy(QNetworkProxy::NoProxy);
    QNetworkRequest req(QUrl(cfg.url));
    for (auto it = cfg.headers.constBegin(); it != cfg.headers.constEnd(); ++it) {
        req.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    }
    req.setTransferTimeout(3000);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("MCP-Protocol-Version", "2026-07-28");
    req.setRawHeader("Mcp-Method", "server/discover");
    const QByteArray body = R"({"jsonrpc":"2.0","id":1,"method":"server/discover","params":{}})";
    QNetworkReply* reply = nam->post(req, body);
    connect(reply, &QNetworkReply::finished, this, [safeThis, cfg, reply, nam, done]() {
        McpServerConfig resolved = cfg;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray respBody = reply->readAll();
        // 2026-07-28 服务器：200 + result.supportedVersions；旧协议服务器：
        // 返回 -32022/-32601 或 4xx——均回退旧协议（2025-11-25）。
        bool is2026 = false;
        if (reply->error() == QNetworkReply::NoError && status == 200) {
            QJsonParseError perr;
            const QJsonDocument doc = QJsonDocument::fromJson(respBody, &perr);
            if (perr.error == QJsonParseError::NoError) {
                const QJsonObject root = doc.object();
                if (root.value(QStringLiteral("result")).isObject()
                    && root.value(QStringLiteral("result")).toObject()
                           .contains(QStringLiteral("supportedVersions"))) {
                    is2026 = true;
                }
            }
        }
        if (is2026) {
            resolved.protocolVersion = QStringLiteral("2026-07-28");
            resolved.type = QStringLiteral("stateless_http");
            McpLogger::info(QStringLiteral("协议自动探测: %1 支持 2026-07-28, 使用无状态模式").arg(cfg.serverName),
                            QStringLiteral("McpServerManager"));
        } else {
            McpLogger::info(QStringLiteral("协议自动探测: %1 为旧协议(2025-11-25)").arg(cfg.serverName),
                            QStringLiteral("McpServerManager"));
        }
        reply->deleteLater();
        nam->deleteLater();
        done(resolved);
    });
}

void McpServerManager::stopServer(const QString& serverName) {
    unregisterClient(serverName);
}

void McpServerManager::processHttpServerConfig(const McpServerConfig& cfg) {
    QPointer<McpServerManager> safeThis(this);
    auto buildAndRegister = [safeThis](const McpServerConfig& c) {
        if (!safeThis) return;
        McpQtClientBuilder builder;
        configureBuilder(builder, c);
        if (c.type == QStringLiteral("stateless_http")) {
            builder.setTransportStatelessHttp(c.url);
        } else {
            builder.setTransportHttp(c.url);
        }

        auto clientPtr = builder.buildAndConnectAsync();
        if (clientPtr) safeThis->registerClient(c.serverName, clientPtr);
    };

    // 协议自动探测：protocolVersion 未显式指定且非 stateless_http 时，
    // 先 POST server/discover(2026-07-28) 试探服务器协议——支持则用无状态模式，
    // 否则回退旧协议。避免用户配错 type 导致 -32022 Unsupported protocol version。
    if (cfg.protocolVersion.isEmpty() && cfg.type != QStringLiteral("stateless_http")) {
        probeProtocolVersion(cfg, [safeThis, buildAndRegister](const McpServerConfig& resolved) {
            if (!safeThis) return;
            buildAndRegister(resolved);
        });
        return;
    }

    if (cfg.type == QStringLiteral("stateless_http") || cfg.type == QStringLiteral("http") || cfg.type == QStringLiteral("sse")) {
        buildAndRegister(cfg);
    } else {
        // Auto negotiate via HEAD request
        m_serverStates.insert(cfg.serverName, McpServerState::Pending);
        QNetworkAccessManager* nam = new QNetworkAccessManager(this);
        QNetworkRequest req(QUrl(cfg.url));
        for (auto it = cfg.headers.constBegin(); it != cfg.headers.constEnd(); ++it) {
            req.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
        }
        QNetworkReply* reply = nam->head(req);
        connect(reply, &QNetworkReply::finished, this, [safeThis, cfg, reply, nam, buildAndRegister]() {
            if (!safeThis) {
                reply->deleteLater();
                nam->deleteLater();
                return;
            }
            if (reply->error() != QNetworkReply::NoError) {
                safeThis->updateServerState(cfg.serverName, McpServerState::Error);
                emit safeThis->clientErrorOccurred(cfg.serverName, mcp_qt::McpError{static_cast<int>(reply->error()), reply->errorString(), QJsonObject{}});
                if (safeThis->isAllToolsReady()) {
                    emit safeThis->allToolsReady();
                }
                reply->deleteLater();
                nam->deleteLater();
                return;
            }
            McpServerConfig newCfg = cfg;
            QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
            if (contentType.contains("text/event-stream", Qt::CaseInsensitive)) {
                newCfg.type = QStringLiteral("http");
            } else {
                newCfg.type = QStringLiteral("stateless_http");
            }
            reply->deleteLater();
            nam->deleteLater();
            buildAndRegister(newCfg);
        });
    }
}


void McpServerManager::registerClient(const QString& serverName, std::shared_ptr<McpQtClient> client) {
    if (!client) return;
    unregisterClient(serverName);

    m_clients.insert(serverName, client);
    m_serverStates.insert(serverName, McpServerState::Pending);
    setupClientSignals(serverName, client);
}

void McpServerManager::unregisterClient(const QString& serverName) {
    auto it = m_clients.find(serverName);
    if (it != m_clients.end()) {
        auto client = *it;
        if (client) {
            client->disconnect(this);
            client->close();
        }
        m_clients.erase(it);
        m_serverStates.remove(serverName);
    }
}

std::shared_ptr<McpQtClient> McpServerManager::client(const QString& serverName) const {
    return m_clients.value(serverName, nullptr);
}

QHash<QString, std::shared_ptr<McpQtClient>> McpServerManager::clients() const {
    return m_clients;
}

QStringList McpServerManager::serverNames() const {
    return m_clients.keys();
}

McpServerState McpServerManager::serverState(const QString& serverName) const {
    return m_serverStates.value(serverName, McpServerState::Error);
}

void McpServerManager::closeAll(int timeoutMs) {
    for (const auto& client : m_clients) {
        if (client) {
            client->close(timeoutMs);
        }
    }
    m_clients.clear();
}

void McpServerManager::updateServerState(const QString& serverName, McpServerState state) {
    if (m_serverStates.value(serverName) != state) {
        // Error 状态变化必须留痕（连接失败/预热失败路径），方便定位故障服务器（issue #8）
        if (state == McpServerState::Error) {
            McpLogger::warning(QStringLiteral("Server %1 entered error state").arg(serverName),
                               QStringLiteral("McpServerManager"));
        }
        m_serverStates[serverName] = state;
        emit clientStateChanged(serverName, state);
    }
}

void McpServerManager::warmupClientTools(const QString& serverName, const std::shared_ptr<McpQtClient>& client) {
    // 连接即预热：自动拉取全部工具并缓存
    if (client->cachedTools().empty()) {
        QPointer<McpServerManager> safeThis(this);
        client->fetchAllToolsAsync([safeThis, serverName](const std::vector<McpQtTool>& tools) {
            if (!safeThis) return;
            qInfo().noquote() << "[McpServerManager]" << serverName << "预热完成:" << tools.size() << "个工具";
            emit safeThis->clientToolsReady(serverName, static_cast<int>(tools.size()));
            safeThis->updateServerState(serverName, McpServerState::Ready);
            if (safeThis->isAllToolsReady()) {
                emit safeThis->allToolsReady();
            }
        });
    } else {
        emit clientToolsReady(serverName, static_cast<int>(client->cachedTools().size()));
        updateServerState(serverName, McpServerState::Ready);
        if (isAllToolsReady()) {
            emit allToolsReady();
        }
    }
}

void McpServerManager::setupClientSignals(const QString& serverName, const std::shared_ptr<McpQtClient>& client) {
    connect(client.get(), &McpQtClient::connected, this, [this, serverName, client]() {
        updateServerState(serverName, McpServerState::Connecting);
        emit clientConnected(serverName);
        warmupClientTools(serverName, client);
    });
    connect(client.get(), &McpQtClient::disconnected, this, [this, serverName]() {
        updateServerState(serverName, McpServerState::Pending);
        emit clientDisconnected(serverName);
    });
    connect(client.get(), &McpQtClient::errorOccurred, this, [this, serverName](const mcp_qt::McpError& error) {
        // 连接失败必须留痕：只 emit 信号会导致宿主侧"0 个工具"却不知失败原因（issue #8）
        // 统一走 McpLogger（全局级别控制 + 文件落盘）
        McpLogger::warning(QStringLiteral("MCP client error for %1: %2").arg(serverName, error.message),
                           QStringLiteral("McpServerManager"));
        updateServerState(serverName, McpServerState::Error);
        emit clientErrorOccurred(serverName, error);
        if (isAllToolsReady()) {
            emit allToolsReady();
        }
    });
    connect(client.get(), &McpQtClient::toolsChanged, this, [this, serverName](const std::vector<mcp_qt::McpQtTool>& newTools) {
        emit clientToolsChanged(serverName, newTools);
    });
    connect(client.get(), &McpQtClient::promptsChanged, this, [this, serverName]() {
        emit clientPromptsChanged(serverName);
    });
    connect(client.get(), &McpQtClient::inputRequired, this, [this, serverName](const QString& reqId, const QJsonObject& inputRequests, const QString& requestState, mcp_qt::MrtrReplyCallback cb) {
        emit clientInputRequired(serverName, reqId, inputRequests, requestState, cb);
    });
}

bool McpServerManager::isAllToolsReady() const {
    if (m_serverStates.isEmpty()) return false;
    for (auto it = m_serverStates.constBegin(); it != m_serverStates.constEnd(); ++it) {
        if (it.value() == McpServerState::Pending || it.value() == McpServerState::Connecting) {
            return false;
        }
    }
    return true;
}

void McpServerManager::startHeartbeat(int intervalMs) {
    if (!m_heartbeatTimer) {
        m_heartbeatTimer = new QTimer(this);
        connect(m_heartbeatTimer, &QTimer::timeout, this, [this]() {
            for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
                const auto& name = it.key();
                const auto& c = it.value();
                if (!c || !c->isConnected()) continue;
                // 异步 ping，失败时 client 的 auto-reconnect 机制会自动触发
                c->pingAsync([name](bool success, const QString& error) {
                    if (!success) {
                        McpLogger::warning(QStringLiteral("Heartbeat ping failed for %1: %2").arg(name, error),
                                           QStringLiteral("McpServerManager"));
                    }
                });
            }
        });
    }
    m_heartbeatTimer->start(intervalMs);
}

void McpServerManager::stopHeartbeat() {
    if (m_heartbeatTimer) {
        m_heartbeatTimer->stop();
    }
}

} // namespace mcp_qt
