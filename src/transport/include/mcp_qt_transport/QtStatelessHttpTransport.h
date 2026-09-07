#pragma once

#include "mcp_core/IMcpTransport.h"
#include <QObject>
#include <QUrl>
#include <QMap>
#include <QNetworkProxy>
#include <memory>
#include <functional>
#include <map>

namespace mcp_qt {

class QtStatelessHttpWorker;

// Token provider 回调
using TokenProvider = std::function<std::string()>;
// Auth retry handler 回调（参数: WWW-Authenticate header 值）
using AuthRetryHandler = std::function<bool(const std::string&)>;

// Streamable HTTP Transport：POST 请求后根据 Content-Type 被动适配 JSON/SSE，
// initialized 通知后启动 GET SSE 监听流，支持 401 OAuth 重试。
//
// 终态架构 §2.1：网络 I/O 全部运行在 McpIoContext 共享 I/O 线程（内部 worker），
// 回调经 queued 连接投递回本对象所在线程；本对象只是线程安全的配置/回调句柄。
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
    void setProxy(const QNetworkProxy& proxy);
    void setTokenProvider(TokenProvider provider);
    void setAuthRetryHandler(AuthRetryHandler handler);

private:
    QUrl m_endpointUrl;
    QMap<QByteArray, QByteArray> m_headers;
    QNetworkProxy m_proxy;
    bool m_isRunning{false};

    std::string m_protocolVersion{"2026-07-28"};

    std::function<void(const std::string&)> m_onMessage;
    std::function<void()> m_onClose;
    std::function<void(const std::string&)> m_onError;

    // x-mcp-header 扩展：session 层通过 setExtraRequestHeaders 注入的附加 headers
    std::map<std::string, std::string> m_extraRequestHeaders;

    // OAuth 支持
    TokenProvider m_tokenProvider;
    AuthRetryHandler m_authRetryHandler;

    // I/O 线程工作对象（start() 时创建并移入共享 I/O 线程）
    QtStatelessHttpWorker* m_worker{nullptr};
};

} // namespace mcp_qt
