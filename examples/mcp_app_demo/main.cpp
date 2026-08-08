// mcp_app_demo — MCP Apps (io.modelcontextprotocol/ui) 渲染与互通演示
// 用 WebView2 渲染器加载一段 MCP App HTML，验证：
//   1. App -> Host：JS postMessage（ui/initialize）被 C++ 收到
//   2. Host -> App：C++ PostWebMessageAsJson 被 JS 收到
#include <QApplication>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QDebug>

#include <mcp_qt_apps/McpAppWebView2Renderer.h>
#include <mcp_qt_apps/McpAppSupport.h>

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    auto renderer = std::make_shared<mcp_qt::McpAppWebView2Renderer>();

    // 模拟服务器 ui:// 资源返回的 MCP App HTML
    const char* html = R"(
      <html><body>
        <h2>MCP App Demo</h2>
        <script>
          window.addEventListener('DOMContentLoaded', function () {
            setTimeout(function () {
              if (!window.mcpAppHost) { document.title = 'bridge-missing'; return; }
              // App -> 宿主：ui/initialize（MCP Apps 初始化握手）
              window.mcpAppHost.postMessage({
                jsonrpc: "2.0", id: 1,
                method: "ui/initialize",
                params: { capabilities: { tools: true, sendOpenLink: true } }
              });
              // 宿主 -> App：监听消息，把结果写到标题
              window.mcpAppHost.addEventListener(function (msg) {
                document.title = 'host-said:' + JSON.stringify(msg);
              });
            }, 300);
          });
        </script>
      </body></html>
    )";

    // 收到 App 消息：打印并回一条响应（模拟宿主确认 ui/initialize）
    renderer->setAppMessageHandler([renderer](const QJsonObject& msg) {
        qDebug() << "[Host received from App]" << msg;
        QString method = msg.value("method").toString();
        if (method == "ui/initialize") {
            const qint64 id = msg.value("id").toVariant().toLongLong();
            QJsonObject result{
                {"clientCapabilities", QJsonObject{
                    {"tools", true},
                    {"sendOpenLink", true}
                }}
            };
            renderer->sendMessageToApp(mcp_qt::McpAppSupport::buildResponse(id, result));
            qDebug() << "[Host sent ui/initialize result to App]";
        }
    });

    QWidget w;
    w.setWindowTitle(QStringLiteral("MCP Apps Demo (WebView2)"));
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
        qDebug() << "[Demo] loadHtml returned";
    });
    QTimer::singleShot(3000, []() { qDebug() << "[Demo] 3s elapsed"; });
    QTimer::singleShot(8000, []() { qDebug() << "[Demo] 8s elapsed"; });

    // 演示 12 秒后退出
    QTimer::singleShot(12000, &app, &QCoreApplication::quit);
    return app.exec();
}
