// MCP Apps AppBridge 协议层单元测试
// 用 mock renderer + mock transport 验证宿主侧代理逻辑（无需 WebView2/真实网络）。
#include "tests/common.h"
#include "mcp_qt_client/McpQtClient.h"
#include "mcp_qt_apps/McpAppBridge.h"
#include "mcp_qt_apps/McpAppSupport.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <functional>
#include <memory>

// 捕获宿主消息的 mock renderer
class MockAppRenderer : public mcp_qt::IMcpAppRenderer {
public:
    QList<QJsonObject> sentMessages;
    std::function<void(const QJsonObject&)> handler;

    QWidget* hostWidget() override { return nullptr; }
    void loadHtml(const QString&, const QUrl&) override {}
    void sendMessageToApp(const QJsonObject& m) override { sentMessages.append(m); }
    void setAppMessageHandler(std::function<void(const QJsonObject&)> h) override { handler = std::move(h); }
    void setPermissionPolicy(const std::vector<QString>&, const std::vector<QString>&) override {}
};

// 捕获出站消息的 mock MCP transport
class AppBridgeMockTransport : public mcp::IMcpTransport {
public:
    std::vector<std::string> sentMessages;
    std::function<void(const std::string&)> onMsgCb;

    bool send(const std::string& message) override { sentMessages.push_back(message); return true; }
    void setOnMessage(std::function<void(const std::string&)> cb) override { onMsgCb = cb; }
    void setOnClose(std::function<void()>) override {}
    void setOnError(std::function<void(const std::string&)>) override {}
    bool start() override { return true; }
    void close() override {}
};

static void waitEvents(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// 在 mock transport 上构造带工具缓存的 stateless client
static std::shared_ptr<mcp_qt::McpQtClient> makeClientWithTools(
        std::shared_ptr<AppBridgeMockTransport> mt, const QJsonArray& tools) {
    int argc = 0;
    char* argv[] = {nullptr};
    std::unique_ptr<QCoreApplication> app;
    if (!QCoreApplication::instance()) {
        app = std::make_unique<QCoreApplication>(argc, argv);
    }

    auto client = mcp_qt::McpQtClient::createForTest();
    client->setStatelessMode(true);
    client->connectToTransportAsync(mt, QStringLiteral("test-client"), QStringLiteral("1.0"));
    waitEvents(20);

    // 发送 tools/list 并喂响应，填充客户端工具缓存
    client->listToolsAsync(QString(), [](const std::vector<mcp_qt::McpQtTool>&, const QString&, const QString&) {});
    waitEvents(20);

    std::string req;
    for (auto it = mt->sentMessages.rbegin(); it != mt->sentMessages.rend(); ++it) {
        if (it->find("\"tools/list\"") != std::string::npos) { req = *it; break; }
    }
    if (!req.empty()) {
        QJsonObject reqObj = QJsonDocument::fromJson(QString::fromStdString(req).toUtf8()).object();
        QJsonValue idVal = reqObj.value(QStringLiteral("id"));
        QJsonObject resp{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), idVal},
                         {QStringLiteral("result"), QJsonObject{{QStringLiteral("tools"), tools}}}};
        mt->onMsgCb(QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString());
        waitEvents(50);
    }
    return client;
}

// ========== 用例 ==========

// ui/initialize 握手
void test_qt_mcp_app_bridge_initialize() {
    MockAppRenderer renderer;
    mcp_qt::McpAppBridge bridge;
    bridge.attach(&renderer, nullptr);
    bridge.start();

    renderer.handler(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 1},
        {QStringLiteral("method"), QStringLiteral("ui/initialize")},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("appCapabilities"), QJsonObject{{QStringLiteral("tools"), true}}}}}
    });

    TM_ASSERT_EQ(renderer.sentMessages.size(), 1, "should respond to ui/initialize");
    if (renderer.sentMessages.isEmpty()) return;
    const QJsonObject resp = renderer.sentMessages.first();
    TM_ASSERT_TRUE(resp.value(QStringLiteral("id")).toInt() == 1, "response id should match");
    const QJsonObject result = resp.value(QStringLiteral("result")).toObject();
    TM_ASSERT_TRUE(result.contains(QStringLiteral("hostCapabilities")), "should include hostCapabilities");
    TM_ASSERT_TRUE(result.contains(QStringLiteral("hostInfo")), "should include hostInfo");
    TM_ASSERT_TRUE(result.contains(QStringLiteral("hostContext")), "should include hostContext");
    TM_ASSERT_TRUE(result.value(QStringLiteral("hostCapabilities")).toObject()
                       .contains(QStringLiteral("serverTools")), "should advertise serverTools");
}

