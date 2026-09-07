#include "mcp_core/McpStatelessSession.h"

#include <future>

namespace mcp {

namespace {

    json notInitializedError() {
        // 2026-07-28 错误码分区：客户端本地错误使用 -32900 系列（-32902 = 未初始化）
        return {{"code", McpClientSession::kErrorNotInitialized}, {"message", "Session not initialized"}};
    }

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

} // namespace

McpStatelessSession::McpStatelessSession(std::shared_ptr<IMcpTransport> transport)
    : McpClientSession(std::move(transport)) {
    // stateless 会话恒为新协议语义
    setStatelessMode(true);
}

// ========== server/discover（2026-07-28 bootstrap RPC） ==========

int64_t McpStatelessSession::discoverServer(std::function<void(const McpServerDiscovery& info, const json& error)> callback) {
    // server/discover 是 bootstrap RPC（2026-07-28），无需 initialize 握手即可发送。
    // stateless 模式下 isReady() 恒为 true；legacy 模式下也允许先 discover 再初始化。
    json params = json::object();
    return sendRequest("server/discover", params, [callback](const json& result, const json& error) {
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

// ========== MRTR（SEP-2322） ==========

void McpStatelessSession::setMrtrHandler(MrtrInputHandler handler) {
    m_mrtrHandler = std::move(handler);
}

// ========== Tasks 扩展（SEP-2663） ==========

int64_t McpStatelessSession::getTask(const std::string& taskId, std::function<void(const McpTask& task, const json& error)> callback) {
    json params = {{"taskId", taskId}};
    return sendRequest("tasks/get", params, [callback](const json& result, const json& error) {
        if (!error.empty()) {
            callback(McpTask{}, error);
            return;
        }
        callback(McpTask::fromJson(result), json::object());
    });
}

int64_t McpStatelessSession::updateTask(const std::string& taskId, const json& inputResponses,
                                  std::function<void(bool success, const json& error)> callback) {
    json params = {{"taskId", taskId}, {"inputResponses", inputResponses}};
    return sendRequest("tasks/update", params, [callback](const json& result, const json& error) {
        (void)result;  // ack-only：成功时为空结果
        callback(error.empty(), error);
    });
}

int64_t McpStatelessSession::cancelTask(const std::string& taskId, std::function<void(bool success, const json& error)> callback) {
    json params = {{"taskId", taskId}};
    return sendRequest("tasks/cancel", params, [callback](const json& result, const json& error) {
        (void)result;  // ack-only：成功时为空结果
        callback(error.empty(), error);
    });
}

McpTask McpStatelessSession::getTaskSync(const std::string& taskId, std::chrono::milliseconds timeout, json* errorOut) {
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

bool McpStatelessSession::updateTaskSync(const std::string& taskId, const json& inputResponses,
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

bool McpStatelessSession::cancelTaskSync(const std::string& taskId, std::chrono::milliseconds timeout, json* errorOut) {
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

// ========== Subscriptions（SEP-2330） ==========

int64_t McpStatelessSession::listenSubscriptions(const json& filter, std::function<void(bool success, const std::string& error)> callback) {
    if (!isReady()) {
        if (callback) callback(false, notInitializedError().dump());
        return 0;
    }
    json params = json::object();
    params["notifications"] = filter;
    return sendRequest("subscriptions/listen", params, [callback](const json& result, const json& error) {
        if (!error.empty()) {
            if (callback) callback(false, error.dump());
            return;
        }
        if (callback) callback(true, "");
    });
}

void McpStatelessSession::cancelSubscription(int64_t requestId) {
    log(LogLevel::Info, "cancelSubscription: requestId=" + std::to_string(requestId));
    {
        m_subscriptions.erase(requestId);
    }
    // stdio：发送 notifications/cancelled；
    // HTTP：关闭流由 transport 负责（本层仅记录）。
    json params = {{"requestId", requestId}};
    sendNotification("notifications/cancelled", params);
}

void McpStatelessSession::setSubscriptionListener(SubscriptionListener listener) {
    m_subscriptionListener = std::move(listener);
}

// ========== 每请求日志级别（SEP-2577） ==========

void McpStatelessSession::setLogLevel(const std::string& level) {
    m_requestLogLevel = level;
}

std::string McpStatelessSession::getLogLevel() const {
    return m_requestLogLevel;
}

// ========== 协议扩展钩子（基类定义，本类承载 stateless 语义） ==========

void McpStatelessSession::prepareRequestParams(const std::string&, json& params) {
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

bool McpStatelessSession::handleSpecialResult(int64_t id, const std::string& reqMethod,
                                             const json& reqParams, const json& result,
                                             const ResponseCallback& cb) {
    // MRTR: 拦截 resultType/status: "input_required" 挂起状态。
    // 注意：tasks/get 的 DetailedTask 也携带 status: "input_required"（SEP-2663），
    // 但其 resultType 为 "complete"，属任务状态而非 MRTR 挂起——必须按请求方法排除
    // tasks 家族，否则任务轮询会被误判为 MRTR 并报 -32901。
    bool isTaskMethod = reqMethod.rfind("tasks/", 0) == 0;
    bool isInputRequired = result.is_object() && !isTaskMethod &&
        ((result.contains("status") && result["status"] == "input_required") ||
         (result.contains("resultType") && result["resultType"] == "input_required"));

    if (isInputRequired) {
        if (m_mrtrHandler) {
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
            std::weak_ptr<McpStatelessSession> weakSelf =
                std::static_pointer_cast<McpStatelessSession>(shared_from_this());
            m_mrtrHandler(std::to_string(id), inputRequests, reqParams, requestState,
                          [weakSelf, reqMethod, reqParams, inputRequests, requestState, cb](const json& userInputs) {
                if (auto self = weakSelf.lock()) {
                    json inputResponses = normalizeInputResponses(inputRequests, userInputs);
                    self->resendMrtrRequest(reqMethod, reqParams, inputResponses, requestState, cb);
                }
            });
            return true;
        }

        // 无 MRTR handler：无法满足 input_required，回上层报错避免请求悬空
        // （-32901 = 客户端本地错误：MRTR 无 handler；见 kErrorCancelled）
        cb(result, {{"code", kErrorCancelled},
                    {"message", "MRTR input_required received but no MrtrInputHandler is registered"}});
        return true;
    }

    // 通用 resultType 语义（SEP-2575）：
    //   所有结果 MUST 携带 resultType；缺省视为 complete；未知值视为无效。
    //   "task"（SEP-2663 Tasks 扩展）为合法值：tools/call 可返回 CreateTaskResult。
    if (result.is_object() && result.contains("resultType") && result["resultType"].is_string()) {
        const std::string rt = result["resultType"].get<std::string>();
        if (!rt.empty() && rt != kResultTypeComplete && rt != kResultTypeInputRequired && rt != kResultTypeTask) {
            log(LogLevel::Warning, "Unknown resultType '" + rt + "' in response for id=" + std::to_string(id));
            cb(json::object(), {{"code", kErrorUnknownResultType},
                                {"message", "Unknown resultType: " + rt}});
            return true;
        }
    }

    return false;
}

void McpStatelessSession::handleSpecialNotification(const std::string& method, const json& params) {
    // 从 _meta 提取 subscriptionId
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
            m_subscriptions[subscriptionId] = accepted;
        }
        log(LogLevel::Info, "subscriptions/acknowledged: subscriptionId=" + std::to_string(subscriptionId));
    }

    // 订阅通知派发：acknowledged 与其它流通知（resources/updated 等）一并
    // 派发给 setSubscriptionListener 注册的 listener（带 subscriptionId）。
    if (subscriptionId != 0 && m_subscriptionListener) {
        m_subscriptionListener(subscriptionId, method, params);
    }
}

void McpStatelessSession::resendMrtrRequest(const std::string& method, json params,
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

} // namespace mcp
