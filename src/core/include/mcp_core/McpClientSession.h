#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <atomic>
#include <future>
#include <chrono>
#include <thread>
#include <vector>
#include "IMcpTransport.h"
#include "McpMessage.h"
#include "McpTool.h"
#include "McpResource.h"
#include "McpPrompt.h"
#include "McpTrafficEvent.h"

namespace mcp {

enum class SessionState {
    Uninitialized,
    Initializing,
    Initialized,
    Shutdown
};

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

using LogCallback = std::function<void(LogLevel level, const std::string& message)>;

/**
 * @brief Manages a Model Context Protocol Client Session.
 * 
 * Tracks pending requests, routes incoming server responses/notifications, and provides
 * simplified wrapper methods for standard MCP operations (initialize, listTools, callTool).
 */
class McpClientSession : public std::enable_shared_from_this<McpClientSession> {
public:
    static constexpr auto MCP_PROTOCOL_VERSION = "2025-11-25";

    // 客户端支持的协议版本列表（按优先级排序，最新在前）
    static inline const std::vector<std::string> SUPPORTED_PROTOCOL_VERSIONS = {
        "2026-07-28",
        "2025-11-25",
        "2025-06-18",
        "2025-03-26"
    };

    using ResponseCallback = std::function<void(const json& result, const json& error)>;
    using NotificationCallback = std::function<void(const json& params)>;
    using RequestCallback = std::function<void(const std::string& method, const json& params, std::function<void(const json& result, const json& error)> callback)>;
    using ProgressCallback = std::function<void(const json& progressInfo)>;

    /**
     * @brief Handler for sampling/createMessage requests from the server.
     *        Server asks client to perform LLM inference.
     *        Calls callback with CreateMessageResult: {model, role, content, ...}
     */
    using SamplingHandler = std::function<void(const json& params, std::function<void(const json& result, const json& error)> callback)>;

    /**
     * @brief Handler for elicitation/create requests from the server.
     *        Server asks client to collect user input.
     *        Calls callback with {action, content} or {action:"declined"} or {action:"cancelled"}
     */
    using ElicitationHandler = std::function<void(const json& params, std::function<void(const json& result, const json& error)> callback)>;

    /**
     * @brief Provider for roots/list requests from the server.
     *        Calls callback with an array of root objects: [{uri, name}, ...]
     */
    using RootsProvider = std::function<void(std::function<void(const json& result, const json& error)> callback)>;

    /**
     * @brief Handler for MRTR (Multi Round-Trip Requests) when server responds with input_required status.
     *        Calls callback with user inputs json to resume the stateless request.
     */
    using MrtrInputHandler = std::function<void(const json& inputSchema, const json& requestParams, std::function<void(const json& userInputs)> callback)>;

    struct PendingRequest {
        std::string method;
        json params;
        ResponseCallback callback;
        std::chrono::steady_clock::time_point timestamp;
    };

    explicit McpClientSession(std::shared_ptr<IMcpTransport> transport);
    ~McpClientSession();

    /**
     * @brief Factory: create session, init handlers, and start transport in one call.
     *
     * Equivalent to:
     *   auto session = std::make_shared<McpClientSession>(transport);
     *   session->init();
     *   session->start();
     *
     * After connect(), call initializeSync() to complete the handshake.
     */
    static std::shared_ptr<McpClientSession> connect(std::shared_ptr<IMcpTransport> transport);

    /**
     * @brief Bind handlers to the transport. Must be called after creation.
     */
    void init();

    /**
     * @brief Start transport communication.
     */
    bool start();

    /**
     * @brief Close the session and transport.
     */
    void close();

    /**
     * @brief Send a JSON-RPC request asynchronously.
     * @return The request ID.
     */
    int64_t sendRequest(const std::string& method, const json& params, ResponseCallback callback, ProgressCallback progressCallback = nullptr);

    /**
     * @brief Active cancellation of a pending request by ID.
     */
    void cancelRequest(int64_t requestId);

    /**
     * @brief Scan and clean up pending requests that have timed out.
     */
    void checkRequestTimeouts(std::chrono::milliseconds timeoutLimit = std::chrono::milliseconds(5000));

