#include "tests/common.h"

// ==========================================
// Ping 测试
// ==========================================
void test_ping() {
    // 场景 1: ping 成功
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        // 初始化会话
        session->initialize("test", "1", [](bool, const mcp::json&) {});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        // 发送 ping
        bool pingSuccess = false;
        session->ping([&](bool success, const mcp::json& error) {
            pingSuccess = success;
        });

        // 模拟 pong 响应
        mcp::json pongResp = {
            {"jsonrpc", "2.0"}, {"id", 2},
            {"result", mcp::json::object()}
        };
        transport->pushServerMessage(pongResp.dump());

        TM_ASSERT_TRUE(pingSuccess, "Ping should succeed");
    }

    // 场景 2: ping 未初始化时失败
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        bool pingFailed = false;
        session->ping([&](bool success, const mcp::json& error) {
            if (!success && error.contains("code") && error["code"] == -32002) {
                pingFailed = true;
            }
        });

        TM_ASSERT_TRUE(pingFailed, "Ping should fail when not initialized");
    }

    // 场景 3: pingSync 成功
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        session->initialize("test", "1", [](bool, const mcp::json&) {});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        // 异步发送 pingSync 响应
        transport->pushServerMessageAsync(R"({"jsonrpc":"2.0","id":2,"result":{}})", 10);

        bool result = session->pingSync(std::chrono::milliseconds(1000));
        TM_ASSERT_TRUE(result, "pingSync should succeed");
    }
}

// ==========================================
// Resource Templates 测试
// ==========================================
void test_resource_templates() {
    // 场景 1: 列出资源模板
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        session->initialize("test", "1", [](bool, const mcp::json&) {});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        bool listOk = false;
        session->listResourceTemplates([&](const std::vector<mcp::McpResourceTemplate>& templates, const mcp::json& error) {
            if (error.empty() && templates.size() == 1) {
                if (templates[0].name == "file_template" && templates[0].uriTemplate == "file:///{path}") {
                    listOk = true;
                }
            }
        });

        mcp::json listResp = {
            {"jsonrpc", "2.0"}, {"id", 2},
            {"result", {{"resourceTemplates", mcp::json::array({
                {{"uriTemplate", "file:///{path}"}, {"name", "file_template"}, {"description", "File resource template"}, {"mimeType", "text/plain"}}
            })}}}
        };
        transport->pushServerMessage(listResp.dump());

        TM_ASSERT_TRUE(listOk, "listResourceTemplates should return templates");
    }

    // 场景 2: 带分页的资源模板列表
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        session->initialize("test", "1", [](bool, const mcp::json&) {});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        bool pageOk = false;
        session->listResourceTemplates("cursor_1", [&](const std::vector<mcp::McpResourceTemplate>& templates, const std::string& nextCursor, const mcp::json& error) {
            if (error.empty() && templates.size() == 1 && nextCursor == "cursor_2") {
                pageOk = true;
            }
        });

        mcp::json pageResp = {
            {"jsonrpc", "2.0"}, {"id", 2},
            {"result", {{"resourceTemplates", mcp::json::array({
                {{"uriTemplate", "db:///{table}"}, {"name", "db_template"}}
            })}, {"nextCursor", "cursor_2"}}}
        };
        transport->pushServerMessage(pageResp.dump());

        TM_ASSERT_TRUE(pageOk, "listResourceTemplates with cursor should return nextCursor");
    }

    // 场景 3: listResourceTemplatesSync
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        session->initialize("test", "1", [](bool, const mcp::json&) {});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        transport->pushServerMessageAsync(R"({"jsonrpc":"2.0","id":2,"result":{"resourceTemplates":[{"uriTemplate":"file:///{path}","name":"tmpl1"}]}})", 10);

        auto templates = session->listResourceTemplatesSync(std::chrono::milliseconds(1000));
        TM_ASSERT_EQ(templates.size(), 1u, "listResourceTemplatesSync should return 1 template");
        if (!templates.empty()) {
            TM_ASSERT_EQ(templates[0].name, "tmpl1", "Template name should be tmpl1");
        }
    }
}

