#include "mcp_qt_client/McpQtClient.h"
#include "mcp_qt_client/McpResourceSubscriptionRouter.h"
#include "tests/common.h"
#include <QEventLoop>
#include <QTimer>
#include <QJsonObject>
#include <QList>

// 辅助等待事件循环运转
static void waitEvents(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// 辅助：创建一个连接了 MockTransport 并已完成初始化的 McpQtClient 实例
static std::shared_ptr<mcp_qt::McpQtClient> createIntegrationMockClient(std::shared_ptr<MockTransport> transport) {
    auto client = mcp_qt::McpQtClient::createForTest();

    transport->onSendCallback = [transport](const std::string& msg) {
        auto json = nlohmann::json::parse(msg);
        if (json["method"] == "initialize") {
            int64_t id = json["id"].get<int64_t>();
            nlohmann::json resp = {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"result", {
                    {"protocolVersion", "2025-11-25"},
                    {"capabilities", {
                        {"tools", {{"listChanged", true}}},
                        {"resources", {{"subscribe", true}}}
                    }},
                    {"serverInfo", {
                        {"name", "mock-server"},
                        {"version", "1.0.0"}
                    }}
                }}
            };
            std::lock_guard<std::mutex> lock(transport->m_state->mutex);
            if (transport->m_state->onMessage) {
                transport->m_state->onMessage(resp.dump());
            }
        }
    };

    bool ok = client->connectToTransportAndWait(transport, "test-client", "1.0.0", 1000);
    if (!ok) return nullptr;
    return client;
}

void test_qt_resource_router() {
    // 1. 验证 McpResourceSubscriptionRouter 本身的基本功能
    {
        mcp_qt::McpResourceSubscriptionRouter router;
        QList<QString> received;

        int token = router.subscribe("file:///data/a.json", [&received](const QString& uri, const QJsonObject&) {
            received.append(uri);
        });
        TM_ASSERT_TRUE(token >= 0, "subscribe should return valid token");
        TM_ASSERT_TRUE(router.hasSubscribers("file:///data/a.json"), "URI subscribers check");

        router.dispatch("file:///data/a.json", QJsonObject{{"uri", "file:///data/a.json"}});
        TM_ASSERT_EQ(received.size(), 1, "callback count check");

        router.unsubscribe("file:///data/a.json", token);
        TM_ASSERT_FALSE(router.hasSubscribers("file:///data/a.json"), "after unsubscribe check");
    }

    // 2. 端到端集成：验证 McpQtClient::subscribeResource 的真实路由和 callback 派发
    {
        auto transport = std::make_shared<MockTransport>();
        auto client = createIntegrationMockClient(transport);
        TM_ASSERT_TRUE(client != nullptr, "Client initialization check");

        // 拦截 subscribeResource 请求，模拟服务端响应成功
        transport->onSendCallback = [transport](const std::string& msg) {
            auto json = nlohmann::json::parse(msg);
            if (json["method"] == "resources/subscribe") {
                int64_t id = json["id"].get<int64_t>();
                nlohmann::json resp = {
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", {}}
                };
                std::lock_guard<std::mutex> lock(transport->m_state->mutex);
                if (transport->m_state->onMessage) {
                    transport->m_state->onMessage(resp.dump());
                }
            }
        };

        // 在 client 上注册 callback 式订阅
        QString targetUri = "file:///shared/resource.txt";
        QList<QJsonObject> receivedParams;
        int token = client->subscribeResource(targetUri, [&receivedParams](const QString&, const QJsonObject& params) {
            receivedParams.append(params);
        });

        TM_ASSERT_TRUE(token >= 0, "subscribeResource callback overload should return a valid token");

        // 模拟服务端推送 notifications/resources/updated 通知
        nlohmann::json notifyJson = {
            {"jsonrpc", "2.0"},
            {"method", "notifications/resources/updated"},
            {"params", {
                {"uri", targetUri.toStdString()},
                {"version", "v2"}
            }}
        };

        {
            std::lock_guard<std::mutex> lock(transport->m_state->mutex);
            transport->m_state->onMessage(notifyJson.dump());
        }

        // 让事件循环运转，以便排队通知分发给客户端
        waitEvents(50);

        // 验证 callback 成功响应
        TM_ASSERT_EQ(receivedParams.size(), 1, "Router should correctly dispatch notify payload to registered callback");
        if (!receivedParams.isEmpty()) {
            TM_ASSERT_EQ(receivedParams[0].value("version").toString().toStdString(), "v2", "Payload properties check");
        }

        // 撤销订阅
        client->unsubscribeResourceByToken(targetUri, token);
    }

    // 3. 可重入安全：回调内部退订自身时不应死锁，且后续通知不再触发
    {
        mcp_qt::McpResourceSubscriptionRouter router;
        int callCount = 0;
        int token = -1;

        token = router.subscribe("file:///reentrant.json", [&](const QString& uri, const QJsonObject&) {
            callCount++;
            bool removed = router.unsubscribe(uri, token);
            TM_ASSERT_TRUE(removed, "Self-unsubscribe inside callback should succeed without deadlock");
        });

        int dispatched = router.dispatch("file:///reentrant.json", QJsonObject{{"uri", "file:///reentrant.json"}});
        TM_ASSERT_EQ(dispatched, 1, "First dispatch should reach the callback once");
        TM_ASSERT_EQ(callCount, 1, "Callback should run exactly once before removal");
        TM_ASSERT_FALSE(router.hasSubscribers("file:///reentrant.json"), "Subscriber should be removed after self-unsubscribe");

        dispatched = router.dispatch("file:///reentrant.json", QJsonObject{{"uri", "file:///reentrant.json"}});
        TM_ASSERT_EQ(dispatched, 0, "Second dispatch should reach no subscribers");
        TM_ASSERT_EQ(callCount, 1, "Callback must not run again after self-unsubscribe");
    }
}
