#include "mcp_core/McpClientSession.h"
#include "mcp_core/McpHeaderEncoding.h"
#include <cctype>
#include <set>
#include <thread>
#include <future>

namespace mcp {

namespace {
    json notInitializedError() {
        // 2026-07-28 错误码分区：客户端本地错误使用 -32900 系列（-32902 = 未初始化）
        return {{"code", McpClientSession::kErrorNotInitialized}, {"message", "Session not initialized"}};
    }

    /**
     * @brief 从结果对象解析 CacheableResult 缓存提示（ttlMs/cacheScope）。
     */
    McpCacheHint parseCacheHint(const json& result) {
        McpCacheHint hint;
        if (result.is_object()) {
            if (result.contains("ttlMs") && result["ttlMs"].is_number_integer()) {
                hint.ttlMs = result["ttlMs"].get<int64_t>();
            }
            if (result.contains("cacheScope") && result["cacheScope"].is_string()) {
                hint.cacheScope = result["cacheScope"].get<std::string>();
            }
        }
        return hint;
    }

    /**
     * @brief RFC 9110 HTTP token 语法：[A-Za-z0-9!#$%&'*+.^_`|~-]+
     *        （控制字符、空白、分隔符均不合法）
     */
    bool isValidHttpTokenName(const std::string& name) {
        if (name.empty()) return false;
        for (char c : name) {
            const unsigned char uc = static_cast<unsigned char>(c);
            const bool tokenChar =
                (uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z') || (uc >= '0' && uc <= '9') ||
                uc == '!' || uc == '#' || uc == '$' || uc == '%' || uc == '&' || uc == '\'' ||
                uc == '*' || uc == '+' || uc == '.' || uc == '^' || uc == '_' || uc == '`' ||
                uc == '|' || uc == '~' || uc == '-';
            if (!tokenChar) return false;
        }
        return true;
    }

    static std::string toLowerAscii(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    }

    /**
     * @brief 校验工具 inputSchema 中的 x-mcp-header 注解（2026-07-28 SEP-2243）。
     *
     * 合法要求（简化但完整）：
     *   1. 注解值非空；
     *   2. 符合 HTTP token 语法（无控制字符/分隔符）；
     *   3. schema 内大小写不敏感唯一（忽略大小写不得重复）；
     *   4. 注解参数类型必须为 primitive（integer/string/boolean）；
     *   5. 静态可达：注解仅出现在 inputSchema.properties.<name> 顶层（properties 链）。
     * 任一非法即整体判定该工具注解非法（listTools 时剔除并警告）。
     */
    bool validateXMcpHeaderAnnotations(const json& schema) {
        if (!schema.is_object() || !schema.contains("properties") || !schema["properties"].is_object()) {
            return true; // 无注解 -> 合法
        }
        const json& properties = schema["properties"];
        std::set<std::string> seen; // 小写化注解名
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            const json& propSchema = it.value();
            if (!propSchema.is_object() || !propSchema.contains("x-mcp-header")) continue;
            if (!propSchema["x-mcp-header"].is_string()) return false;
            const std::string headerName = propSchema["x-mcp-header"].get<std::string>();
            if (headerName.empty() || !isValidHttpTokenName(headerName)) return false;
            const std::string ptype = propSchema.contains("type") && propSchema["type"].is_string()
                                          ? propSchema["type"].get<std::string>() : std::string();
            if (ptype != "integer" && ptype != "string" && ptype != "boolean") return false;
            if (!seen.insert(toLowerAscii(headerName)).second) return false; // 大小写不敏感重复
        }
        return true;
    }

