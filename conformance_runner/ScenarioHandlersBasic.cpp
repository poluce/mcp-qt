#include "RunnerConfig.h"

#include <iostream>
#include <fstream>
#include <mcp_core/HttpSseTransport.h>
#include <mcp_core/McpClientSession.h>

namespace mcp_conformance {

static std::shared_ptr<mcp::McpClientSession> makeHttpSession(const RunnerConfig& config) {
    auto transport = std::make_shared<mcp::HttpSseTransport>(config.serverUrl);
    auto session = std::make_shared<mcp::McpClientSession>(transport);
    if (!config.protocolVersion.empty()) {
        session->setProtocolVersion(config.protocolVersion);
    }
    session->init();
    if (!session->start()) {
        throw std::runtime_error("Failed to start HTTP transport");
    }
    return session;
}

int runInitialize(const RunnerConfig& config) {
    auto session = makeHttpSession(config);
    nlohmann::json serverInfo;
    if (!session->initializeSync("mcp-conformance-client-cpp", "1.0.0", &serverInfo)) return 1;

    // Match TS reference: initialize also lists tools
    nlohmann::json err;
    session->listToolsSync(std::chrono::milliseconds(10000), &err);
    return err.empty() ? 0 : 1;
}

int runToolsCall(const RunnerConfig& config) {
    auto session = makeHttpSession(config);
    nlohmann::json serverInfo;
    if (!session->initializeSync("mcp-conformance-client-cpp", "1.0.0", &serverInfo)) return 1;

    nlohmann::json err;
    auto tools = session->listToolsSync(std::chrono::milliseconds(10000), &err);
    if (!err.empty() || tools.empty()) return 1;

    std::string name = "add_numbers";
    nlohmann::json arguments = {{"a", 5}, {"b", 3}};
    nlohmann::json callErr;
    session->callToolSync(name, arguments, &callErr, std::chrono::milliseconds(10000));
    return callErr.empty() ? 0 : 1;
}

int runSseRetry(const RunnerConfig& config) {
    auto session = makeHttpSession(config);
    nlohmann::json serverInfo;
    if (!session->initializeSync("mcp-conformance-client-cpp", "1.0.0", &serverInfo)) return 1;

    nlohmann::json err;
    auto tools = session->listToolsSync(std::chrono::milliseconds(10000), &err);
    if (!err.empty()) return 1;

    nlohmann::json callErr;
    session->callToolSync("test_reconnection", nlohmann::json::object(), &callErr, std::chrono::milliseconds(10000));
    return callErr.empty() ? 0 : 1;
}

int runElicitationDefaults(const RunnerConfig& config) {
    auto session = makeHttpSession(config);
    session->registerCapabilities({
        {"elicitation", {
            {"form", {
                {"applyDefaults", true}
            }}
        }}
    });

    session->setElicitationHandler([](const nlohmann::json& req, std::function<void(const nlohmann::json&, const nlohmann::json&)> cb) {
        std::ofstream ofs("elicitation_req.json");
        ofs << req.dump(4);
        ofs.close();
        nlohmann::json result = {
            {"action", "accept"},
            {"content", nlohmann::json::object()}
        };
        cb(result, nlohmann::json::object());
    });

    nlohmann::json serverInfo;
    if (!session->initializeSync("mcp-conformance-client-cpp", "1.0.0", &serverInfo)) return 1;

    nlohmann::json callErr;
    session->callToolSync("test_client_elicitation_defaults", nlohmann::json::object(), &callErr, std::chrono::milliseconds(10000));
    return callErr.empty() ? 0 : 1;
}

} // namespace mcp_conformance
