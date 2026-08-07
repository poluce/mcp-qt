#pragma once

#include <mcp_core/McpResource.h>
#include <mcp_core/McpPrompt.h>
#include <mcp_qt_transport/QtHttpSseTransport.h>
#include <mcp_qt_transport/QtProcessStdioTransport.h>
#include <mcp_qt_transport/QtStatelessHttpTransport.h>
#include <mcp_qt_client/McpQtToolResult.h>
#include <mcp_qt_client/McpResourceSubscriptionRouter.h>
#include <mcp_qt_client/McpToolsModel.h>
#include <mcp_core/McpReconnectPolicy.h>
#include <mcp_qt_client/McpError.h>

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QFuture>
#include <QPromise>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>


namespace mcp {
class McpClientSession;
class McpOAuthClient;
class IMcpTransport;
}

namespace mcp_qt {

using MrtrReplyCallback = std::function<void(const QJsonObject& userResponse)>;

enum class McpContentKind {
    Text,
    Image,
    EmbeddedResource
};

struct McpContent {
    McpContentKind kind;
    QString text;
    QString mimeType;
    QByteArray binary;
    QJsonObject rawData;
};

struct McpResult {
    bool isError{false};
    QJsonObject data;
    QString errorString;
    QList<McpContent> contents;
};

struct McpBatchCallRequest {
    QString name;
    QJsonObject arguments;
};

struct McpBatchCallResult {
    QString name;
    QJsonObject arguments;
    McpResult result;
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
    McpQtClientBuilder& setEnvironment(const QMap<QString, QString>& env);
    McpQtClientBuilder& setNamespace(const QString& ns);
    McpQtClientBuilder& setClientInfo(const QString& name, const QString& version);
    McpQtClientBuilder& setTimeout(int ms);
    McpQtClientBuilder& setHttpHeaders(const QMap<QString, QString>& headers);
    McpQtClientBuilder& setHttpProxy(const QNetworkProxy& proxy);
    McpQtClientBuilder& setReconnectPolicy(const mcp::McpReconnectPolicy& policy);
    McpQtClientBuilder& setProtocolVersion(const QString& version);
    McpQtClientBuilder& setStatelessMode(bool enabled);
    std::shared_ptr<McpQtClient> buildAndConnectAndWait(QString* errorString = nullptr);
    std::shared_ptr<McpQtClient> buildAndConnectAsync();
private:
    QString m_namespace;
    int m_transportType{0}; // 0=none, 1=http, 2=stdio, 3=stateless_http
    QString m_url_or_cmd;
    QStringList m_args;
    QString m_clientName{QStringLiteral("mcp-qt-client")};
    QString m_clientVersion{QStringLiteral("1.0.0")};
    QString m_protocolVersion{QStringLiteral("2026-07-28")};
    bool m_statelessMode{false};
    int m_timeoutMs{10000};
    QMap<QString, QString> m_env;
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

    /// 获取 OAuth 客户端实例（用于外部设置 token provider）
    std::shared_ptr<mcp::McpOAuthClient> oauthClient() const { return m_oauth; }

    /// 执行 OAuth 流程（静态方法，供传输层调用）
    static bool runOAuthFlow(const std::string& serverUrl,
                            const nlohmann::json& context,
                            const std::string& wwwAuth,
                            std::shared_ptr<mcp::McpOAuthClient> oauthClient);

    // ========== Server Info ==========

    QJsonObject serverInfo() const;
    QJsonObject serverCapabilities() const;
    QString negotiatedProtocolVersion() const;
    QString instructions() const;

    void setStatelessMode(bool enabled);
    bool isStatelessMode() const;
    void setProtocolVersion(const QString& version);

    // ========== Server Discover（2026-07-28, SEP-2575）==========

    /// server/discover 结果
    struct DiscoverInfo {
        QStringList supportedVersions;
        QJsonObject capabilities;
        QJsonObject serverInfo;
        QString instructions;
        QString resultType;
        qint64 ttlMs{-1};
        QString cacheScope;
        bool empty() const {
            return supportedVersions.isEmpty() && serverInfo.isEmpty() && instructions.isEmpty();
        }
    };

    /// 同步执行 server/discover（无状态模式下推荐在其它 RPC 前调用）
    DiscoverInfo discoverServer(int timeoutMs = 10000);

