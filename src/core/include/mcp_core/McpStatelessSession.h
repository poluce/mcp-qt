#pragma once

#include "mcp_core/McpClientSession.h"

namespace mcp {

/**
 * @brief MCP 2026-07-28 无状态会话（终态架构 §3：stateless 演进核心）。
 *
 * 承载 stateless 协议族的全部专属语义：server/discover、MRTR（input_required
 * 拦截与重发）、Tasks 扩展、subscriptions/listen、每请求 logLevel、self-contained
 * _meta 注入。未来协议版本（stateless 家族）只演进这个类。
 *
 * 基类 McpClientSession 保留 legacy 握手（2025-03-26/06-18/11-25）与共享机制
 * （JSON-RPC 分发、pending 表、超时、通知），冻结不再扩展。
 */
class McpStatelessSession : public McpClientSession {
public:
    explicit McpStatelessSession(std::shared_ptr<IMcpTransport> transport);

    // ========== server/discover（2026-07-28 bootstrap RPC） ==========
    int64_t discoverServer(std::function<void(const McpServerDiscovery& info, const json& error)> callback);

    // ========== MRTR（SEP-2322） ==========
    void setMrtrHandler(MrtrInputHandler handler);

    // ========== Tasks 扩展（SEP-2663） ==========
    int64_t getTask(const std::string& taskId, std::function<void(const McpTask& task, const json& error)> callback);
    int64_t updateTask(const std::string& taskId, const json& inputResponses,
                       std::function<void(bool success, const json& error)> callback);
    int64_t cancelTask(const std::string& taskId, std::function<void(bool success, const json& error)> callback);
    McpTask getTaskSync(const std::string& taskId,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
                        json* errorOut = nullptr);
    bool updateTaskSync(const std::string& taskId, const json& inputResponses,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
                        json* errorOut = nullptr);
    bool cancelTaskSync(const std::string& taskId,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
                        json* errorOut = nullptr);

    // ========== Subscriptions（SEP-2330: subscriptions/listen） ==========
    int64_t listenSubscriptions(const json& filter, std::function<void(bool success, const std::string& error)> callback);
    void cancelSubscription(int64_t requestId);
    void setSubscriptionListener(SubscriptionListener listener);

    // ========== 每请求日志级别（SEP-2577） ==========
    void setLogLevel(const std::string& level);
    std::string getLogLevel() const;

protected:
    // 协议扩展钩子（基类定义，本类承载 stateless 语义）
    void prepareRequestParams(const std::string& method, json& params) override;
    bool handleSpecialResult(int64_t id, const std::string& reqMethod, const json& reqParams,
                             const json& result, const ResponseCallback& cb) override;
    void handleSpecialNotification(const std::string& method, const json& params) override;

private:
    void resendMrtrRequest(const std::string& method, json params, const json& inputResponses,
                           const std::string& requestState, ResponseCallback callback);

    MrtrInputHandler m_mrtrHandler;
    std::unordered_map<int64_t, json> m_subscriptions;
    SubscriptionListener m_subscriptionListener;
    std::string m_requestLogLevel;  // 每请求 logLevel（空=不注入）
};

} // namespace mcp
