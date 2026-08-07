#include "tests/common.h"
#include "mcp_core/McpClientSession.h"
#include "mcp_core/IMcpTransport.h"
#include <nlohmann/json.hpp>
#include <string>
#include <memory>
#include <vector>

class StatelessMockTransport : public mcp::IMcpTransport {
public:
    std::vector<std::string> sentMessages;
    std::function<void(const std::string&)> onMsgCb;

    bool send(const std::string& message) override {
        sentMessages.push_back(message);
        return true;
    }
    void setOnMessage(std::function<void(const std::string&)> cb) override {
        onMsgCb = cb;
    }
    void setOnClose(std::function<void()>) override {}
    void setOnError(std::function<void(const std::string&)>) override {}
    bool start() override { return true; }
    void close() override {}
};

void test_qt_stateless_session_meta_injection() {
    auto mockTransport = std::make_shared<StatelessMockTransport>();
    auto session = std::make_shared<mcp::McpClientSession>(mockTransport);
    session->init();
    session->start();

    // 默认未初始化状态下，isReady 为 false
    TM_ASSERT_TRUE(!session->isReady(), "default uninitialized session should not be ready");

    // 启用 2026-07-28 无状态模式
    session->setStatelessMode(true);
    TM_ASSERT_TRUE(session->isStatelessMode(), "stateless mode should be enabled");
    TM_ASSERT_TRUE(session->isReady(), "stateless session should be ready without initialize handshake");

    // 发起 listTools 请求（无需先调用 initialize）
    session->listTools([](const std::vector<mcp::McpTool>&, const nlohmann::json&) {});

    TM_ASSERT_TRUE(mockTransport->sentMessages.size() == 1, "one request message should be sent over transport");

    // 解析发出的 JSON-RPC 消息
    auto reqJson = nlohmann::json::parse(mockTransport->sentMessages.back());
    TM_ASSERT_TRUE(reqJson["method"] == "tools/list", "method should be tools/list");
    TM_ASSERT_TRUE(reqJson.contains("params"), "request should contain params object");
    TM_ASSERT_TRUE(reqJson["params"].contains("_meta"), "params should contain _meta in stateless mode");

    auto meta = reqJson["params"]["_meta"];
    TM_ASSERT_TRUE(meta.contains("protocolVersion"), "_meta should contain protocolVersion");
    TM_ASSERT_TRUE(meta["protocolVersion"] == "2026-07-28", "default stateless protocolVersion should be 2026-07-28");
    TM_ASSERT_TRUE(meta.contains("clientInfo"), "_meta should contain clientInfo");
    TM_ASSERT_TRUE(meta["clientInfo"]["name"] == "mcp-qt-client", "clientInfo name should match");

    // 边界条件 1：校验标准规范命名空间全称 Key 双写
    TM_ASSERT_TRUE(meta.contains("io.modelcontextprotocol/protocolVersion"), "_meta should contain io.modelcontextprotocol/protocolVersion");
    TM_ASSERT_TRUE(meta["io.modelcontextprotocol/protocolVersion"] == "2026-07-28", "full namespace protocolVersion should match");
    TM_ASSERT_TRUE(meta.contains("io.modelcontextprotocol/clientInfo"), "_meta should contain io.modelcontextprotocol/clientInfo");
    TM_ASSERT_TRUE(meta["io.modelcontextprotocol/clientInfo"]["name"] == "mcp-qt-client", "full namespace clientInfo should match");
}

