#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QNetworkReply>
#include <QElapsedTimer>

#include <functional>

#include "QtSseParser.h"
#include "mcp_qt_transport/QtHttpSseTransport.h"

namespace mcp_qt {

class QtHttpSseWorker : public QObject {
    Q_OBJECT
public:
    explicit QtHttpSseWorker(QString baseUrl, QObject* parent = nullptr);

    void setProtocolVersion(const QString& version);
    void setTokenProvider(std::function<std::string()> provider);
    void setAuthRetryHandler(std::function<bool(const std::string&)> handler);
    void setRequestConfig(const QtHttpRequestConfig& config);

    bool postMessage(const QString& payload, int retryCount = 0);

public slots:
    void startStream();
    void stopStream();

signals:
    void messageReceived(const QString& message);
    void transportError(const QString& error);
    void transportClosed();

private:
    void openSse();
    void handleSseReadyRead();
    void handleSseFinished();
    void handleSseError(QNetworkReply::NetworkError code);
    void handleSseEvent(const QtSseEvent& event);
    void scheduleReconnect();
    QString currentBearerToken() const;
    void flushPendingMessages();
    void parseSseInlineBody(const QByteArray& body);
    void setupRequestHeaders(QNetworkRequest& request) const;

    QString m_baseUrl;
    QString m_postUrl;
    QString m_protocolVersion{"2025-11-25"};
    QString m_sessionId;
    QString m_lastEventId;
    int m_retryMs{2000};
    bool m_retryFieldReceived{false};  // 服务器发过 retry: 字段 → 健康检查用快阈值（SEP-1699）
    int m_authRetryCount{0};
    static constexpr int kMaxAuthRetries = 3;
    QElapsedTimer m_lastDataTime;
    QElapsedTimer m_lastHealthCheckTime;
    bool m_stopping{false};
    bool m_sseConnected{false};
    std::function<std::string()> m_tokenProvider;
    std::function<bool(const std::string&)> m_authRetryHandler;
    QtHttpRequestConfig m_requestConfig;
    class QNetworkAccessManager* m_network{nullptr};
    
    QNetworkReply* m_sseReply{nullptr};
    QTimer* m_reconnectTimer{nullptr};
    QTimer* m_healthCheckTimer{nullptr};
    QtSseParser m_parser;
    bool m_endpointResolved{false};
    QStringList m_pendingMessages;
};

} // namespace mcp_qt
