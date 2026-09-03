#include "QtHttpSseWorker.h"

#include <cstdio>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace mcp_qt {

static QString resolveUrl(const QString& base, const QString& relative) {
    if (relative.isEmpty()) return base;
    if (relative.contains(QStringLiteral("://"))) return relative;

    QUrl baseUrl(base);
    // 启发式：如果 URL 不以 '/' 结尾，并且最后一段不包含 '.'，则将其视为目录并追加 '/'
    if (!base.endsWith(QLatin1Char('/'))) {
        QString lastPart = baseUrl.path().section(QLatin1Char('/'), -1);
        if (!lastPart.isEmpty() && !lastPart.contains(QLatin1Char('.'))) {
            baseUrl.setPath(baseUrl.path() + QLatin1Char('/'));
        }
    }
    return baseUrl.resolved(QUrl(relative)).toString();
}

QtHttpSseWorker::QtHttpSseWorker(QString baseUrl, QObject* parent)
    : QObject(parent), m_baseUrl(std::move(baseUrl)), m_postUrl(m_baseUrl) {
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    m_reconnectTimer->setTimerType(Qt::PreciseTimer);

    connect(m_reconnectTimer, &QTimer::timeout, this, [this]() {
        openSse();
    });

    m_healthCheckTimer = new QTimer(this);
    m_healthCheckTimer->setInterval(50);
    connect(m_healthCheckTimer, &QTimer::timeout, this, [this]() {
        if (!m_sseConnected || m_stopping) return;
        // 健康检查只负责探测"真死连接"（服务器长时间无数据且无关闭信号），
        // 用保守阈值 5000ms。重连时序由关闭事件驱动（GET finished /
        // POST SSE 响应流 finished → scheduleReconnect 按 retry 字段延迟），
        // 健康检查不得抢先启动重连定时器——否则会在服务器正常关闭前
        // （如 tools/call 处理期间 GET 流静默）提前重连，违反 SEP-1699 时序。
        const qint64 stallMs = 5000;
        if (m_lastDataTime.isValid() && m_lastDataTime.elapsed() > stallMs) {
                        // 连接停滞检测：超过阈值未恢复数据，则主动断开触发重连。
            // 先直接启动重连定时器（按 retry 字段延迟），再 abort 清理死连接——
            // 避免等 abort→finished 信号往返额外吃掉时间（SSE retry 时序敏感，SEP-1699）。
            // 冷却期（5s）限制重连风暴，快阈值只加快首次探测。
            if (!m_lastHealthCheckTime.isValid() || m_lastHealthCheckTime.elapsed() > 5000) {
                m_lastHealthCheckTime.start();
                scheduleReconnect();
                if (m_sseReply) {
                    m_sseReply->abort();
                }
            }
        }
    });

    m_parser.setRetryCallback([this](int retryMs) {
        m_retryMs = retryMs;
        m_retryFieldReceived = true;
    });

    // IMPORTANT: Capture Last-Event-ID for reconnection!
    m_parser.setIdCallback([this](const std::string& id) {
        m_lastEventId = QString::fromStdString(id);
    });

    m_parser.setEventCallback([this](const QtSseEvent& event) {
        handleSseEvent(event);
    });
}

void QtHttpSseWorker::setProtocolVersion(const QString& version) { m_protocolVersion = version; }
void QtHttpSseWorker::setTokenProvider(std::function<std::string()> provider) { m_tokenProvider = std::move(provider); }
void QtHttpSseWorker::setAuthRetryHandler(std::function<bool(const std::string&)> handler) { m_authRetryHandler = std::move(handler); }
void QtHttpSseWorker::setRequestConfig(const QtHttpRequestConfig& config) {
    m_requestConfig = config;
    if (m_network && m_requestConfig.proxy) {
        m_network->setProxy(*m_requestConfig.proxy);
    }
}

void QtHttpSseWorker::startStream() {
    m_stopping = false;
    m_postUrl = m_baseUrl;
    m_endpointResolved = true;
    if (!m_network) {
        m_network = new QNetworkAccessManager(this);
        if (m_requestConfig.proxy) {
            m_network->setProxy(*m_requestConfig.proxy);
        }
    }
    if (!m_postNetwork) {
        // POST 独立 QNAM：避免与 GET SSE 长连接共享连接池导致 POST 排队阻塞
        m_postNetwork = new QNetworkAccessManager(this);
        if (m_requestConfig.proxy) {
            m_postNetwork->setProxy(*m_requestConfig.proxy);
        }
    }
    openSse();
}