void test_qt_stateless_session_mrtr_loop() {
    auto mockTransport = std::make_shared<StatelessMockTransport>();
    auto session = std::make_shared<mcp::McpClientSession>(mockTransport);
    session->init();
    session->start();
    session->setStatelessMode(true);

    bool mrtrTriggered = false;
    std::string capturedRequestState;
    session->setMrtrHandler([&mrtrTriggered, &capturedRequestState](
        const std::string& requestId,
        const nlohmann::json& inputRequests,
        const nlohmann::json& requestParams,
        const std::string& requestState,
        std::function<void(const nlohmann::json&)> replyCb) {
        mrtrTriggered = true;
        capturedRequestState = requestState;
        TM_ASSERT_TRUE(inputRequests.is_object() && inputRequests.size() == 1, "inputRequests should be a map");
        TM_ASSERT_TRUE(inputRequests.contains("github_login"), "inputRequests should contain github_login key");
        TM_ASSERT_TRUE(inputRequests["github_login"]["method"] == "elicitation/create", "request method should be elicitation/create");
        TM_ASSERT_TRUE(inputRequests["github_login"]["params"]["requestedSchema"]["properties"].contains("password"), "requested schema should require password");
        // 扁平表单数据，由库自动包装为 ElicitResult
        replyCb({{"password", "secret123"}});
    });

    bool toolCompleted = false;
    nlohmann::json finalContent;
    session->callTool("db_auth", {{"user", "admin"}}, [&toolCompleted, &finalContent](const nlohmann::json& content, const nlohmann::json& error) {
        if (error.empty()) {
            toolCompleted = true;
            finalContent = content;
        }
    });

    TM_ASSERT_TRUE(mockTransport->sentMessages.size() == 1, "first callTool request sent");
    auto firstReq = nlohmann::json::parse(mockTransport->sentMessages[0]);
    int64_t reqId1 = firstReq["id"].get<int64_t>();

    // 模拟符合 2026-07-28 规范的 InputRequiredResult（SEP-2322）
    nlohmann::json inputRequiredResp = {
        {"jsonrpc", "2.0"},
        {"id", reqId1},
        {"result", {
            {"resultType", "input_required"},
            {"inputRequests", {
                {"github_login", {
                    {"method", "elicitation/create"},
                    {"params", {
                        {"mode", "form"},
                        {"message", "Please provide your GitHub username"},
                        {"requestedSchema", {
                            {"type", "object"},
                            {"properties", {{"password", {{"type", "string"}}}}},
                            {"required", {"password"}}
                        }}
                    }}
                }}
            }},
            {"requestState", "AEAD-protected-blob"}
        }}
    };
    mockTransport->onMsgCb(inputRequiredResp.dump());

    // 验证 MRTR 处理器被触发，并自动发出第二个重试 Request 报文
    TM_ASSERT_TRUE(mrtrTriggered, "MRTR handler should be triggered");
    TM_ASSERT_TRUE(capturedRequestState == "AEAD-protected-blob", "requestState should be passed through verbatim");
    TM_ASSERT_TRUE(mockTransport->sentMessages.size() == 2, "second request should be sent automatically after user input");

    auto secondReq = nlohmann::json::parse(mockTransport->sentMessages[1]);
    TM_ASSERT_TRUE(secondReq["method"] == "tools/call", "second request method should still be tools/call");

    // 规范要求：重试的 JSON-RPC id MUST 与首轮不同
    TM_ASSERT_TRUE(secondReq["id"].get<int64_t>() != reqId1, "retry request MUST use a new JSON-RPC id");

    // inputResponses 位于 params 顶层（与 name/arguments/_meta 平级）
    TM_ASSERT_TRUE(secondReq["params"].contains("inputResponses"), "params should contain top-level inputResponses");
    TM_ASSERT_TRUE(secondReq["params"]["inputResponses"]["github_login"]["action"] == "accept", "elicitation result should be accepted");
    TM_ASSERT_TRUE(secondReq["params"]["inputResponses"]["github_login"]["content"]["password"] == "secret123", "form content should be echoed");

    // requestState 必须原样回显（客户端 MUST NOT 解析/修改）
    TM_ASSERT_TRUE(secondReq["params"]["requestState"] == "AEAD-protected-blob", "requestState must be echoed verbatim");

    // _meta 仍在 params 顶层（无状态自描述）
    TM_ASSERT_TRUE(secondReq["params"].contains("_meta"), "params should still contain _meta");

    int64_t reqId2 = secondReq["id"].get<int64_t>();

    // 模拟服务端收到补充输入后，响应最终的成功结果
    nlohmann::json successResp = {
        {"jsonrpc", "2.0"},
        {"id", reqId2},
        {"result", {
            {"resultType", "complete"},
            {"content", {{{"type", "text"}, {"text", "Authenticated successfully"}}}}
        }}
    };
    mockTransport->onMsgCb(successResp.dump());

    TM_ASSERT_TRUE(toolCompleted, "original tool callback should be completed after MRTR loop");
    TM_ASSERT_TRUE(finalContent.contains("content"), "final result should carry content");
}

