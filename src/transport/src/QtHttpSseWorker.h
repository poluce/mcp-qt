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
    // openSse 主动 abort 旧流时置位：旧流 finished 不再触发重连调度
    // （否则每次替换 GET 流都会产生 500ms 重连循环）
    bool m_intentionalAbort{false};
    std::function<std::string()> m_tokenProvider;
    std::function<bool(const std::string&)> m_authRetryHandler;
    QtHttpRequestConfig m_requestConfig;
    class QNetworkAccessManager* m_network{nullptr};
    // POST 专用 QNAM：与 GET SSE 长连接隔离，避免 QNAM 连接池把 POST 排队在
    // GET 连接后面（localhost/IPv6 场景实测 POST 会被卡住直到 GET 被 abort）。
    class QNetworkAccessManager* m_postNetwork{nullptr};
    
    QNetworkReply* m_sseReply{nullptr};
    QTimer* m_reconnectTimer{nullptr};
    QTimer* m_healthCheckTimer{nullptr};
    QtSseParser m_parser;
    bool m_endpointResolved{false};
    QStringList m_pendingMessages;
};

} // namespace mcp_qt
