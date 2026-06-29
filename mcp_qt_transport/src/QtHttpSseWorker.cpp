#include "QtHttpSseWorker.h"

#include <chrono>
#include <thread>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace mcp_qt {

// 从相对路径解析为完整 URL (等同于 HttpSseTransport.cpp 的 resolveUrl 逻辑)
static QString resolveUrl(const QString& base, const QString& relative) {
    if (relative.isEmpty()) return base;
    if (relative.contains("://")) return relative;

    int schemeEnd = base.indexOf("://");
    int hostStart = (schemeEnd != -1) ? schemeEnd + 3 : 0;

    if (relative[0] == '/') {
        int pathStart = base.indexOf('/', hostStart);
        if (pathStart != -1) {
            return base.left(pathStart) + relative;
        }
        return base + relative;
    }

    int pathStart = base.indexOf('/', hostStart);
    if (pathStart == -1) {
        return base + "/" + relative;
    }

    int lastSlash = base.lastIndexOf('/');
    if (lastSlash != -1 && lastSlash >= hostStart) {
        QString lastPart = base.mid(lastSlash + 1);
        if (!lastPart.isEmpty() && !lastPart.contains('.')) {
            return base + "/" + relative;
        }
        return base.left(lastSlash + 1) + relative;
    }
    return base + "/" + relative;
}

QtHttpSseWorker::QtHttpSseWorker(QString baseUrl, QObject* parent)
    : QObject(parent), m_baseUrl(std::move(baseUrl)), m_postUrl(m_baseUrl) {
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    m_reconnectTimer->setTimerType(Qt::PreciseTimer);

    connect(m_reconnectTimer, &QTimer::timeout, this, &QtHttpSseWorker::openSse);
    m_parser.setRetryCallback([this](int retryMs) {
        m_retryMs = retryMs;
        // 如果 timer 已在跑，用新值重启（retry 字段可能在 timer 启动后才到达）
        if (m_reconnectTimer->isActive()) {
            m_reconnectTimer->stop();
            m_reconnectTimer->start(m_retryMs);
        }
    });
    m_parser.setEventCallback([this](const QtSseEvent& event) { handleSseEvent(event); });
}

void QtHttpSseWorker::setProtocolVersion(const QString& version) { m_protocolVersion = version; }
void QtHttpSseWorker::setTokenProvider(std::function<std::string()> provider) { m_tokenProvider = std::move(provider); }
void QtHttpSseWorker::setAuthRetryHandler(std::function<bool(const std::string&)> handler) { m_authRetryHandler = std::move(handler); }

void QtHttpSseWorker::startStream() {
    m_stopping = false;
    if (!m_network) {
        m_network = new QNetworkAccessManager(this);
    }
    openSse();
}

void QtHttpSseWorker::stopStream() {
    m_stopping = true;
    m_reconnectTimer->stop();
    if (m_sseReply) {
        disconnect(m_sseReply, nullptr, this, nullptr);
        m_sseReply->abort();
        m_sseReply->deleteLater();
        m_sseReply = nullptr;
    }
    emit transportClosed();
}

QString QtHttpSseWorker::currentBearerToken() const {
    if (!m_tokenProvider) {
        return {};
    }
    return QString::fromStdString(m_tokenProvider());
}

void QtHttpSseWorker::applyCommonHeaders(QNetworkRequest& request) const {
    request.setRawHeader("Accept", "text/event-stream");
    request.setRawHeader("Cache-Control", "no-cache");
    request.setRawHeader("MCP-Protocol-Version", m_protocolVersion.toUtf8());
    if (!m_sessionId.isEmpty()) {
        request.setRawHeader("MCP-Session-Id", m_sessionId.toUtf8());
    }
    if (!m_lastEventId.isEmpty()) {
        request.setRawHeader("Last-Event-ID", m_lastEventId.toUtf8());
    }
    const QString token = currentBearerToken();
    if (!token.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    }
}

void QtHttpSseWorker::openSse() {
    if (m_stopping) {
        return;
    }
    QNetworkRequest request;
    request.setUrl(QUrl(m_baseUrl));
    applyCommonHeaders(request);
    m_sseReply = m_network->get(request);

    // metaDataChanged：响应头到达时即可判断连接是否成功（不等 SSE 数据）
    connect(m_sseReply, &QNetworkReply::metaDataChanged, this, [this]() {
        if (!m_sseReply) return;
        int status = m_sseReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 200) {
            m_sseConnected = true;
        }
    });
    connect(m_sseReply, &QIODevice::readyRead, this, &QtHttpSseWorker::handleSseReadyRead);
    connect(m_sseReply, &QNetworkReply::finished, this, &QtHttpSseWorker::handleSseFinished);
    connect(m_sseReply, &QNetworkReply::errorOccurred, this, &QtHttpSseWorker::handleSseError);
}

void QtHttpSseWorker::handleSseReadyRead() {
    if (!m_sseReply) {
        return;
    }
    int status = m_sseReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 200) {
        m_sseConnected = true;
    }
    const QByteArray chunk = m_sseReply->readAll();
    m_parser.pushChunk(chunk.toStdString());
}

