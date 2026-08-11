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

    // renderer 是 QWidget：所有权交给 Qt 布局管理（addWidget 会建立父子关系，
    // 由 QWidget w 在析构时统一删除）。若再用 std::shared_ptr 持有，会在
    // main 返回时发生二次 delete（0xC0000409）。因此这里用裸指针。
    auto* renderer = new mcp_qt::McpAppWebView2Renderer();

    // MCP 客户端：本演示未连接真实服务器，tools/call 会走客户端错误路径，
    // 但足以展示 AppBridge 完整的消息代理流转（协议层行为已由单元测试覆盖）。
    auto client = mcp_qt::McpQtClient::createForTest();

    // AppBridge 宿主侧代理
    auto bridge = std::make_shared<mcp_qt::McpAppBridge>();
    bridge->attach(renderer, client);
    bridge->setHostInfo(QStringLiteral("mcp-app-demo"), QStringLiteral("1.0.0"));
    // ui/open-link 放行（打印到控制台）
    bridge->setOpenLinkHandler([](const QJsonObject& params, mcp_qt::McpAppBridge::UiRequestRespond respond) {
        qInfo() << "[Demo] App requested to open:" << params.value("url").toString();
        respond(QJsonObject{}, 0, QString());
    });
    // 宿主支持 inline + fullscreen 显示模式（供 App 的 ui/request-display-mode 协商）
    bridge->setDisplayModes({QStringLiteral("inline"), QStringLiteral("fullscreen")});
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
                params: { appCapabilities: { tools: true, sendOpenLink: true,
                                             availableDisplayModes: ["inline", "fullscreen"] } }
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
              // 4. App -> 宿主：ui/request-display-mode（请求 fullscreen，宿主协商后切换窗口）
              window.mcpAppHost.postMessage({
                jsonrpc: "2.0", id: 4,
                method: "ui/request-display-mode",
                params: { mode: "fullscreen" }
              });
              // 5. App -> 宿主：ui/notifications/size-changed（弹性尺寸报告，宿主据此调整容器）
              window.mcpAppHost.postMessage({
                jsonrpc: "2.0",
                method: "ui/notifications/size-changed",
                params: { width: 900, height: 640 }
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

    // displayMode 实际落地：App 请求 fullscreen 协商通过后切换窗口（fullscreen/inline）
    QObject::connect(bridge.get(), &mcp_qt::McpAppBridge::displayModeChanged, [&w](const QString& mode) {
        if (mode == QStringLiteral("fullscreen")) w.showFullScreen();
        else if (mode == QStringLiteral("inline")) w.showNormal();
    });
    // size-changed 落地：弹性尺寸模式下按 App 报告调整窗口
    QObject::connect(bridge.get(), &mcp_qt::McpAppBridge::appSizeChanged, [&w](int wd, int ht) {
        w.resize(wd, ht);
    });

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