// tools/list 按 visibility 过滤 + 权限白名单
void test_qt_mcp_app_bridge_tools_list_filter() {
    auto mt = std::make_shared<AppBridgeMockTransport>();
    QJsonArray tools{
        QJsonObject{{QStringLiteral("name"), QStringLiteral("both_tool")},
                    {QStringLiteral("description"), QStringLiteral("both")},
                    {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}},
                    {QStringLiteral("_meta"), QJsonObject{
                        {QStringLiteral("ui"), QJsonObject{
                            {QStringLiteral("visibility"), QJsonArray{QStringLiteral("model"), QStringLiteral("app")}}}}}}},
        QJsonObject{{QStringLiteral("name"), QStringLiteral("model_only")},
                    {QStringLiteral("description"), QStringLiteral("model-only")},
                    {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}},
                    {QStringLiteral("_meta"), QJsonObject{
                        {QStringLiteral("ui"), QJsonObject{
                            {QStringLiteral("visibility"), QJsonArray{QStringLiteral("model")}}}}}}},
        QJsonObject{{QStringLiteral("name"), QStringLiteral("default_tool")},
                    {QStringLiteral("description"), QStringLiteral("default visibility")},
                    {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}}}
    };
    auto client = makeClientWithTools(mt, tools);
    TM_ASSERT_EQ(client->cachedTools().size(), 3, "client should cache 3 tools");

    MockAppRenderer renderer;
    mcp_qt::McpAppBridge bridge;
    bridge.attach(&renderer, client);
    bridge.start();

    renderer.handler(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 2},
        {QStringLiteral("method"), QStringLiteral("tools/list")}
    });

    TM_ASSERT_EQ(renderer.sentMessages.size(), 1, "should respond to tools/list");
    if (renderer.sentMessages.isEmpty()) return;
    const QJsonObject result = renderer.sentMessages.first().value(QStringLiteral("result")).toObject();
    const QJsonArray listed = result.value(QStringLiteral("tools")).toArray();
    // model_only 不可见；both_tool 和 default_tool 可见
    TM_ASSERT_EQ(listed.size(), 2, "should filter app-invisible tools");
    QStringList names;
    for (const auto& t : listed) names << t.toObject().value(QStringLiteral("name")).toString();
    TM_ASSERT_TRUE(names.contains(QStringLiteral("both_tool")), "both_tool should be listed");
    TM_ASSERT_TRUE(names.contains(QStringLiteral("default_tool")), "default_tool should be listed");
    TM_ASSERT_TRUE(!names.contains(QStringLiteral("model_only")), "model_only should NOT be listed");
}

