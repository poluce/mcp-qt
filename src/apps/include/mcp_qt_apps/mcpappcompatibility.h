#pragma once

#include <memory>

namespace mcp_qt {

class McpAppContentAdapterRegistry;

/**
 * Creates the optional, host-maintained compatibility adapter set.
 * Generic renderers do not enable this automatically; the embedding host opts in.
 */
std::shared_ptr<McpAppContentAdapterRegistry> createStandardMcpAppCompatibilityAdapters();

} // namespace mcp_qt
