#include "mcp_qt_client/McpQtClient.h"
#include "mcp_qt_client/McpToolsModel.h"
#include "tests/common.h"
#include <QEventLoop>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QSignalSpy>

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

    bool ok = client->connectToTransport(transport, "test-client", "1.0.0", 1000);
    if (!ok) return nullptr;
    return client;
}

void test_qt_tools_model() {
    // 1. 验证 McpToolsModel 在空 Client 时的基础表现与防错
    {
        mcp_qt::McpToolsModel model;
        model.setClient(nullptr);
        TM_ASSERT_EQ(model.rowCount(), 0, "No client: rowCount should be 0");

        QSignalSpy errSpy(&model, &mcp_qt::McpToolsModel::refreshError);
        model.refresh();
        TM_ASSERT_EQ(errSpy.count(), 1, "Should emit error when refreshing without client");
    }

    // 2. 端到端集成：验证 refresh 真实填充，notifications/tools/list-changed 自动刷新，以及防 Churn 优化
    {
        auto transport = std::make_shared<MockTransport>();
        auto client = createIntegrationMockClient(transport);
        TM_ASSERT_TRUE(client != nullptr, "Client must be ready");

        // 绑定 Model
        auto model = client->createToolsModel();
        TM_ASSERT_TRUE(model != nullptr, "Factory createToolsModel check");
        TM_ASSERT_EQ(model->rowCount(), 0, "Initially empty");

        // 用以监听 modelReset 的 QSignalSpy
        QSignalSpy resetSpy(model.get(), &QAbstractItemModel::modelReset);
        QSignalSpy countSpy(model.get(), &mcp_qt::McpToolsModel::countChanged);

        // 第一次 refresh() 时，模拟服务端返回两个 Tool 的列表
        transport->onSendCallback = [transport](const std::string& msg) {
            auto json = nlohmann::json::parse(msg);
            if (json["method"] == "tools/list") {
                int64_t id = json["id"].get<int64_t>();
                nlohmann::json resp = {
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", {
                        {"tools", {
                            {{"name", "tool-1"}, {"description", "first"}, {"inputSchema", {{"type", "object"}}}},
                            {{"name", "tool-2"}, {"description", "second"}, {"inputSchema", {{"type", "object"}}}}
                        }}
                    }}
                };
                std::lock_guard<std::mutex> lock(transport->m_state->mutex);
                if (transport->m_state->onMessage) {
                    transport->m_state->onMessage(resp.dump());
                }
            }
        };

        // 触发填充
        model->refresh();
        waitEvents(50);

        TM_ASSERT_EQ(model->rowCount(), 2, "Model should now contain 2 rows");
        TM_ASSERT_EQ(resetSpy.count(), 1, "Model should be reset exactly once for new data");
        TM_ASSERT_EQ(countSpy.count(), 1, "countChanged should be emitted once");

        // 读取角色 data
        QModelIndex idx = model->index(0, 0);
        TM_ASSERT_EQ(model->data(idx, mcp_qt::McpToolsModel::NameRole).toString().toStdString(), "tool-1", "NameRole check");
        TM_ASSERT_EQ(model->data(idx, mcp_qt::McpToolsModel::DescriptionRole).toString().toStdString(), "first", "DescriptionRole check");

        // --- 防 Churn 验证 ---
        // 我们再次刷新，让服务端返回“一模一样”的数据
        model->refresh();
        waitEvents(50);

        TM_ASSERT_EQ(model->rowCount(), 2, "Row count remains 2");
        TM_ASSERT_EQ(resetSpy.count(), 1, "modelReset count MUST NOT increase because data is identical (Anti-Churn logic)");

        // --- 自动刷新验证 ---
        // 我们让服务端修改工具数据（只返回 1 个工具）
        transport->onSendCallback = [transport](const std::string& msg) {
            auto json = nlohmann::json::parse(msg);
            if (json["method"] == "tools/list") {
                int64_t id = json["id"].get<int64_t>();
                nlohmann::json resp = {
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", {
                        {"tools", {
                            {{"name", "tool-1-modified"}, {"description", "modified"}, {"inputSchema", {{"type", "object"}}}}
                        }}
                    }}
                };
                std::lock_guard<std::mutex> lock(transport->m_state->mutex);
                if (transport->m_state->onMessage) {
                    transport->m_state->onMessage(resp.dump());
                }
            }
        };

        // 模拟服务端广播 notifications/tools/list_changed
        nlohmann::json notifyJson = {
            {"jsonrpc", "2.0"},
            {"method", "notifications/tools/list_changed"},
            {"params", {}}
        };
        {
            std::lock_guard<std::mutex> lock(transport->m_state->mutex);
            transport->m_state->onMessage(notifyJson.dump());
        }

        // 让事件循环转动以接收并自动处理刷新
        waitEvents(50);

        TM_ASSERT_EQ(model->rowCount(), 1, "Model should automatically decrease to 1 row due to list-changed notification");
        TM_ASSERT_EQ(resetSpy.count(), 2, "modelReset should increase to 2 as the data changed");
        TM_ASSERT_EQ(model->data(model->index(0, 0), mcp_qt::McpToolsModel::NameRole).toString().toStdString(), "tool-1-modified", "New tool data check");
    }
}
