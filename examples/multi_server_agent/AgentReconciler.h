#pragma once

#include <QObject>

namespace mcp_qt {
class McpHost;
}

namespace mcp_agent {

class AgentRegistry;

/**
 * @brief 期望态对账器：把 McpHost 的启用集合对账到「所有 agent 服务器集合的并集」。
 *
 * 监听 AgentRegistry::changed() → 自动 reconcile。重叠服务器在并集中只出现一次，
 * 因此被多个 agent 引用的服务器只建立一条连接（去重是对账的结果，而非单独机制）。
 */
class AgentReconciler : public QObject {
    Q_OBJECT
public:
    AgentReconciler(mcp_qt::McpHost* host, AgentRegistry* registry, QObject* parent = nullptr);

    /// 手动触发一次对账（注册表变更时也会自动调用）
    void reconcile();

private:
    mcp_qt::McpHost* m_host;
    AgentRegistry* m_registry;
};

} // namespace mcp_agent
