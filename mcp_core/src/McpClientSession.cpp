#include "mcp_core/McpClientSession.h"
#include <iostream>
#include <fstream>
#include <memory>

namespace mcp {

McpClientSession::McpClientSession(std::shared_ptr<IMcpTransport> transport)
    : m_transport(std::move(transport)) {}

McpClientSession::~McpClientSession() {
    close();
}

std::shared_ptr<McpClientSession> McpClientSession::connect(std::shared_ptr<IMcpTransport> transport) {
    auto session = std::make_shared<McpClientSession>(std::move(transport));
    session->init();
    session->start();
    return session;
}

void McpClientSession::init() {
    std::weak_ptr<McpClientSession> weakSelf = shared_from_this();
    m_transport->setOnMessage([weakSelf](const std::string& msg) {
        if (auto self = weakSelf.lock()) {
            self->handleIncomingMessage(msg);
        }
    });

    m_transport->setOnClose([weakSelf]() {
        if (auto self = weakSelf.lock()) {
            self->log(LogLevel::Warning, "Transport connection closed. Releasing all pending requests.");
            self->m_state = SessionState::Shutdown;
            
            std::vector<ResponseCallback> callbacks;
            {
                std::lock_guard<std::mutex> lock(self->m_mutex);
                for (auto& pair : self->m_pendingRequests) {
                    callbacks.push_back(std::move(pair.second.callback));
                }
                self->m_pendingRequests.clear();
            }

            for (auto& cb : callbacks) {
                if (cb) {
                    json connErr = {
                        {"code", -32603},
                        {"message", "Connection interrupted or server crashed"}
                    };
                    cb(json::object(), connErr);
                }
            }
            if (self->m_onCloseCallback) {
                self->m_onCloseCallback();
            }
        }
    });

    m_transport->setOnError([weakSelf](const std::string& err) {
        if (auto self = weakSelf.lock()) {
            self->log(LogLevel::Error, "Transport error: " + err);
            ErrorCallback cb;
            {
                std::lock_guard<std::mutex> lock(self->m_mutex);
                cb = self->m_errorCallback;
            }
            if (cb) {
                cb(err);
            }
        }
    });
}

bool McpClientSession::start() {
    return m_transport->start();
}

void McpClientSession::close() {
    if (m_transport) {
        m_transport->close();
    }
}

int64_t McpClientSession::sendRequest(const std::string& method, const json& params, ResponseCallback callback, ProgressCallback progressCallback) {
    int64_t id;
    bool hasProgress = (progressCallback != nullptr);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        id = m_nextId++;
        m_pendingRequests[id] = PendingRequest{
            std::move(callback),
            std::chrono::steady_clock::now()
        };
        if (hasProgress) {
            m_progressHandlers[id] = std::move(progressCallback);
        }
    }

    json requestMsg = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method},
        {"params", params}
    };

    if (hasProgress) {
        if (!requestMsg["params"].is_object()) {
            requestMsg["params"] = json::object();
        }
        requestMsg["params"]["_meta"]["progressToken"] = id;
    }

    log(LogLevel::Debug, "sendRequest: method=" + method + ", id=" + std::to_string(id));
    std::string dumpStr = requestMsg.dump();
    if (m_trafficCallback) {
        McpTrafficEvent event;
        event.direction = McpTrafficDirection::Outbound;
        event.kind = McpTrafficKind::Request;
        event.payload = requestMsg;
        event.raw = dumpStr;
        m_trafficCallback(event);
    }
    m_transport->send(dumpStr);
    return id;
}

void McpClientSession::sendNotification(const std::string& method, const json& params) {
    json notificationMsg = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params}
    };
    std::string dumpStr = notificationMsg.dump();
    if (m_trafficCallback) {
        McpTrafficEvent event;
        event.direction = McpTrafficDirection::Outbound;
        event.kind = McpTrafficKind::Notification;
        event.payload = notificationMsg;
        event.raw = dumpStr;
        m_trafficCallback(event);
    }
    m_transport->send(dumpStr);
}

void McpClientSession::registerNotificationHandler(const std::string& method, NotificationCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_notificationHandlers[method] = callback;
}

void McpClientSession::registerRequestHandler(const std::string& method, RequestCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_requestHandlers[method] = callback;
}

void McpClientSession::handleIncomingMessage(const std::string& rawMessage) {
    log(LogLevel::Debug, "handleIncomingMessage: " + rawMessage);
    json j;
    try {
        j = json::parse(rawMessage);
    } catch (...) {
        log(LogLevel::Error, "JSON parsing failed on incoming message: " + rawMessage);
        return; 
    }

    if (!j.is_object()) {
        log(LogLevel::Warning, "Incoming message is not a JSON object: " + rawMessage);
        return;
    }

    if (m_trafficCallback) {
        McpTrafficKind kind = McpTrafficKind::Unknown;
        if (j.contains("id")) {
            if (j.contains("result") || j.contains("error")) {
                kind = McpTrafficKind::Response;
            } else if (j.contains("method")) {
                kind = McpTrafficKind::Request;
            }
        } else if (j.contains("method")) {
            kind = McpTrafficKind::Notification;
        }

        McpTrafficEvent event;
        event.direction = McpTrafficDirection::Inbound;
        event.kind = kind;
        event.payload = j;
        event.raw = rawMessage;
        m_trafficCallback(event);
    }

    if (j.contains("id")) {
        if (j.contains("result") || j.contains("error")) {
            handleResponse(j);
        } else if (j.contains("method")) {
            handleRequestFromServer(j);
        }
    } else if (j.contains("method")) {
        handleNotification(j);
    }
}

