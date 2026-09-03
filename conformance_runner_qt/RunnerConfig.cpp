#include "RunnerConfig.h"

namespace mcp_conformance {

bool parseRunnerConfig(
    int argc,
    const char* const* argv,
    const std::string& scenarioEnv,
    const std::string& contextEnv,
    RunnerConfig* outConfig
) {
    if (!outConfig) return false;

    RunnerConfig cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg.rfind("http://", 0) == 0 || arg.rfind("https://", 0) == 0) {
            cfg.serverUrl = arg;
        } else if ((arg == "--protocol" || arg == "-protocol") && i + 1 < argc) {
            cfg.protocolVersion = argv[++i];
        } else if (arg == "--stateless" || arg == "-stateless") {
            cfg.protocolVersion = "2026-07-28";
        } else if ((arg == "--scenario" || arg == "-scenario") && i + 1 < argc) {
            // argv 场景名（WSL 互操作不传递 WSL 侧 env 给 Windows 进程，
            // 由 run_conformance_wsl.sh 包装脚本把 MCP_CONFORMANCE_SCENARIO 转成 argv）
            cfg.scenario = argv[++i];
        } else if ((arg == "--context" || arg == "-context") && i + 1 < argc) {
            cfg.context = nlohmann::json::parse(argv[++i], nullptr, false);
            if (cfg.context.is_discarded()) {
                cfg.context = nlohmann::json::object();
            }
        }
    }

    if (cfg.protocolVersion.empty()) {
        const char* protoEnv = std::getenv("MCP_PROTOCOL_VERSION");
        if (protoEnv && *protoEnv) {
            cfg.protocolVersion = protoEnv;
        }
    }

    // 场景名优先级：argv（--scenario）> env（MCP_CONFORMANCE_SCENARIO）
    if (cfg.scenario.empty()) {
        cfg.scenario = scenarioEnv;
    }
    if (cfg.context.is_null() && !contextEnv.empty()) {
        cfg.context = nlohmann::json::parse(contextEnv, nullptr, false);
        if (cfg.context.is_discarded()) {
            cfg.context = nlohmann::json::object();
        }
    }

    if (cfg.scenario.empty()) {
        return false;
    }

    *outConfig = std::move(cfg);
    return true;
}

std::string usageText() {
    return "Usage: mcp_client_conformance <server-url>\n"
           "The MCP_CONFORMANCE_SCENARIO env var must be set by the official conformance runner.";
}

} // namespace mcp_conformance