void QtHttpSseWorker::stopStream() {
    m_stopping = true;
    m_reconnectTimer->stop();
    m_healthCheckTimer->stop();
    if (m_sseReply) {
        m_sseReply->abort();
        m_sseReply->deleteLater();
        m_sseReply = nullptr;
    }
    emit transportClosed();
}

void QtHttpSseWorker::setupRequestHeaders(QNetworkRequest& request) const {
    for (auto it = m_requestConfig.defaultHeaders.constBegin(); it != m_requestConfig.defaultHeaders.constEnd(); ++it) {
        request.setRawHeader(it.key(), it.value());
    }
    request.setRawHeader("MCP-Protocol-Version", m_protocolVersion.toUtf8());
    if (!m_sessionId.isEmpty()) {
        request.setRawHeader("MCP-Session-Id", m_sessionId.toUtf8());
    }
    const QString token = currentBearerToken();
    if (!token.isEmpty() && m_requestConfig.allowAuthorizationOverride) {
        request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    }
}

QString QtHttpSseWorker::currentBearerToken() const {
    return m_tokenProvider ? QString::fromStdString(m_tokenProvider()) : QString{};
}

void QtHttpSseWorker::openSse() {
    if (m_stopping) {
        return;
    }
    if (m_sseReply) {
        // 主动替换旧流：标记意图，旧流 finished 不再触发重连（防 500ms 重连循环）
        m_intentionalAbort = true;
        m_sseReply->abort();
        m_sseReply->deleteLater();
        m_sseReply = nullptr;
    }

    QNetworkRequest request;
    request.setUrl(QUrl(m_baseUrl));
    setupRequestHeaders(request);
    request.setRawHeader("Accept", "text/event-stream");
    request.setRawHeader("Cache-Control", "no-cache");
    if (!m_lastEventId.isEmpty()) {
        request.setRawHeader("Last-Event-ID", m_lastEventId.toUtf8());
    }

    m_sseReply = m_network->get(request);
    
    connect(m_sseReply, &QNetworkReply::metaDataChanged, this, [this]() {
        if (m_stopping || !m_sseReply) return;
        int statusCode = m_sseReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString wwwAuthHeader = m_sseReply->rawHeader("WWW-Authenticate");
        
        if (statusCode == 401 || statusCode == 403) {
            m_authRetryCount++;
            if (m_authRetryCount <= kMaxAuthRetries && m_authRetryHandler && m_authRetryHandler(wwwAuthHeader.toStdString())) {
                m_sseReply->abort();
                openSse();
                return;
            }
            
            // If auth retries exhausted or no handler, fail the connection
            m_stopping = true;
            QString errMsg = QString("HTTP %1: Authentication failed").arg(statusCode);
            emit transportError(errMsg);
            if (m_sseReply) m_sseReply->abort();
            return;
        }
        
        if (statusCode >= 400) {
            m_stopping = true;
            QString errMsg = QString("HTTP %1: Connection failed").arg(statusCode);
            emit transportError(errMsg);
            if (m_sseReply) m_sseReply->abort();
            return;
        }

        m_authRetryCount = 0;
        const auto sessionHeader = m_sseReply->rawHeader("MCP-Session-Id");
        if (!sessionHeader.isEmpty()) {
            m_sessionId = QString::fromUtf8(sessionHeader);
        }

        m_sseConnected = true;
        m_lastDataTime.start();
        m_healthCheckTimer->start();
    });

    connect(m_sseReply, &QNetworkReply::readyRead, this, &QtHttpSseWorker::handleSseReadyRead);
    connect(m_sseReply, &QNetworkReply::finished, this, &QtHttpSseWorker::handleSseFinished);
    connect(m_sseReply, &QNetworkReply::errorOccurred, this, &QtHttpSseWorker::handleSseError);
}

void QtHttpSseWorker::handleSseReadyRead() {
    if (m_stopping || !m_sseReply) return;

    const QByteArray data = m_sseReply->readAll();
    if (data.isEmpty()) return;

    m_lastDataTime.start();
    m_parser.pushChunk(data.toStdString());
}

void QtHttpSseWorker::handleSseFinished() {
    if (m_stopping) return;
    if (m_intentionalAbort) {
        // openSse 主动 abort（替换旧流）：不触发重连、不改连接状态
        m_intentionalAbort = false;
        return;
    }
    if (!m_sseConnected) return;
    m_sseConnected = false;
    m_healthCheckTimer->stop();
    scheduleReconnect();
}

void QtHttpSseWorker::handleSseError(QNetworkReply::NetworkError code) {
    if (m_stopping || !m_sseReply) return;
    
    if (code == QNetworkReply::RemoteHostClosedError) {
        // Handled naturally by finished
        return;
    }
    
    m_healthCheckTimer->stop();
    if (code != QNetworkReply::OperationCanceledError) {
        emit transportError(m_sseReply->errorString());
    }
}

