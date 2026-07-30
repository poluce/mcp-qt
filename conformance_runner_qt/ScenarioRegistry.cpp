#include "ScenarioRegistry.h"

#include <map>
#include <stdexcept>

namespace mcp_conformance {

using ScenarioHandler = int (*)(const RunnerConfig&);

int runInitialize(const RunnerConfig&);
int runToolsCall(const RunnerConfig&);
int runSseRetry(const RunnerConfig&);
int runElicitationDefaults(const RunnerConfig&);
int runAuthFlow(const RunnerConfig&);
int runClientCredentialsFlow(const RunnerConfig&);
int runStateless20260728(const RunnerConfig&);

static const std::map<std::string, ScenarioHandler> kHandlers = {
    {"stateless-2026-07-28", &runStateless20260728},
    {"initialize", &runInitialize},
    {"tools_call", &runToolsCall},
    {"sse-retry", &runSseRetry},
    {"elicitation-sep1034-client-defaults", &runElicitationDefaults},
    {"auth/basic-cimd", &runAuthFlow},
    {"auth/metadata-default", &runAuthFlow},
    {"auth/metadata-var1", &runAuthFlow},
    {"auth/metadata-var2", &runAuthFlow},
    {"auth/metadata-var3", &runAuthFlow},
    {"auth/2025-03-26-oauth-metadata-backcompat", &runAuthFlow},
    {"auth/2025-03-26-oauth-endpoint-fallback", &runAuthFlow},
    {"auth/scope-from-www-authenticate", &runAuthFlow},
    {"auth/scope-from-scopes-supported", &runAuthFlow},
    {"auth/scope-omitted-when-undefined", &runAuthFlow},
    {"auth/scope-step-up", &runAuthFlow},
    {"auth/scope-retry-limit", &runAuthFlow},
    {"auth/token-endpoint-auth-basic", &runAuthFlow},
    {"auth/token-endpoint-auth-post", &runAuthFlow},
    {"auth/token-endpoint-auth-none", &runAuthFlow},
    {"auth/offline-access-scope", &runAuthFlow},
    {"auth/offline-access-not-supported", &runAuthFlow},
    {"auth/pre-registration", &runAuthFlow},
    {"auth/client-credentials-jwt", &runClientCredentialsFlow},
    {"auth/client-credentials-basic", &runClientCredentialsFlow},
    {"auth/cross-app-access-complete-flow", &runAuthFlow}
};

int runScenario(const RunnerConfig& config) {
    const auto it = kHandlers.find(config.scenario);
    if (it == kHandlers.end()) {
        throw std::runtime_error("Unknown conformance scenario: " + config.scenario);
    }
    return it->second(config);
}

std::set<std::string> registeredScenarioNames() {
    std::set<std::string> names;
    for (const auto& entry : kHandlers) names.insert(entry.first);
    return names;
}

} // namespace mcp_conformance
