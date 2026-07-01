#pragma once

#ifndef MCP_ENABLE_HTTP
#error "HttpSseTransport requires MCP_ENABLE_HTTP=ON. Reconfigure with -DMCP_ENABLE_HTTP=ON or use Stdio transport instead."
#endif

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include "IMcpTransport.h"

namespace mcp {

class McpOAuthClient;

} // namespace mcp

namespace httplib {
class Client;
}

namespace mcp {

/**
 * @brief Legacy pure C++ HTTP/SSE transport for MCP (experimental on Windows HTTPS/SSE).
 *
 * Preferred usage:
 * - local MCP servers: SubprocessStdioTransport
 * - remote MCP servers inside Qt agents: mcp_qt::QtHttpSseTransport
 *
 * This transport remains available for compatibility, but is not the preferred
 * remote transport for Qt-based agents.
 * 
 * Uses libcurl for HTTP communication with built-in platform SSL support.
 * Establishes an SSE (Server-Sent Events) connection for receiving server messages,
 * and sends client messages via HTTP POST.
 */
class HttpSseTransport : public IMcpTransport {
public:
    explicit HttpSseTransport(const std::string& sseUrl);
    ~HttpSseTransport() override;

    bool send(const std::string& message) override;
    void setOnMessage(std::function<void(const std::string&)> callback) override;
    void setOnClose(std::function<void()> callback) override;
    void setOnError(std::function<void(const std::string&)> callback) override;
    bool start() override;
    void close() override;
    void setProtocolVersion(const std::string& version) override;

    // SSE 流式数据回调（由 libcurl WRITEFUNCTION 调用）
    void onSseData(const char* data, size_t len);

    using TokenProvider = std::function<std::string()>;
    void setTokenProvider(TokenProvider provider);

    using AuthRetryHandler = std::function<bool(const std::string& wwwAuthenticateHeader)>;
    void setAuthRetryHandler(AuthRetryHandler handler);

private:
    void sseReadLoop();
    bool doPost(const std::string& body);

    std::string m_sseUrl;
    std::string m_postUrl;
    std::string m_sessionId;
    std::string m_protocolVersion{"2025-11-25"};

    std::function<void(const std::string&)> m_onMessage;
    std::function<void()> m_onClose;
    std::function<void(const std::string&)> m_onError;

    TokenProvider m_tokenProvider{nullptr};
    AuthRetryHandler m_authRetryHandler{nullptr};
    std::shared_ptr<McpOAuthClient> m_oauthClient;

    std::thread m_sseThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_closed{false};
    std::recursive_mutex m_sendMutex;
    std::mutex m_authMutex;

    std::string m_sseBuffer; // SSE 数据块缓冲区
    std::string m_lastEventId;   // 最后收到的 SSE event id（用于 Last-Event-ID 重连）
    int m_sseRetryMs{2000};      // 服务端指定的 SSE 重连延迟（毫秒），默认 2 秒
    int m_totalAuthRetries{0};   // 认证重试总次数（scope-retry-limit 防护，不因成功 OAuth 重置）
    static constexpr int kMaxAuthRetries = 3; // 最大认证重试次数（scope-retry-limit 期望 ≤3）

    std::unique_ptr<httplib::Client> m_httpClient;
    std::mutex m_clientMutex;

    bool handleDefaultAuthRetry(const std::string& wwwAuthHeader);

public:
    void setSseConnected(bool connected) { m_sseConnected = connected; }

private:
    std::atomic<bool> m_sseConnected{false};
};

} // namespace mcp
