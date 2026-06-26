#include "tests/common.h"
#include "mcp_core/McpOAuthClient.h"

// ==========================================
// Sampling 测试 (服务端请求客户端推理)
// ==========================================
void test_sampling() {
    // 场景 1: 设置 sampling handler 并接收服务端请求
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

        // 注册 sampling handler
        bool samplingHandled = false;
        std::string receivedModel;
        session->setSamplingHandler([&](const mcp::json& params) -> mcp::json {
            samplingHandled = true;
            if (params.contains("modelPreferences")) {
                receivedModel = params["modelPreferences"].value("hints", "");
            }
            return {
                {"model", "test-model-1"},
                {"role", "assistant"},
                {"content", {{"type", "text"}, {"text", "Hello from sampling handler"}}}
            };
        });

        // 模拟服务端发送 sampling/createMessage 请求
        mcp::json samplingReq = {
            {"jsonrpc", "2.0"}, {"id", 100},
            {"method", "sampling/createMessage"},
            {"params", {
                {"messages", mcp::json::array({
                    {{"role", "user"}, {"content", {{"type", "text"}, {"text", "Hello"}}}}
                })},
                {"modelPreferences", {{"hints", "claude-sonnet"}}},
                {"systemPrompt", "You are helpful"},
                {"maxTokens", 100}
            }}
        };
        transport->pushServerMessage(samplingReq.dump());

        TM_ASSERT_TRUE(samplingHandled, "Sampling handler should be invoked");
        TM_ASSERT_EQ(receivedModel, "claude-sonnet", "Model preference should be received");

        // 检查响应是否正确发送
        mcp::json sentResp = mcp::json::parse(transport->lastSentMessage);
        TM_ASSERT_TRUE(sentResp.contains("result"), "Response should contain result");
        TM_ASSERT_EQ(sentResp["result"]["model"].get<std::string>(), "test-model-1", "Response model should match");
        TM_ASSERT_EQ(sentResp["result"]["role"].get<std::string>(), "assistant", "Response role should be assistant");
    }

    // 场景 2: 未设置 sampling handler 时返回错误
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

        // 不注册 handler，直接发送 sampling 请求
        mcp::json samplingReq = {
            {"jsonrpc", "2.0"}, {"id", 101},
            {"method", "sampling/createMessage"},
            {"params", {{"messages", mcp::json::array()}}}
        };
        transport->pushServerMessage(samplingReq.dump());

        mcp::json sentResp = mcp::json::parse(transport->lastSentMessage);
        TM_ASSERT_TRUE(sentResp.contains("error"), "Response should contain error (no handler registered)");
        TM_ASSERT_EQ(sentResp["error"]["code"].get<int>(), -32601, "Error code should be -32601 (Method not found)");
    }
}

// ==========================================
// Roots 测试 (客户端暴露文件系统根目录)
// ==========================================
void test_roots() {
    // 场景 1: 设置 roots provider 并响应服务端请求
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

        // 注册 roots provider
        session->setRootsProvider([]() -> mcp::json {
            return mcp::json::array({
                {{"uri", "file:///home/user/projects"}, {"name", "Projects"}},
                {{"uri", "file:///home/user/documents"}, {"name", "Documents"}}
            });
        });

        // 模拟服务端发送 roots/list 请求
        mcp::json rootsReq = {
            {"jsonrpc", "2.0"}, {"id", 100},
            {"method", "roots/list"},
            {"params", mcp::json::object()}
        };
        transport->pushServerMessage(rootsReq.dump());

        // 检查响应
        std::string rawMsg = transport->lastSentMessage;
        mcp::json sentResp = mcp::json::parse(rawMsg);
        bool hasResult = sentResp.count("result") > 0;
        TM_ASSERT_TRUE(hasResult, "Response should contain result");
        if (hasResult) {
            bool hasRoots = sentResp["result"].count("roots") > 0;
            TM_ASSERT_TRUE(hasRoots, "Result should contain roots");
            if (hasRoots) {
                TM_ASSERT_EQ(sentResp["result"]["roots"].size(), 2u, "Should return 2 roots");
                TM_ASSERT_EQ(sentResp["result"]["roots"][0]["uri"].get<std::string>(), "file:///home/user/projects", "First root URI should match");
            }
        }
    }

    // 场景 2: notifyRootsListChanged 发送通知
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

        // 发送 roots 变更通知
        session->notifyRootsListChanged();

        mcp::json sentMsg = mcp::json::parse(transport->lastSentMessage);
        TM_ASSERT_EQ(sentMsg["method"].get<std::string>(), "notifications/roots/list_changed", "Should send roots list changed notification");
        TM_ASSERT_FALSE(sentMsg.contains("id"), "Notification should not have id");
    }

    // 场景 3: 未设置 roots provider 时返回空数组
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

        // 不设置 provider，直接发送 roots/list 请求
        mcp::json rootsReq = {
            {"jsonrpc", "2.0"}, {"id", 100},
            {"method", "roots/list"},
            {"params", mcp::json::object()}
        };
        transport->pushServerMessage(rootsReq.dump());

        mcp::json sentResp = mcp::json::parse(transport->lastSentMessage);
        // 未设置 provider 时 roots/list handler 未注册，返回 -32601 Method not found
        TM_ASSERT_TRUE(sentResp.contains("error"), "Response should contain error (no roots provider)");
        TM_ASSERT_EQ(sentResp["error"]["code"].get<int>(), -32601, "Error code should be -32601");
    }
}

