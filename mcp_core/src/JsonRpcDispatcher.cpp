#include "mcp_core/JsonRpcDispatcher.h"
#include <exception>
#include <fstream>

namespace mcp {

void JsonRpcDispatcher::registerRequestHandler(const std::string& method, RequestHandler handler) {
    m_requestHandlers[method] = handler;
}

void JsonRpcDispatcher::registerNotificationHandler(const std::string& method, NotificationHandler handler) {
    m_notificationHandlers[method] = handler;
}

std::string JsonRpcDispatcher::dispatch(const std::string& rawMessage) {
    json requestJson;
    try {
        requestJson = json::parse(rawMessage);
    } catch (const json::parse_error& e) {
        return createErrorResponse(std::monostate{}, -32700, "Parse error: " + std::string(e.what()));
    }

    if (!requestJson.is_object()) {
        return createErrorResponse(std::monostate{}, -32600, "Invalid Request: Expected JSON object");
    }

    if (!requestJson.contains("jsonrpc") || requestJson["jsonrpc"] != "2.0") {
        return createErrorResponse(std::monostate{}, -32600, "Invalid Request: jsonrpc version must be 2.0");
    }

    RequestId id = std::monostate{};
    if (requestJson.contains("id")) {
        from_json(requestJson["id"], id);
    }

    if (!requestJson.contains("method") || !requestJson["method"].is_string()) {
        if (!std::holds_alternative<std::monostate>(id)) {
            return createErrorResponse(id, -32600, "Invalid Request: method is missing or not a string");
        }
        return ""; 
    }

    std::string method = requestJson["method"].get<std::string>();
    json params = requestJson.contains("params") ? requestJson["params"] : json::object();

    bool isRequest = !std::holds_alternative<std::monostate>(id);

    if (isRequest) {
        auto it = m_requestHandlers.find(method);
        if (it != m_requestHandlers.end()) {
            try {
                json result = it->second(params, id);
                return createSuccessResponse(id, result);
            } catch (const std::exception& e) {
                return createErrorResponse(id, -32000, "Internal error: " + std::string(e.what()));
            }
        } else {
            return createErrorResponse(id, -32601, "Method not found: " + method);
        }
    } else {
        auto it = m_notificationHandlers.find(method);
        if (it != m_notificationHandlers.end()) {
            try {
                it->second(params);
            } catch (...) {
                // Ignore exceptions in notification handlers
            }
        }
        return "";
    }
}

std::string JsonRpcDispatcher::createErrorResponse(const RequestId& id, int code, const std::string& message, const json& data) {
    json response = {
        {"jsonrpc", "2.0"},
        {"error", {
            {"code", code},
            {"message", message}
        }}
    };
    if (!data.is_null()) {
        response["error"]["data"] = data;
    }
    json idJson;
    to_json(idJson, id);
    response["id"] = idJson;
    return response.dump();
}

std::string JsonRpcDispatcher::createSuccessResponse(const RequestId& id, const json& result) {
    json response = {
        {"jsonrpc", "2.0"},
        {"result", result}
    };
    json idJson;
    to_json(idJson, id);
    response["id"] = idJson;
    return response.dump();
}

} // namespace mcp