    /**
     * @brief Send a JSON-RPC notification.
     */
    void sendNotification(const std::string& method, const json& params);

    /**
     * @brief Register a callback for incoming notifications from the server.
     */
    void registerNotificationHandler(const std::string& method, NotificationCallback callback);

    /**
     * @brief Register a handler for incoming JSON-RPC requests from the server.
     * The handler should return the result object for the response.
     * If no handler is registered, the session auto-replies with -32601 (Method not found).
     */
    void registerRequestHandler(const std::string& method, RequestCallback callback);

    /**
     * @brief Perform the standard MCP initialization handshake.
     */
    void initialize(const std::string& clientName, const std::string& clientVersion,
                    std::function<void(bool success, const json& serverInfo)> callback);

    /**
     * @brief Safely shutdown the session.
     */
    void shutdown(std::function<void(bool success)> callback);

    /**
     * @brief List the tools exposed by the MCP server.
     */
    void listTools(std::function<void(const std::vector<McpTool>& tools, const json& error)> callback);

    /**
     * @brief List the tools exposed by the MCP server with pagination cursor.
     */
    void listTools(const std::string& cursor, std::function<void(const std::vector<McpTool>& tools, const std::string& nextCursor, const json& error)> callback);

    /**
     * @brief Execute/call a specific tool on the MCP server.
     */
    void callTool(const std::string& name, const json& arguments,
                  std::function<void(const json& content, const json& error)> callback,
                  ProgressCallback progressCallback = nullptr);

    /**
     * @brief List the resources exposed by the MCP server.
     */
    void listResources(std::function<void(const json& result, const json& error)> callback);

    /**
     * @brief List the resources exposed by the MCP server with pagination cursor.
     */
    void listResources(const std::string& cursor, std::function<void(const json& result, const std::string& nextCursor, const json& error)> callback);

    /**
     * @brief Read a resource content.
     */
    void readResource(const std::string& uri, std::function<void(const json& result, const json& error)> callback);

    /**
     * @brief Subscribe to a resource.
     */
    void subscribeResource(const std::string& uri, std::function<void(bool success, const json& error)> callback);

    /**
     * @brief Unsubscribe from a resource.
     */
    void unsubscribeResource(const std::string& uri, std::function<void(bool success, const json& error)> callback);

    /**
     * @brief List the prompts exposed by the MCP server.
     */
    void listPrompts(std::function<void(const json& result, const json& error)> callback);

    /**
     * @brief List the prompts exposed by the MCP server with pagination cursor.
     */
    void listPrompts(const std::string& cursor, std::function<void(const json& result, const std::string& nextCursor, const json& error)> callback);

    /**
     * @brief Get a prompt template.
     */
    void getPrompt(const std::string& name, const json& arguments, std::function<void(const json& result, const json& error)> callback);

    /**
     * @brief Send a ping request to the server to check connectivity.
     */
    void ping(std::function<void(bool success, const json& error)> callback);

    /**
     * @brief List resource templates exposed by the MCP server.
     */
    void listResourceTemplates(std::function<void(const std::vector<McpResourceTemplate>& templates, const json& error)> callback);

    /**
     * @brief List resource templates with pagination cursor.
     */
    void listResourceTemplates(const std::string& cursor, std::function<void(const std::vector<McpResourceTemplate>& templates, const std::string& nextCursor, const json& error)> callback);

    /**
     * @brief Request auto-completion suggestions from the server.
     * @param ref Reference to the resource template or prompt being completed.
     * @param argument The argument name and partial value for completion.
     */
    void complete(const json& ref, const json& argument, std::function<void(const json& completion, const json& error)> callback);

    // ==========================================
    // Sampling (双向: 服务端请求客户端推理)
    // ==========================================

    /**
     * @brief Register a handler for sampling/createMessage requests.
     *        When the server sends a sampling/createMessage request, the handler is called
     *        to perform LLM inference and return the result.
     */
    void setSamplingHandler(SamplingHandler handler);

    // ==========================================
    // Elicitation (双向: 服务端请求用户输入)
    // ==========================================