// ==========================================
// Elicitation 完整流程测试
// ==========================================
void test_elicitation_full() {
    // 场景 1: 设置 elicitation handler 并处理服务端请求
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
        session->setElicitationHandler([&](const mcp::json& params) -> mcp::json {
            elicitationHandled = true;
            return {
                {"action", "accept"},
                {"content", {{"name", "张三"}, {"email", "zhangsan@example.com"}}}
            };
        });

        // 模拟服务端发送 elicitation/create 请求
        mcp::json nameProp = {{"type", "string"}};
        mcp::json emailProp = {{"type", "string"}, {"format", "email"}};
        mcp::json props = {{"name", nameProp}, {"email", emailProp}};
        mcp::json schema = {{"type", "object"}, {"properties", props}, {"required", mcp::json::array({"name"})}};
        mcp::json elicitationReq = {
            {"jsonrpc", "2.0"}, {"id", 100},
            {"method", "elicitation/create"},
            {"params", {{"message", "请输入您的信息"}, {"schema", schema}, {"timeout", 30000}}}
        };
        transport->pushServerMessage(elicitationReq.dump());

        TM_ASSERT_TRUE(elicitationHandled, "Elicitation handler should be invoked");

        mcp::json sentResp = mcp::json::parse(transport->lastSentMessage);
        TM_ASSERT_TRUE(sentResp.contains("result"), "Response should contain result");
        TM_ASSERT_EQ(sentResp["result"]["action"].get<std::string>(), "accept", "Action should be accept");
        TM_ASSERT_EQ(sentResp["result"]["content"]["name"].get<std::string>(), "张三", "User name should match");
    }

    // 场景 2: elicitation declined
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

        session->setElicitationHandler([](const mcp::json&) -> mcp::json {
            return {{"action", "declined"}};
        });

        mcp::json elicitationReq = {
            {"jsonrpc", "2.0"}, {"id", 100},
            {"method", "elicitation/create"},
            {"params", {{"message", "请提供密码"}, {"schema", {{"type", "object"}}}}}
        };
        transport->pushServerMessage(elicitationReq.dump());

        mcp::json sentResp = mcp::json::parse(transport->lastSentMessage);
        TM_ASSERT_EQ(sentResp["result"]["action"].get<std::string>(), "declined", "Action should be declined");
    }
}