    /// 异步执行 server/discover
    void discoverServerAsync(std::function<void(const DiscoverInfo& info, const QString& error)> callback);

    // 便捷能力检测
    bool hasToolsCapability() const;
    bool hasPromptsCapability() const;
    bool hasResourcesCapability() const;

    QString nameSpace() const { return m_namespace; }
    void setNamespace(const QString& ns) { m_namespace = ns; }
    QString stripNamespace(const QString& name) const;

    // ========== Tools（对齐 TS `listTools()`, `callTool()`）==========

    using ProgressCallback = std::function<void(float progress, float total, const QString& message)>;

    std::vector<McpQtTool> listTools(int timeoutMs = 10000);
    std::vector<McpQtTool> listTools(const QString& cursor, QString* nextCursor = nullptr, int timeoutMs = 10000);
    std::vector<McpQtTool> fetchAllTools(int timeoutMs = 10000);

    /// 获取当前缓存在客户端中的所有工具列表（不触发网络请求）
    std::vector<McpQtTool> cachedTools() const;

    /// 异步获取工具列表（单页）
    void listToolsAsync(const QString& cursor, std::function<void(const std::vector<McpQtTool>& tools, const QString& nextCursor, const QString& error)> callback);

    /// 异步拉取所有工具（自动分页），完成后触发 toolsReady 信号
    void fetchAllToolsAsync();

    /// 异步拉取所有工具（自动分页），完成后触发回调
    void fetchAllToolsAsync(std::function<void(const std::vector<McpQtTool>& tools)> callback);

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

    /// 调用工具（现代异步 QFuture 接口）
    QFuture<McpResult> callToolFuture(const QString& name, const QJsonObject& arguments);

    /// 调用工具（同步，返回类型化结果，不丢弃原始 JSON）
    McpQtToolResult callToolTyped(const QString& name, const QJsonObject& arguments, int timeoutMs = 10000);

    /// 调用工具（异步，返回类型化结果）
    void callToolTypedAsync(const QString& name, const QJsonObject& arguments,
                            std::function<void(McpQtToolResult)> callback,
                            int timeoutMs = 10000);

    // ========== 公开参数校验 API ==========
    /// 本地校验工具参数是否符合 Schema 要求
    bool validateToolArguments(const QString& name, const QJsonObject& arguments, QString* errorString = nullptr) const;

    // ========== 工具定义导出为 LLM 格式 ==========
    enum class LlmFormat {
        OpenAI,       // OpenAI function calling 格式
        Anthropic,    // Claude/Anthropic tool 格式
        Gemini        // Gemini function declaration 格式
    };

    /// 将给定的 McpQtTool 转换为指定 LLM 格式的 JSON 对象
    static QJsonObject exportToolToLlmFormat(const McpQtTool& tool, LlmFormat format = LlmFormat::OpenAI, const QString& prefix = QString());

    /// 根据工具名称从缓存中导出为指定 LLM 格式 of JSON 对象
    QJsonObject exportToolToLlmFormat(const QString& name, LlmFormat format = LlmFormat::OpenAI) const;

    /// 将当前缓存的所有工具定义导出为指定 LLM 格式的 JSON 数组
    /// 注意：返回值已被整体格式化为 LLM 专有结构（如 OpenAI 的 {"type":"function","function":{...}}），
    /// 调用方可直接赋值给 API 请求的 tools 字段，无需额外包裹。
    QJsonArray exportAllToolsToLlmFormat(LlmFormat format = LlmFormat::OpenAI) const;

    /// 将当前缓存的所有工具定义导出为标准 MCP Schema 格式（仅含 name/description/inputSchema）
    /// 适用于业务方需要自行组装 LLM 专有结构的场景
    QJsonArray exportAllToolsAsMcpSchema() const;

    // ========== 并发多工具调用 ==========
    /// 异步并发调用多个工具，所有调用完成（或超时）后触发回调
    void callToolsConcurrentAsync(const std::vector<McpBatchCallRequest>& requests,
                                  std::function<void(const std::vector<McpBatchCallResult>&)> callback,
                                  int timeoutMs = 10000);