void McpClientSession::handleResponse(const json& responseJson) {
    int64_t id = 0;
    if (responseJson["id"].is_number_integer()) {
        id = responseJson["id"].get<int64_t>();
    } else if (responseJson["id"].is_string()) {
        try {
            id = std::stoll(responseJson["id"].get<std::string>());
        } catch (...) {
            return; 
        }
    } else {
        return; 
    }

    ResponseCallback cb;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pendingRequests.find(id);
        if (it != m_pendingRequests.end()) {
            cb = std::move(it->second.callback);
            m_pendingRequests.erase(it);
            found = true;
        }
        m_progressHandlers.erase(id);
    }

    if (found) {
        log(LogLevel::Info, "Processing response for id=" + std::to_string(id));
    } else {
        log(LogLevel::Warning, "Received response for unregistered id=" + std::to_string(id));
    }

    if (cb) {
        json result = responseJson.contains("result") ? responseJson["result"] : json::object();
        json error = responseJson.contains("error") ? responseJson["error"] : json::object();
        cb(result, error);
    }
}

void McpClientSession::handleNotification(const json& notificationJson) {
    std::string method = notificationJson["method"].get<std::string>();
    json params = notificationJson.contains("params") ? notificationJson["params"] : json::object();

    if (method == "notifications/progress") {
        int64_t progressTokenId = 0;
        if (params.contains("progressToken")) {
            auto& token = params["progressToken"];
            if (token.is_number_integer()) {
                progressTokenId = token.get<int64_t>();
            } else if (token.is_string()) {
                try {
                    progressTokenId = std::stoll(token.get<std::string>());
                } catch (...) {
                    // Ignore parsing error
                }
            }
        }

        if (progressTokenId != 0) {
            ProgressCallback progressCb;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_progressHandlers.find(progressTokenId);
                if (it != m_progressHandlers.end()) {
                    progressCb = it->second;
                }
            }
            if (progressCb) {
                progressCb(params);
            }
        }
    }

    NotificationCallback cb;
    GenericNotificationCallback genCb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_notificationHandlers.find(method);
        if (it != m_notificationHandlers.end()) {
            cb = it->second;
        }
        genCb = m_genericNotificationCallback;
    }

    if (cb) {
        cb(params);
    }
    if (genCb) {
        genCb(method, params);
    }
}

void McpClientSession::handleRequestFromServer(const json& requestJson) {
    int64_t id = requestJson["id"].get<int64_t>();
    std::string method = requestJson["method"].get<std::string>();
    json params = requestJson.contains("params") ? requestJson["params"] : json::object();

    // Check for a registered handler first
    RequestCallback handler;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_requestHandlers.find(method);
        if (it != m_requestHandlers.end()) {
            handler = it->second;
        }
    }

    if (handler) {
        std::weak_ptr<McpClientSession> weakSelf = shared_from_this();
        handler(method, params, [weakSelf, id](const json& result, const json& error) {
            if (auto self = weakSelf.lock()) {
                json response = {
                    {"jsonrpc", "2.0"},
                    {"id", id}
                };
                if (!error.empty()) {
                    response["error"] = error;
                } else {
                    response["result"] = result;
                }
                self->m_transport->send(response.dump());
            }
        });
        return;
    }

    // Default: return Method not found
    json errorResponse = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {
            {"code", -32601},
            {"message", "Method not found: " + method}
        }}
    };
    m_transport->send(errorResponse.dump());
}

