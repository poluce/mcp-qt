// ScenarioHandlersStateless2026.cpp
// 官方 conformance 2026-07-28 客户端场景处理器（conformance@0.2.0-alpha）
// 实现场景：
//   tools_call(2026-07-28) / request-metadata / sep-2322-client-request-state
//   http-standard-headers / http-custom-headers / http-invalid-tool-headers
//   json-schema-ref-no-deref
//
// 交互方式：conformance 框架启动测试 server，将 server-url 作为 argv 传入，
// 并通过 MCP_CONFORMANCE_SCENARIO / MCP_CONFORMANCE_CONTEXT 环境变量提供场景与上下文。
// 本 runner 以 2026-07-28 stateless HTTP 模式连接 server 执行对应操作。
//
// 注意：QtStatelessHttpTransport 依赖 Qt 事件循环驱动异步回调，因此本文件
// 一律使用 runBlocking + 异步 API，禁止使用 *Sync（会阻塞 main thread 死锁）。

#include "RunnerConfig.h"
#include <mcp_core/McpClientSession.h>
#include <mcp_qt_transport/QtStatelessHttpTransport.h>
#include <nlohmann/json.hpp>
#include <QEventLoop>
#include <QTimer>
#include <functional>
#include <iostream>

namespace mcp_conformance {

using mcp::McpClientSession;
using mcp_qt::QtStatelessHttpTransport;
using nlohmann::json;

// 驱动 Qt 事件循环等待异步回调：在 timeoutMs 内调用 initiate(done)，回调触发 done() 或超时退出。
static bool runBlocking(const std::function<void(const std::function<void()>&)>& initiate, int timeoutMs = 15000) {
    QEventLoop loop;
    bool done = false;
    QTimer::singleShot(timeoutMs, &loop, [&loop]() { loop.quit(); });
    initiate([&loop, &done]() { done = true; loop.quit(); });
    loop.exec();
    return done;
}

// 通用 stateless HTTP 连接（2026-07-28 免握手）
static std::shared_ptr<McpClientSession> connectStateless(const RunnerConfig& config, std::string* errOut = nullptr) {
    auto transport = std::make_shared<QtStatelessHttpTransport>(QString::fromStdString(config.serverUrl));
    transport->setProtocolVersion("2026-07-28");
    auto session = std::make_shared<McpClientSession>(transport);
    session->init();
    if (!session->start()) {
        if (errOut) *errOut = "transport start failed";
        return nullptr;
    }
    session->setStatelessMode(true);
    session->setProtocolVersion("2026-07-28");
    return session;
}

// 安装自动 MRTR handler：对 elicitation/create 表单请求自动接受（布尔字段置 true，其余置字符串）
static void installAutoMrtrHandler(std::shared_ptr<McpClientSession> session) {
    session->setMrtrHandler([](const std::string&,
                               const json& inputRequests,
                               const json&,
                               const std::string&,
                               std::function<void(const json&)> replyCb) {
        json inputResponses = json::object();
        for (auto it = inputRequests.begin(); it != inputRequests.end(); ++it) {
            const std::string& key = it.key();
            const json& req = it.value();
            std::string method = req.contains("method") && req["method"].is_string()
                                     ? req["method"].get<std::string>() : std::string();
            if (method == "elicitation/create") {
                json content = json::object();
                if (req.contains("params") && req["params"].contains("requestedSchema") &&
                    req["params"]["requestedSchema"].contains("properties")) {
                    for (auto p = req["params"]["requestedSchema"]["properties"].begin();
                         p != req["params"]["requestedSchema"]["properties"].end(); ++p) {
                        const json& pSchema = p.value();
                        const std::string ptype = pSchema.contains("type") && pSchema["type"].is_string()
                                                      ? pSchema["type"].get<std::string>() : std::string();
                        content[p.key()] = (ptype == "boolean") ? json(true) : json("conformance-value");
                    }
                }
                inputResponses[key] = {{"action", "accept"}, {"content", content}};
            } else {
                inputResponses[key] = {{"content", json::array()}};
            }
        }
        replyCb(inputResponses);
    });
}

// ========== tools_call（2026-07-28 无状态变体） ==========
int runToolsCall2026(const RunnerConfig& c) {
    std::string err;
    auto session = connectStateless(c, &err);
    if (!session) {
        std::cerr << "[FAIL] tools_call2026 connect: " << err << std::endl;
        return 1;
    }
    bool failed = false;
    json rerr;
    bool ok = runBlocking([&](const std::function<void()>& done) {
        session->callTool("add_numbers", {{"a", 5}, {"b", 3}},
                          [&](const json&, const json& e) { rerr = e; done(); });
    });
    if (!ok) { std::cerr << "[FAIL] tools_call2026 callTool timed out" << std::endl; return 1; }
    if (!rerr.empty()) {
        std::cerr << "[FAIL] tools_call2026 callTool error: " << rerr.dump() << std::endl;
        return 1;
    }
    (void)failed;
    return 0;
}

// ========== request-metadata（SEP-2575） ==========
// 首请求会被模拟 -32022 拒绝，客户端必须用支持的版本重试（sep-2575-client-retry-supported-version）。
int runRequestMetadata(const RunnerConfig& c) {
    std::string err;
    auto session = connectStateless(c, &err);
    if (!session) {
        std::cerr << "[FAIL] request-metadata connect: " << err << std::endl;
        return 1;
    }
    json rerr2;
    // 1) server/discover（可能被 -32022 拒绝一次，重试）
    {
        bool ok = runBlocking([&](const std::function<void()>& done) {
            session->discoverServer([&](const mcp::McpServerDiscovery&, const json& e) {
                if (e.empty()) { done(); return; }
                // -32022：按 data.supported 用支持的版本重试
                session->discoverServer([&](const mcp::McpServerDiscovery&, const json& e2) {
                    rerr2 = e2; done();
                });
            });
        });
        if (!ok) { std::cerr << "[FAIL] request-metadata discover timed out" << std::endl; return 1; }
        if (!rerr2.empty()) {
            std::cerr << "[FAIL] request-metadata discover retry: " << rerr2.dump() << std::endl;
            return 1;
        }
    }
    // 2) tools/list
    {
        bool ok = runBlocking([&](const std::function<void()>& done) {
            session->listTools([&](const std::vector<mcp::McpTool>&, const json& e) { rerr2 = e; done(); });
        });
        if (!ok) { std::cerr << "[FAIL] request-metadata listTools timed out" << std::endl; return 1; }
        if (!rerr2.empty()) {
            std::cerr << "[FAIL] request-metadata listTools error: " << rerr2.dump() << std::endl;
            return 1;
        }
    }
    // 3) tools/call（进一步触发 header/_meta 检查）
    {
        bool ok = runBlocking([&](const std::function<void()>& done) {
            session->callTool("add_numbers", {{"a", 1}, {"b", 2}},
                              [&](const json&, const json& e) { rerr2 = e; done(); });
        });
        if (!ok) { std::cerr << "[FAIL] request-metadata callTool timed out" << std::endl; return 1; }
        if (!rerr2.empty()) {
            std::cerr << "[FAIL] request-metadata callTool error: " << rerr2.dump() << std::endl;
            return 1;
        }
    }
    return 0;
}

// ========== sep-2322-client-request-state（MRTR） ==========
int runSep2322ClientRequestState(const RunnerConfig& c) {
    std::string err;
    auto session = connectStateless(c, &err);
    if (!session) {
        std::cerr << "[FAIL] sep-2322 connect: " << err << std::endl;
        return 1;
    }
    json rerr2;
    installAutoMrtrHandler(session);
    {
        bool ok = runBlocking([&](const std::function<void()>& done) {
            session->listTools([&](const std::vector<mcp::McpTool>&, const json& e) { rerr2 = e; done(); });
        });
        if (!ok) { std::cerr << "[FAIL] sep-2322 listTools timed out" << std::endl; return 1; }
        if (!rerr2.empty()) { std::cerr << "[FAIL] sep-2322 listTools error: " << rerr2.dump() << std::endl; return 1; }
    }
    const char* tools[] = {
        "test_mrtr_echo_state",  // 需回显 requestState + 新 id
        "test_mrtr_no_state",    // 不得带 requestState
        "test_mrtr_unrelated",   // 不得携带其它流的状态
        "test_mrtr_no_result_type" // 无 resultType -> 视为 complete
    };
    for (const char* t : tools) {
        bool ok = runBlocking([&](const std::function<void()>& done) {
            session->callTool(t, json::object(),
                              [&](const json&, const json& e) { rerr2 = e; done(); });
        });
        if (!ok) { std::cerr << "[FAIL] sep-2322 call " << t << " timed out" << std::endl; return 1; }
        if (!rerr2.empty()) { std::cerr << "[FAIL] sep-2322 call " << t << ": " << rerr2.dump() << std::endl; return 1; }
    }
    return 0;
}

// ========== http-standard-headers（SEP-2243） ==========
int runHttpStandardHeaders(const RunnerConfig& c) {
    std::string err;
    auto session = connectStateless(c, &err);
    if (!session) {
        std::cerr << "[FAIL] http-standard-headers connect: " << err << std::endl;
        return 1;
    }
    json rerr2;
    {
        bool ok = runBlocking([&](const std::function<void()>& done) {
            session->listTools([&](const std::vector<mcp::McpTool>&, const json& e) { rerr2 = e; done(); });
        });
        if (!ok) { std::cerr << "[FAIL] http-standard listTools timed out" << std::endl; return 1; }
        if (!rerr2.empty()) { std::cerr << "[FAIL] http-standard listTools error: " << rerr2.dump() << std::endl; return 1; }
    }
    {
        bool ok = runBlocking([&](const std::function<void()>& done) {
            session->callTool("test_headers", json::object(),
                              [&](const json&, const json& e) { rerr2 = e; done(); });
        });
        if (!ok) { std::cerr << "[FAIL] http-standard callTool timed out" << std::endl; return 1; }
        if (!rerr2.empty()) { std::cerr << "[FAIL] http-standard callTool error: " << rerr2.dump() << std::endl; return 1; }
    }
    {
        bool ok = runBlocking([&](const std::function<void()>& done) {
            session->listResources([&](const json&, const json& e) { rerr2 = e; done(); });
        });
        if (!ok) { std::cerr << "[FAIL] http-standard listResources timed out" << std::endl; return 1; }
        if (!rerr2.empty()) { std::cerr << "[FAIL] http-standard listResources error: " << rerr2.dump() << std::endl; return 1; }
    }
    {
        bool ok = runBlocking([&](const std::function<void()>& done) {
            session->readResource("file:///path/to/file%20name.txt",
                                  [&](const json&, const json& e) { rerr2 = e; done(); });
        });
        if (!ok) { std::cerr << "[FAIL] http-standard readResource timed out" << std::endl; return 1; }
        if (!rerr2.empty()) { std::cerr << "[FAIL] http-standard readResource error: " << rerr2.dump() << std::endl; return 1; }
    }
    {
        bool ok = runBlocking([&](const std::function<void()>& done) {
            session->listPrompts([&](const json&, const json& e) { rerr2 = e; done(); });
        });
        if (!ok) { std::cerr << "[FAIL] http-standard listPrompts timed out" << std::endl; return 1; }
        if (!rerr2.empty()) { std::cerr << "[FAIL] http-standard listPrompts error: " << rerr2.dump() << std::endl; return 1; }
    }
    {
        bool ok = runBlocking([&](const std::function<void()>& done) {
            session->getPrompt("test_prompt", json::object(),
                               [&](const json&, const json& e) { rerr2 = e; done(); });
        });
        if (!ok) { std::cerr << "[FAIL] http-standard getPrompt timed out" << std::endl; return 1; }
        if (!rerr2.empty()) { std::cerr << "[FAIL] http-standard getPrompt error: " << rerr2.dump() << std::endl; return 1; }
    }
    return 0;
}

// ========== http-custom-headers（SEP-2243 x-mcp-header 镜像 + 编码） ==========
int runHttpCustomHeaders(const RunnerConfig& c) {
    std::string err;
    auto session = connectStateless(c, &err);
    if (!session) {
        std::cerr << "[FAIL] http-custom-headers connect: " << err << std::endl;
        return 1;
    }
    json rerr2;
    // 先拉工具列表以缓存 x-mcp-header 注解 schema
    {
        bool ok = runBlocking([&](const std::function<void()>& done) {
            session->listTools([&](const std::vector<mcp::McpTool>&, const json& e) { rerr2 = e; done(); });
        });
        if (!ok) { std::cerr << "[FAIL] http-custom listTools timed out" << std::endl; return 1; }
        if (!rerr2.empty()) { std::cerr << "[FAIL] http-custom listTools error: " << rerr2.dump() << std::endl; return 1; }
    }

    // 上下文提供精确调用参数（MCP_CONFORMANCE_CONTEXT.toolCalls）。
    // 注意：conformance 在 Windows shell:true 下传递含中文/换行的 context 环境变量可能丢失，
    // 因此当 context 为空时回退到与 conformance 场景一致的硬编码参数。
    json toolCalls = c.context.value("toolCalls", json::array());
    if (toolCalls.empty()) {
        toolCalls = json::array();
        toolCalls.push_back({
            {"name", "test_custom_headers"},
            {"arguments", {
                {"region", "us-west1"},
                {"priority", 42},
                {"verbose", false},
                {"debug", true},
                {"empty_val", ""},
                {"method_val", "test-method"},
                {"float_val", 3.14159},
                {"non_ascii_val", u8"Hello, 世界"},
                {"whitespace_val", u8" padded "},
                {"leading_space_val", u8" us-west1"},
                {"trailing_space_val", u8"us-west1 "},
                {"internal_space_val", u8"us west 1"},
                {"control_char_val", "line1\nline2"},
                {"crlf_val", "line1\r\nline2"},
                {"tab_val", "\tindented"},
                {"query", "SELECT * FROM users"}
            }}
        });
        toolCalls.push_back({
            {"name", "test_custom_headers_null"},
            {"arguments", {
                {"region", "us-east1"},
                {"priority", 1},
                {"verbose", nullptr},
                {"query", "SELECT 1"}
            }}
        });
    }
    for (const auto& tc : toolCalls) {
        const std::string name = tc.value("name", std::string());
        const json args = tc.value("arguments", json::object());
        bool ok = runBlocking([&](const std::function<void()>& done) {
            session->callTool(name, args,
                              [&](const json&, const json& e) { rerr2 = e; done(); });
        });
        if (!ok) { std::cerr << "[FAIL] http-custom call " << name << " timed out" << std::endl; return 1; }
        if (!rerr2.empty()) { std::cerr << "[FAIL] http-custom call " << name << ": " << rerr2.dump() << std::endl; return 1; }
    }
    return 0;
}

// ========== http-invalid-tool-headers（SEP-2243 非法注解工具剔除） ==========
int runHttpInvalidToolHeaders(const RunnerConfig& c) {
    std::string err;
    auto session = connectStateless(c, &err);
    if (!session) {
        std::cerr << "[FAIL] http-invalid-tool-headers connect: " << err << std::endl;
        return 1;
    }
    json rerr2;
    // tools/list：合法工具保留，非法 x-mcp-header 工具应被剔除
    {
        bool ok = runBlocking([&](const std::function<void()>& done) {
            session->listTools([&](const std::vector<mcp::McpTool>&, const json& e) { rerr2 = e; done(); });
        });
        if (!ok) { std::cerr << "[FAIL] http-invalid listTools timed out" << std::endl; return 1; }
        if (!rerr2.empty()) { std::cerr << "[FAIL] http-invalid listTools error: " << rerr2.dump() << std::endl; return 1; }
    }
    // 调用 valid_tool 证明合法工具可用
    {
        bool ok = runBlocking([&](const std::function<void()>& done) {
            session->callTool("valid_tool", {{"region", "us-west1"}},
                              [&](const json&, const json& e) { rerr2 = e; done(); });
        });
        if (!ok) { std::cerr << "[FAIL] http-invalid call valid_tool timed out" << std::endl; return 1; }
        if (!rerr2.empty()) { std::cerr << "[FAIL] http-invalid call valid_tool error: " << rerr2.dump() << std::endl; return 1; }
    }
    return 0;
}

// ========== json-schema-ref-no-deref（SEP-2106） ==========
// 仅需 tools/list；不得自动拉取工具 schema 中的网络 $ref（canary URL）。
int runJsonSchemaRefNoDeref(const RunnerConfig& c) {
    std::string err;
    auto session = connectStateless(c, &err);
    if (!session) {
        std::cerr << "[FAIL] json-schema-ref-no-deref connect: " << err << std::endl;
        return 1;
    }
    json rerr2;
    {
        bool ok = runBlocking([&](const std::function<void()>& done) {
            session->listTools([&](const std::vector<mcp::McpTool>&, const json& e) { rerr2 = e; done(); });
        });
        if (!ok) { std::cerr << "[FAIL] json-schema-ref-no-deref listTools timed out" << std::endl; return 1; }
        if (!rerr2.empty()) { std::cerr << "[FAIL] json-schema-ref-no-deref listTools error: " << rerr2.dump() << std::endl; return 1; }
    }
    return 0;
}

} // namespace mcp_conformance