// tools/call 权限拒绝（allowedTools 白名单）
void test_qt_mcp_app_bridge_tools_call_permission_denied() {
    auto mt = std::make_shared<AppBridgeMockTransport>();
    QJsonArray tools{
        QJsonObject{{QStringLiteral("name"), QStringLiteral("allowed_tool")},
                    {QStringLiteral("description"), QStringLiteral("allowed")},
                    {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}}},
        QJsonObject{{QStringLiteral("name"), QStringLiteral("blocked_tool")},
                    {QStringLiteral("description"), QStringLiteral("blocked")},
                    {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}}}
    };
    auto client = makeClientWithTools(mt, tools);

    MockAppRenderer renderer;
    mcp_qt::McpAppBridge bridge;
    bridge.attach(&renderer, client);
    bridge.setPermissionPolicy({QStringLiteral("allowed_tool")}, {});
    bridge.start();

    // 调用白名单外工具 → 拒绝
    renderer.handler(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 10},
        {QStringLiteral("method"), QStringLiteral("tools/call")},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), QStringLiteral("blocked_tool")},
                                               {QStringLiteral("arguments"), QJsonObject{}}}}
    });
    TM_ASSERT_EQ(renderer.sentMessages.size(), 1, "should respond to tools/call");
    if (renderer.sentMessages.isEmpty()) return;
    const QJsonObject err = renderer.sentMessages.first().value(QStringLiteral("error")).toObject();
    TM_ASSERT_TRUE(!err.isEmpty(), "denied call should return error");
    TM_ASSERT_TRUE(err.value(QStringLiteral("code")).toInt() == -32000, "denied code should be -32000");
}

// tools/call 成功代理：AppBridge 转发到 client，结果回传
void test_qt_mcp_app_bridge_tools_call_success() {
    auto mt = std::make_shared<AppBridgeMockTransport>();
    QJsonArray tools{
        QJsonObject{{QStringLiteral("name"), QStringLiteral("calc")},
                    {QStringLiteral("description"), QStringLiteral("calc tool")},
                    {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}}}
    };
    auto client = makeClientWithTools(mt, tools);

    MockAppRenderer renderer;
    mcp_qt::McpAppBridge bridge;
    bridge.attach(&renderer, client);
    bridge.start();

    renderer.handler(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 5},
        {QStringLiteral("method"), QStringLiteral("tools/call")},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), QStringLiteral("calc")},
                                               {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("a"), 1}}}}}
    });
    waitEvents(20);  // 让 callToolAsync 发送请求

    // 解析 client 发出的 tools/call 请求，喂结果
    std::string req;
    for (auto it = mt->sentMessages.rbegin(); it != mt->sentMessages.rend(); ++it) {
        if (it->find("\"tools/call\"") != std::string::npos) { req = *it; break; }
    }
    TM_ASSERT_TRUE(!req.empty(), "AppBridge should forward tools/call to server");
    if (req.empty()) return;

    QJsonObject reqObj = QJsonDocument::fromJson(QString::fromStdString(req).toUtf8()).object();
    TM_ASSERT_TRUE(reqObj.value(QStringLiteral("method")).toString() == QStringLiteral("tools/call"), "method should be tools/call");
    TM_ASSERT_TRUE(reqObj.value(QStringLiteral("params")).toObject().value(QStringLiteral("name")).toString() == QStringLiteral("calc"), "tool name should be calc");

    QJsonValue idVal = reqObj.value(QStringLiteral("id"));
    QJsonObject result{
        {QStringLiteral("content"), QJsonArray{
            QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                        {QStringLiteral("text"), QStringLiteral("calc result: 6")}}}},
        {QStringLiteral("structuredContent"), QJsonObject{{QStringLiteral("sum"), 6}}}
    };
    QJsonObject resp{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                     {QStringLiteral("id"), idVal},
                     {QStringLiteral("result"), result}};
    mt->onMsgCb(QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString());
    waitEvents(50);

    TM_ASSERT_EQ(renderer.sentMessages.size(), 1, "AppBridge should send one response to App");
    if (renderer.sentMessages.isEmpty()) return;
    const QJsonObject appResp = renderer.sentMessages.first();
    TM_ASSERT_TRUE(appResp.value(QStringLiteral("id")).toInt() == 5, "response id should match App request");
    const QJsonObject resultObj = appResp.value(QStringLiteral("result")).toObject();
    TM_ASSERT_TRUE(resultObj.value(QStringLiteral("structuredContent")).toObject().value(QStringLiteral("sum")).toInt() == 6, "tool result should be proxied back");
}

