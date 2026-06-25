#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <atomic>
#include "IMcpTransport.h"
#include "McpMessage.h"
#include "McpTool.h"

namespace mcp {

enum class SessionState {
    Uninitialized,
    Initializing,
    Initialized,
    Shutdown
};

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
     * @brief Send a JSON-RPC notification.
     */
    void sendNotification(const std::string& method, const json& params);

    /**
     * @brief Register a callback for incoming notifications from the server.
     */
    void registerNotificationHandler(const std::string& method, NotificationCallback callback);

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
     * @brief Read a resource content.
     */
    void readResource(const std::string& uri, std::function<void(const json& result, const json& error)> callback);

    /**
     * @brief List the prompts exposed by the MCP server.
     */
    void listPrompts(std::function<void(const json& result, const json& error)> callback);

    /**
     * @brief Get a prompt template.
     */
    void getPrompt(const std::string& name, const json& arguments, std::function<void(const json& result, const json& error)> callback);

    SessionState state() const { return m_state; }

private:
    void handleIncomingMessage(const std::string& rawMessage);
    void handleResponse(const json& responseJson);
    void handleNotification(const json& notificationJson);
    void handleRequestFromServer(const json& requestJson);

    std::shared_ptr<IMcpTransport> m_transport;
    std::mutex m_mutex;
    int64_t m_nextId = 1;
    
    std::unordered_map<int64_t, ResponseCallback> m_pendingRequests;
    std::unordered_map<std::string, NotificationCallback> m_notificationHandlers;
    std::atomic<SessionState> m_state{SessionState::Uninitialized};
};

} // namespace mcp