void McpClientSession::initialize(const std::string& clientName, const std::string& clientVersion,
                                  std::function<void(bool success, const json& serverInfo)> callback) {
    SessionState expected = SessionState::Uninitialized;
    if (!m_state.compare_exchange_strong(expected, SessionState::Initializing)) {
        json err = {
            {"code", -32600},
            {"message", "Initialize already in progress or completed"}
        };
        callback(false, err);
        return;
    }

    json params = {
        {"protocolVersion", m_overrideProtocolVersion.empty() ? MCP_PROTOCOL_VERSION : m_overrideProtocolVersion},
        {"capabilities", m_capabilities},
        {"clientInfo", {
            {"name", clientName},
            {"version", clientVersion}
        }}
    };

    auto self = shared_from_this();
    sendRequest("initialize", params, [self, callback](const json& result, const json& error) {
        if (!error.empty()) {
            self->m_state = SessionState::Uninitialized; 
            callback(false, error);
        } else {
            std::string serverVer;
            if (result.contains("protocolVersion") && result["protocolVersion"].is_string()) {
                serverVer = result["protocolVersion"].get<std::string>();
            }

            // 检查服务端版本是否在客户端支持列表中
            bool versionSupported = false;
            for (const auto& ver : SUPPORTED_PROTOCOL_VERSIONS) {
                if (serverVer == ver) {
                    versionSupported = true;
                    break;
                }
            }

            if (!versionSupported) {
                self->m_state = SessionState::Uninitialized;
                json verErr = {
                    {"code", -32002},
                    {"message", "Version Mismatch: Server returned unsupported version " + serverVer}
                };
                callback(false, verErr);
                return;
            }

            {
                std::lock_guard<std::mutex> lock(self->m_mutex);
                if (result.contains("protocolVersion") && result["protocolVersion"].is_string()) {
                    self->m_negotiatedProtocolVersion = result["protocolVersion"].get<std::string>();
                } else {
                    self->m_negotiatedProtocolVersion = MCP_PROTOCOL_VERSION;
                }

                if (result.contains("capabilities") && result["capabilities"].is_object()) {
                    self->m_serverCapabilities = result["capabilities"];
                } else {
                    self->m_serverCapabilities = json::object();
                }

                if (result.contains("serverInfo") && result["serverInfo"].is_object()) {
                    self->m_serverVersion = result["serverInfo"];
                } else {
                    self->m_serverVersion = json::object();
                }

                if (result.contains("instructions") && result["instructions"].is_string()) {
                    self->m_instructions = result["instructions"].get<std::string>();
                } else {
                    self->m_instructions = "";
                }
            }

            if (self->m_transport) {
                self->m_transport->setProtocolVersion(self->m_negotiatedProtocolVersion);
            }

            self->m_state = SessionState::Initialized;
            self->sendNotification("notifications/initialized", json::object());
            callback(true, result);
        }
    });
}

void McpClientSession::shutdown(std::function<void(bool success)> callback) {
    if (m_state != SessionState::Initialized) {
        callback(false);
        return;
    }
    auto self = shared_from_this();
    sendRequest("shutdown", json::object(), [self, callback](const json& result, const json& error) {
        if (!error.empty()) {
            callback(false);
        } else {
            self->m_state = SessionState::Shutdown;
            callback(true);
        }
    });
}

void McpClientSession::listTools(std::function<void(const std::vector<McpTool>& tools, const json& error)> callback) {
    if (m_state != SessionState::Initialized) {
        json err = {
            {"code", -32002},
            {"message", "Session not initialized"}
        };
        callback({}, err);
        return;
    }
    sendRequest("tools/list", json::object(), [callback](const json& result, const json& error) {
        if (!error.empty()) {
            callback({}, error);
        } else {
            std::vector<McpTool> toolsList;
            bool parseOk = true;
            if (result.contains("tools") && result["tools"].is_array()) {
                for (const auto& item : result["tools"]) {
                    try {
                        toolsList.push_back(item.get<McpTool>());
                    } catch (...) {
                        parseOk = false;
                    }
                }
            }
            if (!parseOk) {
                toolsList.clear();
            }
            callback(toolsList, json::object());
        }
    });
}

void McpClientSession::listTools(const std::string& cursor, std::function<void(const std::vector<McpTool>& tools, const std::string& nextCursor, const json& error)> callback) {
    if (m_state != SessionState::Initialized) {
        json err = {
            {"code", -32002},
            {"message", "Session not initialized"}
        };
        callback({}, "", err);
        return;
    }
    json params = json::object();
    if (!cursor.empty()) {
        params["cursor"] = cursor;
    }
    sendRequest("tools/list", params, [callback](const json& result, const json& error) {
        if (!error.empty()) {
            callback({}, "", error);
        } else {
            std::vector<McpTool> toolsList;
            std::string nextCursor;
            bool parseOk = true;
            if (result.contains("tools") && result["tools"].is_array()) {
                for (const auto& item : result["tools"]) {
                    try {
                        toolsList.push_back(item.get<McpTool>());
                    } catch (...) {
                        parseOk = false;
                    }
                }
            }
            if (!parseOk) {
                toolsList.clear();
            }
            if (result.contains("nextCursor") && result["nextCursor"].is_string()) {
                nextCursor = result["nextCursor"].get<std::string>();
            }
            callback(toolsList, nextCursor, json::object());
        }
    });
}

void McpClientSession::callTool(const std::string& name, const json& arguments,
                                std::function<void(const json& content, const json& error)> callback,
                                ProgressCallback progressCallback) {
    if (m_state != SessionState::Initialized) {
        json err = {
            {"code", -32002},
            {"message", "Session not initialized"}
        };
        callback(json::object(), err);
        return;
    }
    json params = {
        {"name", name},
        {"arguments", arguments}
    };

    sendRequest("tools/call", params, [callback](const json& result, const json& error) {
        if (!error.empty()) {
            callback(json::object(), error);
        } else {
            callback(result, json::object());
        }
    }, std::move(progressCallback));
}

void McpClientSession::listResources(std::function<void(const json& result, const json& error)> callback) {
    listResources("", [callback](const json& result, const std::string&, const json& error) {
        callback(result, error);
    });
}

void McpClientSession::listResources(const std::string& cursor, std::function<void(const json& result, const std::string& nextCursor, const json& error)> callback) {
    if (m_state != SessionState::Initialized) {
        json err = {
            {"code", -32002},
            {"message", "Session not initialized"}
        };
        callback(json::object(), "", err);
        return;
    }
    json params = json::object();
    if (!cursor.empty()) {
        params["cursor"] = cursor;
    }
    sendRequest("resources/list", params, [callback](const json& result, const json& error) {
        if (!error.empty()) {
            callback(json::object(), "", error);
        } else {
            std::string nextCursor;
            if (result.contains("nextCursor") && result["nextCursor"].is_string()) {
                nextCursor = result["nextCursor"].get<std::string>();
            }
            callback(result, nextCursor, json::object());
        }
    });
}