void QtHttpSseWorker::handleSseEvent(const QtSseEvent& event) {
    if (event.eventName == "endpoint") {
        m_postUrl = resolveUrl(m_baseUrl, QString::fromStdString(event.data));
        m_endpointResolved = true;
        flushPendingMessages();
        return;
    }
    emit messageReceived(QString::fromStdString(event.data));
}

void QtHttpSseWorker::scheduleReconnect() {
    if (m_stopping || m_reconnectTimer->isActive()) return;
    m_reconnectTimer->start(m_retryMs);
}

bool QtHttpSseWorker::postMessage(const QString& payload, int retryCount) {
    if (m_stopping) {
        return false;
    }
    if (!m_endpointResolved) {
        m_pendingMessages.append(payload);
        return true;
    }
    if (!m_network) {
        m_network = new QNetworkAccessManager(this);
        if (m_requestConfig.proxy) {
            m_network->setProxy(*m_requestConfig.proxy);
        }
    }
    QNetworkRequest request;
    request.setUrl(QUrl(m_postUrl));
    setupRequestHeaders(request);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json, text/event-stream");

    QNetworkReply* reply = m_postNetwork->post(request, payload.toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply, payload, retryCount]() {
        const auto sessionHeader = reply->rawHeader("MCP-Session-Id");
        bool justGotSession = !sessionHeader.isEmpty() && m_sessionId.isEmpty();
        if (!sessionHeader.isEmpty()) {
            m_sessionId = QString::fromUtf8(sessionHeader);
        }

        if (justGotSession) {
            m_reconnectTimer->stop();
            for (int i = 0; i < 50 && !m_sseConnected && !m_stopping; ++i) {
                if (!m_sseReply) openSse();
                QEventLoop loop;
                QTimer::singleShot(100, &loop, &QEventLoop::quit);
                loop.exec();
            }
        }

        int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString wwwAuthHeader = reply->rawHeader("WWW-Authenticate");

        if (statusCode == 401 || statusCode == 403) {
            if (m_authRetryHandler && retryCount < 3 && m_authRetryHandler(wwwAuthHeader.toStdString())) {
                reply->deleteLater();
                postMessage(payload, retryCount + 1);
                return;
            }
            QString errMsg = QString("HTTP %1: Post message authentication failed").arg(statusCode);
            QByteArray authErrBody = reply->readAll();
            if (!authErrBody.isEmpty()) {
                errMsg += QStringLiteral("\nResponse Body: ") + QString::fromUtf8(authErrBody);
            }
            emit transportError(errMsg);
            reply->deleteLater();
            return;
        }

        const QByteArray body = reply->readAll();
        if (!body.isEmpty()) {
            QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
            if (contentType.contains("text/event-stream", Qt::CaseInsensitive)) {
                parseSseInlineBody(body);
                scheduleReconnect();
            } else {
                emit messageReceived(QString::fromUtf8(body));
            }
        }
        reply->deleteLater();
    });
    connect(reply, &QNetworkReply::errorOccurred, this, [this, reply](QNetworkReply::NetworkError) {
        QString errMsg = reply->errorString();
        QByteArray errorBody = reply->readAll();
        if (!errorBody.isEmpty()) {
            errMsg += QStringLiteral("\nResponse Body: ") + QString::fromUtf8(errorBody);
        }
        emit transportError(errMsg);
    });

    return true;
}

void QtHttpSseWorker::parseSseInlineBody(const QByteArray& body) {
    QtSseParser postParser;
    postParser.setRetryCallback([this](int retryMs) {
        m_retryMs = retryMs;
        m_retryFieldReceived = true;
    });
    postParser.setIdCallback([this](const std::string& id) {
        m_lastEventId = QString::fromStdString(id);
    });
    postParser.setEventCallback([this](const QtSseEvent& event) {
        if (!event.lastEventId.empty()) {
            m_lastEventId = QString::fromStdString(event.lastEventId);
        }
        emit messageReceived(QString::fromStdString(event.data));
    });

    std::string sseData = QString::fromUtf8(body).toStdString();
    if (sseData.rfind("\n\n") == std::string::npos) {
        sseData += "\n\n";
    }
    postParser.pushChunk(sseData);
}

void QtHttpSseWorker::flushPendingMessages() {
    for (const auto& msg : m_pendingMessages) {
        postMessage(msg);
    }
    m_pendingMessages.clear();
}

} // namespace mcp_qt
