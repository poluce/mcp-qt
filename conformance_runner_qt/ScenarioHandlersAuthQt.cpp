#include "RunnerConfig.h"
#include <mcp_qt_client/McpQtClient.h>
#include <mcp_qt_transport/QtHttpSseTransport.h>
#include <mcp_core/McpOAuthClient.h>
#include <QTimer>
#include <atomic>
#include <iostream>

namespace mcp_conformance {

// ========== 基本场景（McpQtClient / Qt 原生 QNAM）==========

int runInitialize(const RunnerConfig& c) {
    auto cl = mcp_qt::McpQtClient::createForTest();
    if (!c.protocolVersion.empty()) {
        cl->setProtocolVersion(QString::fromStdString(c.protocolVersion));
    }
    auto t = std::make_shared<mcp_qt::QtHttpSseTransport>(c.serverUrl);
    if (!c.protocolVersion.empty()) {
        t->setProtocolVersion(c.protocolVersion);
    }
    QString err;
    if (!cl->connectToTransportAndWait(t, "mcp-conformance-client-cpp", "1.0.0", 10000, &err)) {
        std::cerr << "[runInitialize] connect failed: " << err.toStdString() << std::endl;
        return 1;
    }

    QEventLoop loop;
    bool hasError = false;
    QString listToolsErr;
    cl->listToolsAsync("", [&](const std::vector<mcp_qt::McpQtTool>&, const QString&, const QString& errVal) {
        hasError = !errVal.isEmpty();
        listToolsErr = errVal;
        loop.quit();
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();

    if (hasError) {
        std::cerr << "[runInitialize] listToolsAsync failed: " << listToolsErr.toStdString() << std::endl;
        return 1;
    }
    return 0;
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
    auto cl = mcp_qt::McpQtClient::createForTest();
    if (!c.protocolVersion.empty()) {
        cl->setProtocolVersion(QString::fromStdString(c.protocolVersion));
    }
    auto t = std::make_shared<mcp_qt::QtHttpSseTransport>(c.serverUrl);
    if (!c.protocolVersion.empty()) {
        t->setProtocolVersion(c.protocolVersion);
    }
    QString errStr;
    if (!cl->connectToTransportAndWait(t, "mcp-qt-client", "1.0.0", 10000, &errStr)) {
        return 1;
    }

    QEventLoop loop;
    bool hasError = false;
    cl->listToolsAsync("", [&](const std::vector<mcp_qt::McpQtTool>&, const QString&, const QString& err) {
        hasError = !err.isEmpty();
        loop.quit();
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    if (hasError) return 1;

    auto res = cl->callTool("get_system_time", QJsonObject{});
    
    // The test suite will intentionally close the stream to trigger a reconnect.
    // When it does, callTool will fail. We must NOT exit immediately, otherwise
    // the client dies before it can reconnect! sse-retry 会中途断开，等待它自动重连并走完
    // 给足时间重连 (500ms 重连 + 余量)
    QEventLoop loop2;
    QTimer::singleShot(5000, &loop2, &QEventLoop::quit);
    loop2.exec();
    
    return 0;
}

int runElicitationDefaults(const RunnerConfig& c) {
    // 使用 createForTest + connectToTransportAndWait，确保在 initialize 前注册 handler 和 capability
    auto cl = mcp_qt::McpQtClient::createForTest();
    if (!c.protocolVersion.empty()) {
        cl->setProtocolVersion(QString::fromStdString(c.protocolVersion));
    }

    // 预注册 elicitation capability（在 connectToTransportAndWait 中会在 initialize 前生效）
    QJsonObject ec; ec["form"] = QJsonObject{{"applyDefaults", true}};
    cl->registerCapability("elicitation", ec);

    // 预置 handler（connectToTransportAndWait 中会在 start/initialize 前安装 to session）
    cl->setElicitationHandler([](const QJsonObject&, std::function<void(const QJsonObject&, const QJsonObject&)> callback) {
        QJsonObject r; r["action"] = "accept"; r["content"] = QJsonObject{};
        callback(r, QJsonObject{});
    });

    // 现在连接——connectToTransportAndWait 会先应用 handler 和能力，再 start 和 initialize
    auto t = std::make_shared<mcp_qt::QtStatelessHttpTransport>(QString::fromStdString(c.serverUrl));
    if (!c.protocolVersion.empty()) {
        t->setProtocolVersion(c.protocolVersion);
    }
    QString errStr;
    if (!cl->connectToTransportAndWait(t, "mcp-qt-client", "1.0.0", 10000, &errStr)) return 1;

    // 先获取工具列表
    QEventLoop loop1;
    std::vector<mcp_qt::McpQtTool> tools;
    cl->listToolsAsync("", [&](const std::vector<mcp_qt::McpQtTool>& t, const QString&, const QString& err) {
        tools = t;
        loop1.quit();
    });
    QTimer::singleShot(10000, &loop1, &QEventLoop::quit);
    loop1.exec();

    if (tools.empty()) {
        std::cerr << "[Elicitation] No tools available" << std::endl;
        return 1;
    }

    // 调用第一个可用的工具
    QString toolName = tools[0].name;
    std::cerr << "[Elicitation] Calling tool: " << toolName.toStdString() << std::endl;

    QEventLoop loop2;
    bool hasError = false;
    cl->callToolAsync(toolName, QJsonObject{},
        [&](mcp_qt::McpResult res) {
            hasError = res.isError || res.data.isEmpty();
            loop2.quit();
        });
    QTimer::singleShot(10000, &loop2, &QEventLoop::quit);
    loop2.exec();

    return hasError ? 1 : 0;
}

// ========== Auth 场景（QtStatelessHttpTransport + OAuth 支持）==========

static int _raQt(const RunnerConfig& c, bool ct) {
    auto cl = mcp_qt::McpQtClient::createForTest();
    auto t = std::make_shared<mcp_qt::QtStatelessHttpTransport>(QString::fromStdString(c.serverUrl));
    if (!c.protocolVersion.empty()) {
        t->setProtocolVersion(c.protocolVersion);
    } else {
        // 旧 conformance 框架（0.1.x）不传 spec-version：按 2025-11-25 legacy 处理
        t->setProtocolVersion("2025-11-25");
    }

    // 2026-07-28 无状态模式：跳过 legacy initialize 握手（SEP-2575/2567）。
    // 仅当框架显式指定 spec-version=2026-07-28 时启用；为空（旧框架）或其它版本走 legacy。
    if (c.protocolVersion == "2026-07-28") {
        cl->setStatelessMode(true);
    }

    // 设置 OAuth 支持
    auto oc = cl->oauthClient();
    t->setTokenProvider([oc]() -> std::string {
        if (!oc) return {};
        return oc->getCurrentToken().accessToken;
    });

    // OAuth 重试计数器（限制最多 3 次，符合 auth/scope-retry-limit 场景要求）
    auto authRetryCount = std::make_shared<int>(0);
    constexpr int kMaxAuthRetries = 3;
    // OAuth 失败标志：拒绝类场景（iss 不匹配/缺失等）中客户端必须快速失败，避免超时
    auto oauthFailed = std::make_shared<std::atomic<bool>>(false);

    t->setAuthRetryHandler([oc, &c, authRetryCount, oauthFailed](const std::string& wwwAuth) -> bool {
        if (!oc) return false;
        if (*oauthFailed) return false;

        // 检查重试次数限制
        (*authRetryCount)++;
        if (*authRetryCount > kMaxAuthRetries) {
            std::cerr << "[OAuth] Max auth retries (" << kMaxAuthRetries << ") exceeded, giving up" << std::endl;
            return false;
        }

        nlohmann::json ctx;
        // 使用 context 中的所有 OAuth 相关字段
        if (!c.context.empty()) {
            if (c.context.contains("client_id")) ctx["client_id"] = c.context["client_id"];
            if (c.context.contains("client_secret")) ctx["client_secret"] = c.context["client_secret"];
            if (c.context.contains("private_key_pem")) ctx["private_key_pem"] = c.context["private_key_pem"];
            if (c.context.contains("signing_algorithm")) ctx["signing_algorithm"] = c.context["signing_algorithm"];
        }
        // 401 即服务器拒绝当前 token：先作废，避免 OAuthFlowLock 复用被拒 token
        // （SEP-2352：AS 变更后旧 token 仍"未过期"，不复用则无法触发重新发现+重新注册）
        oc->setCurrentToken(mcp::OAuthToken{});
        bool ok = mcp_qt::McpQtClient::runOAuthFlow(c.serverUrl, ctx, wwwAuth, oc);
        if (!ok) {
            *oauthFailed = true;
            return false;
        }
        return true;
    });

    QString errStr;
    // 使用 connectToTransportAndWait 自动进行 initialize 握手与 Auth 认证逻辑
    if (!cl->connectToTransportAndWait(t, "mcp-qt-client", "1.0.0", 15000, &errStr)) return 1;
    // OAuth 拒绝（iss 校验失败等）：客户端应整体失败，不再继续（allowClientError 场景）
    if (*oauthFailed) return 1;

    QEventLoop loop;
    bool hasError = false;
    cl->listToolsAsync("", [&](const std::vector<mcp_qt::McpQtTool>&, const QString&, const QString& err) {
        hasError = !err.isEmpty();
        loop.quit();
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    if (hasError) return 1;

    if (ct) {
        auto res = cl->callTool("get_system_time", QJsonObject{});
        if (res.isError) return 1;
    }
    return 0;
}

int runAuthFlow(const RunnerConfig& c) { return _raQt(c, true); }
int runClientCredentialsFlow(const RunnerConfig& c) { return _raQt(c, false); }

} // namespace mcp_conformance
