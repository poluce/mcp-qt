// mcp_app_demo — MCP Apps (io.modelcontextprotocol/ui) 渲染与互通演示
// 用 WebView2 渲染器 + McpAppBridge 演示完整 AppBridge 协议：
//   1. App -> Host：ui/initialize 初始化握手（McpAppBridge 自动响应）
//   2. App -> Host：tools/call 工具调用代理（McpAppBridge 转发并回传结果/错误）
//   3. App -> Host：ui/open-link 打开链接（自定义 handler 放行）
#include <QApplication>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QDebug>

#include <mcp_qt_apps/McpAppBridge.h>
#include <mcp_qt_apps/McpAppWebView2Renderer.h>
#include <mcp_qt_apps/McpAppSupport.h>
#include <mcp_qt_client/McpQtClient.h>

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    auto renderer = std::make_shared<mcp_qt::McpAppWebView2Renderer>();

    // MCP 客户端：本演示未连接真实服务器，tools/call 会走客户端错误路径，
    // 但足以展示 AppBridge 完整的消息代理流转（协议层行为已由单元测试覆盖）。
    auto client = mcp_qt::McpQtClient::createForTest();

    // AppBridge 宿主侧代理
    auto bridge = std::make_shared<mcp_qt::McpAppBridge>();
    bridge->attach(renderer.get(), client);
    bridge->setHostInfo(QStringLiteral("mcp-app-demo"), QStringLiteral("1.0.0"));
    // ui/open-link 放行（打印到控制台）
    bridge->setOpenLinkHandler([](const QJsonObject& params, mcp_qt::McpAppBridge::UiRequestRespond respond) {
        qInfo() << "[Demo] App requested to open:" << params.value("url").toString();
        respond(QJsonObject{}, 0, QString());
    });
    bridge->start();

    // 模拟服务器 ui:// 资源返回的 MCP App HTML
    const char* html = R"(
      <html><body>
        <h2>MCP App Demo (AppBridge)</h2>
        <div id="status">initializing...</div>
        <script>
          window.addEventListener('DOMContentLoaded', function () {
            setTimeout(function () {
              if (!window.mcpAppHost) { document.title = 'bridge-missing'; return; }
              var status = document.getElementById('status');
              // 1. App -> 宿主：ui/initialize（McpAppBridge 自动响应 hostCapabilities/hostInfo）
              window.mcpAppHost.postMessage({
                jsonrpc: "2.0", id: 1,
                method: "ui/initialize",
                params: { appCapabilities: { tools: true, sendOpenLink: true } }
              });
              // 2. App -> 宿主：tools/call（演示工具调用代理，本 demo 未连接服务器 → 错误回传）
              window.mcpAppHost.postMessage({
                jsonrpc: "2.0", id: 2,
                method: "tools/call",
                params: { name: "demo_tool", arguments: { q: "hello" } }
              });
              // 3. App -> 宿主：ui/open-link（自定义 handler 放行）
              window.mcpAppHost.postMessage({
                jsonrpc: "2.0", id: 3,
                method: "ui/open-link",
                params: { url: "https://modelcontextprotocol.io" }
              });
              // 监听宿主响应，更新页面标题
              window.mcpAppHost.addEventListener(function (msg) {
                if (msg && msg.id === 1 && msg.result) {
                  var caps = msg.result.hostCapabilities || {};
                  status.textContent = 'ui/initialize OK, serverTools=' + (caps.serverTools ? 'yes' : 'no');
                } else if (msg && msg.id === 2) {
                  status.textContent = 'tools/call response: ' + JSON.stringify(msg.error || msg.result);
                } else if (msg && msg.id === 3) {
                  status.textContent = 'ui/open-link ' + (msg.result ? 'OK' : 'denied');
                }
                document.title = 'host-said:' + JSON.stringify(msg);
              });
            }, 300);
          });
        </script>
      </body></html>
    )";

    QWidget w;
    w.setWindowTitle(QStringLiteral("MCP Apps Demo (WebView2 + AppBridge)"));
    auto* layout = new QVBoxLayout(&w);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(renderer->hostWidget());
    w.resize(640, 400);
    w.show();

    renderer->initializeAsync([](bool ok, const QString& err) {
        qDebug() << "[WebView2 init]" << (ok ? "OK" : ("FAIL: " + err));
    });
    const QString htmlStr = QString::fromUtf8(html);
    QTimer::singleShot(100, [renderer, htmlStr]() {
        qDebug() << "[Demo] loadHtml start";
        renderer->loadHtml(htmlStr, QUrl());
    });

    // 演示 12 秒后退出
    QTimer::singleShot(12000, &app, &QCoreApplication::quit);
    return app.exec();
}