// ==========================================
// Completion 测试
// ==========================================
void test_complete() {
    // 场景 1: 资源模板补全
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        session->initialize("test", "1", [](bool, const mcp::json&) {});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        bool completeOk = false;
        mcp::json ref = {{"type", "ref/resource"}, {"uri", "file:///logs"}};
        mcp::json argument = {{"name", "path"}, {"value", "app"}};
        session->complete(ref, argument, [&](const mcp::json& result, const mcp::json& error) {
            if (error.empty() && result.contains("completion")) {
                const auto& comp = result["completion"];
                if (comp.contains("values") && comp["values"].is_array() && comp["values"].size() == 2) {
                    completeOk = true;
                }
            }
        });

        mcp::json completeResp = {
            {"jsonrpc", "2.0"}, {"id", 2},
            {"result", {{"completion", {{"values", {"app.log", "app.json"}}, {"hasMore", false}}}}}
        };
        transport->pushServerMessage(completeResp.dump());

        TM_ASSERT_TRUE(completeOk, "complete should return completion values");
    }

    // 场景 2: Prompt 参数补全
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        session->initialize("test", "1", [](bool, const mcp::json&) {});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        bool promptCompleteOk = false;
        mcp::json ref = {{"type", "ref/prompt"}, {"name", "code_review"}};
        mcp::json argument = {{"name", "language"}, {"value", "py"}};
        session->complete(ref, argument, [&](const mcp::json& result, const mcp::json& error) {
            if (error.empty() && result.contains("completion")) {
                promptCompleteOk = true;
            }
        });

        mcp::json completeResp = {
            {"jsonrpc", "2.0"}, {"id", 2},
            {"result", {{"completion", {{"values", {"python", "pypy"}}, {"hasMore", false}}}}}
        };
        transport->pushServerMessage(completeResp.dump());

        TM_ASSERT_TRUE(promptCompleteOk, "complete for prompt args should work");
    }

    // 场景 3: completeSync
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        session->initialize("test", "1", [](bool, const mcp::json&) {});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        transport->pushServerMessageAsync(R"({"jsonrpc":"2.0","id":2,"result":{"completion":{"values":["result1"]}}})", 10);

        mcp::json ref = {{"type", "ref/resource"}, {"uri", "file:///test"}};
        mcp::json argument = {{"name", "path"}, {"value", "t"}};
        mcp::json error;
        auto result = session->completeSync(ref, argument, &error, std::chrono::milliseconds(1000));
        TM_ASSERT_TRUE(error.empty(), "completeSync should not have error");
        TM_ASSERT_TRUE(result.contains("completion"), "completeSync should return completion");
    }
}

// ==========================================
// Elicitation 能力测试
// ==========================================
void test_elicitation() {
    // 场景 1: 初始化时声明 elicitation 能力
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        bool initOk = false;
        session->initialize("test", "1", [&](bool success, const mcp::json&) {
            initOk = success;
        });

        // 捕获客户端发送的 initialize 请求
        mcp::json sentMsg = mcp::json::parse(transport->lastSentMessage);
        TM_ASSERT_TRUE(sentMsg.contains("params"), "Initialize should have params");
        TM_ASSERT_TRUE(sentMsg["params"].contains("capabilities"), "Should have capabilities");
        TM_ASSERT_TRUE(sentMsg["params"]["capabilities"].contains("elicitation"), "Should have elicitation capability");

        const auto& elic = sentMsg["params"]["capabilities"]["elicitation"];
        TM_ASSERT_TRUE(elic.contains("modes"), "Elicitation should have modes");
        TM_ASSERT_TRUE(elic["modes"].is_array(), "Modes should be array");

        // 完成初始化
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        TM_ASSERT_TRUE(initOk, "Initialize should succeed with elicitation capability");
    }

    // 场景 2: 注册 elicitation 请求处理器
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        session->initialize("test", "1", [](bool, const mcp::json&) {});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        // 注册 elicitation handler
        bool elicitationHandled = false;
        session->registerRequestHandler("elicitation/create", [&](const std::string& method, const mcp::json& params) -> mcp::json {
            elicitationHandled = true;
            return {{"action", "accept"}, {"content", {{"name", "test_value"}}}};
        });

        // 模拟服务器发送 elicitation/create 请求
        mcp::json nameProp = {{"type", "string"}};
        mcp::json props = {{"name", nameProp}};
        mcp::json schema = {{"type", "object"}, {"properties", props}};
        mcp::json elicitationReq = {
            {"jsonrpc", "2.0"}, {"id", 100},
            {"method", "elicitation/create"},
            {"params", {{"message", "Please provide your name"}, {"schema", schema}}}
        };
        transport->pushServerMessage(elicitationReq.dump());

        TM_ASSERT_TRUE(elicitationHandled, "Elicitation request should be handled");
    }
}