    /**
     * @brief Register a handler for elicitation/create requests.
     *        When the server sends an elicitation/create request, the handler is called
     *        to collect user input via form or URL.
     */
    void setElicitationHandler(ElicitationHandler handler);

    // ==========================================
    // Roots (双向: 客户端暴露文件系统根目录)
    // ==========================================

    /**
     * @brief Register a provider for roots/list requests.
     *        When the server requests roots/list, the provider returns the root array.
     * 
     *        Note: The session inherently registers a default handler for roots/list at initialization.
     *        If no provider is set via this method, the default handler responds with an empty roots array `{"roots": []}`
     *        to prevent "-32601 Method not found" errors during server handshake. Calling this method
     *        allows you to supply the actual workspace roots.
     */
    void setRootsProvider(RootsProvider provider);

    /**
     * @brief Register a handler for MRTR (Multi Round-Trip Requests) status: input_required.
     *        When the server in stateless mode requires additional user input, the handler is triggered.
     */
    void setMrtrHandler(MrtrInputHandler handler);

    /**
     * @brief Notify the server that the roots list has changed.
     *        Sends notifications/roots/list_changed.
     */
    void notifyRootsListChanged();

    // ==========================================
    // Notification Debounce (通知去重/合并)
    // ==========================================

    /**
     * @brief Enable debouncing for a notification method.
     *        Rapid consecutive notifications of the same method will be merged:
     *        only the last one within the debounce window is actually sent.
     * @param method The notification method to debounce.
     * @param debounceWindow The time window for merging notifications.
     */
    void enableNotificationDebounce(const std::string& method,
                                    std::chrono::milliseconds debounceWindow = std::chrono::milliseconds(100));

    /**
     * @brief Send a notification with automatic debounce if configured.
     *        If the method has debounce enabled, rapid calls are merged.
     */
    void sendNotificationDebounced(const std::string& method, const json& params);

    // ==========================================
    // Synchronous Blocking APIs (Helper wrappers)
    // ==========================================
    
    bool initializeSync(const std::string& clientName, const std::string& clientVersion,
                        json* serverInfoOut = nullptr,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    bool shutdownSync(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    std::vector<McpTool> listToolsSync(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000), json* errorOut = nullptr);
    
    std::vector<McpTool> listToolsSync(const std::string& cursor, std::string* nextCursorOut,
                                       std::chrono::milliseconds timeout = std::chrono::milliseconds(5000), json* errorOut = nullptr);

    json callToolSync(const std::string& name, const json& arguments,
                      json* errorOut = nullptr,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
                      ProgressCallback progressCallback = nullptr);

    json listResourcesSync(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000), json* errorOut = nullptr);
    
    json listResourcesSync(const std::string& cursor, std::string* nextCursorOut,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(5000), json* errorOut = nullptr);

