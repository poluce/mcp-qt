#include "mcp_qt_apps/McpAppContentAdapter.h"

namespace mcp_qt {

void McpAppContentAdapterRegistry::add(std::shared_ptr<IMcpAppContentAdapter> adapter)
{
    if (adapter) m_adapters.push_back(std::move(adapter));
}

McpAppContent McpAppContentAdapterRegistry::adapt(McpAppContent content) const
{
    for (const auto& adapter : m_adapters) {
        if (!adapter || !adapter->matches(content)) continue;
        adapter->adapt(content);
        content.appliedAdapters.append(adapter->id());
    }
    return content;
}

} // namespace mcp_qt
