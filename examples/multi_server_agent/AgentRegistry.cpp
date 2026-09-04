#include "AgentRegistry.h"

namespace mcp_agent {

void AgentRegistry::setAgentServers(const QString& agent, const QStringList& servers) {
    m_agentServers[agent] = servers;
    emit changed();
}

void AgentRegistry::removeAgent(const QString& agent) {
    if (m_agentServers.remove(agent)) {
        emit changed();
    }
}

void AgentRegistry::clearAgents() {
    if (m_agentServers.isEmpty()) return;
    m_agentServers.clear();
    emit changed();
}

void AgentRegistry::setAgents(const QMap<QString, QStringList>& agents) {
    m_agentServers = agents;
    emit changed();
}

QStringList AgentRegistry::serversFor(const QString& agent) const {
    return m_agentServers.value(agent);
}

QStringList AgentRegistry::agentsFor(const QString& server) const {
    QStringList agents;
    for (auto it = m_agentServers.constBegin(); it != m_agentServers.constEnd(); ++it) {
        if (it.value().contains(server)) {
            agents << it.key();
        }
    }
    return agents;
}

QStringList AgentRegistry::allServers() const {
    QStringList servers;
    for (const auto& list : m_agentServers) {
        for (const auto& s : list) {
            if (!servers.contains(s)) {
                servers << s;
            }
        }
    }
    return servers;
}

QStringList AgentRegistry::allAgents() const {
    return m_agentServers.keys();
}

} // namespace mcp_agent