    /**
     * @brief 从 tool inputSchema 与 arguments 提取 x-mcp-header 注解参数，
     *        组装为 {"Mcp-Param-<Name>": <编码值>} 请求头 map。
     *        仅处理通过校验的注解（非空、token 语法、primitive、静态可达、唯一）。
     */
    std::map<std::string, std::string> collectXMcpHeaders(const json& schema, const json& args) {
        std::map<std::string, std::string> headers;
        if (!schema.is_object() || !schema.contains("properties") || !schema["properties"].is_object()) {
            return headers;
        }
        const json& properties = schema["properties"];
        std::set<std::string> seen; // 小写化注解名，大小写不敏感唯一
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            const std::string& propName = it.key();
            const json& propSchema = it.value();
            if (!propSchema.is_object() || !propSchema.contains("x-mcp-header")) continue;
            if (!propSchema["x-mcp-header"].is_string()) continue;
            const std::string headerName = propSchema["x-mcp-header"].get<std::string>();
            if (headerName.empty() || !isValidHttpTokenName(headerName)) continue;
            const std::string ptype = propSchema.contains("type") && propSchema["type"].is_string()
                                          ? propSchema["type"].get<std::string>() : std::string();
            if (ptype != "integer" && ptype != "string" && ptype != "boolean") continue;
            if (!seen.insert(toLowerAscii(headerName)).second) continue; // 重复注解跳过
            if (!args.is_object() || !args.contains(propName)) continue;
            const json& val = args[propName];
            std::string valueStr;
            if (val.is_string()) {
                valueStr = val.get<std::string>();
            } else if (val.is_boolean()) {
                valueStr = val.get<bool>() ? "true" : "false";
            } else if (val.is_number_integer()) {
                valueStr = std::to_string(val.get<int64_t>());
            } else if (val.is_number_unsigned()) {
                valueStr = std::to_string(val.get<uint64_t>());
            } else {
                continue; // 非 primitive 值无法编码
            }
            headers["Mcp-Param-" + headerName] = mcpHeaderEncodeValue(valueStr);
        }
        return headers;
    }

    McpTrafficKind detectTrafficKind(const json& j) {
        if (j.contains("id")) {
            if (j.contains("result") || j.contains("error")) return McpTrafficKind::Response;
            if (j.contains("method")) return McpTrafficKind::Request;
        } else if (j.contains("method")) {
            return McpTrafficKind::Notification;
        }
        return McpTrafficKind::Unknown;
    }

}

void McpClientSession::emitTrafficEvent(McpTrafficDirection dir, McpTrafficKind kind,
                                         const json& payload, const std::string& raw) {
    if (!m_trafficCallback) return;
    m_trafficCallback({dir, kind, payload, raw});
}

