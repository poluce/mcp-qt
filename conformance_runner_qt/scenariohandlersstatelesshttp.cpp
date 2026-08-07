#include "RunnerConfig.h"
#include <mcp_core/McpClientSession.h>
#include <mcp_qt_transport/QtStatelessHttpTransport.h>
#include <nlohmann/json.hpp>
#include <QEventLoop>
#include <QTimer>
#include <functional>
#include <iostream>

namespace mcp_conformance {

// 驱动 Qt 事件循环等待异步回调：在 timeoutMs 内调用 initiate(done)，回调触发 done() 或超时退出。
// 返回是否在超时前完成。
static bool runBlocking(const std::function<void(const std::function<void()>&)>& initiate, int timeoutMs = 10000) {
    QEventLoop loop;
    bool done = false;
    QTimer::singleShot(timeoutMs, &loop, [&loop]() { loop.quit(); });
    initiate([&loop, &done]() { done = true; loop.quit(); });
    loop.exec();
    return done;
}

// 2026-07-28 无状态 HTTP 端到端场景：
//   server/discover -> tools/list -> tools/call -> MRTR approve_delete
// 连接 test_mcp_server_2026.js（默认 http://127.0.0.1:3001/mcp）。
int runStateless20260728Http(const RunnerConfig& config) {
    std::string url = config.serverUrl.empty() ? "http://127.0.0.1:3001/mcp" : config.serverUrl;

    auto transport = std::make_shared<mcp_qt::QtStatelessHttpTransport>(QString::fromStdString(url));
    transport->setProtocolVersion("2026-07-28");

    auto session = std::make_shared<mcp::McpClientSession>(transport);
    session->init();
    if (!session->start()) {
        std::cerr << "[FAIL] transport start failed" << std::endl;
        return 1;
    }
    // 2026-07-28 无状态模式：免 initialize 握手
    session->setStatelessMode(true);
    session->setProtocolVersion("2026-07-28");

    // ---------- 1. server/discover（SEP-2575） ----------
    mcp::McpServerDiscovery discovery;
    nlohmann::json err;
    bool ok = runBlocking([&session, &discovery, &err](const std::function<void()>& done) {
        session->discoverServer([&discovery, &err, done](const mcp::McpServerDiscovery& info, const nlohmann::json& e) {
            discovery = info;
            err = e;
            done();
        });
    });
    if (!ok) {
        std::cerr << "[FAIL] server/discover timed out" << std::endl;
        return 1;
    }
    if (!err.empty()) {
        std::cerr << "[FAIL] server/discover error: " << err.dump() << std::endl;
        return 1;
    }
    bool has2026 = false;
    for (const auto& v : discovery.supportedVersions) {
        if (v == "2026-07-28") has2026 = true;
    }
    if (!has2026) {
        std::cerr << "[FAIL] supportedVersions does not contain 2026-07-28" << std::endl;
        return 1;
    }
    if (discovery.serverInfo.empty()) {
        std::cerr << "[FAIL] serverInfo empty in discover response" << std::endl;
        return 1;
    }
    std::string serverName = discovery.serverInfo.contains("name") && discovery.serverInfo["name"].is_string()
        ? discovery.serverInfo["name"].get<std::string>() : std::string();
    std::cout << "[PASS] server/discover: supportedVersions includes 2026-07-28, serverInfo.name=" << serverName << std::endl;

    // ---------- 2. tools/list ----------
    std::vector<mcp::McpTool> tools;
    ok = runBlocking([&session, &tools, &err](const std::function<void()>& done) {
        session->listTools([&tools, &err, done](const std::vector<mcp::McpTool>& toolList, const nlohmann::json& e) {
            tools = toolList;
            err = e;
            done();
        });
    });
    if (!ok) {
        std::cerr << "[FAIL] tools/list timed out" << std::endl;
        return 1;
    }
    if (!err.empty() || tools.empty()) {
        std::cerr << "[FAIL] tools/list error: " << (err.empty() ? "empty tool list" : err.dump()) << std::endl;
        return 1;
    }
    std::cout << "[PASS] tools/list returned " << tools.size() << " tools" << std::endl;

    // ---------- 3. tools/call calculate_add ----------
    nlohmann::json callResult;
    ok = runBlocking([&session, &callResult, &err](const std::function<void()>& done) {
        session->callTool("calculate_add", {{"a", 2}, {"b", 40}},
                          [&callResult, &err, done](const nlohmann::json& content, const nlohmann::json& e) {
                              callResult = content;
                              err = e;
                              done();
                          });
    });
    if (!ok) {
        std::cerr << "[FAIL] tools/call timed out" << std::endl;
        return 1;
    }
    if (!err.empty()) {
        std::cerr << "[FAIL] tools/call error: " << err.dump() << std::endl;
        return 1;
    }
    bool complete = callResult.contains("resultType") && callResult["resultType"] == "complete";
    if (!complete) {
        std::cerr << "[FAIL] tools/call result missing resultType=complete: " << callResult.dump() << std::endl;
        return 1;
    }
    std::cout << "[PASS] tools/call calculate_add -> resultType=complete" << std::endl;

    // ---------- 4. MRTR approve_delete（SEP-2322） ----------
    // 注册 MRTR handler：自动确认输入请求（elicitation/create -> inputResponses）
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
                                     ? req["method"].get<std::string>() : std::string();
            if (method == "elicitation/create" &&
                req.contains("params") && req["params"].contains("requestedSchema") &&
                req["params"]["requestedSchema"].contains("properties")) {
                nlohmann::json content = nlohmann::json::object();
                for (auto p = req["params"]["requestedSchema"]["properties"].begin();
                     p != req["params"]["requestedSchema"]["properties"].end(); ++p) {
                    // 自动接受所有字段（confirm 布尔 -> true）
                    if (p.key() == "confirm") {
                        content[p.key()] = true;
                    } else {
                        content[p.key()] = "auto-accept";
                    }
                }
                inputResponses[key] = {{"action", "accept"}, {"content", content}};
            } else {
                inputResponses[key] = {{"content", nlohmann::json::array()}};
            }
        }
        replyCb(inputResponses);
    });

    nlohmann::json mrtrResult;
    ok = runBlocking([&session, &mrtrResult, &err](const std::function<void()>& done) {
        session->callTool("approve_delete", {{"target", "prod-db"}},
                          [&mrtrResult, &err, done](const nlohmann::json& content, const nlohmann::json& e) {
                              mrtrResult = content;
                              err = e;
                              done();
                          });
    });
    if (!ok) {
        std::cerr << "[FAIL] approve_delete MRTR flow timed out" << std::endl;
        return 1;
    }
    if (!err.empty()) {
        std::cerr << "[FAIL] approve_delete MRTR flow error: " << err.dump() << std::endl;
        return 1;
    }
    bool mrtrComplete = mrtrResult.contains("resultType") && mrtrResult["resultType"] == "complete";
    if (!mrtrComplete) {
        std::cerr << "[FAIL] approve_delete did not complete after MRTR round-trip: "
                  << mrtrResult.dump() << std::endl;
        return 1;
    }
    std::cout << "[PASS] MRTR approve_delete: input_required -> resend with inputResponses -> complete" << std::endl;

    std::cout << "=== 2026-07-28 stateless HTTP end-to-end: ALL PASS ===" << std::endl;
    session->close();
    return 0;
}

} // namespace mcp_conformance
