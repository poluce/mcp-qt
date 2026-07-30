#include "RunnerConfig.h"

#include "ConsoleStdioTransport.h"
#include <mcp_core/McpClientSession.h>

namespace mcp_conformance {

int runToolsList(const RunnerConfig& config) {
    auto transport = std::make_shared<mcp::ConsoleStdioTransport>();
    auto session = std::make_shared<mcp::McpClientSession>(transport);
    session->init();
    if (!session->start()) return 1;

    if (config.protocolVersion == "2026-07-28") {
        session->setStatelessMode(true);
        session->setProtocolVersion("2026-07-28");
    } else {
        nlohmann::json serverInfo;
        if (!session->initializeSync("mcp-conformance-client-cpp", "1.0.0", &serverInfo)) return 1;
    }

    nlohmann::json err;
    session->listToolsSync(std::chrono::milliseconds(10000), &err);
    return err.empty() ? 0 : 1;
}

} // namespace mcp_conformance
