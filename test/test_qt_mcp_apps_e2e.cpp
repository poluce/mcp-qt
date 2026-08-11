// MCP Apps A4 端到端测试（通用协议验证）：
//   - 默认起本地 mock 服务器（test/mcp_apps_server.js）验证完整链路；
//   - 设环境变量 MCP_APPS_E2E_URL=http://host:port/mcp 时连接外部 MCP Apps 服务器
//     （如官方 @modelcontextprotocol/server-threejs：npx @modelcontextprotocol/server-threejs → :3001/mcp）。
#include "tests/common.h"
#include "mcp_qt_client/McpQtClient.h"
#include "mcp_qt_apps/McpAppSupport.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

namespace {

static void e2eWaitEvents(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

} // namespace

void test_qt_mcp_apps_end_to_end() {
    QProcess server;
    const QString externalUrl = qEnvironmentVariable("MCP_APPS_E2E_URL");
    QString baseUrlStr;

    if (externalUrl.isEmpty()) {
        // 起本地 mock MCP Apps 服务器
        server.setProcessChannelMode(QProcess::MergedChannels);
        server.setProgram(QStringLiteral("node"));
#ifdef QT_TESTCASE_SOURCEDIR
        const QString scriptPath = QStringLiteral(QT_TESTCASE_SOURCEDIR) + QStringLiteral("/mcp_apps_server.js");
#else
        const QString scriptPath = QStringLiteral("test/mcp_apps_server.js");
#endif
        server.setArguments({scriptPath, QStringLiteral("--port"), QStringLiteral("0")});
        server.start();
        if (!server.waitForStarted(5000)) {
            TM_ASSERT_TRUE(false, "mock MCP Apps server failed to start (node available?)");
            return;
        }
        int port = 0;
        {
            QElapsedTimer timer;
            timer.start();
            QByteArray buf;
            const QRegularExpression re(QStringLiteral("LISTENING (\\d+)"));
            while (timer.elapsed() < 5000 && port == 0) {
                if (server.bytesAvailable() > 0) buf += server.readAll();
                const auto m = re.match(QString::fromUtf8(buf));
                if (m.hasMatch()) port = m.captured(1).toInt();
                if (port == 0) e2eWaitEvents(100);
            }
        }
        TM_ASSERT_TRUE(port > 0, "mock MCP Apps server should report a listening port");
        if (port <= 0) { server.terminate(); server.waitForFinished(2000); return; }
        baseUrlStr = QStringLiteral("http://127.0.0.1:%1/mcp").arg(port);
        qInfo() << "[e2e] using local mock server" << baseUrlStr;
    } else {
        baseUrlStr = externalUrl;
        qInfo() << "[e2e] using external server" << baseUrlStr;
    }

    const QUrl baseUrl(baseUrlStr);
    auto client = mcp_qt::McpQtClient::connectHttpAsync(baseUrl.toString());
    if (!client) {
        TM_ASSERT_TRUE(false, "connectHttpAsync should return a client");
        if (server.state() != QProcess::NotRunning) { server.terminate(); server.waitForFinished(2000); }
        return;
    }

    QEventLoop loop;
    bool connected = false;
    QObject::connect(client.get(), &mcp_qt::McpQtClient::connected, &loop, [&] { connected = true; loop.quit(); });
    QObject::connect(client.get(), &mcp_qt::McpQtClient::errorOccurred, &loop, [&](const mcp_qt::McpError& e) { qInfo() << "[e2e] conn error" << e.toString(); loop.quit(); });
    QTimer::singleShot(8000, &loop, &QEventLoop::quit);
    loop.exec();
    TM_ASSERT_TRUE(connected, "client should connect to the MCP Apps server");
    if (!connected) {
        if (server.state() != QProcess::NotRunning) { server.terminate(); server.waitForFinished(2000); }
        return;
    }

    // tools/list：应有非空工具列表
    std::vector<mcp_qt::McpQtTool> tools;
    QEventLoop l2;
    client->listToolsAsync(QString(), [&](const std::vector<mcp_qt::McpQtTool>& t, const QString&, const QString& err) {
        if (err.isEmpty()) tools = t;
        l2.quit();
    });
    QTimer::singleShot(8000, &l2, &QEventLoop::quit);
    l2.exec();
    TM_ASSERT_TRUE(!tools.empty(), "tools/list should return at least one tool");

    // 找到声明 MCP Apps UI 资源（_meta.ui.resourceUri）的工具
    // 兼容两种结构：inputSchema._meta 内嵌，或 client 将 _meta 剥离到 tool.meta
    QUrl resourceUri;
    for (const auto& t : tools) {
        QUrl ru = mcp_qt::toolUiResourceUri(t.inputSchema);
        if (ru.isEmpty() && !t.meta.isEmpty()) {
            QJsonObject schemaWithMeta = t.inputSchema;
            schemaWithMeta[QStringLiteral("_meta")] = t.meta;
            ru = mcp_qt::toolUiResourceUri(schemaWithMeta);
        }
        if (!ru.isEmpty()) { resourceUri = ru; break; }
    }
    TM_ASSERT_TRUE(!resourceUri.isEmpty(), "at least one tool should declare _meta.ui.resourceUri");

    // 拉取 ui 资源：http(s) 资源走 HTTP（fetchUiResource）；ui:// 资源走标准 resources/read
    if (!resourceUri.isEmpty()) {
        bool got = false;
        QString html;
        const QString scheme = resourceUri.scheme();
        if (scheme == QLatin1String("http") || scheme == QLatin1String("https")) {
            mcp_qt::McpAppSupport support;
            support.fetchUiResource(baseUrl, resourceUri, [&](const QString& h, const QString& err) {
                got = true;
                html = err.isEmpty() ? h : err;
            });
        } else {
            client->readResourceAsync(resourceUri.toString(), [&](const QJsonObject& result, const QString& err) {
                got = true;
                if (err.isEmpty()) {
                    const QJsonArray contents = result.value(QStringLiteral("contents")).toArray();
                    if (!contents.isEmpty()) html = contents.first().toObject().value(QStringLiteral("text")).toString();
                } else {
                    html = err;
                }
            });
        }
        e2eWaitEvents(3000);
        TM_ASSERT_TRUE(got, "ui resource fetch should complete");
        TM_ASSERT_TRUE(!html.isEmpty() && html.contains(QLatin1String("<html"), Qt::CaseInsensitive), "ui resource should be an HTML document");
    }

    if (server.state() != QProcess::NotRunning) {
        server.terminate();
        server.waitForFinished(2000);
    }
}
