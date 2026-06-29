#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <functional>

#include "QtSseParser.h"

namespace mcp_qt {

class QtHttpSseWorker : public QObject {
    Q_OBJECT
public:
    explicit QtHttpSseWorker(QString baseUrl, QObject* parent = nullptr);

    void setProtocolVersion(const QString& version);
    void setTokenProvider(std::function<std::string()> provider);
    void setAuthRetryHandler(std::function<bool(const std::string&)> handler);

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
    void applyCommonHeaders(class QNetworkRequest& request) const;

    QString m_baseUrl;
    QString m_postUrl;
    QString m_protocolVersion{"2025-11-25"};
    QString m_sessionId;
    QString m_lastEventId;
    int m_retryMs{2000};
    QElapsedTimer m_reconnectDeadline;
    bool m_stopping{false};
    bool m_sseConnected{false};
    std::function<std::string()> m_tokenProvider;
    std::function<bool(const std::string&)> m_authRetryHandler;
    class QNetworkAccessManager* m_network{nullptr};
    QPointer<QNetworkReply> m_sseReply;
    QTimer* m_reconnectTimer{nullptr};
    QtSseParser m_parser;
};

} // namespace mcp_qt