void McpClientSession::readResource(const std::string& uri, std::function<void(const json& result, const json& error)> callback) {
    if (m_state != SessionState::Initialized) {
        json err = {
            {"code", -32002},
            {"message", "Session not initialized"}
        };
        callback(json::object(), err);
        return;
    }
    json params = {
        {"uri", uri}
    };
    sendRequest("resources/read", params, [callback](const json& result, const json& error) {
        callback(result, error);
    });
}

void McpClientSession::subscribeResource(const std::string& uri, std::function<void(bool success, const json& error)> callback) {
    if (m_state != SessionState::Initialized) {
        json err = {
            {"code", -32002},
            {"message", "Session not initialized"}
        };
        callback(false, err);
        return;
    }
    json params = {
        {"uri", uri}
    };
    sendRequest("resources/subscribe", params, [callback](const json&, const json& error) {
        if (!error.empty()) {
            callback(false, error);
        } else {
            callback(true, json::object());
        }
    });
}

void McpClientSession::unsubscribeResource(const std::string& uri, std::function<void(bool success, const json& error)> callback) {
    if (m_state != SessionState::Initialized) {
        json err = {
            {"code", -32002},
            {"message", "Session not initialized"}
        };
        callback(false, err);
        return;
    }
    json params = {
        {"uri", uri}
    };
    sendRequest("resources/unsubscribe", params, [callback](const json&, const json& error) {
        if (!error.empty()) {
            callback(false, error);
        } else {
            callback(true, json::object());
        }
    });
}

void McpClientSession::listPrompts(std::function<void(const json& result, const json& error)> callback) {
    listPrompts("", [callback](const json& result, const std::string&, const json& error) {
        callback(result, error);
    });
}

void McpClientSession::listPrompts(const std::string& cursor, std::function<void(const json& result, const std::string& nextCursor, const json& error)> callback) {
    if (m_state != SessionState::Initialized) {
        json err = {
            {"code", -32002},
            {"message", "Session not initialized"}
        };
        callback(json::object(), "", err);
        return;
    }
    json params = json::object();
    if (!cursor.empty()) {
        params["cursor"] = cursor;
    }
    sendRequest("prompts/list", params, [callback](const json& result, const json& error) {
        if (!error.empty()) {
            callback(json::object(), "", error);
        } else {
            std::string nextCursor;
            if (result.contains("nextCursor") && result["nextCursor"].is_string()) {
                nextCursor = result["nextCursor"].get<std::string>();
            }
            callback(result, nextCursor, json::object());
        }
    });
}

void McpClientSession::getPrompt(const std::string& name, const json& arguments, std::function<void(const json& result, const json& error)> callback) {
    if (m_state != SessionState::Initialized) {
        json err = {
            {"code", -32002},
            {"message", "Session not initialized"}
        };
        callback(json::object(), err);
        return;
    }
    json params = {
        {"name", name},
        {"arguments", arguments}
    };
    sendRequest("prompts/get", params, [callback](const json& result, const json& error) {
        callback(result, error);
    });
}

void McpClientSession::cancelRequest(int64_t requestId) {
    log(LogLevel::Info, "Request cancelled locally: id=" + std::to_string(requestId));
    ResponseCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pendingRequests.find(requestId);
        if (it != m_pendingRequests.end()) {
            cb = std::move(it->second.callback);
            m_pendingRequests.erase(it);
        }
        m_progressHandlers.erase(requestId);
    }
    if (cb) {
        json cancelErr = {
            {"code", -32000},
            {"message", "Request cancelled locally"}
        };
        cb(json::object(), cancelErr);
    }

    json params = {
        {"requestId", requestId}
    };
    sendNotification("notifications/cancelled", params);
}

void McpClientSession::checkRequestTimeouts(std::chrono::milliseconds timeoutLimit) {
    std::vector<std::pair<int64_t, ResponseCallback>> expiredRequests;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto now = std::chrono::steady_clock::now();
        for (auto it = m_pendingRequests.begin(); it != m_pendingRequests.end(); ) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.timestamp);
            if (elapsed >= timeoutLimit) {
                expiredRequests.push_back({it->first, std::move(it->second.callback)});
                m_progressHandlers.erase(it->first);
                it = m_pendingRequests.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto& pair : expiredRequests) {
        log(LogLevel::Warning, "Request timed out: id=" + std::to_string(pair.first));
        if (pair.second) {
            json timeoutErr = {
                {"code", -32001},
                {"message", "Request timeout"}
            };
            pair.second(json::object(), timeoutErr);
        }
    }
}

