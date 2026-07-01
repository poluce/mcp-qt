#include "mcp_qt_transport/QtStatelessHttpTransport.h"
#include <QNetworkRequest>
#include <QNetworkProxy>

namespace mcp_qt {

QtStatelessHttpTransport::QtStatelessHttpTransport(const QString& endpointUrl, QObject* parent)
    : QObject(parent), m_endpointUrl(endpointUrl), m_nam(new QNetworkAccessManager(this))
{
    m_headers.insert("Content-Type", "application/json");
}

QtStatelessHttpTransport::~QtStatelessHttpTransport() {
    close();
}

bool QtStatelessHttpTransport::start() {
    if (m_isRunning) return true;
    m_isRunning = true;
    return true; // Stateless transport doesn't require maintaining a persistent connection
}

void QtStatelessHttpTransport::close() {
    if (!m_isRunning) return;
    m_isRunning = false;
    
    // We can't really "close" a stateless connection, but we can notify the upper layer
    if (m_onClose) {
        m_onClose();
    }
}

bool QtStatelessHttpTransport::send(const std::string& message) {
    if (!m_isRunning) return false;
    if (!m_nam) return false;

    QNetworkRequest request(m_endpointUrl);
    for (auto it = m_headers.constBegin(); it != m_headers.constEnd(); ++it) {
        request.setRawHeader(it.key(), it.value());
    }

    QByteArray data = QByteArray::fromStdString(message);
    QNetworkReply* reply = m_nam->post(request, data);
    
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onReplyFinished(reply);
    });
    
    return true;
}

void QtStatelessHttpTransport::onReplyFinished(QNetworkReply* reply) {
    reply->deleteLater();
    
    if (!m_isRunning) return;

    if (reply->error() != QNetworkReply::NoError) {
        QString errorMsg = QString("HTTP POST failed: %1").arg(reply->errorString());
        if (m_onError) {
            m_onError(errorMsg.toStdString());
        }
        return;
    }

    QByteArray responseData = reply->readAll();
    if (!responseData.isEmpty() && m_onMessage) {
        m_onMessage(responseData.toStdString());
    }
}

void QtStatelessHttpTransport::setOnMessage(std::function<void(const std::string&)> callback) {
    m_onMessage = std::move(callback);
}

void QtStatelessHttpTransport::setOnClose(std::function<void()> callback) {
    m_onClose = std::move(callback);
}

void QtStatelessHttpTransport::setOnError(std::function<void(const std::string&)> callback) {
    m_onError = std::move(callback);
}

void QtStatelessHttpTransport::setProtocolVersion(const std::string&) {
    // Optional metadata, no-op for basic stateless
}

void QtStatelessHttpTransport::setCustomHeaders(const QMap<QByteArray, QByteArray>& headers) {
    m_headers = headers;
    if (!m_headers.contains("Content-Type")) {
        m_headers.insert("Content-Type", "application/json");
    }
}

void QtStatelessHttpTransport::setProxy(const QNetworkProxy& proxy) {
    if (m_nam) {
        m_nam->setProxy(proxy);
    }
}

} // namespace mcp_qt
