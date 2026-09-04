#include "tests/common.h"
#include "mcp_qt_client/McpServerManager.h"
#include "mcp_qt_client/McpToolRouter.h"
#include "mcp_qt_client/McpJsonConfigLoader.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QCoreApplication>
#include <string>
#include <QThread>

void test_qt_server_manager_router() {
    // 1. 测试生命周期管家 McpServerManager
    mcp_qt::McpServerManager manager;
    
    // 注入模拟配置，这里包含一个 stdio 和一个 http 的配置，但不进行实际的物理连接启动
    QJsonObject config{
        {"mcpServers", QJsonObject{
            {"test-server-a", QJsonObject{
                {"command", "node"},
                {"args", QJsonArray{"script.js"}},
                {"env", QJsonObject{
                    {"TEST_VAR", "123"}
                }}
            }},
            {"test-server-b", QJsonObject{
                {"url", "http://localhost:12345/sse"},
                {"type", "http"}
            }}
        }}
    };

    // 检查环境变量设置与加载逻辑
    mcp_qt::McpJsonConfigLoader loader(config);
    bool loadOk = manager.loadServers(loader.load());
    TM_ASSERT_TRUE(loadOk, "McpServerManager config load should succeed");

    QCoreApplication::processEvents();
    QThread::msleep(100);
    QCoreApplication::processEvents();

    // 验证客户端是否创建成功
    auto clientA = manager.client("test-server-a");
    TM_ASSERT_TRUE(clientA != nullptr, "clientA should be instantiated");
    // clientB（http 类型，未显式 protocolVersion）会先做协议自动探测，
    // 注册是异步的——轮询等待探测完成（探测含 3s 超时，最多等 5 秒）。
    auto clientB = manager.client("test-server-b");
    for (int i = 0; i < 100 && !clientB; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(50);
        clientB = manager.client("test-server-b");
    }
    TM_ASSERT_TRUE(clientB != nullptr, "clientB should be instantiated");

    // 2. 测试工具命名空间与路由层 McpToolRouter
    mcp_qt::McpToolRouter router(&manager);

    // 手动注册一个专用于 Mock 的测试客户端
    auto mockClient = mcp_qt::McpQtClient::createForTest(&manager);
    manager.registerClient("mock-server", mockClient);

    // 模拟在该 Mock 客户端里塞入一些工具 definition
    mcp_qt::McpQtTool t1{QStringLiteral("calculate"), QStringLiteral("计算器功能"), QJsonObject{}};
    
    // 模拟服务端 toolsChanged 信号
    emit mockClient->toolsChanged({t1});

    // 导出并检查带有命名空间的 LLM 工具定义
    QJsonArray llmTools = router.exportAllToolsToLlmFormat(mcp_qt::McpQtClient::LlmFormat::OpenAI);
    
    // 我们在这个 manager 中有 3 个已注册的 client (test-server-a, test-server-b, mock-server)
    // 只有 mock-server 模拟触发了 toolsChanged（即有缓存工具）
    TM_ASSERT_EQ(llmTools.size(), 1, "We should have exactly 1 tool exported");
    
    QJsonObject toolObj = llmTools.at(0).toObject();
    TM_ASSERT_EQ(toolObj["type"].toString().toStdString(), std::string("function"), "LLM format type check");
    
    QJsonObject funcObj = toolObj["function"].toObject();
    TM_ASSERT_EQ(funcObj["name"].toString().toStdString(), std::string("mock-server_calculate"), "Tool name must be namespaced with mock-server prefix");

    // 3. 测试路由解析与分发
    QPair<QString, QString> parsed = router.parseToolName("mock-server_calculate");
    TM_ASSERT_EQ(parsed.first.toStdString(), std::string("mock-server"), "Router should resolve serverName correctly");
    TM_ASSERT_EQ(parsed.second.toStdString(), std::string("calculate"), "Router should resolve original tool name correctly");

    // 测试未注册前缀的后备方案解析
    QPair<QString, QString> parsedFallback = router.parseToolName("mock-server_other_tool");
    TM_ASSERT_EQ(parsedFallback.first.toStdString(), std::string("mock-server"), "Fallback should resolve serverName correctly");
    TM_ASSERT_EQ(parsedFallback.second.toStdString(), std::string("other_tool"), "Fallback should resolve original tool name correctly");

    // 测试解析不存在的服务器
    QPair<QString, QString> parsedInvalid = router.parseToolName("unknown-server_do_something");
    TM_ASSERT_TRUE(parsedInvalid.first.isEmpty(), "Parsed server name should be empty for unknown prefix");
}
