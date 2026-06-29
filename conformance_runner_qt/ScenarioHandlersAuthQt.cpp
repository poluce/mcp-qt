#include "RunnerConfig.h"
#include <mcp_qt_client/McpQtClient.h>
#include <mcp_core/McpClientSession.h>
#include <mcp_core/HttpSseTransport.h>

namespace mcp_conformance {

// ========== 基本场景（McpQtClient / Qt 原生 QNAM）==========

int runInitialize(const RunnerConfig& c) {
    auto cl = mcp_qt::McpQtClient::connectHttp(QString::fromStdString(c.serverUrl));
    if (!cl) return 1;
    return cl->listTools().empty() ? 1 : 0;
}

int runToolsCall(const RunnerConfig& c) {
    auto cl = mcp_qt::McpQtClient::connectHttp(QString::fromStdString(c.serverUrl));
    if (!cl) return 1;
    auto t = cl->listTools();
    if (t.empty()) return 1;
    QJsonObject a; a["a"] = 5; a["b"] = 3;
    return cl->callTool("add_numbers", a).isEmpty() ? 1 : 0;
}

int runSseRetry(const RunnerConfig& c) {
    auto cl = mcp_qt::McpQtClient::connectHttp(QString::fromStdString(c.serverUrl));
    if (!cl) return 1;
    return cl->callTool("test_reconnection", QJsonObject{}).isEmpty() ? 1 : 0;
}

int runElicitationDefaults(const RunnerConfig& c) {
    auto cl = mcp_qt::McpQtClient::connectHttp(QString::fromStdString(c.serverUrl));
    if (!cl) return 1;
    QJsonObject ec; ec["form"] = QJsonObject{{"applyDefaults", true}};
    cl->registerCapability("elicitation", ec);
    cl->setElicitationHandler([](const QJsonObject&) -> QJsonObject {
        QJsonObject r; r["action"] = "accept"; r["content"] = QJsonObject{}; return r;
    });
    return cl->callTool("test_client_elicitation_defaults", QJsonObject{}).isEmpty() ? 1 : 0;
}

// ========== Auth 场景（HttpSseTransport / 内建完整 OAuth，235/235 已验证）==========

static int _ra(const RunnerConfig& c, bool ct) {
    auto t = std::make_shared<mcp::HttpSseTransport>(c.serverUrl);
    auto s = std::make_shared<mcp::McpClientSession>(t);
    if (!c.protocolVersion.empty()) s->setProtocolVersion(c.protocolVersion);
    s->init(); if (!s->start()) return 1;
    nlohmann::json si; if (!s->initializeSync("mcp-conformance-client-cpp","1.0.0",&si)) return 1;
    nlohmann::json e; s->listToolsSync(std::chrono::milliseconds(10000),&e); if (!e.empty()) return 1;
    if (ct) { nlohmann::json ce; s->callToolSync("test-tool",nlohmann::json::object(),&ce,std::chrono::milliseconds(15000)); if (!ce.empty()) return 1; }
    return 0;
}
int runAuthFlow(const RunnerConfig& c) { return _ra(c, true); }
int runClientCredentialsFlow(const RunnerConfig& c) { return _ra(c, false); }

} // namespace mcp_conformance