    json readResourceSync(const std::string& uri, json* errorOut = nullptr, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    bool subscribeResourceSync(const std::string& uri, json* errorOut = nullptr, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    bool unsubscribeResourceSync(const std::string& uri, json* errorOut = nullptr, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    json listPromptsSync(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000), json* errorOut = nullptr);
    
    json listPromptsSync(const std::string& cursor, std::string* nextCursorOut,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds(5000), json* errorOut = nullptr);

    json getPromptSync(const std::string& name, const json& arguments,
                       json* errorOut = nullptr,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    bool pingSync(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000), json* errorOut = nullptr);

    std::vector<McpResourceTemplate> listResourceTemplatesSync(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000), json* errorOut = nullptr);

    std::vector<McpResourceTemplate> listResourceTemplatesSync(const std::string& cursor, std::string* nextCursorOut,
                                                               std::chrono::milliseconds timeout = std::chrono::milliseconds(5000), json* errorOut = nullptr);

    json completeSync(const json& ref, const json& argument,
                      json* errorOut = nullptr,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    // ==========================================
    // Raw String APIs (Uncoupled from nlohmann/json)
    // ==========================================
    using RawResponseCallback = std::function<void(const std::string& resultJson, const std::string& errorJson)>;
    
    int64_t sendRequestRaw(const std::string& method, const std::string& paramsJson, RawResponseCallback callback);
    
    void callToolRaw(const std::string& name, const std::string& argumentsJson,
                     std::function<void(const std::string& contentJson, const std::string& errorJson)> callback);
                     
    std::string callToolSyncRaw(const std::string& name, const std::string& argumentsJson,
                                std::string* errorJsonOut = nullptr,
                                std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    void setLogCallback(LogCallback callback);

    using ErrorCallback = std::function<void(const std::string& error)>;
    void setOnError(ErrorCallback callback);

    using CloseCallback = std::function<void()>;
    void setOnClose(CloseCallback callback);

    using GenericNotificationCallback = std::function<void(const std::string& method, const json& params)>;
    void setNotificationCallback(GenericNotificationCallback callback);

    using TrafficCallback = std::function<void(const McpTrafficEvent&)>;
    void setTrafficCallback(TrafficCallback callback);

    /**
     * @brief Override the default protocol version for the next initialize call.
     *        When set, this value is used as the protocolVersion in the initialize request
     *        instead of MCP_PROTOCOL_VERSION. Pass an empty string to reset to default.
     */
    void setProtocolVersion(const std::string& version);

    /**
     * @brief Enable or disable stateless mode (MCP 2026-07-28 core capability).
     *        In stateless mode, requests carry self-contained _meta headers and do not
     *        require explicit initialize handshake.
     */
    void setStatelessMode(bool enabled);
    bool isStatelessMode() const;
    bool isReady() const;

    void registerCapabilities(const json& capabilities);
    std::string getNegotiatedProtocolVersion() const;
    json getServerCapabilities() const;
    json getServerVersion() const;
    std::string getInstructions() const;

    SessionState state() const { return m_state; }
    int64_t getLastRequestId() const { return m_lastRequestId.load(); }

private:
    void handleIncomingMessage(const std::string& rawMessage);
    void handleResponse(const json& responseJson);
    void handleNotification(const json& notificationJson);
    void handleRequestFromServer(const json& requestJson);
    void resendMrtrRequest(const std::string& method, json params, const json& userInputs, ResponseCallback callback);
    void injectStatelessMeta(json& params);

    void log(LogLevel level, const std::string& message);
    void emitTrafficEvent(McpTrafficDirection dir, McpTrafficKind kind, const json& payload, const std::string& raw);

    std::shared_ptr<IMcpTransport> m_transport;
    mutable std::mutex m_mutex;
    int64_t m_nextId = 1;
    std::atomic<int64_t> m_lastRequestId{0};

    std::unordered_map<int64_t, PendingRequest> m_pendingRequests;
    std::unordered_map<int64_t, ProgressCallback> m_progressHandlers;
    std::unordered_map<std::string, NotificationCallback> m_notificationHandlers;
    std::unordered_map<std::string, RequestCallback> m_requestHandlers;
    std::atomic<SessionState> m_state{SessionState::Uninitialized};
    LogCallback m_logCallback;
    ErrorCallback m_errorCallback;
    CloseCallback m_onCloseCallback;
    GenericNotificationCallback m_genericNotificationCallback;
    TrafficCallback m_trafficCallback;

    // 双向能力处理器
    SamplingHandler m_samplingHandler;
    ElicitationHandler m_elicitationHandler;
    RootsProvider m_rootsProvider;
    MrtrInputHandler m_mrtrHandler;

    // 通知去重状态
    struct DebounceState {
        std::chrono::milliseconds window{100};
        std::string lastParamsJson;
        std::thread timerThread;
        std::mutex timerMutex;
        bool timerActive = false;
    };
    std::unordered_map<std::string, DebounceState> m_debounceStates;
    mutable std::mutex m_debounceMutex;

    json m_capabilities = {
        {"roots", {{"listChanged", false}}},
        {"sampling", json::object()},
        {"elicitation", {{"modes", {"form", "url"}}}}
    };
    bool m_statelessMode{false};
    std::string m_clientName{"mcp-qt-client"};
    std::string m_clientVersion{"1.0.0"};
    std::string m_negotiatedProtocolVersion;
    std::string m_overrideProtocolVersion;
    json m_serverCapabilities;
    json m_serverVersion;
    std::string m_instructions;
};

} // namespace mcp
