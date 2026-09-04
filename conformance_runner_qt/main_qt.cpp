#include "RunnerConfig.h"
#include "ScenarioRegistry.h"

#include <QCoreApplication>

#include <cstdlib>
#include <iostream>

static std::string getEnv(const std::string& key, const std::string& defaultVal = "") {
    const char* val = std::getenv(key.c_str());
    return val ? std::string(val) : defaultVal;
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    std::setvbuf(stdout, NULL, _IONBF, 0);
    std::setvbuf(stderr, NULL, _IONBF, 0);

    const std::string scenario = getEnv("MCP_CONFORMANCE_SCENARIO", "");
    const std::string context  = getEnv("MCP_CONFORMANCE_CONTEXT", "{}");
    const std::string protocolVersion = getEnv("MCP_CONFORMANCE_PROTOCOL_VERSION", "");

    mcp_conformance::RunnerConfig config;
    if (!mcp_conformance::parseRunnerConfig(argc, argv, scenario, context, &config)) {
        std::cerr << mcp_conformance::usageText() << std::endl;
        return 1;
    }
    // harness 0.1.16 用 `a ?? '2025-11-25'` 传版本，空串时 ?? 不回退 → 拿到空串。
    // 空串视为未指定，回退到 harness 期望的 2025-11-25，使客户端提议版本落入 harness 支持列表。
    // 注意：argv（--protocol，WSL 包装脚本传入）优先于环境变量——WSL 互操作不传递
    // WSL 侧 env 给 Windows 进程，环境变量在此恒为空，直接覆盖会丢掉 argv 的版本。
    if (config.protocolVersion.empty()) {
        config.protocolVersion = protocolVersion.empty() ? "2025-11-25" : protocolVersion;
    }

    return mcp_conformance::runScenario(config);
}
