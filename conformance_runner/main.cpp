#include <iostream>
#include <string>
#include <cstdlib>
#include <memory>
#include <chrono>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include <mcp_core/McpClientSession.h>
#include <mcp_core/HttpSseTransport.h>
#include <mcp_core/ConsoleStdioTransport.h>

#ifdef _WIN32
#include <windows.h>
#endif

// 获取环境变量
static std::string getEnv(const std::string& key, const std::string& defaultVal = "") {
    const char* val = std::getenv(key.c_str());
    return val ? std::string(val) : defaultVal;
}

// 纯 C++ 的 HTTP 场景运行函数
static int runHttpMode(const std::string& serverUrl, const std::string& scenario, const std::string& contextStr) {
    nlohmann::json ctx = nlohmann::json::parse(contextStr, nullptr, false);
    if (ctx.is_discarded()) {
        ctx = nlohmann::json::object();
    }

    std::cerr << "[Conformance] HTTP mode, scenario: " << scenario << ", url: " << serverUrl << std::endl;

    auto transport = std::make_shared<mcp::HttpSseTransport>(serverUrl);
    auto session = std::make_shared<mcp::McpClientSession>(transport);

    session->setLogCallback([](mcp::LogLevel level, const std::string& msg) {
        if (level >= mcp::LogLevel::Warning) {
            std::cerr << "[SDK] " << msg << std::endl;
        }
    });

    transport->setOnError([](const std::string& err) {
        std::cerr << "[Transport Error] " << err << std::endl;
    });

    if (scenario == "elicitation-sep1034-client-defaults") {
        session->setElicitationHandler([](const nlohmann::json& params) -> nlohmann::json {
            std::cerr << "[Conformance] Elicitation invoked: " << params.dump() << std::endl;
            return {
                {"action", "accept"},
                {"content", nlohmann::json::object()}
            };
        });
    }

    session->init();
    if (!session->start()) {
        std::cerr << "[Conformance] Failed to start HTTP transport" << std::endl;
        return 1;
    }

    // 给后台 SSE 建立连接并获取 MCP-Session-Id 预留时间
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // 执行初始化握手
    nlohmann::json serverInfo;
    if (!session->initializeSync("mcp-conformance-client-cpp", "1.0.0", &serverInfo)) {
        std::cerr << "[Conformance] Initialize handshake failed" << std::endl;
        return 1;
    }
    std::cerr << "[Conformance] Initialize OK" << std::endl;

    bool scenarioOk = true;

    if (scenario == "initialize") {
        // 仅完成握手即可
    } else if (scenario == "tools_list") {
        nlohmann::json err;
        auto tools = session->listToolsSync(std::chrono::milliseconds(10000), &err);
        if (!err.empty()) {
            std::cerr << "[Conformance] listTools failed: " << err.dump() << std::endl;
            scenarioOk = false;
        } else {
            std::cerr << "[Conformance] listed " << tools.size() << " tools" << std::endl;
        }
    } else if (scenario == "tools_call" || scenario == "sse_retry" || scenario == "sse-retry" || scenario == "elicitation-sep1034-client-defaults") {
        nlohmann::json err;
        auto tools = session->listToolsSync(std::chrono::milliseconds(10000), &err);
        if (!err.empty()) {
            std::cerr << "[Conformance] listTools failed: " << err.dump() << std::endl;
            scenarioOk = false;
        } else {
            std::string name = ctx.value("name", "");
            if (name.empty() && !tools.empty()) {
                name = tools[0].name;
            }
            nlohmann::json arguments = ctx.value("arguments", nlohmann::json::object());
            nlohmann::json callErr;
            nlohmann::json callResp = session->callToolSync(name, arguments, &callErr, std::chrono::milliseconds(10000));
            if (!callErr.dump().empty() && !callErr.is_null() && !callErr.empty()) {
                std::cerr << "[Conformance] callTool failed: " << callErr.dump() << std::endl;
                scenarioOk = false;
            }
        }
    } else if (scenario == "resources_list") {
        nlohmann::json err;
        nlohmann::json resp = session->listResourcesSync(std::chrono::milliseconds(10000), &err);
        if (!err.empty()) {
            std::cerr << "[Conformance] listResources failed: " << err.dump() << std::endl;
            scenarioOk = false;
        }
    } else if (scenario == "prompts_list") {
        nlohmann::json err;
        nlohmann::json resp = session->listPromptsSync(std::chrono::milliseconds(10000), &err);
        if (!err.empty()) {
            std::cerr << "[Conformance] listPrompts failed: " << err.dump() << std::endl;
            scenarioOk = false;
        }
    } else if (scenario == "shutdown") {
        if (!session->shutdownSync(std::chrono::milliseconds(5000))) {
            std::cerr << "[Conformance] shutdown failed" << std::endl;
            scenarioOk = false;
        }
    } else {
        std::cerr << "[Conformance] unknown scenario: " << scenario << std::endl;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::cerr << "[Conformance] scenario " << scenario
              << (scenarioOk ? " PASSED" : " FAILED") << std::endl;
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), scenarioOk ? 0 : 1);
#else
    ::_exit(scenarioOk ? 0 : 1);
