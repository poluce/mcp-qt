#include "mcp_core/McpClientSession.h"
#include "mcp_core/McpHeaderEncoding.h"
#include <cctype>
#include <set>

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

    /**
     * @brief Normalize the handler's reply into a spec-conformant InputResponses map.
     *
     * The official 2026-07-28 InputResponses object is a map whose keys match the
     * InputRequests keys (values are ElicitResult / CreateMessageResult / ListRootsResult).
     *
     * Two accepted shapes from the application handler:
     *   1. Already keyed by request id (top-level keys match inputRequests keys) -> used as-is.
     *   2. A single input request was present and the handler returned flat {field: value}
     *      form data -> wrapped into { "<key>": { "action": "accept", "content": {...} } }.
     */
    json normalizeInputResponses(const json& inputRequests, const json& raw) {
        if (!raw.is_object()) {
            return raw.is_null() ? json::object() : json{{"value", raw}};
        }
        if (inputRequests.is_object() && !inputRequests.empty()) {
            bool topLevelMatches = true;
            for (auto it = inputRequests.begin(); it != inputRequests.end(); ++it) {
                if (!raw.contains(it.key())) {
                    topLevelMatches = false;
                    break;
                }
            }
            if (topLevelMatches) return raw;

            if (inputRequests.size() == 1) {
                const std::string key = inputRequests.begin().key();
                const json& req = inputRequests.begin().value();
                std::string method = req.is_object() && req.contains("method") && req["method"].is_string()
                                         ? req["method"].get<std::string>()
                                         : std::string();
                json wrapped;
                if (method == "elicitation/create") {
                    wrapped = {{"action", "accept"}, {"content", raw}};
                } else {
                    wrapped = raw;
                }
                json result;
                result[key] = wrapped;
                return result;
            }
        }
        // Cannot determine mapping; pass through (server SHOULD ignore unknown keys).
        return raw;
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
        m_lastRequestId.store(id);
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

    // 2026-07-28 无状态模式下自动充实 self-contained _meta 元数据（兼容标准命名空间全称）
    injectStatelessMeta(requestMsg["params"]);

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
        std::lock_guard<std::mutex> lock(m_mutex);
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

        // MCP 2026-07-28 MRTR: 拦截 resultType/status: "input_required" 挂起状态。
        // 注意：tasks/get 的 DetailedTask 也携带 status: "input_required"（SEP-2663），
        // 但其 resultType 为 "complete"，属任务状态而非 MRTR 挂起——必须按请求方法排除
        // tasks 家族，否则任务轮询会被误判为 MRTR 并报 -32901。
        bool isTaskMethod = reqMethod.rfind("tasks/", 0) == 0;
        bool isInputRequired = result.is_object() && !isTaskMethod &&
            ((result.contains("status") && result["status"] == "input_required") ||
             (result.contains("resultType") && result["resultType"] == "input_required"));

        if (isInputRequired) {
            MrtrInputHandler mrtrHandler;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                mrtrHandler = m_mrtrHandler;
            }
            if (mrtrHandler) {
                // 规范 InputRequests map: { key: { method, params } } (SEP-2322)
                json inputRequests = json::object();
                if (result.contains("inputRequests") && result["inputRequests"].is_object()) {
                    inputRequests = result["inputRequests"];
                } else if (result.contains("inputSchema") && result["inputSchema"].is_object()) {
                    // 兼容旧式 inputSchema 字段：包装为单个 elicitation 请求
                    inputRequests = json{
                        {"input", {
                            {"method", "elicitation/create"},
                            {"params", {
                                {"mode", "form"},
                                {"message", "Server requests additional input"},
                                {"requestedSchema", result["inputSchema"]}
                            }}
                        }}
                    };
                }

                // 客户端 MUST NOT 解析/修改 requestState；仅在重发时原样回显
                std::string requestState;
                if (result.contains("requestState") && result["requestState"].is_string()) {
                    requestState = result["requestState"].get<std::string>();
                }

                log(LogLevel::Info, "Intercepted MRTR input_required status for request id=" + std::to_string(id));
                std::weak_ptr<McpClientSession> weakSelf = shared_from_this();
                mrtrHandler(std::to_string(id), inputRequests, reqParams, requestState,
                            [weakSelf, reqMethod, reqParams, inputRequests, requestState, cb](const json& userInputs) {
                    if (auto self = weakSelf.lock()) {
                        json inputResponses = normalizeInputResponses(inputRequests, userInputs);
                        self->resendMrtrRequest(reqMethod, reqParams, inputResponses, requestState, cb);
                    }
                });
                return;
            }

            // 无 MRTR handler：无法满足 input_required，回上层报错避免请求悬空
            // （-32901 = 客户端本地错误：MRTR 无 handler；见 kErrorCancelled）
            cb(result, {{"code", kErrorCancelled},
                        {"message", "MRTR input_required received but no MrtrInputHandler is registered"}});
            return;
        }

        // MCP 2026-07-28 通用 resultType 语义（SEP-2575）：
        //   所有结果 MUST 携带 resultType；缺省视为 complete；未知值视为无效。
        //   "task"（SEP-2663 Tasks 扩展）为合法值：tools/call 可返回 CreateTaskResult。
        if (result.is_object() && result.contains("resultType") && result["resultType"].is_string()) {
            const std::string rt = result["resultType"].get<std::string>();
            if (!rt.empty() && rt != kResultTypeComplete && rt != kResultTypeInputRequired && rt != kResultTypeTask) {
                log(LogLevel::Warning, "Unknown resultType '" + rt + "' in response for id=" + std::to_string(id));
                cb(json::object(), {{"code", kErrorUnknownResultType},
                                    {"message", "Unknown resultType: " + rt}});
                return;
            }
        }

        cb(result, error);
    }
}

