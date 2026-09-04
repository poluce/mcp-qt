#pragma once

#include "mcp_core/IMcpTransport.h"
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>
#include <QPointer>
#include <QTimer>
#include <memory>
#include <functional>
#include <map>

namespace mcp_qt {

// Token provider 回调
using TokenProvider = std::function<std::string()>;
// Auth retry handler 回调（参数: WWW-Authenticate header 值）
using AuthRetryHandler = std::function<bool(const std::string&)>;

// Streamable HTTP Transport：POST 请求后根据 Content-Type 被动适配 JSON/SSE，
// initialized 通知后启动 GET SSE 监听流，支持 401 OAuth 重试
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
    void setExtraRequestHeaders(const std::map<std::string, std::string>& headers) override;

    // 扩展配置
    void setCustomHeaders(const QMap<QByteArray, QByteArray>& headers);
    void setProxy(const class QNetworkProxy& proxy);
    void setTokenProvider(TokenProvider provider);
    void setAuthRetryHandler(AuthRetryHandler handler);

private slots:
    void onReplyFinished(QNetworkReply* reply);

private:
    // 启动 GET SSE 监听流
    void startSseListener();
    void handleSseResponse(QNetworkReply* reply);
    void processSseData(const QByteArray& data);

    // 构建请求头
    void applyCommonHeaders(QNetworkRequest& request, bool isGet = false);
    QString currentBearerToken() const;

    // 按 SEP-2243 对 MCP header 值做 Base64 sentinel 编码
    QByteArray encodeMcpHeaderValue(const std::string& raw) const;

    // 解析 SSE 响应
    QByteArray m_sseBuffer;

    QUrl m_endpointUrl;
    QPointer<QNetworkAccessManager> m_nam;
    QMap<QByteArray, QByteArray> m_headers;
    bool m_isRunning{false};
    bool m_sseListenerActive{false};

    std::string m_protocolVersion{"2026-07-28"};

    std::function<void(const std::string&)> m_onMessage;
    std::function<void()> m_onClose;
    std::function<void(const std::string&)> m_onError;

    // x-mcp-header 扩展：session 层通过 setExtraRequestHeaders 注入的附加 headers
    std::map<std::string, std::string> m_extraRequestHeaders;

    // OAuth 支持
    TokenProvider m_tokenProvider;
    AuthRetryHandler m_authRetryHandler;
    int m_authRetryCount{0};
    static constexpr int kMaxAuthRetries = 3;

    // 请求重试支持
    QByteArray m_lastRequestData;
    bool m_isRetrying{false};

    // Session 支持
    QString m_sessionId;

    // SSE 监听
    QNetworkReply* m_sseReply{nullptr};
};

} // namespace mcp_qt