#endif
}

// 纯 C++ 的 Stdio 场景运行函数
static int runStdioMode(const std::string& scenario, const std::string& contextStr) {
    nlohmann::json ctx = nlohmann::json::parse(contextStr, nullptr, false);
    if (ctx.is_discarded()) {
        ctx = nlohmann::json::object();
    }
    std::cerr << "[Conformance] Stdio mode, scenario: " << scenario << std::endl;

    auto transport = std::make_shared<mcp::ConsoleStdioTransport>();
    auto session   = std::make_shared<mcp::McpClientSession>(transport);

    session->setLogCallback([](mcp::LogLevel level, const std::string& msg) {
        if (level >= mcp::LogLevel::Warning) std::cerr << "[SDK] " << msg << std::endl;
    });

    session->init();
    if (!session->start()) {
        std::cerr << "[Conformance] Failed to start stdio transport" << std::endl;
        return 1;
    }

    nlohmann::json serverInfo;
    if (!session->initializeSync("mcp-conformance-client-cpp", "1.0.0", &serverInfo)) {
        std::cerr << "[Conformance] Initialize handshake failed" << std::endl;
        return 1;
    }
    std::cerr << "[Conformance] Initialize OK" << std::endl;

    bool scenarioOk = true;
    if (scenario == "tools_list") {
        nlohmann::json err;
        auto tools = session->listToolsSync(std::chrono::milliseconds(10000), &err);
        if (!err.empty()) scenarioOk = false;
    } else if (scenario == "tools_call") {
        nlohmann::json err;
        auto tools = session->listToolsSync(std::chrono::milliseconds(10000), &err);
        if (!err.empty()) scenarioOk = false;
        else {
            std::string name = ctx.value("name", "");
            if (name.empty() && !tools.empty()) name = tools[0].name;
            nlohmann::json arguments = ctx.value("arguments", nlohmann::json::object());
            nlohmann::json callErr;
            session->callToolSync(name, arguments, &callErr, std::chrono::milliseconds(10000));
            if (!callErr.empty()) scenarioOk = false;
        }
    } else if (scenario == "resources_list") {
        nlohmann::json err;
        session->listResourcesSync(std::chrono::milliseconds(10000), &err);
        if (!err.empty()) scenarioOk = false;
    } else if (scenario == "prompts_list") {
        nlohmann::json err;
        session->listPromptsSync(std::chrono::milliseconds(10000), &err);
        if (!err.empty()) scenarioOk = false;
    } else if (scenario == "shutdown") {
        if (!session->shutdownSync(std::chrono::milliseconds(5000))) scenarioOk = false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::cerr << "[Conformance] scenario " << scenario
              << (scenarioOk ? " PASSED" : " FAILED") << std::endl;
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), scenarioOk ? 0 : 1);
#else
    ::_exit(scenarioOk ? 0 : 1);
#endif
}

int main(int argc, char* argv[]) {
    std::freopen("F:\\B_My_Document\\GitHub\\mcp-cpp-agent\\sdk_debug.log", "w", stderr);
    std::setvbuf(stdout, NULL, _IONBF, 0);
    std::setvbuf(stderr, NULL, _IONBF, 0);

    bool isConformance = false;
    std::string serverUrl;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg.rfind("http://", 0) == 0 || arg.rfind("https://", 0) == 0) {
            serverUrl = arg;
            isConformance = true;
        }
    }

    std::string scenario = getEnv("MCP_CONFORMANCE_SCENARIO", "");
    if (!scenario.empty()) {
        isConformance = true;
    }

    if (!isConformance) {
        std::cerr << "Usage: mcp_client_conformance <server-url>" << std::endl;
        return 1;
    }

    std::string context  = getEnv("MCP_CONFORMANCE_CONTEXT", "{}");

    if (!serverUrl.empty()) {
        return runHttpMode(serverUrl, scenario, context);
    }

    return runStdioMode(scenario, context);
}
