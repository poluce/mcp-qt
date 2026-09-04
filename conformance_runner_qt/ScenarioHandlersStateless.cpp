#include "RunnerConfig.h"
#include "ConsoleStdioTransport.h"
#include <mcp_core/McpClientSession.h>
#include <nlohmann/json.hpp>
#include <iostream>

namespace mcp_conformance {

int runStateless20260728(const RunnerConfig& config) {
    auto transport = std::make_shared<mcp::ConsoleStdioTransport>();
    auto session = std::make_shared<mcp::McpClientSession>(transport);
    session->init();
    if (!session->start()) return 1;

    // 启用 2026-07-28 无状态模式
    session->setStatelessMode(true);
    session->setProtocolVersion("2026-07-28");

    std::cout << "[2026-07-28 Verified] Negotiated Protocol Version: " << session->getNegotiatedProtocolVersion() << std::endl;
    std::cout << "[2026-07-28 Verified] Is Stateless Mode: " << (session->isStatelessMode() ? "TRUE" : "FALSE") << std::endl;

    // 注册 MRTR 处理回调（2026-07-28 规范 InputRequests map + requestState）
    session->setMrtrHandler([](const std::string& requestId,
                               const nlohmann::json& inputRequests,
                               const nlohmann::json& requestParams,
                               const std::string& requestState,
                               std::function<void(const nlohmann::json&)> replyCb) {
        nlohmann::json inputResponses = nlohmann::json::object();
        for (auto it = inputRequests.begin(); it != inputRequests.end(); ++it) {
            const std::string& key = it.key();
            const nlohmann::json& req = it.value();
            std::string method = req.contains("method") && req["method"].is_string()
                                     ? req["method"].get<std::string>()
                                     : std::string();
            if (method == "elicitation/create" &&
                req.contains("params") && req["params"].contains("requestedSchema") &&
                req["params"]["requestedSchema"].contains("properties")) {
                nlohmann::json content = nlohmann::json::object();
                for (auto p = req["params"]["requestedSchema"]["properties"].begin();
                     p != req["params"]["requestedSchema"]["properties"].end(); ++p) {
                    content[p.key()] = "conformance-value";
                }
                inputResponses[key] = {{"action", "accept"}, {"content", content}};
            } else {
                inputResponses[key] = {{"content", nlohmann::json::array()}};
            }
        }
        replyCb(inputResponses);
    });

    // 免 initializeSync 握手直接调用 listTools
    nlohmann::json err;
    session->listToolsSync(std::chrono::milliseconds(10000), &err);
    return err.empty() ? 0 : 1;
}

} // namespace mcp_conformance