// ==========================================
// OAuth 2.0 + PKCE 测试
// ==========================================
void test_oauth_client() {
    // 场景 1: OAuth token 状态管理
    {
        mcp::McpOAuthClient oauth;
        TM_ASSERT_FALSE(oauth.hasValidToken(), "Should have no valid token initially");

        mcp::OAuthToken token;
        token.accessToken = "test_access_token";
        token.refreshToken = "test_refresh_token";
        token.expiresIn = 3600;
        token.obtainedAt = std::chrono::steady_clock::now();
        oauth.setCurrentToken(token);

        TM_ASSERT_TRUE(oauth.hasValidToken(), "Should have valid token after setting");
        auto current = oauth.getCurrentToken();
        TM_ASSERT_EQ(current.accessToken, "test_access_token", "Access token should match");
        TM_ASSERT_EQ(current.refreshToken, "test_refresh_token", "Refresh token should match");
    }

    // 场景 2: Token 过期检测
    {
        mcp::OAuthToken token;
        token.accessToken = "expired_token";
        token.expiresIn = 1; // 1 秒过期
        token.obtainedAt = std::chrono::steady_clock::now() - std::chrono::seconds(2);

        TM_ASSERT_TRUE(token.isExpired(), "Token should be expired after 2 seconds");
        TM_ASSERT_TRUE(token.isExpiringSoon(30), "Token should be expiring soon");
    }

    // 场景 3: PKCE auth URL 生成
    {
        mcp::McpOAuthClient oauth;
        mcp::OAuthServerMetadata metadata;
        metadata.authorizationEndpoint = "https://auth.example.com/authorize";

        auto authReq = oauth.buildAuthorizationUrl(metadata, "client_id_123", "", {"openid", "profile"});

        TM_ASSERT_FALSE(authReq.authorizationUrl.empty(), "Authorization URL should not be empty");
        TM_ASSERT_FALSE(authReq.codeVerifier.empty(), "Code verifier should not be empty");
        TM_ASSERT_FALSE(authReq.codeChallenge.empty(), "Code challenge should not be empty");
        TM_ASSERT_FALSE(authReq.state.empty(), "State should not be empty");
        TM_ASSERT_STR_CONTAINS(authReq.authorizationUrl, "response_type=code", "URL should contain response_type");
        TM_ASSERT_STR_CONTAINS(authReq.authorizationUrl, "client_id=client_id_123", "URL should contain client_id");
        TM_ASSERT_STR_CONTAINS(authReq.authorizationUrl, "code_challenge=", "URL should contain code_challenge");
        TM_ASSERT_STR_CONTAINS(authReq.authorizationUrl, "scope=openid+profile", "URL should contain scope");
    }

    // 场景 4: OAuthServerMetadata 解析
    {
        mcp::json j = {
            {"issuer", "https://auth.example.com"},
            {"authorization_endpoint", "https://auth.example.com/authorize"},
            {"token_endpoint", "https://auth.example.com/token"},
            {"registration_endpoint", "https://auth.example.com/register"},
            {"scopes_supported", {"openid", "profile", "email"}},
            {"response_types_supported", {"code"}},
            {"grant_types_supported", {"authorization_code", "refresh_token"}},
            {"code_challenge_methods_supported", {"S256", "plain"}}
        };
        auto metadata = mcp::OAuthServerMetadata::fromJson(j);
        TM_ASSERT_EQ(metadata.issuer, "https://auth.example.com", "Issuer should match");
        TM_ASSERT_EQ(metadata.authorizationEndpoint, "https://auth.example.com/authorize", "Auth endpoint should match");
        TM_ASSERT_EQ(metadata.tokenEndpoint, "https://auth.example.com/token", "Token endpoint should match");
        TM_ASSERT_EQ(metadata.registrationEndpoint, "https://auth.example.com/register", "Registration endpoint should match");
        TM_ASSERT_EQ(metadata.scopesSupported.size(), 3u, "Should have 3 scopes");
        TM_ASSERT_EQ(metadata.codeChallengeMethodsSupported.size(), 2u, "Should have 2 challenge methods");
    }

    // 场景 5: OAuthClientRegistration 解析
    {
        mcp::json j = {
            {"client_id", "test_client_id"},
            {"client_secret", "test_client_secret"},
            {"redirect_uris", {"http://localhost:8080/callback"}},
            {"client_name", "Test MCP Client"},
            {"client_id_issued_at", 1700000000},
            {"client_secret_expires_at", 1700086400}
        };
        auto reg = mcp::OAuthClientRegistration::fromJson(j);
        TM_ASSERT_EQ(reg.clientId, "test_client_id", "Client ID should match");
        TM_ASSERT_EQ(reg.clientSecret, "test_client_secret", "Client secret should match");
        TM_ASSERT_EQ(reg.redirectUris.size(), 1u, "Should have 1 redirect URI");
        TM_ASSERT_EQ(reg.clientName, "Test MCP Client", "Client name should match");
    }
}

// ==========================================
// Notification Debounce 测试
// ==========================================
void test_notification_debounce() {
    // 场景 1: 未配置去重时直接发送
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

        session->sendNotificationDebounced("test/method", {{"key", "value"}});
        mcp::json sent = mcp::json::parse(transport->lastSentMessage);
        TM_ASSERT_EQ(sent["method"].get<std::string>(), "test/method", "Should send immediately without debounce");
    }

    // 场景 2: 配置去重后，快速连续调用只发送最后一次
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

        // 配置去重，窗口 200ms
        session->enableNotificationDebounce("resources/updated", std::chrono::milliseconds(200));

        // 快速连续发送 3 次
        session->sendNotificationDebounced("resources/updated", {{"uri", "file:///a"}, {"v", 1}});
        session->sendNotificationDebounced("resources/updated", {{"uri", "file:///a"}, {"v", 2}});
        session->sendNotificationDebounced("resources/updated", {{"uri", "file:///a"}, {"v", 3}});

        // 等待去重窗口结束
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // 最后一条应该是 v=3
        mcp::json sent = mcp::json::parse(transport->lastSentMessage);
        TM_ASSERT_EQ(sent["method"].get<std::string>(), "resources/updated", "Method should match");
        TM_ASSERT_EQ(sent["params"]["v"].get<int>(), 3, "Should send the last value (v=3)");
    }
}
