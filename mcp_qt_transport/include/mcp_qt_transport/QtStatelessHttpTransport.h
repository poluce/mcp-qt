#pragma once

#include "mcp_core/IMcpTransport.h"
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>
#include <QPointer>
#include <memory>
#include <functional>

namespace mcp_qt {

/**
 * @brief Stateless HTTP POST Transport for Serverless/Cloud-Native MCP architectures.
 * 
 * Instead of maintaining a persistent connection like SSE or WebSocket,
 * this transport sends each JSON-RPC request as a standalone HTTP POST request.
 * The server processes the request and returns the JSON-RPC response in the HTTP response body.
 */
class QtStatelessHttpTransport : public QObject, public mcp::IMcpTransport {
    Q_OBJECT
public:
    explicit QtStatelessHttpTransport(const QString& endpointUrl, QObject* parent = nullptr);
    ~QtStatelessHttpTransport() override;

    // mcp::IMcpTransport interface
    bool start() override;
    void close() override;
    bool send(const std::string& message) override;

    void setOnMessage(std::function<void(const std::string&)> callback) override;
    void setOnClose(std::function<void()> callback) override;
    void setOnError(std::function<void(const std::string&)> callback) override;
    void setProtocolVersion(const std::string& version) override;

    void setCustomHeaders(const QMap<QByteArray, QByteArray>& headers);
    void setProxy(const class QNetworkProxy& proxy);

private slots:
    void onReplyFinished(QNetworkReply* reply);

private:
    QUrl m_endpointUrl;
    QPointer<QNetworkAccessManager> m_nam;
    QMap<QByteArray, QByteArray> m_headers;
    bool m_isRunning{false};
    
    std::function<void(const std::string&)> m_onMessage;
    std::function<void()> m_onClose;
    std::function<void(const std::string&)> m_onError;
};

} // namespace mcp_qt
