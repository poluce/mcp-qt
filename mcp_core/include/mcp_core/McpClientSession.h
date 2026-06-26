#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <atomic>
#include <future>
#include "IMcpTransport.h"
#include "McpMessage.h"
#include "McpTool.h"
#include "McpResource.h"
#include "McpPrompt.h"

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

    using ResponseCallback = std::function<void(const json& result, const json& error)>;
    using NotificationCallback = std::function<void(const json& params)>;
    using RequestCallback = std::function<json(const std::string& method, const json& params)>;

    struct PendingRequest {
        ResponseCallback callback;
        std::chrono::steady_clock::time_point timestamp;
    };

    explicit McpClientSession(std::shared_ptr<IMcpTransport> transport);
    ~McpClientSession();

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
    int64_t sendRequest(const std::string& method, const json& params, ResponseCallback callback);

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
                  std::function<void(const json& content, const json& error)> callback);

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
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

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

    SessionState state() const { return m_state; }

private:
    void handleIncomingMessage(const std::string& rawMessage);
    void handleResponse(const json& responseJson);
    void handleNotification(const json& notificationJson);
    void handleRequestFromServer(const json& requestJson);

    void log(LogLevel level, const std::string& message);

    std::shared_ptr<IMcpTransport> m_transport;
    std::mutex m_mutex;
    int64_t m_nextId = 1;
    
    std::unordered_map<int64_t, PendingRequest> m_pendingRequests;
    std::unordered_map<std::string, NotificationCallback> m_notificationHandlers;
    std::unordered_map<std::string, RequestCallback> m_requestHandlers;
    std::atomic<SessionState> m_state{SessionState::Uninitialized};
    LogCallback m_logCallback;
};

} // namespace mcp
