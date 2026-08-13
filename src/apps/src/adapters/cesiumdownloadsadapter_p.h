#pragma once

#include <memory>

namespace mcp_qt {
class IMcpAppContentAdapter;
namespace content_adapters {
std::shared_ptr<IMcpAppContentAdapter> createCesiumDownloadsAdapter();
}
} // namespace mcp_qt