// ui/open-link 默认拒绝 + 自定义 handler
void test_qt_mcp_app_bridge_open_link() {
    MockAppRenderer renderer;
    mcp_qt::McpAppBridge bridge;
    bridge.attach(&renderer, nullptr);
    bridge.start();

    // 默认拒绝
    renderer.handler(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 3},
        {QStringLiteral("method"), QStringLiteral("ui/open-link")},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("url"), QStringLiteral("https://example.com")}}}
    });
    TM_ASSERT_EQ(renderer.sentMessages.size(), 1, "should respond to ui/open-link");
    if (renderer.sentMessages.isEmpty()) return;
    const QJsonObject err = renderer.sentMessages.first().value(QStringLiteral("error")).toObject();
    TM_ASSERT_TRUE(!err.isEmpty(), "default open-link should be denied");

    // 自定义 handler 放行
    bool handlerCalled = false;
    bridge.setOpenLinkHandler([&handlerCalled](const QJsonObject& params, mcp_qt::McpAppBridge::UiRequestRespond respond) {
        handlerCalled = true;
        TM_ASSERT_TRUE(params.value(QStringLiteral("url")).toString().startsWith(QStringLiteral("https")), "params should carry url");
        respond(QJsonObject{}, 0, QString());
    });
    renderer.sentMessages.clear();
    renderer.handler(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 4},
        {QStringLiteral("method"), QStringLiteral("ui/open-link")},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("url"), QStringLiteral("https://example.org")}}}
    });
    TM_ASSERT_TRUE(handlerCalled, "custom open-link handler should be invoked");
    TM_ASSERT_EQ(renderer.sentMessages.size(), 1, "custom handler should produce a response");
    if (renderer.sentMessages.isEmpty()) return;
    TM_ASSERT_TRUE(renderer.sentMessages.first().contains(QStringLiteral("result")), "custom handler response should be a result");
}

// 未知方法 → -32601
void test_qt_mcp_app_bridge_unknown_method() {
    MockAppRenderer renderer;
    mcp_qt::McpAppBridge bridge;
    bridge.attach(&renderer, nullptr);
    bridge.start();

    renderer.handler(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 99},
        {QStringLiteral("method"), QStringLiteral("foo/bar")},
        {QStringLiteral("params"), QJsonObject{}}
    });
    TM_ASSERT_EQ(renderer.sentMessages.size(), 1, "should respond to unknown method");
    if (renderer.sentMessages.isEmpty()) return;
    const QJsonObject err = renderer.sentMessages.first().value(QStringLiteral("error")).toObject();
    TM_ASSERT_TRUE(err.value(QStringLiteral("code")).toInt() == -32601, "unknown method should return -32601");
}

// tools/call 可见性拒绝（visibility: ["model"]）
void test_qt_mcp_app_bridge_tools_call_visibility_denied() {
    auto mt = std::make_shared<AppBridgeMockTransport>();
    QJsonArray tools{
        QJsonObject{{QStringLiteral("name"), QStringLiteral("model_only")},
                    {QStringLiteral("description"), QStringLiteral("model-only")},
                    {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}},
                    {QStringLiteral("_meta"), QJsonObject{
                        {QStringLiteral("ui"), QJsonObject{
                            {QStringLiteral("visibility"), QJsonArray{QStringLiteral("model")}}}}}}}
    };
    auto client = makeClientWithTools(mt, tools);

    MockAppRenderer renderer;
    mcp_qt::McpAppBridge bridge;
    bridge.attach(&renderer, client);
    bridge.start();

    renderer.handler(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 6},
        {QStringLiteral("method"), QStringLiteral("tools/call")},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), QStringLiteral("model_only")},
                                               {QStringLiteral("arguments"), QJsonObject{}}}}
    });
    TM_ASSERT_EQ(renderer.sentMessages.size(), 1, "should respond to tools/call");
    if (renderer.sentMessages.isEmpty()) return;
    const QJsonObject err = renderer.sentMessages.first().value(QStringLiteral("error")).toObject();
    TM_ASSERT_TRUE(!err.isEmpty(), "model-only tool should be denied for app");
}
