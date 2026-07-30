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
    session->setMrtrHandler([&mrtrTriggered](const nlohmann::json& schema, const nlohmann::json& requestParams, std::function<void(const nlohmann::json& userInputs)> replyCb) {
        mrtrTriggered = true;
        TM_ASSERT_TRUE(schema.contains("properties"), "schema should contain properties");
        TM_ASSERT_TRUE(schema["properties"].contains("password"), "schema should require password");
        // 回传补充输入的密码
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

    // 模拟服务端响应 input_required
    nlohmann::json inputRequiredResp = {
        {"jsonrpc", "2.0"},
        {"id", reqId1},
        {"result", {
            {"status", "input_required"},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"password", {{"type", "string"}}}
                }}
            }}
        }}
    };
    mockTransport->onMsgCb(inputRequiredResp.dump());

    // 验证 MRTR 处理器被触发，并且自动向 Transport 发出了第二个重试 Request 报文
    TM_ASSERT_TRUE(mrtrTriggered, "MRTR handler should be triggered");
    TM_ASSERT_TRUE(mockTransport->sentMessages.size() == 2, "second request should be sent automatically after user input");

    auto secondReq = nlohmann::json::parse(mockTransport->sentMessages[1]);
    TM_ASSERT_TRUE(secondReq["method"] == "tools/call", "second request method should still be tools/call");
    TM_ASSERT_TRUE(secondReq["params"]["_meta"].contains("mrtr_input"), "second request params._meta should contain mrtr_input");
    TM_ASSERT_TRUE(secondReq["params"]["_meta"]["mrtr_input"]["password"] == "secret123", "mrtr_input password should be secret123");
    TM_ASSERT_TRUE(secondReq["params"]["_meta"].contains("inputResponses"), "second request params._meta should contain inputResponses");

    int64_t reqId2 = secondReq["id"].get<int64_t>();

    // 模拟服务端收到补充输入后，响应最终的成功结果
    nlohmann::json successResp = {
        {"jsonrpc", "2.0"},
        {"id", reqId2},
        {"result", {
            {"status", "success"},
            {"content", {{{"type", "text"}, {"text", "Authenticated successfully"}}}}
        }}
    };
    mockTransport->onMsgCb(successResp.dump());

    TM_ASSERT_TRUE(toolCompleted, "original tool callback should be completed after MRTR loop");
    TM_ASSERT_TRUE(finalContent.contains("status") && finalContent["status"] == "success", "final result status should be success");
}

void test_qt_stateless_session_multi_round_mrtr_loop() {
    // 边界测试 2：连续 3 轮交互与 resultType / inputRequests 官方别名解析
    auto mockTransport = std::make_shared<StatelessMockTransport>();
    auto session = std::make_shared<mcp::McpClientSession>(mockTransport);
    session->init();
    session->start();
    session->setStatelessMode(true);

    int mrtrCallCount = 0;
    session->setMrtrHandler([&mrtrCallCount](const nlohmann::json& schema, const nlohmann::json& requestParams, std::function<void(const nlohmann::json& userInputs)> replyCb) {
        mrtrCallCount++;
        if (mrtrCallCount == 1) {
            // 第一轮补充密码
            TM_ASSERT_TRUE(schema["properties"].contains("password"), "round 1 asks for password");
            replyCb({{"password", "myPass123"}});
        } else if (mrtrCallCount == 2) {
            // 第二轮补充 MFA 二次验证码
            TM_ASSERT_TRUE(schema["properties"].contains("mfaCode"), "round 2 asks for mfaCode");
            replyCb({{"mfaCode", "888999"}});
        }
    });

    bool toolCompleted = false;
    session->callTool("secure_action", nlohmann::json::object(), [&toolCompleted](const nlohmann::json&, const nlohmann::json& error) {
        if (error.empty()) {
            toolCompleted = true;
        }
    });

    // 第一轮服务端响应: 使用规范别名 resultType: "input_required" 和 inputRequests 字段
    auto req1 = nlohmann::json::parse(mockTransport->sentMessages[0]);
    nlohmann::json resp1 = {
        {"jsonrpc", "2.0"},
        {"id", req1["id"]},
        {"result", {
            {"resultType", "input_required"},
            {"inputRequests", {
                {"type", "object"},
                {"properties", {{"password", {{"type", "string"}}}}}
            }}
        }}
    };
    mockTransport->onMsgCb(resp1.dump());

    TM_ASSERT_TRUE(mrtrCallCount == 1, "round 1 MRTR triggered");
    TM_ASSERT_TRUE(mockTransport->sentMessages.size() == 2, "second request sent");

    // 第二轮服务端响应: 发现还需要 MFA 验证码
    auto req2 = nlohmann::json::parse(mockTransport->sentMessages[1]);
    nlohmann::json resp2 = {
        {"jsonrpc", "2.0"},
        {"id", req2["id"]},
        {"result", {
            {"resultType", "input_required"},
            {"inputRequests", {
                {"type", "object"},
                {"properties", {{"mfaCode", {{"type", "string"}}}}}
            }}
        }}
    };
    mockTransport->onMsgCb(resp2.dump());

    TM_ASSERT_TRUE(mrtrCallCount == 2, "round 2 MRTR triggered");
    TM_ASSERT_TRUE(mockTransport->sentMessages.size() == 3, "third request sent");

    // 第三轮服务端响应: 成功完成操作
    auto req3 = nlohmann::json::parse(mockTransport->sentMessages[2]);
    nlohmann::json resp3 = {
        {"jsonrpc", "2.0"},
        {"id", req3["id"]},
        {"result", {{"status", "completed"}}}
    };
    mockTransport->onMsgCb(resp3.dump());

    TM_ASSERT_TRUE(toolCompleted, "tool completed after multi-round MRTR loop");
}