void QtHttpSseWorker::handleSseFinished() {
    if (!m_sseReply) {
        return;
    }
    const auto sessionHeader = m_sseReply->rawHeader("MCP-Session-Id");
    if (!sessionHeader.isEmpty()) {
        m_sessionId = QString::fromUtf8(sessionHeader);
    }

    int statusCode = m_sseReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString wwwAuthHeader = m_sseReply->rawHeader("WWW-Authenticate");

    m_sseReply->deleteLater();
    m_sseReply = nullptr;

    if (m_stopping) {
        return;
    }

    // 401/403：有认证处理器的重试
    if (statusCode == 401 || statusCode == 403) {
        if (m_authRetryHandler && m_authRetryHandler(wwwAuthHeader.toStdString())) {
            openSse();
            return;
        }
        scheduleReconnect();
        return;
    }

    // 非 200 且有 sessionId：快速重连（100ms，不等默认 2000ms）
    if (statusCode != 200 && !m_sessionId.isEmpty()) {
        QTimer::singleShot(100, this, &QtHttpSseWorker::openSse);
        return;
    }

    scheduleReconnect();
}

void QtHttpSseWorker::handleSseError(QNetworkReply::NetworkError) {
    if (!m_sseReply) {
        return;
    }
    emit transportError(m_sseReply->errorString());
}

void QtHttpSseWorker::handleSseEvent(const QtSseEvent& event) {
    if (!event.lastEventId.empty()) {
        m_lastEventId = QString::fromStdString(event.lastEventId);
    }
    if (event.eventName == "endpoint") {
        m_postUrl = resolveUrl(m_baseUrl, QString::fromStdString(event.data));
        return;
    }
    emit messageReceived(QString::fromStdString(event.data));
}

void QtHttpSseWorker::scheduleReconnect() {
    m_reconnectTimer->stop();
    m_reconnectTimer->start(m_retryMs);
}

bool QtHttpSseWorker::postMessage(const QString& payload, int retryCount) {
    if (m_stopping) {
        return false;
    }
    if (!m_network) {
        m_network = new QNetworkAccessManager(this);
    }
    QNetworkRequest request;
    request.setUrl(QUrl(m_postUrl));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json, text/event-stream");
    request.setRawHeader("MCP-Protocol-Version", m_protocolVersion.toUtf8());
    if (!m_sessionId.isEmpty()) {
        request.setRawHeader("MCP-Session-Id", m_sessionId.toUtf8());
    }
    const QString token = currentBearerToken();
    if (!token.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    }

    QNetworkReply* reply = m_network->post(request, payload.toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply, payload, retryCount]() {
        const auto sessionHeader = reply->rawHeader("MCP-Session-Id");
        bool justGotSession = !sessionHeader.isEmpty() && m_sessionId.isEmpty();
        if (!sessionHeader.isEmpty()) {
            m_sessionId = QString::fromUtf8(sessionHeader);
        }

        // 首次获得 sessionId：同步等待 SSE 连接建立（与 C++ 版 doPost 行为一致，最多等 5s）
        if (justGotSession) {
            m_reconnectTimer->stop();
            for (int i = 0; i < 50 && !m_sseConnected && !m_stopping; ++i) {
                openSse();
                QEventLoop loop;
                QMetaObject::Connection c1 = connect(m_sseReply, &QNetworkReply::metaDataChanged, &loop, &QEventLoop::quit);
                QMetaObject::Connection c2 = connect(m_sseReply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
                loop.exec();
                disconnect(c1); disconnect(c2);
                if (m_sseConnected) break;
                QTimer::singleShot(100, &loop, SLOT(quit())); loop.exec(); // wait 100ms
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
            emit transportError(QString("HTTP %1: Post message authentication failed").arg(statusCode));
            reply->deleteLater();
            return;
        }

        const QByteArray body = reply->readAll();
        if (!body.isEmpty()) {
            QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
            if (contentType.contains("text/event-stream", Qt::CaseInsensitive)) {
                QtSseParser postParser;
                postParser.setRetryCallback([this](int retryMs) {
                    m_retryMs = retryMs;
                    if (m_reconnectTimer->isActive()) {
                        m_reconnectTimer->stop();
                        m_reconnectTimer->start(m_retryMs);
                    }
                });
                postParser.setEventCallback([this](const QtSseEvent& event) {
                    emit messageReceived(QString::fromStdString(event.data));
                });
                std::string sseData = QString::fromUtf8(body).toStdString();
                if (sseData.rfind("\n\n") == std::string::npos) {
                    sseData += "\n\n";
                }
                postParser.pushChunk(sseData);
            } else {
                emit messageReceived(QString::fromUtf8(body));
            }
        }
        reply->deleteLater();
    });
    connect(reply, &QNetworkReply::errorOccurred, this, [this, reply](QNetworkReply::NetworkError) {
        emit transportError(reply->errorString());
    });

    return true;
}

} // namespace mcp_qt
