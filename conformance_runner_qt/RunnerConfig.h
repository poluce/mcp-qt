#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace mcp_conformance {

struct RunnerConfig {
    std::string serverUrl;
    std::string scenario;
    nlohmann::json context{nlohmann::json::object()};
    std::string protocolVersion;
};

bool parseRunnerConfig(
    int argc,
    const char* const* argv,
    const std::string& scenarioEnv,
    const std::string& contextEnv,
    RunnerConfig* outConfig
);

std::string usageText();

} // namespace mcp_conformance
