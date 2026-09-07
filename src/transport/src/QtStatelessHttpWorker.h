#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QMap>
#include <QByteArray>
#include <functional>
#include <map>
#include <string>

class QNetworkProxy;

namespace mcp_qt {

/**
 * @brief QtStatelessHttpTransport 的 I/O 线程工作对象（终态架构 §2.1）。
 *
 * QNAM 与全部网络状态都运行在 McpIoContext 的 I/O 线程上（QNAM 在 start()
 * 内惰性创建，保证构造于 I/O 线程）；结果经信号投递回 transport 所在线程
 * （queued 连接），session 状态机因此单线程运行在 client 所在线程。
 */
class QtStatelessHttpWorker : public QObject {
    Q_OBJECT
public:
    explicit QtStatelessHttpWorker(const QUrl& endpointUrl);
    ~QtStatelessHttpWorker() override;

    void setProtocolVersion(const std::string& version);
    void setExtraRequestHeaders(const std::map<std::string, std::string>& headers);
    void setCustomHeaders(const QMap<QByteArray, QByteArray>& headers);
    void setProxy(const QNetworkProxy& proxy);
    void setTokenProvider(std::function<std::string()> provider);
    void setAuthRetryHandler(std::function<bool(const std::string&)> handler);

    /// 在 I/O 线程上执行：创建 QNAM，进入运行态。
    void start();
    /// 在 I/O 线程上执行：中止在途请求，退出运行态。
    void stop();
    /// 在 I/O 线程上执行：发送一条 JSON-RPC 消息。
    void postMessage(const QString& message);

signals:
    void messageReceived(const QString& message);
    void transportError(const QString& error);
    void transportClosed();

private:
    void onReplyFinished(QNetworkReply* reply, bool isInitializedNotification);
    void startSseListener();
    void handleSseResponse(QNetworkReply* reply);
    void processSseData(const QByteArray& data);
    void applyCommonHeaders(QNetworkRequest& request, bool isGet = false);
    QString currentBearerToken() const;
    QByteArray encodeMcpHeaderValue(const std::string& raw) const;

    QUrl m_endpointUrl;
    QNetworkAccessManager* m_nam{nullptr};  // I/O 线程内惰性创建
    QMap<QByteArray, QByteArray> m_headers;
    bool m_isRunning{false};
    bool m_sseListenerActive{false};

    std::string m_protocolVersion{"2026-07-28"};
    std::map<std::string, std::string> m_extraRequestHeaders;

    std::function<std::string()> m_tokenProvider;
    std::function<bool(const std::string&)> m_authRetryHandler;
    int m_authRetryCount{0};
    static constexpr int kMaxAuthRetries = 3;

    QByteArray m_lastRequestData;
    bool m_isRetrying{false};

    QString m_sessionId;
    QNetworkReply* m_sseReply{nullptr};
    QByteArray m_sseBuffer;
};

} // namespace mcp_qt