void test_qt_stateless_session_multi_round_mrtr_loop() {
    // 边界测试 2：连续 3 轮交互，逐轮回显不同的 requestState
    auto mockTransport = std::make_shared<StatelessMockTransport>();
    auto session = std::make_shared<mcp::McpClientSession>(mockTransport);
    session->init();
    session->start();
    session->setStatelessMode(true);

    int mrtrCallCount = 0;
    std::vector<std::string> capturedStates;
    session->setMrtrHandler([&mrtrCallCount, &capturedStates](
        const std::string& requestId,
        const nlohmann::json& inputRequests,
        const nlohmann::json& requestParams,
        const std::string& requestState,
        std::function<void(const nlohmann::json&)> replyCb) {
        mrtrCallCount++;
        capturedStates.push_back(requestState);
        if (mrtrCallCount == 1) {
            TM_ASSERT_TRUE(inputRequests.contains("step1"), "round 1 asks for step1");
            replyCb({{"password", "myPass123"}});
        } else if (mrtrCallCount == 2) {
            TM_ASSERT_TRUE(inputRequests.contains("step2"), "round 2 asks for step2");
            replyCb({{"mfaCode", "888999"}});
        }
    });

    bool toolCompleted = false;
    session->callTool("secure_action", nlohmann::json::object(), [&toolCompleted](const nlohmann::json&, const nlohmann::json& error) {
        if (error.empty()) {
            toolCompleted = true;
        }
    });

    // 第一轮：规范 inputRequests + requestState = "state-1"
    auto req1 = nlohmann::json::parse(mockTransport->sentMessages[0]);
    nlohmann::json resp1 = {
        {"jsonrpc", "2.0"},
        {"id", req1["id"]},
        {"result", {
            {"resultType", "input_required"},
            {"inputRequests", {
                {"step1", {
                    {"method", "elicitation/create"},
                    {"params", {{"mode", "form"}, {"message", "password?"}, {"requestedSchema", {{"type", "object"}, {"properties", {{"password", {{"type", "string"}}}}}}}}}
                }}
            }},
            {"requestState", "state-1"}
        }}
    };
    mockTransport->onMsgCb(resp1.dump());

    TM_ASSERT_TRUE(mrtrCallCount == 1, "round 1 MRTR triggered");
    TM_ASSERT_TRUE(mockTransport->sentMessages.size() == 2, "second request sent");

    auto req2 = nlohmann::json::parse(mockTransport->sentMessages[1]);
    TM_ASSERT_TRUE(req2["params"]["requestState"] == "state-1", "round 1 requestState echoed verbatim");
    TM_ASSERT_TRUE(req2["params"]["inputResponses"]["step1"]["action"] == "accept", "round 1 response keyed by step1");

    // 第二轮：requestState 变为 "state-2"
    nlohmann::json resp2 = {
        {"jsonrpc", "2.0"},
        {"id", req2["id"]},
        {"result", {
            {"resultType", "input_required"},
            {"inputRequests", {
                {"step2", {
                    {"method", "elicitation/create"},
                    {"params", {{"mode", "form"}, {"message", "mfa?"}, {"requestedSchema", {{"type", "object"}, {"properties", {{"mfaCode", {{"type", "string"}}}}}}}}}
                }}
            }},
            {"requestState", "state-2"}
        }}
    };
    mockTransport->onMsgCb(resp2.dump());

    TM_ASSERT_TRUE(mrtrCallCount == 2, "round 2 MRTR triggered");
    TM_ASSERT_TRUE(mockTransport->sentMessages.size() == 3, "third request sent");

    auto req3 = nlohmann::json::parse(mockTransport->sentMessages[2]);
    TM_ASSERT_TRUE(req3["params"]["requestState"] == "state-2", "round 2 requestState echoed verbatim");
    TM_ASSERT_TRUE(req3["params"]["inputResponses"]["step2"]["content"]["mfaCode"] == "888999", "round 2 mfa echoed");

    // 第三轮：成功完成
    nlohmann::json resp3 = {
        {"jsonrpc", "2.0"},
        {"id", req3["id"]},
        {"result", {{"resultType", "complete"}, {"status", "completed"}}}
    };
    mockTransport->onMsgCb(resp3.dump());

    TM_ASSERT_TRUE(toolCompleted, "tool completed after multi-round MRTR loop");
    TM_ASSERT_TRUE(capturedStates.size() == 2 && capturedStates[0] == "state-1" && capturedStates[1] == "state-2", "per-round requestState observed");
}
