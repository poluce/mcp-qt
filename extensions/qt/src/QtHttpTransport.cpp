#include "mcp_qt/QtHttpTransport.h"
#include <QNetworkRequest>
#include <QDebug>

namespace mcp {

QtHttpTransport::QtHttpTransport(const QUrl& sseUrl, QObject* parent)
    : QObject(parent)
    , m_sseUrl(sseUrl)
    , m_nam(new QNetworkAccessManager(this))
    , m_sseReply(nullptr)
    , m_reconnectTimer(new QTimer(this))
    , m_isClosedActively(false)
{
    m_reconnectTimer->setSingleShot(true);
    m_reconnectTimer->setInterval(2000); // 2 seconds
    connect(m_reconnectTimer, &QTimer::timeout, this, &QtHttpTransport::performReconnect);

    // Default fallback endpoint if not dynamically configured by SSE endpoint events
    m_postUrl = sseUrl.resolved(QUrl("message"));
    connect(m_nam, &QNetworkAccessManager::finished, this, &QtHttpTransport::handlePostFinished);
}

QtHttpTransport::~QtHttpTransport() {
    close();
}

bool QtHttpTransport::send(const std::string& message) {
    if (!m_postUrl.isValid()) {
        return false;
    }

    // Extract MCP-Session-Id from SSE reply headers (arrives on first readyRead)
    if (m_sessionId.isEmpty() && m_sseReply) {
        m_sessionId = QString::fromUtf8(m_sseReply->rawHeader("MCP-Session-Id"));
    }

    QNetworkRequest request(m_postUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("MCP-Protocol-Version", "2025-11-25");
    request.setRawHeader("Accept", "application/json, text/event-stream");
    if (!m_sessionId.isEmpty()) {
        request.setRawHeader("MCP-Session-Id", m_sessionId.toUtf8());
    }

    QByteArray postData = QByteArray::fromStdString(message);
    m_nam->post(request, postData);
    return true;
}

void QtHttpTransport::setOnMessage(std::function<void(const std::string&)> callback) {
    m_onMessage = std::move(callback);
}

void QtHttpTransport::setOnClose(std::function<void()> callback) {
    m_onClose = std::move(callback);
}

void QtHttpTransport::setOnError(std::function<void(const std::string&)> callback) {
    m_onError = std::move(callback);
}

bool QtHttpTransport::start() {
    if (m_sseReply) {
        return false;
    }
    
    m_isClosedActively = false;
    m_reconnectTimer->stop();
    
    QNetworkRequest request(m_sseUrl);
    request.setRawHeader("Accept", "text/event-stream");
    request.setRawHeader("MCP-Protocol-Version", "2025-11-25");
    if (!m_lastEventId.isEmpty()) {
        request.setRawHeader("Last-Event-ID", m_lastEventId.toUtf8());
    }
    
    m_sseReply = m_nam->get(request);
    connect(m_sseReply, &QNetworkReply::readyRead, this, &QtHttpTransport::handleSseReadyRead);
    connect(m_sseReply, &QNetworkReply::finished, this, &QtHttpTransport::handleSseFinished);
    
    return true;
}

void QtHttpTransport::close() {
    m_isClosedActively = true;
    m_reconnectTimer->stop();
    
    // Disconnect finished signal to prevent handlePostFinished callbacks during destruction
    disconnect(m_nam, &QNetworkAccessManager::finished, this, &QtHttpTransport::handlePostFinished);
    
    if (m_sseReply) {
        m_sseReply->abort();
        m_sseReply->deleteLater();
        m_sseReply = nullptr;
    }
}

void QtHttpTransport::handleSseReadyRead() {
    if (!m_sseReply) return;
    
    m_sseBuffer.append(m_sseReply->readAll());
    
    // Parse SSE block separated by double newlines \n\n or \r\n\r\n
    int blockEnd;
    while ((blockEnd = m_sseBuffer.indexOf("\n\n")) != -1) {
        QByteArray block = m_sseBuffer.left(blockEnd);
        m_sseBuffer.remove(0, blockEnd + 2);
        
        QString eventType = "message";
        QString dataContent;
        
        // Normalize line endings and split
        block.replace("\r", "");
        QList<QByteArray> lines = block.split('\n');
        for (const QByteArray& line : lines) {
            if (line.startsWith("event:")) {
                eventType = QString::fromUtf8(line.mid(6).trimmed());
            } else if (line.startsWith("data:")) {
                dataContent = QString::fromUtf8(line.mid(5).trimmed());
            } else if (line.startsWith("id:")) {
                m_lastEventId = QString::fromUtf8(line.mid(3).trimmed());
            }
        }
        
        if (!dataContent.isEmpty()) {
            if (eventType == "endpoint") {
                // MCP specification: server sends SSE "endpoint" event specifying target message URL
                m_postUrl = m_sseUrl.resolved(QUrl(dataContent));
            } else if (m_onMessage) {
                m_onMessage(dataContent.toStdString());
            }
        }
    }
}

void QtHttpTransport::handleSseFinished() {
    bool wasError = false;
    std::string errMsg;
    if (m_sseReply) {
        if (m_sseReply->error() != QNetworkReply::NoError && m_sseReply->error() != QNetworkReply::OperationCanceledError) {
            wasError = true;
            errMsg = m_sseReply->errorString().toStdString();
        }
        m_sseReply->deleteLater();
        m_sseReply = nullptr;
    }
    
    if (m_isClosedActively) {
        if (m_onClose) {
            m_onClose();
        }
    } else {
        if (wasError && m_onError) {
            m_onError("SSE disconnected unexpectedly (" + errMsg + "). Reconnecting in 2 seconds...");
        } else if (m_onError) {
            m_onError("SSE disconnected unexpectedly. Reconnecting in 2 seconds...");
        }
        m_reconnectTimer->start();
    }
}

void QtHttpTransport::performReconnect() {
    if (m_onError) {
        m_onError("Attempting to reconnect SSE...");
    }
    start();
}

void QtHttpTransport::handlePostFinished(QNetworkReply* reply) {
    if (reply) {
        if (reply->error() != QNetworkReply::NoError && reply->error() != QNetworkReply::OperationCanceledError) {
            if (m_onError) {
                m_onError("HTTP POST error: " + reply->errorString().toStdString());
            }
        }
        reply->deleteLater();
    }
}

} // namespace mcp

