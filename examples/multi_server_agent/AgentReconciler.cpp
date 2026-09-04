#include "AgentReconciler.h"
#include "AgentRegistry.h"
#include "mcp_qt_client/McpHost.h"

namespace mcp_agent {

AgentReconciler::AgentReconciler(mcp_qt::McpHost* host, AgentRegistry* registry, QObject* parent)
    : QObject(parent), m_host(host), m_registry(registry) {
    connect(registry, &AgentRegistry::changed, this, &AgentReconciler::reconcile);
}

void AgentReconciler::reconcile() {
    if (!m_host) return;
    const QStringList wanted = m_registry->allServers();
    // 对账对象是全部配置的服务器（含未启用的），而非仅已注册的
    for (const QString& name : m_host->configuredServerNames()) {
        m_host->setServerEnabled(name, wanted.contains(name), /*persist*/ false);
    }
}

} // namespace mcp_agent
