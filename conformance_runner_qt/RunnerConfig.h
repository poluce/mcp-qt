#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace mcp_conformance {

struct RunnerConfig {
    std::string serverUrl;
    std::string scenario;
    // 注意：不能用 {nlohmann::json::object()} 花括号初始化——json 的 initializer_list
    // 构造路径会把它变成 [{}] 数组，导致 is_null() 恒为 false、context 环境变量永不解析。
    nlohmann::json context;
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
