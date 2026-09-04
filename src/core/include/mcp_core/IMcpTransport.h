#pragma once
#include <string>
#include <map>
#include <functional>

namespace mcp {

/**
 * @brief Interface representing the transport layer for Model Context Protocol.
 * 
 * Enables sending and receiving raw string messages via different protocols (Stdio, HTTP, WebSockets).
 */
class IMcpTransport {
public:
    virtual ~IMcpTransport() = default;

    /**
     * @brief Set the negotiated protocol version (useful for HTTP SSE transport metadata).
     */
    virtual void setProtocolVersion(const std::string& version) {}

    /**
     * @brief Set extra per-request HTTP headers to attach to the next send(s).
     *
     * Used by the 2026-07-28 `x-mcp-header` feature (SEP-2243): the session layer
     * extracts tool parameters annotated with `x-mcp-header` and delivers them as
     * `Mcp-Param-{Name}` headers here. Transports without HTTP semantics may ignore.
     */
    virtual void setExtraRequestHeaders(const std::map<std::string, std::string>& headers) {}

    /**
     * @brief Send a raw text message over the transport.
     * @param message The serialized JSON-RPC message.
     * @return true if successfully sent, false otherwise.
     */
    virtual bool send(const std::string& message) = 0;

    /**
     * @brief Register a callback for incoming messages.
     * @param callback Function to call when a message is received.
     */
    virtual void setOnMessage(std::function<void(const std::string&)> callback) = 0;

    /**
     * @brief Register a callback for when the transport is closed.
     * @param callback Function to call on closure.
     */
    virtual void setOnClose(std::function<void()> callback) = 0;

    /**
     * @brief Register a callback for errors.
     * @param callback Function to call with error details.
     */
    virtual void setOnError(std::function<void(const std::string&)> callback) = 0;

    /**
     * @brief Start the transport listener/processor.
     * @return true if successfully started, false otherwise.
     */
    virtual bool start() = 0;

    /**
     * @brief Close the transport channel.
     */
    virtual void close() = 0;
};

} // namespace mcp
