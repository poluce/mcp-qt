#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>
#include <QTimer>
#include "mcp_core/IMcpTransport.h"

namespace mcp {

/**
 * @brief Qt-based HTTP / SSE (Server-Sent Events) transport channel for MCP.
 * 
 * Establishes a persistent SSE connection for receiving server-to-client notifications
 * and responses, and uses HTTP POST requests to send client-to-server requests.
 */
class QtHttpTransport : public QObject, public IMcpTransport {
    Q_OBJECT
public:
    explicit QtHttpTransport(const QUrl& sseUrl, QObject* parent = nullptr);
    ~QtHttpTransport() override;

    // IMcpTransport interface
    bool send(const std::string& message) override;
    void setOnMessage(std::function<void(const std::string&)> callback) override;
    void setOnClose(std::function<void()> callback) override;
    void setOnError(std::function<void(const std::string&)> callback) override;
    bool start() override;
    void close() override;

private slots:
    void handleSseReadyRead();
    void handleSseFinished();
    void handlePostFinished(QNetworkReply* reply);
    void performReconnect();

private:
    QUrl m_sseUrl;
    QUrl m_postUrl; 
    QNetworkAccessManager* m_nam;
    QNetworkReply* m_sseReply;

    std::function<void(const std::string&)> m_onMessage;
    std::function<void()> m_onClose;
    std::function<void(const std::string&)> m_onError;

    QByteArray m_sseBuffer;
    QString m_sessionId;  // MCP-Session-Id from server (for session-aware transport)

    // For Streamable HTTP spec requirements:
    QString m_lastEventId;
    QTimer* m_reconnectTimer;
    bool m_isClosedActively;
};

} // namespace mcp

