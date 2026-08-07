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
int runStateless20260728Http(const RunnerConfig&);
int runToolsCall2026(const RunnerConfig&);
int runRequestMetadata(const RunnerConfig&);
int runSep2322ClientRequestState(const RunnerConfig&);
int runHttpStandardHeaders(const RunnerConfig&);
int runHttpCustomHeaders(const RunnerConfig&);
int runHttpInvalidToolHeaders(const RunnerConfig&);
int runJsonSchemaRefNoDeref(const RunnerConfig&);

static const std::map<std::string, ScenarioHandler> kHandlers = {
    {"stateless-2026-07-28", &runStateless20260728},
    {"stateless-2026-07-28-http", &runStateless20260728Http},
    // 官方 conformance 0.2.0-alpha 的 2026-07-28 客户端场景
    {"tools_call", &runToolsCall2026},
    {"request-metadata", &runRequestMetadata},
    {"sep-2322-client-request-state", &runSep2322ClientRequestState},
    {"http-standard-headers", &runHttpStandardHeaders},
    {"http-custom-headers", &runHttpCustomHeaders},
    {"http-invalid-tool-headers", &runHttpInvalidToolHeaders},
    {"json-schema-ref-no-deref", &runJsonSchemaRefNoDeref},
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
    {"auth/cross-app-access-complete-flow", &runAuthFlow},
    // 2026-07-28 auth 场景（OAuth iss 校验等）
    {"auth/authorization-server-migration", &runAuthFlow},
    {"auth/iss-supported", &runAuthFlow},
    {"auth/iss-not-advertised", &runAuthFlow},
    {"auth/iss-supported-missing", &runAuthFlow},
    {"auth/iss-wrong-issuer", &runAuthFlow},
    {"auth/iss-unexpected", &runAuthFlow},
    {"auth/iss-normalized", &runAuthFlow},
    {"auth/metadata-issuer-mismatch", &runAuthFlow}
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
