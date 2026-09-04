#include "mcp_qt_apps/McpAppCompatibility.h"
#include "mcp_qt_apps/McpAppContentAdapter.h"
#include "CesiumDownloadsAdapter_p.h"

namespace mcp_qt {

std::shared_ptr<McpAppContentAdapterRegistry> createStandardMcpAppCompatibilityAdapters()
{
    auto registry = std::make_shared<McpAppContentAdapterRegistry>();
    registry->add(content_adapters::createCesiumDownloadsAdapter());
    return registry;
}

} // namespace mcp_qt
