#include "tests/common.h"
#include <atomic>

void test_notifications() {
    // Scenario 1: Listen to notifications/resources/updated notification
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();
        
        session->initialize("test", "1", [](bool, const mcp::json&){});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        bool notificationReceived = false;
        session->registerNotificationHandler("notifications/resources/updated", [&](const mcp::json& params) {
            if (params.contains("uri") && params["uri"] == "file:///logs/system.log") {
                notificationReceived = true;
            }
        });

        // Server pushes resources update notification
        mcp::json notifyMsg = {
            {"jsonrpc", "2.0"},
            {"method", "notifications/resources/updated"},
            {"params", {{"uri", "file:///logs/system.log"}}}
        };
        transport->pushServerMessage(notifyMsg.dump());
        TM_ASSERT_TRUE(notificationReceived, "Scenario 1: subscription notification not received.");
    }

    // Scenario 2: prompts/listChanged Notification
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();
        
        session->initialize("test", "1", [](bool, const mcp::json&){});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        // prompts/listChanged Notification
        bool promptsChangedReceived = false;
        session->registerNotificationHandler("notifications/prompts/list-changed", [&](const mcp::json&) {
            promptsChangedReceived = true;
        });

        mcp::json notifyMsg = {
            {"jsonrpc", "2.0"},
            {"method", "notifications/prompts/list-changed"},
            {"params", mcp::json::object()}
        };
        transport->pushServerMessage(notifyMsg.dump());
        TM_ASSERT_TRUE(promptsChangedReceived, "Scenario 2: prompts list-changed notification failed.");
    }

    // Scenario 3: Async middle-of-request notification & deregistration test
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();
        
        session->initialize("test", "1", [](bool, const mcp::json&){});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        // 3.1: Register notifications
        std::atomic<int> updateNotificationCount{0};
        session->registerNotificationHandler("notifications/resources/updated", [&](const mcp::json&) {
            updateNotificationCount++;
        });

        std::atomic<bool> listChangedReceived{false};
        session->registerNotificationHandler("notifications/prompts/list-changed", [&](const mcp::json&) {
            listChangedReceived = true;
        });

        // 3.2: Initiate listTools request
        bool listCallbackExecuted = false;
        session->listTools([&](const std::vector<mcp::McpTool>&, const mcp::json&) {
            listCallbackExecuted = true;
        });

        // 3.3: Async mock push: Notifications arrive first, then the response
        mcp::json notify1 = {{"jsonrpc", "2.0"}, {"method", "notifications/resources/updated"}, {"params", mcp::json::object()}};
        mcp::json notify2 = {{"jsonrpc", "2.0"}, {"method", "notifications/prompts/list-changed"}, {"params", mcp::json::object()}};
        mcp::json listResp = {
            {"jsonrpc", "2.0"},
            {"id", 2},
            {"result", {{"tools", mcp::json::array()}}}
        };

        transport->pushServerMessageAsync(notify1.dump(), 10);
        transport->pushServerMessageAsync(notify2.dump(), 30);
        transport->pushServerMessageAsync(listResp.dump(), 60);

        // 等待 listTools 回调执行完成
        int maxWaitMs = 500;
        while (!listCallbackExecuted && maxWaitMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            maxWaitMs -= 5;
        }

        // 再等一小段时间确保通知回调也已完成
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        TM_ASSERT_TRUE(listCallbackExecuted, "Scenario 3: Response callback should be executed despite async middle notifications.");
        TM_ASSERT_EQ(updateNotificationCount.load(), 1, "Scenario 3: Async resources update notification should be caught.");
        TM_ASSERT_TRUE(listChangedReceived.load(), "Scenario 3: Async prompts list-changed notification should be caught.");

        // 3.4: Deregistration test (Deregister prompts listChanged)
        session->registerNotificationHandler("notifications/prompts/list-changed", nullptr);
        listChangedReceived = false;

        // Push notification again, shouldn't fire
        transport->pushServerMessage(notify2.dump());
        TM_ASSERT_FALSE(listChangedReceived.load(), "Scenario 3: Deregistered notification handler should not be triggered.");
    }
}