void McpClientSession::handleNotification(const json& notificationJson) {
    std::string method = notificationJson["method"].get<std::string>();
    json params = notificationJson.contains("params") ? notificationJson["params"] : json::object();

    // MCP 2026-07-28 subscriptions/listen (SEP-2330):
    // 从 _meta."io.modelcontextprotocol/subscriptionId" 提取 subscriptionId。
    int64_t subscriptionId = 0;
    if (params.is_object() && params.contains("_meta") && params["_meta"].is_object()) {
        const auto& meta = params["_meta"];
        if (meta.contains("io.modelcontextprotocol/subscriptionId")) {
            const auto& sid = meta["io.modelcontextprotocol/subscriptionId"];
            if (sid.is_number_integer()) {
                subscriptionId = sid.get<int64_t>();
            } else if (sid.is_string()) {
                try {
                    subscriptionId = std::stoll(sid.get<std::string>());
                } catch (...) {
                    // Ignore parsing error
                }
            }
        }
    }

    // acknowledged：记录服务器同意的 notifications 子集（subscriptionId -> filter）
    if (method == "notifications/subscriptions/acknowledged") {
        json accepted = params.contains("notifications") ? params["notifications"] : json();
        if (subscriptionId != 0) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_subscriptions[subscriptionId] = accepted;
        }
        log(LogLevel::Info, "subscriptions/acknowledged: subscriptionId=" + std::to_string(subscriptionId));
    }

    // 订阅通知派发：acknowledged 与其它流通知（resources/updated 等）一并
    // 派发给 setSubscriptionListener 注册的 listener（带 subscriptionId）。
    if (subscriptionId != 0) {
        SubscriptionListener subListener;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            subListener = m_subscriptionListener;
        }
        if (subListener) {
            subListener(subscriptionId, method, params);
        }
    }

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
    {
        std::lock_guard<std::mutex> lk(m_mutex);
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
        return;
    }
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
                    {"code", kErrorNotInitialized},
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
    if (!isReady()) {
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

void McpClientSession::discoverServer(std::function<void(const McpServerDiscovery& info, const json& error)> callback) {
    // server/discover 是 bootstrap RPC（2026-07-28），无需 initialize 握手即可发送。
    // stateless 模式下 isReady() 恒为 true；legacy 模式下也允许先 discover 再初始化。
    json params = json::object();
    sendRequest("server/discover", params, [callback](const json& result, const json& error) {
        McpServerDiscovery info;
        if (!error.empty()) {
            callback(info, error);
            return;
        }
        if (result.contains("supportedVersions") && result["supportedVersions"].is_array()) {
            for (const auto& v : result["supportedVersions"]) {
                if (v.is_string()) info.supportedVersions.push_back(v.get<std::string>());
            }
        }
        if (result.contains("capabilities")) info.capabilities = result["capabilities"];
        if (result.contains("_meta") && result["_meta"].is_object() &&
            result["_meta"].contains("io.modelcontextprotocol/serverInfo")) {
            info.serverInfo = result["_meta"]["io.modelcontextprotocol/serverInfo"];
        } else if (result.contains("serverInfo")) {
            info.serverInfo = result["serverInfo"];
        }
        if (result.contains("instructions") && result["instructions"].is_string()) {
            info.instructions = result["instructions"].get<std::string>();
        }
        if (result.contains("resultType") && result["resultType"].is_string()) {
            info.resultType = result["resultType"].get<std::string>();
        }
        if (result.contains("ttlMs") && result["ttlMs"].is_number_integer()) {
            info.ttlMs = result["ttlMs"].get<int64_t>();
        }
        if (result.contains("cacheScope") && result["cacheScope"].is_string()) {
            info.cacheScope = result["cacheScope"].get<std::string>();
        }
        callback(info, json::object());
    });
}

void McpClientSession::listTools(std::function<void(const std::vector<McpTool>& tools, const json& error)> callback) {
    listTools("", [callback](const std::vector<McpTool>& tools, const std::string&, const json& error) {
        callback(tools, error);
    });
}

void McpClientSession::listTools(const std::string& cursor, std::function<void(const std::vector<McpTool>& tools, const std::string& nextCursor, const json& error)> callback) {
    if (!isReady()) {
        callback({}, "", notInitializedError());
        return;
    }
    json params = json::object();
    if (!cursor.empty()) {
        params["cursor"] = cursor;
    }
    sendRequest("tools/list", params, [this, callback](const json& result, const json& error) {
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
                std::lock_guard<std::mutex> lock(m_mutex);
                for (const auto& t : toolsList) {
                    m_toolCache[t.name] = t;
                }
            }
            callback(toolsList, nextCursor, json::object());
        }
    });
}