    /// 同步并发调用多个工具，阻塞等待所有调用完成（或超时）
    std::vector<McpBatchCallResult> callToolsConcurrent(const std::vector<McpBatchCallRequest>& requests,
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
    
    /**
     * @brief 设置客户端的工作区根目录提供者 (Roots Provider)。
     * 
     * 客户端在初始化时默认已声明支持 `roots` 能力。
     * - 如果不调用此方法，底层会自动拦截服务端的 `roots/list` 请求并默认返回空列表 `{"roots": []}`，避免 "-32601 Method not found" 错误。
     * - 调用此方法可注入实际的 Provider 处理逻辑。可随时重复调用以替换旧的 Provider。
     */
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
    void setClientCapabilities(const QJsonObject& caps);

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
    void errorOccurred(const mcp_qt::McpError& error);
    
    /// MCP 2026-07-28 MRTR: 服务端返回 input_required，请求客户端补全输入后再重试原请求。
    /// requestId: 待补全请求的 JSON-RPC id；inputRequests: 规范 InputRequests map
    /// （key -> {method, params}）；requestState: 服务端 opaque 状态，重试时必须原样回显。
    /// 调用 replyCallback 时传入 InputResponses map（key -> 对应结果），库会自动完成重试。
    void inputRequired(const QString& requestId, const QJsonObject& inputRequests,
                       const QString& requestState, mcp_qt::MrtrReplyCallback replyCallback);

    /// 收到服务端的任意通知
    void notificationReceived(const QString& method, const QJsonObject& params);
    // 协议规范事件：服务端列表变更通知
    void toolsChanged(const std::vector<mcp_qt::McpQtTool>& newTools);
    /// fetchAllToolsAsync() 完成时触发，传递全量工具列表
    void toolsReady(const std::vector<mcp_qt::McpQtTool>& tools);
    void resourcesChanged();
    void promptsChanged();

    // 遥测监控信号
    void toolCalled(const QString& name, const QJsonObject& arguments);
    void toolFinished(const QString& name, const mcp_qt::McpResult& result);
    void progressReported(const QString& toolName, float progress, float total, const QString& message);

    // 重连状态信号
    void reconnecting();
    void reconnected();
    void recoveryFailed(const QString& message);

private:
    explicit McpQtClient(QObject* parent = nullptr);

    
    void doInitializeAsync(const QString& clientName, const QString& clientVersion);
    void doDiscoverAsync();
    void setupTransportCommon(std::shared_ptr<mcp::IMcpTransport> transport);

    bool doInitializeAndWait(const QString& clientName, const QString& clientVersion, int timeoutMs, QString* errorString = nullptr);
    bool doOAuth(const OAuthConfig& oauth);




    std::shared_ptr<mcp::McpClientSession> m_session;
    std::shared_ptr<mcp::McpOAuthClient> m_oauth;
    bool m_initialized{false};
    mutable std::map<QString, McpQtTool> m_toolCache;

    TrafficLogger m_trafficLogger;
    McpResourceSubscriptionRouter m_resourceRouter;

    struct PendingCapability {
        QString name;
        QJsonObject config;
    };
    std::vector<PendingCapability> m_pendingCapabilities;

    // 重连与恢复上下文
    QString m_namespace;
    int m_transportType{0}; // 0=none/test, 1=http, 2=stdio
    QString m_url_or_cmd;
    QStringList m_args;
    QMap<QString, QString> m_env;
    QMap<QString, QString> m_httpHeaders;
    std::optional<QNetworkProxy> m_proxy;
    QString m_clientName{QStringLiteral("mcp-qt-client")};
    QString m_clientVersion{QStringLiteral("1.0.0")};
    QString m_protocolVersion{QStringLiteral("2026-07-28")};
    bool m_statelessMode{false};
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
    void refreshToolsCacheAsync();
    void replayQueuedRequests();
    bool isReplayableMethod(const QString& method) const;

    // 析构时触发所有挂起的 fetchAllToolsAsync 回调，防止调用方永久等待
    void fireAllPendingFetchCallbacks();

    nlohmann::json m_clientCapabilities;

    // 挂起的 fetchAllToolsAsync 回调（析构时强制触发，正常完成时通过 ID 注销）
    mutable std::mutex m_pendingFetchMutex;
    std::unordered_map<uint64_t, std::function<void()>> m_pendingFetchCallbacks;
    std::atomic<uint64_t> m_nextFetchId{1};

    template <typename Initiator>
    bool runSyncWithTimeout(Initiator&& initiator, int timeoutMs);
};

} // namespace mcp_qt


