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

    // 注册 MRTR 处理回调
    session->setMrtrHandler([](const nlohmann::json& schema, const nlohmann::json& requestParams, std::function<void(const nlohmann::json& userInputs)> replyCb) {
        nlohmann::json response = nlohmann::json::object();
        if (schema.contains("properties")) {
            for (auto it = schema["properties"].begin(); it != schema["properties"].end(); ++it) {
                response[it.key()] = "conformance-value";
            }
        }
        replyCb(response);
    });

    // 免 initializeSync 握手直接调用 listTools
    nlohmann::json err;
    session->listToolsSync(std::chrono::milliseconds(10000), &err);
    return err.empty() ? 0 : 1;
}

} // namespace mcp_conformance
