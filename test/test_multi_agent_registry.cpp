#include "tests/common.h"

#include "AgentRegistry.h"
#include "AgentReconciler.h"
#include "mcp_qt_client/McpHost.h"

#include <QFile>
#include <QTemporaryDir>

// ============================================================================
// 多 agent × 多 MCP MVP：AgentRegistry 数据模型（并集去重 / 反向索引 / 变更）
// ============================================================================
void test_multi_agent_registry_union_and_reverse() {
    mcp_agent::AgentRegistry registry;
    bool changed = false;
    QObject::connect(&registry, &mcp_agent::AgentRegistry::changed, &registry, [&changed]() { changed = true; });

    registry.setAgents({
        {QStringLiteral("agentA"), {QStringLiteral("s1"), QStringLiteral("s2")}},
        {QStringLiteral("agentB"), {QStringLiteral("s2"), QStringLiteral("s3")}}
    });

    TM_ASSERT_TRUE(changed, "setAgents emits changed");
    TM_ASSERT_EQ(registry.allAgents().size(), 2, "two agents registered");
    TM_ASSERT_EQ(registry.allServers().size(), 3, "union dedups shared s2 -> {s1,s2,s3}");
    TM_ASSERT_TRUE(registry.allServers().contains(QStringLiteral("s2")), "shared server in union");
    TM_ASSERT_EQ(registry.agentsFor(QStringLiteral("s2")).size(), 2, "reverse index: s2 referenced by both agents");
    TM_ASSERT_EQ(registry.serversFor(QStringLiteral("agentA")).size(), 2, "agentA servers");

    registry.removeAgent(QStringLiteral("agentB"));
    TM_ASSERT_EQ(registry.allServers().size(), 2, "after removing agentB, union = {s1,s2}");
    TM_ASSERT_EQ(registry.agentsFor(QStringLiteral("s2")).size(), 1, "s2 now referenced by agentA only");
}

// ============================================================================
// 多 agent × 多 MCP MVP：期望态对账（注册表并集 → McpHost 启用集，重叠只启用一次）
// ============================================================================
void test_multi_agent_reconciler_enables_union() {
    QTemporaryDir tmpDir;
    QFile cfg(tmpDir.filePath(QStringLiteral("cfg.json")));
    cfg.open(QIODevice::WriteOnly);
    cfg.write(R"({
        "mcpServers": {
            "s1": {"command": "nonexistent-mcp-test-cmd", "args": ["s1"]},
            "s2": {"command": "nonexistent-mcp-test-cmd", "args": ["s2"]},
            "s3": {"command": "nonexistent-mcp-test-cmd", "args": ["s3"]},
            "s4": {"command": "nonexistent-mcp-test-cmd", "args": ["s4"]}
        }
    })");
    cfg.close();

    mcp_qt::McpHost host;
    TM_ASSERT_TRUE(host.loadConfigFromFile(tmpDir.filePath(QStringLiteral("cfg.json"))), "config loaded");
    TM_ASSERT_TRUE(host.isServerEnabled(QStringLiteral("s4")), "after load all servers enabled by default");

    mcp_agent::AgentRegistry registry;
    mcp_agent::AgentReconciler reconciler(&host, &registry);
    registry.setAgents({
        {QStringLiteral("agentA"), {QStringLiteral("s1"), QStringLiteral("s2")}},
        {QStringLiteral("agentB"), {QStringLiteral("s2"), QStringLiteral("s3")}}
    });

    TM_ASSERT_TRUE(host.isServerEnabled(QStringLiteral("s1")), "union: s1 enabled");
    TM_ASSERT_TRUE(host.isServerEnabled(QStringLiteral("s2")), "union: s2 enabled (shared by both agents)");
    TM_ASSERT_TRUE(host.isServerEnabled(QStringLiteral("s3")), "union: s3 enabled");
    TM_ASSERT_FALSE(host.isServerEnabled(QStringLiteral("s4")), "s4 not referenced by any agent -> disabled");
}