McpClientSession::McpClientSession(std::shared_ptr<IMcpTransport> transport)
    : m_transport(std::move(transport)) {
    // 默认注册 roots/list 请求处理器，避免服务端询问时报错 "Method not found"
    registerRequestHandler("roots/list", [this](const std::string&, const json&, std::function<void(const json& result, const json& error)> cb) {
        RootsProvider rootsCb;
        {
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
        id = m_nextId++;
        m_pendingRequests[id] = PendingRequest{
            method,
            params,
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

    // 协议扩展钩子：stateless 子类在此充实 self-contained _meta 元数据
    prepareRequestParams(method, requestMsg["params"]);

    if (hasProgress) {
        if (!requestMsg["params"].is_object()) {
            requestMsg["params"] = json::object();
        }
        requestMsg["params"]["_meta"]["progressToken"] = id;
    }

    log(LogLevel::Debug, "sendRequest: method=" + method + ", id=" + std::to_string(id));
    std::string dumpStr = requestMsg.dump();
    emitTrafficEvent(McpTrafficDirection::Outbound, McpTrafficKind::Request, requestMsg, dumpStr);
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
    emitTrafficEvent(McpTrafficDirection::Outbound, McpTrafficKind::Notification, notificationMsg, dumpStr);
    m_transport->send(dumpStr);
}

void McpClientSession::registerNotificationHandler(const std::string& method, NotificationCallback callback) {
    m_notificationHandlers[method] = callback;
}

void McpClientSession::registerRequestHandler(const std::string& method, RequestCallback callback) {
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

    emitTrafficEvent(McpTrafficDirection::Inbound, detectTrafficKind(j), j, rawMessage);

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
    std::string reqMethod;
    json reqParams;
    bool found = false;
    {
        auto it = m_pendingRequests.find(id);
        if (it != m_pendingRequests.end()) {
            reqMethod = std::move(it->second.method);
            reqParams = std::move(it->second.params);
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

        // 协议扩展钩子：stateless 子类在此拦截 MRTR input_required 与未知 resultType
        if (handleSpecialResult(id, reqMethod, reqParams, result, cb)) {
            return;
        }

        cb(result, error);
    }
}

void McpClientSession::handleNotification(const json& notificationJson) {
    std::string method = notificationJson["method"].get<std::string>();
    json params = notificationJson.contains("params") ? notificationJson["params"] : json::object();

    // MCP 2026-07-28 subscriptions/listen (SEP-2330):
    // 从 _meta."io.modelcontextprotocol/subscriptionId" 提取 subscriptionId。
    // 协议扩展钩子：stateless 子类在此处理 subscriptions/acknowledged 与订阅派发
    handleSpecialNotification(method, params);

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

int64_t McpClientSession::initialize(const std::string& clientName, const std::string& clientVersion,
                                  std::function<void(bool success, const json& serverInfo)> callback) {
    {
        m_clientName = clientName;
        m_clientVersion = clientVersion;
    }
    if (m_statelessMode) {
        // 2026-07-28 已移除 initialize 握手（SEP-2575/2567）：stateless 模式下
        // initialize 方法不存在，应返回标准 JSON-RPC Method not found（-32601），
        // 而非 -32022 UnsupportedProtocolVersionError（那是服务器对 legacy initialize
        // 的协议协商错误）。调用方应改用 server/discover + 直接 RPC。
        callback(false, json{
            {"code", -32601},
            {"message", "Method not found: initialize"}
        });
        return 0;
    }
    SessionState expected = SessionState::Uninitialized;
    if (!m_state.compare_exchange_strong(expected, SessionState::Initializing)) {
        json err = {
            {"code", -32600},
            {"message", "Initialize already in progress or completed"}
        };
        callback(false, err);
        return 0;
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
    return sendRequest("initialize", params, [self, callback](const json& result, const json& error) {
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
                    {"code", kErrorNotInitialized},
                    {"message", "Version Mismatch: Server returned unsupported version " + serverVer}
                };
                callback(false, verErr);
                return;
            }

            {
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

int64_t McpClientSession::shutdown(std::function<void(bool success)> callback) {
    if (!isReady()) {
        callback(false);
        return 0;
    }
    auto self = shared_from_this();
    return sendRequest("shutdown", json::object(), [self, callback](const json& result, const json& error) {
        if (!error.empty()) {
            callback(false);
        } else {
            self->m_state = SessionState::Shutdown;
            callback(true);
        }
    });
}


int64_t McpClientSession::listTools(std::function<void(const std::vector<McpTool>& tools, const json& error)> callback) {
    return listTools("", [callback](const std::vector<McpTool>& tools, const std::string&, const json& error) {
        callback(tools, error);
    });
}

int64_t McpClientSession::listTools(const std::string& cursor, std::function<void(const std::vector<McpTool>& tools, const std::string& nextCursor, const json& error)> callback) {
    if (!isReady()) {
        callback({}, "", notInitializedError());
        return 0;
    }
    json params = json::object();
    if (!cursor.empty()) {
        params["cursor"] = cursor;
    }
    return sendRequest("tools/list", params, [this, callback](const json& result, const json& error) {
        if (!error.empty()) {
            callback({}, "", error);
        } else {
            std::vector<McpTool> toolsList;
            std::string nextCursor;
            bool parseOk = true;
            if (result.contains("tools") && result["tools"].is_array()) {
                for (const auto& item : result["tools"]) {
                    try {
                        McpTool tool = item.get<McpTool>();
                        // 2026-07-28 x-mcp-header (SEP-2243)：非法注解的工具从结果中剔除
                        //（保留工具本身可用性，仅不暴露注解参数）。
                        if (validateXMcpHeaderAnnotations(tool.inputSchema)) {
                            toolsList.push_back(std::move(tool));
                        } else {
                            log(LogLevel::Warning, "Tool '" + tool.name + "' dropped from tools/list: invalid x-mcp-header annotation");
                        }
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
            // 填充工具 schema 缓存（callTool 时提取 x-mcp-header 请求头用）
            {
                for (const auto& t : toolsList) {
                    m_toolCache[t.name] = t;
                }
            }
            callback(toolsList, nextCursor, json::object());
        }
    });
}

int64_t McpClientSession::callTool(const std::string& name, const json& arguments,
                                std::function<void(const json& content, const json& error)> callback,
                                ProgressCallback progressCallback) {
    if (!isReady()) {
        callback(json::object(), notInitializedError());
        return 0;
    }

    // 2026-07-28 x-mcp-header (SEP-2243)：若工具 schema 缓存中存在该工具，
    // 从 arguments 提取带 x-mcp-header 注解的参数值，编码为 Mcp-Param-{Name} 请求头。
    McpTool cachedTool;
    {
        auto it = m_toolCache.find(name);
        if (it != m_toolCache.end()) {
            cachedTool = it->second;
        }
    }
    if (!cachedTool.name.empty() && cachedTool.inputSchema.is_object()) {
        auto headers = collectXMcpHeaders(cachedTool.inputSchema, arguments);
        // 始终显式设置（空 map 即清空），避免上一次调用的 header 泄漏到本次请求
        m_transport->setExtraRequestHeaders(headers);
    }

    json params = {
        {"name", name},
        {"arguments", arguments}
    };

    return sendRequest("tools/call", params, [callback](const json& result, const json& error) {
        if (!error.empty()) {
            callback(json::object(), error);
        } else {
            callback(result, json::object());
        }
    }, std::move(progressCallback));
}

int64_t McpClientSession::listResources(std::function<void(const json& result, const json& error)> callback) {
    return listResources("", [callback](const json& result, const std::string&, const json& error) {
        callback(result, error);
    });
}

int64_t McpClientSession::listResources(const std::string& cursor, std::function<void(const json& result, const std::string& nextCursor, const json& error)> callback) {
    if (!isReady()) {
        callback(json::object(), "", notInitializedError());
        return 0;
    }
    json params = json::object();
    if (!cursor.empty()) {
        params["cursor"] = cursor;
    }
    return sendRequest("resources/list", params, [callback](const json& result, const json& error) {
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

int64_t McpClientSession::readResource(const std::string& uri, std::function<void(const json& result, const json& error)> callback) {
    if (!isReady()) {
        callback(json::object(), notInitializedError());
        return 0;
    }
    json params = {
        {"uri", uri}
    };
    return sendRequest("resources/read", params, [callback](const json& result, const json& error) {
        callback(result, error);
    });
}

int64_t McpClientSession::subscribeResource(const std::string& uri, std::function<void(bool success, const json& error)> callback) {
    if (!isReady()) {
        callback(false, notInitializedError());
        return 0;
    }
    json params = {
        {"uri", uri}
    };
    return sendRequest("resources/subscribe", params, [callback](const json&, const json& error) {
        if (!error.empty()) {
            callback(false, error);
        } else {
            callback(true, json::object());
        }
    });
}

int64_t McpClientSession::unsubscribeResource(const std::string& uri, std::function<void(bool success, const json& error)> callback) {
    if (!isReady()) {
        callback(false, notInitializedError());
        return 0;
    }
    json params = {
        {"uri", uri}
    };
    return sendRequest("resources/unsubscribe", params, [callback](const json&, const json& error) {
        if (!error.empty()) {
            callback(false, error);
        } else {
            callback(true, json::object());
        }
    });
}

int64_t McpClientSession::listPrompts(std::function<void(const json& result, const json& error)> callback) {
    return listPrompts("", [callback](const json& result, const std::string&, const json& error) {
        callback(result, error);
    });
}

int64_t McpClientSession::listPrompts(const std::string& cursor, std::function<void(const json& result, const std::string& nextCursor, const json& error)> callback) {
    if (!isReady()) {
        callback(json::object(), "", notInitializedError());
        return 0;
    }
    json params = json::object();
    if (!cursor.empty()) {
        params["cursor"] = cursor;
    }
    return sendRequest("prompts/list", params, [callback](const json& result, const json& error) {
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

int64_t McpClientSession::getPrompt(const std::string& name, const json& arguments, std::function<void(const json& result, const json& error)> callback) {
    if (!isReady()) {
        callback(json::object(), notInitializedError());
        return 0;
    }
    json params = {
        {"name", name},
        {"arguments", arguments}
    };
    return sendRequest("prompts/get", params, [callback](const json& result, const json& error) {
        callback(result, error);
    });
}

void McpClientSession::cancelRequest(int64_t requestId) {
    log(LogLevel::Info, "Request cancelled locally: id=" + std::to_string(requestId));
    ResponseCallback cb;
    {
        auto it = m_pendingRequests.find(requestId);
        if (it != m_pendingRequests.end()) {
            cb = std::move(it->second.callback);
            m_pendingRequests.erase(it);
        }
        m_progressHandlers.erase(requestId);
    }
    if (cb) {
        json cancelErr = {
            {"code", kErrorCancelled},
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
                {"code", kErrorTimeout},
                {"message", "Request timeout"}
            };
            pair.second(json::object(), timeoutErr);
        }
    }
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
    if (errorOut) *errorOut = {{"code", kErrorTimeout}, {"message", "Synchronous listTools timed out"}};
    return {};
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

int64_t McpClientSession::callToolRaw(const std::string& name, const std::string& argumentsJson,
                                   std::function<void(const std::string& contentJson, const std::string& errorJson)> callback) {
    json args = json::object();
    if (!argumentsJson.empty()) {
        try {
            args = json::parse(argumentsJson);
        } catch (...) {
            log(LogLevel::Error, "callToolRaw: Failed to parse input argumentsJson: " + argumentsJson);
            callback("{}", "{\"code\":-32602,\"message\":\"Invalid arguments: JSON parse error\"}");
            return 0;
        }
    }
    return callTool(name, args, [callback](const json& res, const json& err) {
        callback(res.dump(), err.empty() ? "" : err.dump());
    });
}

// ==========================================
// Ping
// ==========================================

int64_t McpClientSession::ping(std::function<void(bool success, const json& error)> callback) {
    if (modernMode()) {
        // MCP 2026-07-28 已移除 ping 方法（SEP-2575/2567）
        log(LogLevel::Warning, "ping removed in 2026-07-28; Method not found");
        callback(false, {{"code", -32601}, {"message", "Method not found: ping"}});
        return 0;
    }
    if (!isReady()) {
        callback(false, notInitializedError());
        return 0;
    }
    return sendRequest("ping", json::object(), [callback](const json& result, const json& error) {
        if (!error.empty()) {
            callback(false, error);
        } else {
            callback(true, json::object());
        }
    });
}

// ==========================================
// Resource Templates
// ==========================================

int64_t McpClientSession::listResourceTemplates(std::function<void(const std::vector<McpResourceTemplate>& templates, const json& error)> callback) {
    return listResourceTemplates("", [callback](const std::vector<McpResourceTemplate>& templates, const std::string&, const json& error) {
        callback(templates, error);
    });
}

int64_t McpClientSession::listResourceTemplates(const std::string& cursor, std::function<void(const std::vector<McpResourceTemplate>& templates, const std::string& nextCursor, const json& error)> callback) {
    if (!isReady()) {
        callback({}, "", notInitializedError());
        return 0;
    }
    json params = json::object();
    if (!cursor.empty()) {
        params["cursor"] = cursor;
    }
    return sendRequest("resources/templates/list", params, [callback](const json& result, const json& error) {
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

// ==========================================
// Completion (auto-complete)
// ==========================================

int64_t McpClientSession::complete(const json& ref, const json& argument, std::function<void(const json& completion, const json& error)> callback) {
    if (!isReady()) {
        callback(json::object(), notInitializedError());
        return 0;
    }
    json params = {
        {"ref", ref},
        {"argument", argument}
    };
    return sendRequest("completion/complete", params, [callback](const json& result, const json& error) {
        callback(result, error);
    });
}

// ==========================================
// Sampling (双向: 服务端请求客户端推理)
// ==========================================

void McpClientSession::setSamplingHandler(SamplingHandler handler) {
    if (modernMode()) {
        log(LogLevel::Warning, "Feature 'sampling' is deprecated in MCP 2026-07-28 specification but maintained for backwards compatibility.");
    }
    {
        m_samplingHandler = std::move(handler);
    }

    // 注册 sampling/createMessage 请求处理器（在锁外调用避免死锁）
    registerRequestHandler("sampling/createMessage", [this](const std::string&, const json& params, std::function<void(const json& result, const json& error)> cb) {
        SamplingHandler samplingCb;
        {
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
        m_elicitationHandler = std::move(handler);
    }

    // 注册 elicitation/create 请求处理器（在锁外调用避免死锁）
    registerRequestHandler("elicitation/create", [this](const std::string&, const json& params, std::function<void(const json& result, const json& error)> cb) {
        ElicitationHandler elicitCb;
        {
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
    if (modernMode()) {
        log(LogLevel::Warning, "Feature 'roots' is deprecated in MCP 2026-07-28 specification but maintained for backwards compatibility.");
    }
    m_rootsProvider = std::move(provider);
}


// ==========================================
// Tasks 扩展（SEP-2663, io.modelcontextprotocol/tasks）

void McpClientSession::notifyRootsListChanged() {
    if (modernMode()) {
        // MCP 2026-07-28 已移除 roots/list_changed 通知（SEP-2575/2567）：
        // 不发送，仅记录日志警告。
        log(LogLevel::Warning, "notifications/roots/list_changed removed in 2026-07-28; notification suppressed");
        return;
    }
    sendNotification("notifications/roots/list_changed", json::object());
}

// ==========================================
// Subscriptions (MCP 2026-07-28, SEP-2330: subscriptions/listen)

// ==========================================
// CacheableResult (MCP 2026-07-28): list/read 结果携带 ttlMs/cacheScope
// ==========================================

int64_t McpClientSession::listToolsWithCache(const std::string& cursor, std::function<void(const std::vector<McpTool>& tools, const std::string& nextCursor, const McpCacheHint& hint, const json& error)> callback) {
    if (!isReady()) {
        callback({}, "", McpCacheHint{}, notInitializedError());
        return 0;
    }
    json params = json::object();
    if (!cursor.empty()) {
        params["cursor"] = cursor;
    }
    return sendRequest("tools/list", params, [callback](const json& result, const json& error) {
        if (!error.empty()) {
            callback({}, "", McpCacheHint{}, error);
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
            callback(toolsList, nextCursor, parseCacheHint(result), json::object());
        }
    });
}

int64_t McpClientSession::listResourcesWithCache(const std::string& cursor, std::function<void(const json& result, const std::string& nextCursor, const McpCacheHint& hint, const json& error)> callback) {
    if (!isReady()) {
        callback(json::object(), "", McpCacheHint{}, notInitializedError());
        return 0;
    }
    json params = json::object();
    if (!cursor.empty()) {
        params["cursor"] = cursor;
    }
    return sendRequest("resources/list", params, [callback](const json& result, const json& error) {
        if (!error.empty()) {
            callback(json::object(), "", McpCacheHint{}, error);
        } else {
            std::string nextCursor;
            if (result.contains("nextCursor") && result["nextCursor"].is_string()) {
                nextCursor = result["nextCursor"].get<std::string>();
            }
            callback(result, nextCursor, parseCacheHint(result), json::object());
        }
    });
}

int64_t McpClientSession::listPromptsWithCache(const std::string& cursor, std::function<void(const json& result, const std::string& nextCursor, const McpCacheHint& hint, const json& error)> callback) {
    if (!isReady()) {
        callback(json::object(), "", McpCacheHint{}, notInitializedError());
        return 0;
    }
    json params = json::object();
    if (!cursor.empty()) {
        params["cursor"] = cursor;
    }
    return sendRequest("prompts/list", params, [callback](const json& result, const json& error) {
        if (!error.empty()) {
            callback(json::object(), "", McpCacheHint{}, error);
        } else {
            std::string nextCursor;
            if (result.contains("nextCursor") && result["nextCursor"].is_string()) {
                nextCursor = result["nextCursor"].get<std::string>();
            }
            callback(result, nextCursor, parseCacheHint(result), json::object());
        }
    });
}

int64_t McpClientSession::listResourceTemplatesWithCache(const std::string& cursor, std::function<void(const std::vector<McpResourceTemplate>& templates, const std::string& nextCursor, const McpCacheHint& hint, const json& error)> callback) {
    if (!isReady()) {
        callback({}, "", McpCacheHint{}, notInitializedError());
        return 0;
    }
    json params = json::object();
    if (!cursor.empty()) {
        params["cursor"] = cursor;
    }
    return sendRequest("resources/templates/list", params, [callback](const json& result, const json& error) {
        if (!error.empty()) {
            callback({}, "", McpCacheHint{}, error);
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
            callback(templates, nextCursor, parseCacheHint(result), json::object());
        }
    });
}

int64_t McpClientSession::readResourceWithCache(const std::string& uri, std::function<void(const json& result, const McpCacheHint& hint, const json& error)> callback) {
    if (!isReady()) {
        callback(json::object(), McpCacheHint{}, notInitializedError());
        return 0;
    }
    json params = {{"uri", uri}};
    return sendRequest("resources/read", params, [callback](const json& result, const json& error) {
        callback(result, parseCacheHint(result), error);
    });
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
    // 单线程契约的唯一例外：去重定时器在后台线程运行，只访问 m_debounceStates
    // （m_debounceMutex 保护）与 sendNotification（transport->send 线程安全）。
    std::string paramsJson;
    std::chrono::milliseconds window;
    {
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
        window = state.window;
        paramsJson = state.lastParamsJson;
    }

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
    m_logCallback = std::move(callback);
}

void McpClientSession::setOnError(ErrorCallback callback) {
    m_errorCallback = std::move(callback);
}

void McpClientSession::setOnClose(CloseCallback callback) {
    m_onCloseCallback = std::move(callback);
}

void McpClientSession::setNotificationCallback(GenericNotificationCallback callback) {
    m_genericNotificationCallback = std::move(callback);
}

void McpClientSession::setTrafficCallback(TrafficCallback callback) {
    m_trafficCallback = std::move(callback);
}

void McpClientSession::setProtocolVersion(const std::string& version) {
    m_overrideProtocolVersion = version;
}

void McpClientSession::setStatelessMode(bool enabled) {
    m_statelessMode = enabled;
}


bool McpClientSession::isStatelessMode() const {
    return m_statelessMode;
}

bool McpClientSession::isReady() const {
    return modernMode() || m_state == SessionState::Initialized;
}

void McpClientSession::log(LogLevel level, const std::string& message) {
    LogCallback cb;
    {
        cb = m_logCallback;
    }
    if (cb) {
        cb(level, message);
    }
}

void McpClientSession::registerCapabilities(const json& capabilities) {
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
    return m_negotiatedProtocolVersion;
}

json McpClientSession::getServerCapabilities() const {
    return m_serverCapabilities;
}

json McpClientSession::getServerVersion() const {
    return m_serverVersion;
}

std::string McpClientSession::getInstructions() const {
    return m_instructions;
}

// ==========================================
// 协议扩展钩子（基类空实现；McpStatelessSession 覆写承载 stateless 语义）
// ==========================================

void McpClientSession::prepareRequestParams(const std::string&, json&) {
    // legacy 协议：无请求前充实
}

bool McpClientSession::handleSpecialResult(int64_t, const std::string&, const json&,
                                           const json&, const ResponseCallback&) {
    // legacy 协议：无特殊 resultType 处理
    return false;
}

void McpClientSession::handleSpecialNotification(const std::string&, const json&) {
    // legacy 协议：无特殊通知处理
}

} // namespace mcp
