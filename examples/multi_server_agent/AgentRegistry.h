#pragma once

#include <QMap>
#include <QObject>
#include <QStringList>

namespace mcp_agent {

/**
 * @brief 多 agent × 多 MCP 注册表：agent → 服务器集合（各 agent 可重叠）。
 *
 * 纯数据模型 + 变更事件，不含连接管理。对账（AgentReconciler）监听 changed()
 * 把期望态落到 McpHost 的启用集合。
 */
class AgentRegistry : public QObject {
    Q_OBJECT
public:
    explicit AgentRegistry(QObject* parent = nullptr) : QObject(parent) {}

    /// 声明/替换某 agent 的服务器集合（空列表 = 该 agent 不依赖任何服务器）
    void setAgentServers(const QString& agent, const QStringList& servers);
    void removeAgent(const QString& agent);
    /// 清空所有 agent 注册（重载配置时调用）
    void clearAgents();
    /// 一次性替换整张注册表（只发一次 changed，避免逐 agent 对账）
    void setAgents(const QMap<QString, QStringList>& agents);

    QStringList serversFor(const QString& agent) const;
    /// 反向索引：哪些 agent 引用了该服务器
    QStringList agentsFor(const QString& server) const;
    /// 所有 agent 服务器集合的并集（去重）——即期望启动的服务器全集
    QStringList allServers() const;
    QStringList allAgents() const;
    bool isEmpty() const { return m_agentServers.isEmpty(); }

signals:
    void changed();

private:
    QMap<QString, QStringList> m_agentServers;
};

} // namespace mcp_agent