void McpClientSession::callTool(const std::string& name, const json& arguments,
                                std::function<void(const json& content, const json& error)> callback,
                                ProgressCallback progressCallback) {
    if (!isReady()) {
        callback(json::object(), notInitializedError());
        return;
    }

    // 2026-07-28 x-mcp-header (SEP-2243)：若工具 schema 缓存中存在该工具，
    // 从 arguments 提取带 x-mcp-header 注解的参数值，编码为 Mcp-Param-{Name} 请求头。
    McpTool cachedTool;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
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
    if (!isReady()) {
        callback(json::object(), "", notInitializedError());
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
    if (!isReady()) {
        callback(json::object(), notInitializedError());
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
    if (!isReady()) {
        callback(false, notInitializedError());
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
    if (!isReady()) {
        callback(false, notInitializedError());
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
    if (!isReady()) {
        callback(json::object(), "", notInitializedError());
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
    if (!isReady()) {
        callback(json::object(), notInitializedError());
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

// ==========================================
// Ping
// ==========================================

void McpClientSession::ping(std::function<void(bool success, const json& error)> callback) {
    if (modernMode()) {
        // MCP 2026-07-28 已移除 ping 方法（SEP-2575/2567）
        log(LogLevel::Warning, "ping removed in 2026-07-28; Method not found");
        callback(false, {{"code", -32601}, {"message", "Method not found: ping"}});
        return;
    }
    if (!isReady()) {
        callback(false, notInitializedError());
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

// ==========================================
// Resource Templates
// ==========================================

void McpClientSession::listResourceTemplates(std::function<void(const std::vector<McpResourceTemplate>& templates, const json& error)> callback) {
    listResourceTemplates("", [callback](const std::vector<McpResourceTemplate>& templates, const std::string&, const json& error) {
        callback(templates, error);
    });
}

void McpClientSession::listResourceTemplates(const std::string& cursor, std::function<void(const std::vector<McpResourceTemplate>& templates, const std::string& nextCursor, const json& error)> callback) {
    if (!isReady()) {
        callback({}, "", notInitializedError());
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

// ==========================================
// Completion (auto-complete)
// ==========================================

void McpClientSession::complete(const json& ref, const json& argument, std::function<void(const json& completion, const json& error)> callback) {
    if (!isReady()) {
        callback(json::object(), notInitializedError());
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

// ==========================================
// Sampling (双向: 服务端请求客户端推理)
// ==========================================

void McpClientSession::setSamplingHandler(SamplingHandler handler) {
    if (modernMode()) {
        log(LogLevel::Warning, "Feature 'sampling' is deprecated in MCP 2026-07-28 specification but maintained for backwards compatibility.");
    }
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
    if (modernMode()) {
        log(LogLevel::Warning, "Feature 'roots' is deprecated in MCP 2026-07-28 specification but maintained for backwards compatibility.");
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_rootsProvider = std::move(provider);
}

void McpClientSession::setMrtrHandler(MrtrInputHandler handler) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_mrtrHandler = std::move(handler);
}

void McpClientSession::injectStatelessMeta(json& params) {
    if (modernMode()) {
        if (!params.is_object()) {
            params = json::object();
        }
        if (!params.contains("_meta") || !params["_meta"].is_object()) {
            params["_meta"] = json::object();
        }
        auto& meta = params["_meta"];
        std::string ver = !m_overrideProtocolVersion.empty() ? m_overrideProtocolVersion
            : (m_negotiatedProtocolVersion.empty() ? "2026-07-28" : m_negotiatedProtocolVersion);
        json clientInfoObj = {{"name", m_clientName}, {"version", m_clientVersion}};

        meta["protocolVersion"] = ver;
        meta["io.modelcontextprotocol/protocolVersion"] = ver;
        meta["clientInfo"] = clientInfoObj;
        meta["io.modelcontextprotocol/clientInfo"] = clientInfoObj;
        meta["capabilities"] = m_capabilities;
        meta["io.modelcontextprotocol/clientCapabilities"] = m_capabilities;

        // 2026-07-28 per-request logLevel（SEP-2577）：客户端可选地声明希望接收的日志级别
        if (!m_requestLogLevel.empty()) {
            meta["logLevel"] = m_requestLogLevel;
            meta["io.modelcontextprotocol/logLevel"] = m_requestLogLevel;
        }
    }
}

void McpClientSession::resendMrtrRequest(const std::string& method, json params,
                                         const json& inputResponses, const std::string& requestState,
                                         ResponseCallback callback) {
    if (!params.is_object()) {
        params = json::object();
    }
    // 规范 wire 格式（SEP-2322 / InputResponseRequestParams）:
    //   inputResponses 与 requestState 位于 params 顶层（与 name/arguments/_meta 平级）
    params["inputResponses"] = inputResponses;
    if (!requestState.empty()) {
        params["requestState"] = requestState;
    }
    log(LogLevel::Info, "Resending MRTR request method=" + method
        + " with top-level inputResponses" + (requestState.empty() ? "" : " and requestState"));
    // 新的 JSON-RPC id 由 sendRequest 自动分配（MUST differ from the initial request）
    sendRequest(method, params, std::move(callback));
}

// ==========================================
// Tasks 扩展（SEP-2663, io.modelcontextprotocol/tasks）
// ==========================================

void McpClientSession::getTask(const std::string& taskId, std::function<void(const McpTask& task, const json& error)> callback) {
    json params = {{"taskId", taskId}};
    sendRequest("tasks/get", params, [callback](const json& result, const json& error) {
        if (!error.empty()) {
            callback(McpTask{}, error);
            return;
        }
        callback(McpTask::fromJson(result), json::object());
    });
}

void McpClientSession::updateTask(const std::string& taskId, const json& inputResponses,
                                  std::function<void(bool success, const json& error)> callback) {
    json params = {{"taskId", taskId}, {"inputResponses", inputResponses}};
    sendRequest("tasks/update", params, [callback](const json& result, const json& error) {
        (void)result;  // ack-only：成功时为空结果
        callback(error.empty(), error);
    });
}

void McpClientSession::cancelTask(const std::string& taskId, std::function<void(bool success, const json& error)> callback) {
    json params = {{"taskId", taskId}};
    sendRequest("tasks/cancel", params, [callback](const json& result, const json& error) {
        (void)result;  // ack-only：成功时为空结果
        callback(error.empty(), error);
    });
}

McpTask McpClientSession::getTaskSync(const std::string& taskId, std::chrono::milliseconds timeout, json* errorOut) {
    auto pr = std::make_shared<std::promise<std::pair<McpTask, json>>>();
    auto fut = pr->get_future();
    getTask(taskId, [pr](const McpTask& task, const json& error) {
        pr->set_value({task, error});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (errorOut) *errorOut = res.second;
        return res.first;
    }
    if (errorOut) *errorOut = {{"code", kErrorTimeout}, {"message", "Synchronous getTask timed out"}};
    return McpTask{};
}

bool McpClientSession::updateTaskSync(const std::string& taskId, const json& inputResponses,
                                      std::chrono::milliseconds timeout, json* errorOut) {
    auto pr = std::make_shared<std::promise<std::pair<bool, json>>>();
    auto fut = pr->get_future();
    updateTask(taskId, inputResponses, [pr](bool success, const json& error) {
        pr->set_value({success, error});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (errorOut) *errorOut = res.second;
        return res.first;
    }
    if (errorOut) *errorOut = {{"code", kErrorTimeout}, {"message", "Synchronous updateTask timed out"}};
    return false;
}

bool McpClientSession::cancelTaskSync(const std::string& taskId, std::chrono::milliseconds timeout, json* errorOut) {
    auto pr = std::make_shared<std::promise<std::pair<bool, json>>>();
    auto fut = pr->get_future();
    cancelTask(taskId, [pr](bool success, const json& error) {
        pr->set_value({success, error});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (errorOut) *errorOut = res.second;
        return res.first;
    }
    if (errorOut) *errorOut = {{"code", kErrorTimeout}, {"message", "Synchronous cancelTask timed out"}};
    return false;
}

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

void McpClientSession::listenSubscriptions(const json& filter, std::function<void(bool success, const std::string& error)> callback) {
    if (!isReady()) {
        if (callback) callback(false, notInitializedError().dump());
        return;
    }
    json params = json::object();
    params["notifications"] = filter;
    sendRequest("subscriptions/listen", params, [callback](const json& result, const json& error) {
        if (!error.empty()) {
            if (callback) callback(false, error.dump());
            return;
        }
        if (callback) callback(true, "");
    });
}

void McpClientSession::cancelSubscription(int64_t requestId) {
    log(LogLevel::Info, "cancelSubscription: requestId=" + std::to_string(requestId));
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_subscriptions.erase(requestId);
    }
    // stdio：发送 notifications/cancelled；
    // HTTP：关闭流由 transport 负责（本层仅记录）。
    json params = {{"requestId", requestId}};
    sendNotification("notifications/cancelled", params);
}

void McpClientSession::setSubscriptionListener(SubscriptionListener listener) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_subscriptionListener = std::move(listener);
}

// ==========================================
// CacheableResult (MCP 2026-07-28): list/read 结果携带 ttlMs/cacheScope
// ==========================================

void McpClientSession::listToolsWithCache(const std::string& cursor, std::function<void(const std::vector<McpTool>& tools, const std::string& nextCursor, const McpCacheHint& hint, const json& error)> callback) {
    if (!isReady()) {
        callback({}, "", McpCacheHint{}, notInitializedError());
        return;
    }
    json params = json::object();
    if (!cursor.empty()) {
        params["cursor"] = cursor;
    }
    sendRequest("tools/list", params, [callback](const json& result, const json& error) {
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

void McpClientSession::listResourcesWithCache(const std::string& cursor, std::function<void(const json& result, const std::string& nextCursor, const McpCacheHint& hint, const json& error)> callback) {
    if (!isReady()) {
        callback(json::object(), "", McpCacheHint{}, notInitializedError());
        return;
    }
    json params = json::object();
    if (!cursor.empty()) {
        params["cursor"] = cursor;
    }
    sendRequest("resources/list", params, [callback](const json& result, const json& error) {
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

void McpClientSession::listPromptsWithCache(const std::string& cursor, std::function<void(const json& result, const std::string& nextCursor, const McpCacheHint& hint, const json& error)> callback) {
    if (!isReady()) {
        callback(json::object(), "", McpCacheHint{}, notInitializedError());
        return;
    }
    json params = json::object();
    if (!cursor.empty()) {
        params["cursor"] = cursor;
    }
    sendRequest("prompts/list", params, [callback](const json& result, const json& error) {
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

void McpClientSession::listResourceTemplatesWithCache(const std::string& cursor, std::function<void(const std::vector<McpResourceTemplate>& templates, const std::string& nextCursor, const McpCacheHint& hint, const json& error)> callback) {
    if (!isReady()) {
        callback({}, "", McpCacheHint{}, notInitializedError());
        return;
    }
    json params = json::object();
    if (!cursor.empty()) {
        params["cursor"] = cursor;
    }
    sendRequest("resources/templates/list", params, [callback](const json& result, const json& error) {
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

void McpClientSession::readResourceWithCache(const std::string& uri, std::function<void(const json& result, const McpCacheHint& hint, const json& error)> callback) {
    if (!isReady()) {
        callback(json::object(), McpCacheHint{}, notInitializedError());
        return;
    }
    json params = {{"uri", uri}};
    sendRequest("resources/read", params, [callback](const json& result, const json& error) {
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

void McpClientSession::setStatelessMode(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_statelessMode = enabled;
}

void McpClientSession::setLogLevel(const std::string& level) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_requestLogLevel = level;
}

std::string McpClientSession::getLogLevel() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_requestLogLevel;
}

bool McpClientSession::isStatelessMode() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_statelessMode;
}

bool McpClientSession::isReady() const {
    return modernMode() || m_state == SessionState::Initialized;
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
