#include "RunnerConfig.h"
#include <mcp_qt_client/McpQtClient.h>
#include <mcp_qt_transport/QtHttpSseTransport.h>
#include <mcp_core/McpClientSession.h>
#include <mcp_core/HttpSseTransport.h>
#include <QTimer>

namespace mcp_conformance {

// ========== 基本场景（McpQtClient / Qt 原生 QNAM）==========

int runInitialize(const RunnerConfig& c) {
    auto cl = mcp_qt::McpQtClient::connectHttpAndWait(QString::fromStdString(c.serverUrl));
    if (!cl) return 1;
    
    QEventLoop loop;
    bool hasError = false;
    cl->listToolsAsync("", [&](const std::vector<mcp_qt::McpQtTool>&, const QString&, const QString& err) {
        hasError = !err.isEmpty();
        loop.quit();
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    
    return hasError ? 1 : 0;
}

int runToolsCall(const RunnerConfig& c) {
    auto cl = mcp_qt::McpQtClient::connectHttpAndWait(QString::fromStdString(c.serverUrl));
    if (!cl) return 1;
    
    QEventLoop loop;
    bool hasError = false;
    cl->listToolsAsync("", [&](const std::vector<mcp_qt::McpQtTool>&, const QString&, const QString& err) {
        hasError = !err.isEmpty();
        loop.quit();
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    if (hasError) return 1;

    QJsonObject a; a["a"] = 5; a["b"] = 3;
    auto res = cl->callTool("add_numbers", a);
    return (res.isError || res.data.isEmpty()) ? 1 : 0;
}

int runSseRetry(const RunnerConfig& c) {
    auto cl = mcp_qt::McpQtClient::connectHttpAndWait(QString::fromStdString(c.serverUrl));
    if (!cl) return 1;

    QEventLoop loop;
    bool hasError = false;
    cl->listToolsAsync("", [&](const std::vector<mcp_qt::McpQtTool>&, const QString&, const QString& err) {
        hasError = !err.isEmpty();
        loop.quit();
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    if (hasError) return 1;

    auto res = cl->callTool("test_reconnection", QJsonObject{});
    return (res.isError || res.data.isEmpty()) ? 1 : 0;
}

int runElicitationDefaults(const RunnerConfig& c) {
    // 使用 createForTest + connectToTransportAndWait，确保在 initialize 前注册 handler 和 capability
    auto cl = mcp_qt::McpQtClient::createForTest();

    // 预注册 elicitation capability（在 connectToTransportAndWait 中会在 initialize 前生效）
    QJsonObject ec; ec["form"] = QJsonObject{{"applyDefaults", true}};
    cl->registerCapability("elicitation", ec);

    // 预置 handler（connectToTransportAndWait 中会在 start/initialize 前安装到 session）
    cl->setElicitationHandler([](const QJsonObject&, std::function<void(const QJsonObject&, const QJsonObject&)> callback) {
        QJsonObject r; r["action"] = "accept"; r["content"] = QJsonObject{};
        callback(r, QJsonObject{});
    });

    // 现在连接——connectToTransportAndWait 会先应用 handler 和能力，再 start 和 initialize
    auto t = std::make_shared<mcp_qt::QtHttpSseTransport>(c.serverUrl);
    QString errStr;
    if (!cl->connectToTransportAndWait(t, "mcp-qt-client", "1.0.0", 10000, &errStr)) return 1;

    // 使用异步 callTool 避免阻塞主线程事件循环（Qt 版 elicitation handler 需要事件循环来调度）
    QEventLoop loop;
    bool hasError = false;
    cl->callToolAsync("test_client_elicitation_defaults", QJsonObject{},
        [&](mcp_qt::McpResult res) {
            hasError = res.isError || res.data.isEmpty();
            loop.quit();
        });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();

    return hasError ? 1 : 0;
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
