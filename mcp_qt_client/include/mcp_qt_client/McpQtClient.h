#pragma once

#include <mcp_core/McpClientSession.h>
#include <mcp_core/McpTool.h>
#include <mcp_core/McpResource.h>
#include <mcp_core/McpPrompt.h>
#include <mcp_core/McpOAuthClient.h>
#include <mcp_qt_transport/QtHttpSseTransport.h>
#include <mcp_qt_transport/QtProcessStdioTransport.h>
#include <mcp_qt_transport/QtStatelessHttpTransport.h>
#include <mcp_qt_client/McpQtToolResult.h>
#include <mcp_qt_client/McpResourceSubscriptionRouter.h>
#include <mcp_qt_client/McpToolsModel.h>
#include <mcp_core/McpReconnectPolicy.h>

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace mcp_qt {

struct McpResult {
    bool isError{false};
    QJsonObject data;
    QString errorString;
};

struct McpQtTool {
    QString name;
    QString description;
    QJsonObject inputSchema;
};

class McpQtClient;
class McpToolsModel;
class McpPromptsModel;
class McpResourcesModel;
class McpResourceTemplatesModel;

class McpQtClientBuilder {
public:
    McpQtClientBuilder& setTransportHttp(const QString& url);
    McpQtClientBuilder& setTransportStatelessHttp(const QString& url);
    McpQtClientBuilder& setTransportStdio(const QString& command, const QStringList& args = {});
    McpQtClientBuilder& setClientInfo(const QString& name, const QString& version);
    McpQtClientBuilder& setTimeout(int ms);
    McpQtClientBuilder& setHttpHeaders(const QMap<QString, QString>& headers);
    McpQtClientBuilder& setHttpProxy(const QNetworkProxy& proxy);
    McpQtClientBuilder& setReconnectPolicy(const mcp::McpReconnectPolicy& policy);
    std::shared_ptr<McpQtClient> buildAndConnectAndWait(QString* errorString = nullptr);
    std::shared_ptr<McpQtClient> buildAndConnectAsync();
private:
    int m_transportType{0}; // 0=none, 1=http, 2=stdio, 3=stateless_http
    QString m_url_or_cmd;
    QStringList m_args;
    QString m_clientName{QStringLiteral("mcp-qt-client")};
    QString m_clientVersion{QStringLiteral("1.0.0")};
    int m_timeoutMs{10000};
    QMap<QString, QString> m_httpHeaders;
    std::optional<QNetworkProxy> m_proxy;
    mcp::McpReconnectPolicy m_reconnectPolicy;
};

/**
 * @brief 高层 MCP 客户端（QObject，信号/槽，语义对齐 TS SDK `Client`）
 *
 * @code
 *   // HTTP/SSE
 *   auto c = McpQtClient::connectHttp("http://localhost:8080/mcp");
 *   // Stdio 子进程
 *   auto c = McpQtClient::connectStdio("python", {"server.py"});
 *   // OAuth
 *   auto c = McpQtClient::connectWithOAuth({.serverUrl="...", .clientId="..."});
 *
 *   auto tools = c->listTools();
 *   c->callTool("add", {{"a",5},{"b",3}});
 *   c->setLoggingLevel("debug");
 * @endcode
 */
class McpQtClient : public QObject {
    Q_OBJECT
    friend class McpQtClientBuilder;
public:
    using Ptr = std::shared_ptr<McpQtClient>;

    struct OAuthConfig {
        QString serverUrl;
        QString clientId;
        QString clientSecret;
        QString redirectUri{QStringLiteral("http://localhost:3000/callback")};
        QStringList scopes;
    };

    ~McpQtClient() override;

    // ========== 工厂（对齐 TS `new Client({name,version}).connect(transport)`）==========

    /// HTTP/SSE 连接
    
    /// HTTP/SSE 连接 (纯异步，需监听 connected() 和 errorOccurred() 信号)
    static Ptr connectHttpAsync(const QString& serverUrl,
                                const QString& clientName = QStringLiteral("mcp-qt-client"),
                                const QString& clientVersion = QStringLiteral("1.0.0"));

    /// Stdio 子进程连接 (纯异步)
    static Ptr connectStdioAsync(const QString& command, const QStringList& args = {},
                                 const QString& clientName = QStringLiteral("mcp-qt-client"),
                                 const QString& clientVersion = QStringLiteral("1.0.0"));

    /// HTTP/SSE + OAuth (纯异步)
    static Ptr connectWithOAuthAsync(const OAuthConfig& oauth,
                                     const QString& clientName = QStringLiteral("mcp-qt-client"),
                                     const QString& clientVersion = QStringLiteral("1.0.0"));

    static Ptr connectHttpAndWait(const QString& serverUrl,
                           const QString& clientName = QStringLiteral("mcp-qt-client"),
                           const QString& clientVersion = QStringLiteral("1.0.0"),
                           int timeoutMs = 10000,
                           QString* errorString = nullptr);

    /// Stdio 子进程连接
    static Ptr connectStdioAndWait(const QString& command, const QStringList& args = {},
                            const QString& clientName = QStringLiteral("mcp-qt-client"),
                            const QString& clientVersion = QStringLiteral("1.0.0"),
                            int timeoutMs = 10000,
                            QString* errorString = nullptr);

    /// 测试专用静态工厂（允许直接分配 client 实例以进行 MockTransport 测试）
    static Ptr createForTest(QObject* parent = nullptr) {
        return Ptr(new McpQtClient(parent));
    }

    /// HTTP/SSE + OAuth
    static Ptr connectWithOAuthAndWait(const OAuthConfig& oauth,
                                const QString& clientName = QStringLiteral("mcp-qt-client"),
                                const QString& clientVersion = QStringLiteral("1.0.0"),
                                int timeoutMs = 30000);

    // ========== Server Info ==========

    QJsonObject serverInfo() const;
    QJsonObject serverCapabilities() const;
    QString negotiatedProtocolVersion() const;
    QString instructions() const;

    // 便捷能力检测
    bool hasToolsCapability() const;
    bool hasPromptsCapability() const;
    bool hasResourcesCapability() const;

    // ========== Tools（对齐 TS `listTools()`, `callTool()`）==========

    using ProgressCallback = std::function<void(float progress, float total, const QString& message)>;

    std::vector<McpQtTool> listTools(int timeoutMs = 10000);
    std::vector<McpQtTool> listTools(const QString& cursor, QString* nextCursor = nullptr, int timeoutMs = 10000);
    std::vector<McpQtTool> fetchAllTools(int timeoutMs = 10000);

    /// 异步获取工具列表
    void listToolsAsync(const QString& cursor, std::function<void(const std::vector<McpQtTool>& tools, const QString& nextCursor, const QString& error)> callback);

    /// 创建工具列表 Model（需调用方自行管理 Model 生命周期）
    /// 返回的 McpToolsModel 已绑定当前 client，可直接调用 refresh() 填充数据
    std::unique_ptr<McpToolsModel> createToolsModel(QObject* parent = nullptr);

    /// 调用工具（同步，对齐 TS `callTool()`）
    McpResult callTool(const QString& name, const QJsonObject& arguments, int timeoutMs = 10000);

    /// 调用工具 + 进度通知（对齐 TS `callTool({...}, {onProgress})`）
    McpResult callTool(const QString& name, const QJsonObject& arguments,
                       ProgressCallback onProgress, int timeoutMs = 10000);

    /// 调用工具（纯异步，防止阻塞主线程）
    void callToolAsync(const QString& name, const QJsonObject& arguments,
                       std::function<void(McpResult)> callback,
                       ProgressCallback onProgress = nullptr);

    /// 调用工具（纯异步，绑定上下文保护生命周期，回调切换到接收方所在线程）
    void callToolAsync(const QString& name, const QJsonObject& arguments,
                       QObject* context,
                       std::function<void(McpResult)> callback,
                       ProgressCallback onProgress = nullptr);

    /// 调用工具（同步，返回类型化结果，不丢弃原始 JSON）
    McpQtToolResult callToolTyped(const QString& name, const QJsonObject& arguments, int timeoutMs = 10000);

    /// 调用工具（异步，返回类型化结果）
    void callToolTypedAsync(const QString& name, const QJsonObject& arguments,
                            std::function<void(McpQtToolResult)> callback,
                            int timeoutMs = 10000);

    // ========== Resources（对齐 TS `listResources()`, `readResource()`, `subscribeResource()`）==========

    QJsonObject listResources(int timeoutMs = 10000);
    QJsonObject listResources(const QString& cursor, QString* nextCursor = nullptr, int timeoutMs = 10000);
    QJsonObject fetchAllResources(int timeoutMs = 10000);

    /// 异步获取资源列表
    void listResourcesAsync(const QString& cursor, std::function<void(const QJsonObject& result, const QString& nextCursor, const QString& error)> callback);

    QJsonObject readResource(const QString& uri, int timeoutMs = 10000);
    void readResourceAsync(const QString& uri, std::function<void(const QJsonObject& result, const QString& error)> callback);

    bool subscribeResource(const QString& uri, int timeoutMs = 10000);
    void subscribeResourceAsync(const QString& uri, std::function<void(bool success, const QString& error)> callback);

    bool unsubscribeResource(const QString& uri, int timeoutMs = 10000);
    void unsubscribeResourceAsync(const QString& uri, std::function<void(bool success, const QString& error)> callback);

    /// 订阅资源更新（callback 派发式）——发送 resources/subscribe 并注册路由回调
    /// @param uri      要订阅的资源 URI
    /// @param callback 收到 notifications/resources/updated 时派发
    /// @param timeoutMs RPC 超时（毫秒）
    /// @return 路由 token，传入 unsubscribeResource() 撤销回调（返回 -1 表示失败）
    int subscribeResource(const QString& uri,
                          std::function<void(const QString& uri, const QJsonObject& params)> callback,
                          int timeoutMs = 10000);
    void subscribeResourceAsync(const QString& uri,
                                std::function<void(const QString& uri, const QJsonObject& params)> onUpdate,
                                std::function<void(int routerToken, const QString& error)> callback);

    /// 撤销订阅（通过 token）并发送 resources/unsubscribe
    bool unsubscribeResourceByToken(const QString& uri, int routerToken, int timeoutMs = 10000);
    void unsubscribeResourceByTokenAsync(const QString& uri, int routerToken, std::function<void(bool success, const QString& error)> callback);

    // ========== Resource Templates（对齐 TS `listResourceTemplates()`）==========

    std::vector<mcp::McpResourceTemplate> listResourceTemplates(int timeoutMs = 10000);
    std::vector<mcp::McpResourceTemplate> listResourceTemplates(const QString& cursor, QString* nextCursor = nullptr, int timeoutMs = 10000);
    std::vector<mcp::McpResourceTemplate> fetchAllResourceTemplates(int timeoutMs = 10000);

    void listResourceTemplatesAsync(const QString& cursor, std::function<void(const std::vector<mcp::McpResourceTemplate>& result, const QString& nextCursor, const QString& error)> callback);

    std::unique_ptr<McpResourceTemplatesModel> createResourceTemplatesModel(QObject* parent = nullptr);

    // ========== Prompts（对齐 TS `listPrompts()`, `getPrompt()`）==========

    QJsonObject listPrompts(int timeoutMs = 10000);
    QJsonObject listPrompts(const QString& cursor, QString* nextCursor = nullptr, int timeoutMs = 10000);
    QJsonObject fetchAllPrompts(int timeoutMs = 10000);

    /// 异步获取提示词列表
    void listPromptsAsync(const QString& cursor, std::function<void(const QJsonObject& result, const QString& nextCursor, const QString& error)> callback);

    QJsonObject getPrompt(const QString& name, const QJsonObject& arguments, int timeoutMs = 10000);
    void getPromptAsync(const QString& name, const QJsonObject& arguments, std::function<void(const QJsonObject& result, const QString& error)> callback);

    // ========== 其他（对齐 TS `ping()`, `complete()`, `setLoggingLevel()`）==========

    bool ping(int timeoutMs = 5000);
    void pingAsync(std::function<void(bool success, const QString& error)> callback);

    QJsonObject complete(const QJsonObject& ref, const QJsonObject& argument, int timeoutMs = 10000);
    void completeAsync(const QJsonObject& ref, const QJsonObject& argument, std::function<void(const QJsonObject& completion, const QString& error)> callback);
    /// 设置服务端日志级别，发送 logging/setLevel 请求
    bool setLoggingLevel(const QString& level, int timeoutMs = 5000);

    using TrafficLogger = std::function<void(const QJsonObject& event)>;
    void setTrafficLogger(TrafficLogger logger);

    // ========== 双向能力（对齐 TS `setRequestHandler()`）==========

    using ElicitationHandler = std::function<void(const QJsonObject& params, std::function<void(const QJsonObject& result, const QJsonObject& error)> callback)>;
    void setElicitationHandler(ElicitationHandler handler);
    void setElicitationHandler(QObject* context, ElicitationHandler handler);

    using SamplingHandler = std::function<void(const QJsonObject& params, std::function<void(const QJsonObject& result, const QJsonObject& error)> callback)>;
    void setSamplingHandler(SamplingHandler handler);
    void setSamplingHandler(QObject* context, SamplingHandler handler);

    using RootsProvider = std::function<void(std::function<void(const QJsonArray& result, const QJsonObject& error)> callback)>;
    void setRootsProvider(RootsProvider provider);
    void setRootsProvider(QObject* context, RootsProvider provider);
    void notifyRootsListChanged();

    // ========== 通知（对齐 TS `notification()` 等）==========

    void registerNotificationHandler(const QString& method, std::function<void(const QJsonObject& params)> handler);
    void registerNotificationHandler(const QString& method, QObject* context, std::function<void(const QJsonObject& params)> handler);
    void enableNotificationDebounce(const QString& method, int debounceMs = 100);
    /// 发送任意通知给服务端
    void sendNotification(const QString& method, const QJsonObject& params);

    /// 发送请求（异步，对齐 TS `client.request()`）。返回 requestId，可用于 cancelRequest
    int64_t sendRequest(const QString& method, const QJsonObject& params,
                        std::function<void(const QJsonObject& result, const QJsonObject& error)> callback,
                        ProgressCallback onProgress = nullptr);

    /// 发送请求（纯异步，绑定上下文保护生命周期，回调切换到接收方所在线程）
    int64_t sendRequest(const QString& method, const QJsonObject& params,
                        QObject* context,
                        std::function<void(const QJsonObject& result, const QJsonObject& error)> callback,
                        ProgressCallback onProgress = nullptr);
    /// 取消指定请求
    void cancelRequest(int64_t requestId);

    // ========== 能力（对齐 TS `registerCapabilities()`）==========

    void registerCapability(const QString& name, const QJsonObject& config);

    // ========== 生命周期与重连（对齐 TS `close()`）==========

    bool isConnected() const;
    /// 优雅关闭（发送 shutdown 请求后关闭 transport）
    void close(int timeoutMs = 5000);

    /// 设置重连策略
    void setReconnectPolicy(const mcp::McpReconnectPolicy& policy);
    mcp::McpReconnectPolicy reconnectPolicy() const;

    /// 设置重连 Transport 构造工厂（测试或高级地址切换用）
    void setTransportFactory(std::function<std::shared_ptr<mcp::IMcpTransport>()> factory);

    // ========== 异步连接 ==========

    /// 连接到已有 transport（对齐 TS `connect(transport)`）
    
    /// 连接到已有 transport（纯异步）
    void connectToTransportAsync(std::shared_ptr<mcp::IMcpTransport> transport,
                                 const QString& clientName, const QString& clientVersion);

    bool connectToTransportAndWait(std::shared_ptr<mcp::IMcpTransport> transport,
                           const QString& clientName, const QString& clientVersion, int timeoutMs = 10000, QString* errorString = nullptr);

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString& message);
    
    /// 收到服务端的任意通知
    void notificationReceived(const QString& method, const QJsonObject& params);
    // 协议规范事件：服务端列表变更通知
    void toolsChanged();
    void resourcesChanged();
    void promptsChanged();

    // 重连状态信号
    void reconnecting();
    void reconnected();
    void recoveryFailed(const QString& message);

private:
    explicit McpQtClient(QObject* parent = nullptr);

    
    void doInitializeAsync(const QString& clientName, const QString& clientVersion);
    void setupTransportCommon(std::shared_ptr<mcp::IMcpTransport> transport);

    bool doInitializeAndWait(const QString& clientName, const QString& clientVersion, int timeoutMs, QString* errorString = nullptr);
    bool doOAuth(const OAuthConfig& oauth);

    static nlohmann::json toNlohmann(const QJsonObject& obj);
    static QJsonObject fromNlohmann(const nlohmann::json& j);

    std::shared_ptr<mcp::McpClientSession> m_session;
    std::shared_ptr<mcp::McpOAuthClient> m_oauth;
    bool m_initialized{false};
    mutable std::map<QString, QJsonObject> m_toolSchemaCache;
    bool validateToolSchemaLocally(const QString& name, const QJsonObject& arguments, QString* errorString) const;

    TrafficLogger m_trafficLogger;
    McpResourceSubscriptionRouter m_resourceRouter;

    struct PendingCapability {
        QString name;
        QJsonObject config;
    };
    std::vector<PendingCapability> m_pendingCapabilities;

    // 重连与恢复上下文
    int m_transportType{0}; // 0=none/test, 1=http, 2=stdio
    QString m_url_or_cmd;
    QStringList m_args;
    QMap<QString, QString> m_httpHeaders;
    std::optional<QNetworkProxy> m_proxy;
    QString m_clientName{QStringLiteral("mcp-qt-client")};
    QString m_clientVersion{QStringLiteral("1.0.0")};
    int m_timeoutMs{10000};

    mcp::McpReconnectPolicy m_reconnectPolicy;
    class QTimer* m_reconnectTimer{nullptr};
    int m_reconnectAttempts{0};
    bool m_isUserClosed{false};
    bool m_inRecovery{false};

    struct NotificationHandlerEntry {
        QString method;
        QPointer<QObject> context;
        std::function<void(const QJsonObject&)> handler;
        bool hasContext;
    };
    QList<NotificationHandlerEntry> m_savedNotificationHandlers;
    
    struct CapabilityHandlerEntry {
        QPointer<QObject> context;
        void* handler; // Type-erased, casted when used, or we can use specific fields
        bool hasContext;
    };
    
    SamplingHandler m_savedSamplingHandler{nullptr};
    QPointer<QObject> m_savedSamplingContext;
    bool m_hasSavedSamplingContext{false};

    ElicitationHandler m_savedElicitationHandler{nullptr};
    QPointer<QObject> m_savedElicitationContext;
    bool m_hasSavedElicitationContext{false};

    RootsProvider m_savedRootsProvider{nullptr};
    QPointer<QObject> m_savedRootsContext;
    bool m_hasSavedRootsContext{false};

    std::function<std::shared_ptr<mcp::IMcpTransport>()> m_transportFactory;
    QList<QPointer<McpToolsModel>> m_toolsModels;
    QList<QPointer<McpPromptsModel>> m_promptsModels;
    QList<QPointer<McpResourcesModel>> m_resourcesModels;
    QList<QPointer<McpResourceTemplatesModel>> m_templatesModels;

    struct ReplayableRequest {
        QString method;
        QJsonObject params;
        QPointer<QObject> context;
        bool hasContext{false};
        std::function<void(const QJsonObject&, const QJsonObject&)> callback;
        ProgressCallback progressCallback;
    };
    mutable std::mutex m_replayMutex;
    std::vector<ReplayableRequest> m_queuedReplayRequests;
    std::unordered_map<int64_t, ReplayableRequest> m_inFlightReplayableRequests;

    void handleTransportFailure();
    void executeReconnectAttempt();
    void restoreNotificationHandlers();
    void restoreResourceSubscriptions();
    void refreshToolsAfterRecovery();
    void replayQueuedRequests();
    bool isReplayableMethod(const QString& method) const;
};

} // namespace mcp_qt
