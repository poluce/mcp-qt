#pragma once

#include <QString>

namespace mcp_agent {

// Resolve an API key without requiring the GUI/IDE process to be restarted
// after a Windows user environment variable was created or changed.
QString resolveLlmApiKey();

} // namespace mcp_agent