// ==========================================
// Tool Annotations 测试
// ==========================================
void test_tool_annotations() {
    // 场景 1: ToolAnnotations 序列化/反序列化
    {
        mcp::ToolAnnotations annotations;
        annotations.title = "Test Tool";
        annotations.readOnlyHint = true;
        annotations.destructiveHint = false;
        annotations.idempotentHint = true;
        annotations.openWorldHint = false;
        annotations.description = "A test tool";

        mcp::json j = annotations.toJson();
        TM_ASSERT_EQ(j["title"].get<std::string>(), "Test Tool", "Title should match");
        TM_ASSERT_TRUE(j["readOnlyHint"].get<bool>(), "readOnlyHint should be true");
        TM_ASSERT_FALSE(j["destructiveHint"].get<bool>(), "destructiveHint should be false");
        TM_ASSERT_TRUE(j["idempotentHint"].get<bool>(), "idempotentHint should be true");
        TM_ASSERT_FALSE(j["openWorldHint"].get<bool>(), "openWorldHint should be false");
        TM_ASSERT_EQ(j["description"].get<std::string>(), "A test tool", "Description should match");

        // 反序列化
        auto parsed = mcp::ToolAnnotations::fromJson(j);
        TM_ASSERT_EQ(parsed.title, "Test Tool", "Parsed title should match");
        TM_ASSERT_TRUE(parsed.readOnlyHint, "Parsed readOnlyHint should be true");
        TM_ASSERT_FALSE(parsed.destructiveHint, "Parsed destructiveHint should be false");
    }

    // 场景 2: McpTool 带 annotations 序列化
    {
        mcp::McpTool tool;
        tool.name = "safe_read";
        tool.description = "Read a file safely";
        tool.inputSchema = {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}}};
        tool.annotations.title = "Safe File Reader";
        tool.annotations.readOnlyHint = true;
        tool.annotations.destructiveHint = false;

        mcp::json j;
        mcp::to_json(j, tool);

        TM_ASSERT_TRUE(j.contains("annotations"), "Serialized tool should have annotations");
        TM_ASSERT_EQ(j["annotations"]["title"].get<std::string>(), "Safe File Reader", "Annotation title should match");
        TM_ASSERT_TRUE(j["annotations"]["readOnlyHint"].get<bool>(), "readOnlyHint should be true");

        // 反序列化
        mcp::McpTool parsed;
        mcp::from_json(j, parsed);
        TM_ASSERT_EQ(parsed.annotations.title, "Safe File Reader", "Parsed annotation title should match");
        TM_ASSERT_TRUE(parsed.annotations.readOnlyHint, "Parsed readOnlyHint should be true");
    }

    // 场景 3: McpTool 无 annotations 时不序列化 annotations 字段
    {
        mcp::McpTool tool;
        tool.name = "basic_tool";
        tool.description = "A basic tool";
        tool.inputSchema = mcp::json::object();

        mcp::json j;
        mcp::to_json(j, tool);

        TM_ASSERT_FALSE(j.contains("annotations"), "Tool without annotations should not have annotations field");
    }

    // 场景 4: McpTool 从 JSON 反序列化含 annotations
    {
        mcp::json j = {
            {"name", "destructive_delete"},
            {"description", "Delete something"},
            {"inputSchema", {{"type", "object"}}},
            {"annotations", {{"title", "Delete Tool"}, {"destructiveHint", true}, {"readOnlyHint", false}}}
        };

        auto tool = j.get<mcp::McpTool>();
        TM_ASSERT_EQ(tool.name, "destructive_delete", "Tool name should match");
        TM_ASSERT_EQ(tool.annotations.title, "Delete Tool", "Annotation title should match");
        TM_ASSERT_TRUE(tool.annotations.destructiveHint, "destructiveHint should be true");
        TM_ASSERT_FALSE(tool.annotations.readOnlyHint, "readOnlyHint should be false");
    }
}
