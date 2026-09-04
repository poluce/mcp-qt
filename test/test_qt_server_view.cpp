#include "tests/common.h"
#include "mcp_qt_client/McpHost.h"
#include "mcp_qt_client/McpServerView.h"
#include "mcp_qt_client/McpServerManager.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QCoreApplication>

// ============================================================================
// McpServerView（issue #9）：按会话/Agent 过滤可见 MCP 服务器与能力
// ============================================================================

void test_qt_server_view_filtering() {
    mcp_qt::McpHost host;

    // 注册两个 mock client（不实际连接，仅提供工具缓存）
    auto mockA = mcp_qt::McpQtClient::createForTest(&host);
    host.manager()->registerClient(QStringLiteral("server-a"), mockA);
    auto mockB = mcp_qt::McpQtClient::createForTest(&host);
    host.manager()->registerClient(QStringLiteral("server-b"), mockB);

    // 模拟服务端 toolsChanged 信号填充工具缓存
    mcp_qt::McpQtTool ta{QStringLiteral("echo"), QStringLiteral("echo tool"), QJsonObject{}};
    emit mockA->toolsChanged({ta});
    mcp_qt::McpQtTool tb{QStringLiteral("fetch"), QStringLiteral("fetch tool"), QJsonObject{}};
    emit mockB->toolsChanged({tb});

    // 视图：只可见 server-a
    mcp_qt::McpServerView view(&host);
    view.setVisibleServers({QStringLiteral("server-a")});
    TM_ASSERT_EQ(view.visibleServers().size(), 1, "visible servers should be 1");
    TM_ASSERT_EQ(view.serverNames().size(), 1, "serverNames alias should match");

    QJsonArray tools = view.exportAllToolsToLlmFormat();
    TM_ASSERT_EQ(tools.size(), 1, "only server-a tools should be visible");
    QJsonObject funcObj = tools.at(0).toObject().value(QStringLiteral("function")).toObject();
    TM_ASSERT_EQ(funcObj.value(QStringLiteral("name")).toString().toStdString(),
                 std::string("server-a_echo"), "tool name should keep server-a prefix");

    // 空列表 = 全部可见
    view.setVisibleServers({});
    tools = view.exportAllToolsToLlmFormat();
    TM_ASSERT_EQ(tools.size(), 2, "empty visible list means all servers visible");

    // toolsForServer 查询
    view.setVisibleServers({QStringLiteral("server-a")});
    auto taList = view.toolsForServer(QStringLiteral("server-a"));
    TM_ASSERT_EQ(taList.size(), 1, "toolsForServer should return server-a tools");
    TM_ASSERT_EQ(taList.at(0).name.toStdString(), std::string("echo"), "toolsForServer returns original names");
    auto tbList = view.toolsForServer(QStringLiteral("server-b"));
    TM_ASSERT_EQ(tbList.size(), 0, "invisible server should return empty tools");
}

void test_qt_server_view_multi_view_isolation() {
    mcp_qt::McpHost host;

    auto mockA = mcp_qt::McpQtClient::createForTest(&host);
    host.manager()->registerClient(QStringLiteral("server-a"), mockA);
    auto mockB = mcp_qt::McpQtClient::createForTest(&host);
    host.manager()->registerClient(QStringLiteral("server-b"), mockB);

    mcp_qt::McpQtTool ta{QStringLiteral("echo"), QStringLiteral("echo tool"), QJsonObject{}};
    emit mockA->toolsChanged({ta});
    mcp_qt::McpQtTool tb{QStringLiteral("fetch"), QStringLiteral("fetch tool"), QJsonObject{}};
    emit mockB->toolsChanged({tb});

    // 两个视图互不影响（模拟两个 agent 会话）
    mcp_qt::McpServerView viewA(&host);
    viewA.setVisibleServers({QStringLiteral("server-a")});
    mcp_qt::McpServerView viewB(&host);
    viewB.setVisibleServers({QStringLiteral("server-b")});

    QJsonArray toolsA = viewA.exportAllToolsToLlmFormat();
    QJsonArray toolsB = viewB.exportAllToolsToLlmFormat();
    TM_ASSERT_EQ(toolsA.size(), 1, "viewA should see only server-a");
    TM_ASSERT_EQ(toolsB.size(), 1, "viewB should see only server-b");
    TM_ASSERT_TRUE(toolsA.at(0).toObject().value(QStringLiteral("function")).toObject()
                       .value(QStringLiteral("name")).toString() == QStringLiteral("server-a_echo"),
                   "viewA tool should be server-a_echo");
    TM_ASSERT_TRUE(toolsB.at(0).toObject().value(QStringLiteral("function")).toObject()
                       .value(QStringLiteral("name")).toString() == QStringLiteral("server-b_fetch"),
                   "viewB tool should be server-b_fetch");

    // 动态切换：viewA 增加可见服务器后导出变化
    viewA.setVisibleServers({QStringLiteral("server-a"), QStringLiteral("server-b")});
    TM_ASSERT_EQ(viewA.exportAllToolsToLlmFormat().size(), 2, "viewA should see both after switch");
    // viewB 不受影响
    TM_ASSERT_EQ(viewB.exportAllToolsToLlmFormat().size(), 1, "viewB should be unaffected");
}

void test_qt_server_view_schema_export() {
    mcp_qt::McpHost host;

    auto mockA = mcp_qt::McpQtClient::createForTest(&host);
    host.manager()->registerClient(QStringLiteral("server-a"), mockA);
    auto mockB = mcp_qt::McpQtClient::createForTest(&host);
    host.manager()->registerClient(QStringLiteral("server-b"), mockB);

    mcp_qt::McpQtTool ta{QStringLiteral("echo"), QStringLiteral("echo tool"), QJsonObject{}};
    emit mockA->toolsChanged({ta});
    mcp_qt::McpQtTool tb{QStringLiteral("fetch"), QStringLiteral("fetch tool"), QJsonObject{}};
    emit mockB->toolsChanged({tb});

    // MCP Schema 格式导出同样按视图过滤
    mcp_qt::McpServerView view(&host);
    view.setVisibleServers({QStringLiteral("server-b")});
    QJsonArray schema = view.exportAllToolsAsMcpSchema();
    TM_ASSERT_EQ(schema.size(), 1, "schema export should filter by visible servers");
    TM_ASSERT_EQ(schema.at(0).toObject().value(QStringLiteral("name")).toString().toStdString(),
                 std::string("server-b_fetch"), "schema tool name should keep prefix");

    // 不可见服务器的 prompts/resources 导出为空（mock client 无 session，list 返回空）
    QJsonArray prompts = view.exportAllPrompts();
    TM_ASSERT_EQ(prompts.size(), 0, "prompts export should not crash and be empty for mock");
    QJsonArray resources = view.exportAllResources();
    TM_ASSERT_EQ(resources.size(), 0, "resources export should not crash and be empty for mock");
}
