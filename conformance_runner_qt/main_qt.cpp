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
    config.protocolVersion = protocolVersion;

    return mcp_conformance::runScenario(config);
}