bool McpClientSession::initializeSync(const std::string& clientName, const std::string& clientVersion,
                                      json* serverInfoOut, std::chrono::milliseconds timeout) {
    auto pr = std::make_shared<std::promise<std::pair<bool, json>>>();
    auto fut = pr->get_future();
    initialize(clientName, clientVersion, [pr](bool success, const json& info) {
        pr->set_value({success, info});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (serverInfoOut) *serverInfoOut = res.second;
        return res.first;
    }
    if (serverInfoOut) {
        *serverInfoOut = {{"code", -32001}, {"message", "Synchronous initialize timed out"}};
    }
    return false;
}

bool McpClientSession::shutdownSync(std::chrono::milliseconds timeout) {
    auto pr = std::make_shared<std::promise<bool>>();
    auto fut = pr->get_future();
    shutdown([pr](bool success) {
        pr->set_value(success);
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        return fut.get();
    }
    return false;
}

std::vector<McpTool> McpClientSession::listToolsSync(std::chrono::milliseconds timeout, json* errorOut) {
    auto pr = std::make_shared<std::promise<std::pair<std::vector<McpTool>, json>>>();
    auto fut = pr->get_future();
    listTools([pr](const std::vector<McpTool>& tools, const json& error) {
        pr->set_value({tools, error});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (errorOut) *errorOut = res.second;
        return res.first;
    }
    if (errorOut) *errorOut = {{"code", -32001}, {"message", "Synchronous listTools timed out"}};
    return {};
}

std::vector<McpTool> McpClientSession::listToolsSync(const std::string& cursor, std::string* nextCursorOut,
                                                     std::chrono::milliseconds timeout, json* errorOut) {
    struct ListToolsResult {
        std::vector<McpTool> tools;
        std::string nextCursor;
        json error;
    };
    auto pr = std::make_shared<std::promise<ListToolsResult>>();
    auto fut = pr->get_future();
    listTools(cursor, [pr](const std::vector<McpTool>& tools, const std::string& nextCursor, const json& error) {
        pr->set_value({tools, nextCursor, error});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (nextCursorOut) *nextCursorOut = res.nextCursor;
        if (errorOut) *errorOut = res.error;
        return res.tools;
    }
    if (errorOut) *errorOut = {{"code", -32001}, {"message", "Synchronous listTools with cursor timed out"}};
    return {};
}

json McpClientSession::callToolSync(const std::string& name, const json& arguments,
                                    json* errorOut, std::chrono::milliseconds timeout,
                                    ProgressCallback progressCallback) {
    auto pr = std::make_shared<std::promise<std::pair<json, json>>>();
    auto fut = pr->get_future();
    callTool(name, arguments, [pr](const json& result, const json& error) {
        pr->set_value({result, error});
    }, std::move(progressCallback));
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (errorOut) *errorOut = res.second;
        return res.first;
    }
    if (errorOut) *errorOut = {{"code", -32001}, {"message", "Synchronous callTool timed out"}};
    return json::object();
}

json McpClientSession::listResourcesSync(std::chrono::milliseconds timeout, json* errorOut) {
    auto pr = std::make_shared<std::promise<std::pair<json, json>>>();
    auto fut = pr->get_future();
    listResources([pr](const json& result, const json& error) {
        pr->set_value({result, error});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (errorOut) *errorOut = res.second;
        return res.first;
    }
    if (errorOut) *errorOut = {{"code", -32001}, {"message", "Synchronous listResources timed out"}};
    return json::object();
}

json McpClientSession::listResourcesSync(const std::string& cursor, std::string* nextCursorOut,
                                         std::chrono::milliseconds timeout, json* errorOut) {
    struct ListResourcesResult {
        json result;
        std::string nextCursor;
        json error;
    };
    auto pr = std::make_shared<std::promise<ListResourcesResult>>();
    auto fut = pr->get_future();
    listResources(cursor, [pr](const json& result, const std::string& nextCursor, const json& error) {
        pr->set_value({result, nextCursor, error});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (nextCursorOut) *nextCursorOut = res.nextCursor;
        if (errorOut) *errorOut = res.error;
        return res.result;
    }
    if (errorOut) *errorOut = {{"code", -32001}, {"message", "Synchronous listResources with cursor timed out"}};
    return json::object();
}

json McpClientSession::readResourceSync(const std::string& uri, json* errorOut, std::chrono::milliseconds timeout) {
    auto pr = std::make_shared<std::promise<std::pair<json, json>>>();
    auto fut = pr->get_future();
    readResource(uri, [pr](const json& result, const json& error) {
        pr->set_value({result, error});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (errorOut) *errorOut = res.second;
        return res.first;
    }
    if (errorOut) *errorOut = {{"code", -32001}, {"message", "Synchronous readResource timed out"}};
    return json::object();
}

bool McpClientSession::subscribeResourceSync(const std::string& uri, json* errorOut, std::chrono::milliseconds timeout) {
    auto pr = std::make_shared<std::promise<std::pair<bool, json>>>();
    auto fut = pr->get_future();
    subscribeResource(uri, [pr](bool success, const json& error) {
        pr->set_value({success, error});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (errorOut) *errorOut = res.second;
        return res.first;
    }
    if (errorOut) *errorOut = {{"code", -32001}, {"message", "Synchronous subscribeResource timed out"}};
    return false;
}

bool McpClientSession::unsubscribeResourceSync(const std::string& uri, json* errorOut, std::chrono::milliseconds timeout) {
    auto pr = std::make_shared<std::promise<std::pair<bool, json>>>();
    auto fut = pr->get_future();
    unsubscribeResource(uri, [pr](bool success, const json& error) {
        pr->set_value({success, error});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (errorOut) *errorOut = res.second;
        return res.first;
    }
    if (errorOut) *errorOut = {{"code", -32001}, {"message", "Synchronous unsubscribeResource timed out"}};
    return false;
}

json McpClientSession::listPromptsSync(std::chrono::milliseconds timeout, json* errorOut) {
    auto pr = std::make_shared<std::promise<std::pair<json, json>>>();
    auto fut = pr->get_future();
    listPrompts([pr](const json& result, const json& error) {
        pr->set_value({result, error});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (errorOut) *errorOut = res.second;
        return res.first;
    }
    if (errorOut) *errorOut = {{"code", -32001}, {"message", "Synchronous listPrompts timed out"}};
    return json::object();
}

json McpClientSession::listPromptsSync(const std::string& cursor, std::string* nextCursorOut,
                                       std::chrono::milliseconds timeout, json* errorOut) {
    struct ListPromptsResult {
        json result;
        std::string nextCursor;
        json error;
    };
    auto pr = std::make_shared<std::promise<ListPromptsResult>>();
    auto fut = pr->get_future();
    listPrompts(cursor, [pr](const json& result, const std::string& nextCursor, const json& error) {
        pr->set_value({result, nextCursor, error});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (nextCursorOut) *nextCursorOut = res.nextCursor;
        if (errorOut) *errorOut = res.error;
        return res.result;
    }
    if (errorOut) *errorOut = {{"code", -32001}, {"message", "Synchronous listPrompts with cursor timed out"}};
    return json::object();
}

json McpClientSession::getPromptSync(const std::string& name, const json& arguments,
                                     json* errorOut, std::chrono::milliseconds timeout) {
    auto pr = std::make_shared<std::promise<std::pair<json, json>>>();
    auto fut = pr->get_future();
    getPrompt(name, arguments, [pr](const json& result, const json& error) {
        pr->set_value({result, error});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (errorOut) *errorOut = res.second;
        return res.first;
    }
    if (errorOut) *errorOut = {{"code", -32001}, {"message", "Synchronous getPrompt timed out"}};
    return json::object();
}

int64_t McpClientSession::sendRequestRaw(const std::string& method, const std::string& paramsJson, RawResponseCallback callback) {
    json params = json::object();
    if (!paramsJson.empty()) {
        try {
            params = json::parse(paramsJson);
        } catch (...) {
            log(LogLevel::Error, "sendRequestRaw: Failed to parse input paramsJson: " + paramsJson);
            callback("{}", "{\"code\":-32602,\"message\":\"Invalid params: JSON parse error\"}");
            return -1;
        }
    }
    return sendRequest(method, params, [callback](const json& res, const json& err) {
        callback(res.dump(), err.empty() ? "" : err.dump());
    });
}

void McpClientSession::callToolRaw(const std::string& name, const std::string& argumentsJson,
                                   std::function<void(const std::string& contentJson, const std::string& errorJson)> callback) {
    json args = json::object();
    if (!argumentsJson.empty()) {
        try {
            args = json::parse(argumentsJson);
        } catch (...) {
            log(LogLevel::Error, "callToolRaw: Failed to parse input argumentsJson: " + argumentsJson);
            callback("{}", "{\"code\":-32602,\"message\":\"Invalid arguments: JSON parse error\"}");
            return;
        }
    }
    callTool(name, args, [callback](const json& res, const json& err) {
        callback(res.dump(), err.empty() ? "" : err.dump());
    });
}

std::string McpClientSession::callToolSyncRaw(const std::string& name, const std::string& argumentsJson,
                                              std::string* errorJsonOut, std::chrono::milliseconds timeout) {
    auto pr = std::make_shared<std::promise<std::pair<std::string, std::string>>>();
    auto fut = pr->get_future();
    callToolRaw(name, argumentsJson, [pr](const std::string& res, const std::string& err) {
        pr->set_value({res, err});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (errorJsonOut) *errorJsonOut = res.second;
        return res.first;
    }
    if (errorJsonOut) *errorJsonOut = "{\"code\":-32001,\"message\":\"Synchronous callToolRaw timed out\"}";
    return "{}";
}

// ==========================================
// Ping
// ==========================================

void McpClientSession::ping(std::function<void(bool success, const json& error)> callback) {
    if (m_state != SessionState::Initialized) {
        json err = {
            {"code", -32002},
            {"message", "Session not initialized"}
        };
        callback(false, err);
        return;
    }
    sendRequest("ping", json::object(), [callback](const json& result, const json& error) {
        if (!error.empty()) {
            callback(false, error);
        } else {
            callback(true, json::object());
        }
    });
}

bool McpClientSession::pingSync(std::chrono::milliseconds timeout, json* errorOut) {
    auto pr = std::make_shared<std::promise<std::pair<bool, json>>>();
    auto fut = pr->get_future();
    ping([pr](bool success, const json& error) {
        pr->set_value({success, error});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (errorOut) *errorOut = res.second;
        return res.first;
    }
    if (errorOut) *errorOut = {{"code", -32001}, {"message", "Synchronous ping timed out"}};
    return false;
}

// ==========================================
// Resource Templates
// ==========================================

void McpClientSession::listResourceTemplates(std::function<void(const std::vector<McpResourceTemplate>& templates, const json& error)> callback) {
    listResourceTemplates("", [callback](const std::vector<McpResourceTemplate>& templates, const std::string&, const json& error) {
        callback(templates, error);
    });
}

void McpClientSession::listResourceTemplates(const std::string& cursor, std::function<void(const std::vector<McpResourceTemplate>& templates, const std::string& nextCursor, const json& error)> callback) {
    if (m_state != SessionState::Initialized) {
        json err = {
            {"code", -32002},
            {"message", "Session not initialized"}
        };
        callback({}, "", err);
        return;
    }
    json params = json::object();
    if (!cursor.empty()) {
        params["cursor"] = cursor;
    }
    sendRequest("resources/templates/list", params, [callback](const json& result, const json& error) {
        if (!error.empty()) {
            callback({}, "", error);
        } else {
            std::vector<McpResourceTemplate> templates;
            bool parseOk = true;
            if (result.contains("resourceTemplates") && result["resourceTemplates"].is_array()) {
                for (const auto& item : result["resourceTemplates"]) {
                    try {
                        templates.push_back(McpResourceTemplate::fromJson(item));
                    } catch (...) {
                        parseOk = false;
                    }
                }
            }
            if (!parseOk) {
                templates.clear();
            }
            std::string nextCursor;
            if (result.contains("nextCursor") && result["nextCursor"].is_string()) {
                nextCursor = result["nextCursor"].get<std::string>();
            }
            callback(templates, nextCursor, json::object());
        }
    });
}

std::vector<McpResourceTemplate> McpClientSession::listResourceTemplatesSync(std::chrono::milliseconds timeout, json* errorOut) {
    auto pr = std::make_shared<std::promise<std::pair<std::vector<McpResourceTemplate>, json>>>();
    auto fut = pr->get_future();
    listResourceTemplates([pr](const std::vector<McpResourceTemplate>& templates, const json& error) {
        pr->set_value({templates, error});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (errorOut) *errorOut = res.second;
        return res.first;
    }
    if (errorOut) *errorOut = {{"code", -32001}, {"message", "Synchronous listResourceTemplates timed out"}};
    return {};
}

std::vector<McpResourceTemplate> McpClientSession::listResourceTemplatesSync(const std::string& cursor, std::string* nextCursorOut,
                                                                             std::chrono::milliseconds timeout, json* errorOut) {
    struct ListTemplatesResult {
        std::vector<McpResourceTemplate> templates;
        std::string nextCursor;
        json error;
    };
    auto pr = std::make_shared<std::promise<ListTemplatesResult>>();
    auto fut = pr->get_future();
    listResourceTemplates(cursor, [pr](const std::vector<McpResourceTemplate>& templates, const std::string& nextCursor, const json& error) {
        pr->set_value({templates, nextCursor, error});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (nextCursorOut) *nextCursorOut = res.nextCursor;
        if (errorOut) *errorOut = res.error;
        return res.templates;
    }
    if (errorOut) *errorOut = {{"code", -32001}, {"message", "Synchronous listResourceTemplates with cursor timed out"}};
    return {};
}

// ==========================================
// Completion (auto-complete)
// ==========================================

void McpClientSession::complete(const json& ref, const json& argument, std::function<void(const json& completion, const json& error)> callback) {
    if (m_state != SessionState::Initialized) {
        json err = {
            {"code", -32002},
            {"message", "Session not initialized"}
        };
        callback(json::object(), err);
        return;
    }
    json params = {
        {"ref", ref},
        {"argument", argument}
    };
    sendRequest("completion/complete", params, [callback](const json& result, const json& error) {
        callback(result, error);
    });
}

json McpClientSession::completeSync(const json& ref, const json& argument,
                                    json* errorOut, std::chrono::milliseconds timeout) {
    auto pr = std::make_shared<std::promise<std::pair<json, json>>>();
    auto fut = pr->get_future();
    complete(ref, argument, [pr](const json& completion, const json& error) {
        pr->set_value({completion, error});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (errorOut) *errorOut = res.second;
        return res.first;
    }
    if (errorOut) *errorOut = {{"code", -32001}, {"message", "Synchronous complete timed out"}};
    return json::object();
}

// ==========================================
// Sampling (双向: 服务端请求客户端推理)
// ==========================================

void McpClientSession::setSamplingHandler(SamplingHandler handler) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_samplingHandler = std::move(handler);
    }

    // 注册 sampling/createMessage 请求处理器（在锁外调用避免死锁）
    registerRequestHandler("sampling/createMessage", [this](const std::string&, const json& params, std::function<void(const json& result, const json& error)> cb) {
        SamplingHandler samplingCb;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            samplingCb = m_samplingHandler;
        }
        if (!samplingCb) {
            cb(json::object(), {{"code", -32601}, {"message", "No sampling handler registered"}});
            return;
        }
        samplingCb(params, cb);
    });
}

// ==========================================
// Elicitation (双向: 服务端请求用户输入)
// ==========================================

void McpClientSession::setElicitationHandler(ElicitationHandler handler) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_elicitationHandler = std::move(handler);
    }

    // 注册 elicitation/create 请求处理器（在锁外调用避免死锁）
    registerRequestHandler("elicitation/create", [this](const std::string&, const json& params, std::function<void(const json& result, const json& error)> cb) {
        ElicitationHandler elicitCb;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            elicitCb = m_elicitationHandler;
        }
        if (!elicitCb) {
            cb({{"action", "declined"}}, json::object());
            return;
        }
        elicitCb(params, [params, cb](const json& resOut, const json& err) {
            json res = resOut;
            if (res.contains("action") && res["action"] == "accept") {
                if (!res.contains("content") || res["content"].is_null()) {
                    res["content"] = json::object();
                }
                if (params.contains("requestedSchema") && params["requestedSchema"].contains("properties")) {
                    auto props = params["requestedSchema"]["properties"];
                    if (props.is_object()) {
                        for (auto it = props.begin(); it != props.end(); ++it) {
                            std::string key = it.key();
                            auto propVal = it.value();
                            if (propVal.is_object() && propVal.contains("default")) {
                                if (!res["content"].contains(key)) {
                                    res["content"][key] = propVal["default"];
                                }
                            }
                        }
                    }
                }
            }
            cb(res, err);
        });
    });
}

// ==========================================
// Roots (双向: 客户端暴露文件系统根目录)
// ==========================================

void McpClientSession::setRootsProvider(RootsProvider provider) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_rootsProvider = std::move(provider);
    }

    // 注册 roots/list 请求处理器（在锁外调用避免死锁）
    registerRequestHandler("roots/list", [this](const std::string&, const json&, std::function<void(const json& result, const json& error)> cb) {
        RootsProvider rootsCb;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            rootsCb = m_rootsProvider;
        }
        if (!rootsCb) {
            cb({{"roots", json::array()}}, json::object());
            return;
        }
        rootsCb([cb](const json& result, const json& error) {
            cb({{"roots", result}}, error);
        });
    });
}

void McpClientSession::notifyRootsListChanged() {
    sendNotification("notifications/roots/list_changed", json::object());
}

// ==========================================
// Notification Debounce (通知去重/合并)
// ==========================================

void McpClientSession::enableNotificationDebounce(const std::string& method,
                                                   std::chrono::milliseconds debounceWindow) {
    std::lock_guard<std::mutex> lock(m_debounceMutex);
    auto& state = m_debounceStates[method];
    state.window = debounceWindow;
}

void McpClientSession::sendNotificationDebounced(const std::string& method, const json& params) {
    std::lock_guard<std::mutex> lock(m_debounceMutex);
    auto it = m_debounceStates.find(method);
    if (it == m_debounceStates.end()) {
        // 未配置去重，直接发送
        sendNotification(method, params);
        return;
    }

    auto& state = it->second;
    state.lastParamsJson = params.dump();

    // 如果定时器已在运行，只更新 params（自然去重）
    if (state.timerActive) {
        return;
    }

    // 启动新定时器
    state.timerActive = true;
    auto window = state.window;
    auto paramsJson = state.lastParamsJson;

    // 在后台线程延迟发送
    std::thread([this, method, paramsJson, window]() {
        std::this_thread::sleep_for(window);

        std::string finalParams;
        {
            std::lock_guard<std::mutex> lock(m_debounceMutex);
            auto st = m_debounceStates.find(method);
            if (st != m_debounceStates.end()) {
                finalParams = st->second.lastParamsJson;
                st->second.timerActive = false;
            }
        }

        if (!finalParams.empty()) {
            try {
                sendNotification(method, json::parse(finalParams));
            } catch (...) {
                sendNotification(method, json::object());
            }
        }
    }).detach();
}

void McpClientSession::setLogCallback(LogCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_logCallback = std::move(callback);
}

void McpClientSession::setOnError(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_errorCallback = std::move(callback);
}

void McpClientSession::setOnClose(CloseCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_onCloseCallback = std::move(callback);
}

void McpClientSession::setNotificationCallback(GenericNotificationCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_genericNotificationCallback = std::move(callback);
}

void McpClientSession::setTrafficCallback(TrafficCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_trafficCallback = std::move(callback);
}

void McpClientSession::setProtocolVersion(const std::string& version) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_overrideProtocolVersion = version;
}

void McpClientSession::log(LogLevel level, const std::string& message) {
    LogCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cb = m_logCallback;
    }
    if (cb) {
        cb(level, message);
    }
}

void McpClientSession::registerCapabilities(const json& capabilities) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!capabilities.is_object()) {
        return;
    }
    for (auto it = capabilities.begin(); it != capabilities.end(); ++it) {
        const std::string& key = it.key();
        if (m_capabilities.contains(key) && m_capabilities[key].is_object() && it.value().is_object()) {
            m_capabilities[key].update(it.value());
        } else {
            m_capabilities[key] = it.value();
        }
    }
}

std::string McpClientSession::getNegotiatedProtocolVersion() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_negotiatedProtocolVersion;
}

json McpClientSession::getServerCapabilities() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_serverCapabilities;
}

json McpClientSession::getServerVersion() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_serverVersion;
}

std::string McpClientSession::getInstructions() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_instructions;
}

} // namespace mcp
