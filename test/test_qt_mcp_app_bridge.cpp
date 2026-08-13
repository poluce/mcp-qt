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
    void setUiMeta(const QJsonObject&) override {}
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

// ui 元数据辅助：prefersBorder / domain / 外部域收集（去重）/ HTML 哈希
void test_mcp_app_support_ui_meta_helpers() {
    // uiPrefersBorder：缺省 true（安全默认）
    TM_ASSERT_TRUE(mcp_qt::McpAppSupport::uiPrefersBorder(QJsonObject{}), "prefersBorder default should be true");
    TM_ASSERT_FALSE(mcp_qt::McpAppSupport::uiPrefersBorder(QJsonObject{{QStringLiteral("prefersBorder"), false}}), "prefersBorder=false should be honored");

    // uiDomain
    TM_ASSERT_TRUE(mcp_qt::McpAppSupport::uiDomain(QJsonObject{{QStringLiteral("domain"), QStringLiteral("https://x.example")}}) == QStringLiteral("https://x.example"), "domain should be parsed");
    TM_ASSERT_TRUE(mcp_qt::McpAppSupport::uiDomain(QJsonObject{}).isEmpty(), "no domain -> empty");

    // cspExternalDomains：跨指令收集 + 去重
    const QStringList ext = mcp_qt::McpAppSupport::cspExternalDomains(QJsonObject{
        {QStringLiteral("csp"), QJsonObject{
            {QStringLiteral("connectDomains"), QJsonArray{QStringLiteral("https://api.example.com")}},
            {QStringLiteral("resourceDomains"), QJsonArray{QStringLiteral("https://cdn.example.com"), QStringLiteral("https://api.example.com")}},
            {QStringLiteral("frameDomains"), QJsonArray{}},
            {QStringLiteral("baseUriDomains"), QJsonArray{QStringLiteral("https://base.example.com")}}
        }}
    });
    TM_ASSERT_TRUE(ext.contains(QStringLiteral("https://api.example.com")), "connect domain should be collected");
    TM_ASSERT_TRUE(ext.contains(QStringLiteral("https://cdn.example.com")), "resource domain should be collected");
    TM_ASSERT_TRUE(ext.contains(QStringLiteral("https://base.example.com")), "base domain should be collected");
    TM_ASSERT_EQ(ext.size(), 3, "domains should be deduplicated");

    // hashHtml：SHA-256 确定性
    const QString h1 = mcp_qt::McpAppSupport::hashHtml("<html>hi</html>");
    const QString h2 = mcp_qt::McpAppSupport::hashHtml("<html>hi</html>");
    const QString h3 = mcp_qt::McpAppSupport::hashHtml("<html>bye</html>");
    TM_ASSERT_EQ(h1.size(), 64, "sha256 hex should be 64 chars");
    TM_ASSERT_TRUE(h1 == h2, "same content -> same hash");
    TM_ASSERT_TRUE(h1 != h3, "different content -> different hash");
}

// ========== MUST 级合规：沙箱安全构造 + initialized 门禁 + size-changed ==========

// 默认 CSP（无 ui.csp）→ 限制性默认；有 csp → 按域扩展 + 未声明域最严格
void test_mcp_app_support_csp_construction() {
    // 无 csp → 规范 Restrictive Default
    const QString def = mcp_qt::McpAppSupport::buildCsp(QJsonObject{});
    TM_ASSERT_TRUE(def.contains(QStringLiteral("default-src 'none'")), "default CSP must set default-src 'none'");
    TM_ASSERT_TRUE(def.contains(QStringLiteral("script-src 'self' 'unsafe-inline'")), "default CSP must allow inline script (app needs it)");
    TM_ASSERT_TRUE(def.contains(QStringLiteral("style-src 'self' 'unsafe-inline'")), "default CSP must allow inline style");
    TM_ASSERT_TRUE(def.contains(QStringLiteral("img-src 'self' data:")), "default CSP must allow self+data images");
    TM_ASSERT_TRUE(def.contains(QStringLiteral("connect-src 'none'")), "default CSP must block network connections");

    // 有 csp → 按 connectDomains/resourceDomains 扩展；frame/base 未声明则最严格
    const QJsonObject uiMeta{
        {QStringLiteral("csp"), QJsonObject{
            {QStringLiteral("connectDomains"), QJsonArray{QStringLiteral("https://api.example.com")}},
            {QStringLiteral("resourceDomains"), QJsonArray{QStringLiteral("https://cdn.example.com")}},
            {QStringLiteral("frameDomains"), QJsonArray{}},
            {QStringLiteral("baseUriDomains"), QJsonArray{}}
        }}
    };
    const QString csp = mcp_qt::McpAppSupport::buildCsp(uiMeta);
    TM_ASSERT_TRUE(csp.contains(QStringLiteral("connect-src 'self' https://api.example.com")), "connectDomains should extend connect-src");
    TM_ASSERT_TRUE(csp.contains(QStringLiteral("img-src 'self' data: https://cdn.example.com")), "resourceDomains should extend img-src");
    TM_ASSERT_TRUE(csp.contains(QStringLiteral("frame-src 'none'")), "empty frameDomains must fall back to 'none'");
    TM_ASSERT_TRUE(csp.contains(QStringLiteral("base-uri 'self'")), "empty baseUriDomains must fall back to 'self'");
    TM_ASSERT_TRUE(csp.contains(QStringLiteral("object-src 'none'")), "object-src must always be 'none'");
}

// permissions → iframe allow 属性（Permission Policy）
void test_mcp_app_support_allow_attribute() {
    const QString allow = mcp_qt::McpAppSupport::buildAllowAttribute(QJsonObject{
        {QStringLiteral("permissions"), QJsonObject{
            {QStringLiteral("camera"), QJsonObject{}},
            {QStringLiteral("microphone"), QJsonObject{}},
            {QStringLiteral("clipboardWrite"), QJsonObject{}}
        }}
    });
    TM_ASSERT_TRUE(allow.contains(QStringLiteral("camera")), "camera permission → allow camera");
    TM_ASSERT_TRUE(allow.contains(QStringLiteral("microphone")), "microphone permission → allow microphone");
    TM_ASSERT_TRUE(allow.contains(QStringLiteral("clipboard-write")), "clipboardWrite → allow clipboard-write");
    TM_ASSERT_TRUE(!allow.contains(QStringLiteral("geolocation")), "geolocation not requested");

    // 无 permissions → 空
    const QString none = mcp_qt::McpAppSupport::buildAllowAttribute(QJsonObject{});
    TM_ASSERT_TRUE(none.isEmpty(), "no permissions → empty allow attribute");
}

// initialized 门禁：收到 ui/notifications/initialized 前 Host MUST NOT 发消息
void test_qt_mcp_app_bridge_initialized_gate() {
    MockAppRenderer renderer;
    mcp_qt::McpAppBridge bridge;
    bridge.attach(&renderer, nullptr);
    bridge.start();

    // 未初始化 → 主动通知暂存，不得提前发给 View
    bridge.sendToolInput(QJsonObject{{QStringLiteral("a"), 1}});
    bridge.sendToolResult(QJsonObject{{QStringLiteral("content"), QJsonArray{}}});
    bridge.teardownResource(QString());
    TM_ASSERT_EQ(renderer.sentMessages.size(), 0, "no host messages before ui/notifications/initialized");

    // 收到 initialized → 自动按 input/result 顺序冲刷暂存通知
    renderer.handler(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), QStringLiteral("ui/notifications/initialized")},
        {QStringLiteral("params"), QJsonObject{}}
    });
    TM_ASSERT_EQ(renderer.sentMessages.size(), 2, "queued host messages should flush after initialized");
    if (renderer.sentMessages.size() < 2) return;
    TM_ASSERT_TRUE(renderer.sentMessages.at(0).value(QStringLiteral("method")).toString() == QStringLiteral("ui/notifications/tool-input"), "first should be tool-input");
    TM_ASSERT_TRUE(renderer.sentMessages.at(1).value(QStringLiteral("method")).toString() == QStringLiteral("ui/notifications/tool-result"), "second should be tool-result");
}

// size-changed → appSizeChanged 信号
void test_qt_mcp_app_bridge_size_changed() {
    MockAppRenderer renderer;
    mcp_qt::McpAppBridge bridge;
    bridge.attach(&renderer, nullptr);
    bridge.start();

    int gotW = -1, gotH = -1;
    QObject::connect(&bridge, &mcp_qt::McpAppBridge::appSizeChanged,
                     [&](int w, int h) { gotW = w; gotH = h; });

    renderer.handler(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), QStringLiteral("ui/notifications/size-changed")},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("width"), 320},
                                               {QStringLiteral("height"), 480}}}
    });
    TM_ASSERT_EQ(gotW, 320, "size-changed width should propagate via signal");
    TM_ASSERT_EQ(gotH, 480, "size-changed height should propagate via signal");
}

// ========== 缺失项补齐：hostContext / displayMode 协商 / message 信号 / teardown 等待 ==========

// ui/initialize 返回完整 hostContext（theme/locale/timeZone/containerDimensions/availableDisplayModes）
void test_qt_mcp_app_bridge_host_context_full() {
    MockAppRenderer renderer;
    mcp_qt::McpAppBridge bridge;
    bridge.attach(&renderer, nullptr);
    bridge.setTheme(QStringLiteral("dark"));
    bridge.setLocale(QStringLiteral("zh-CN"));
    bridge.setTimeZone(QStringLiteral("Asia/Shanghai"));
    bridge.setContainerDimensions(QJsonObject{{QStringLiteral("width"), 400}});
    bridge.setSafeAreaInsets(QJsonObject{{QStringLiteral("bottom"), 24}});
    bridge.setDisplayModes({QStringLiteral("inline"), QStringLiteral("fullscreen")});
    bridge.start();

    renderer.handler(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 1},
        {QStringLiteral("method"), QStringLiteral("ui/initialize")},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("appCapabilities"), QJsonObject{
            {QStringLiteral("availableDisplayModes"), QJsonArray{QStringLiteral("inline"), QStringLiteral("fullscreen")}}}
        }}}
    });

    TM_ASSERT_EQ(renderer.sentMessages.size(), 1, "should respond to ui/initialize");
    if (renderer.sentMessages.isEmpty()) return;
    const QJsonObject hc = renderer.sentMessages.first().value(QStringLiteral("result")).toObject()
                               .value(QStringLiteral("hostContext")).toObject();
    TM_ASSERT_TRUE(hc.value(QStringLiteral("theme")).toString() == QStringLiteral("dark"), "theme should be dark");
    TM_ASSERT_TRUE(hc.value(QStringLiteral("locale")).toString() == QStringLiteral("zh-CN"), "locale should be zh-CN");
    TM_ASSERT_TRUE(hc.value(QStringLiteral("timeZone")).toString() == QStringLiteral("Asia/Shanghai"), "timeZone should be set");
    TM_ASSERT_TRUE(hc.value(QStringLiteral("containerDimensions")).toObject().value(QStringLiteral("width")).toInt() == 400, "containerDimensions should be set");
    TM_ASSERT_TRUE(hc.value(QStringLiteral("safeAreaInsets")).toObject().value(QStringLiteral("bottom")).toInt() == 24, "safeAreaInsets should be set");
    TM_ASSERT_TRUE(hc.value(QStringLiteral("availableDisplayModes")).toArray().contains(QStringLiteral("fullscreen")), "availableDisplayModes should include fullscreen");
    TM_ASSERT_TRUE(hc.value(QStringLiteral("displayMode")).toString() == QStringLiteral("inline"), "current displayMode should be inline");
}

// displayMode 协商：目标模式需同时被 App 声明 与 宿主支持，否则返回当前模式
void test_qt_mcp_app_bridge_display_mode_negotiation() {
    MockAppRenderer renderer;
    mcp_qt::McpAppBridge bridge;
    bridge.attach(&renderer, nullptr);
    bridge.setDisplayModes({QStringLiteral("inline"), QStringLiteral("fullscreen"), QStringLiteral("pip")});
    bridge.start();
    renderer.handler(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 1},
        {QStringLiteral("method"), QStringLiteral("ui/initialize")},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("appCapabilities"), QJsonObject{
            {QStringLiteral("availableDisplayModes"), QJsonArray{QStringLiteral("fullscreen"), QStringLiteral("pip")}}}
        }}}
    });
    renderer.sentMessages.clear();

    // Host 在 View 发出 initialized 之前 MUST NOT 主动发送通知。
    renderer.handler(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), QStringLiteral("ui/notifications/initialized")}
    });

    // 请求合法模式（App 与 Host 都支持）
    bool modeChanged = false;
    QString changedMode;
    QObject::connect(&bridge, &mcp_qt::McpAppBridge::displayModeChanged,
                     [&](const QString& m) { modeChanged = true; changedMode = m; });
    renderer.handler(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 2},
        {QStringLiteral("method"), QStringLiteral("ui/request-display-mode")},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("fullscreen")}}}
    });
    TM_ASSERT_EQ(renderer.sentMessages.size(), 2, "should respond and notify host-context change");
    if (renderer.sentMessages.size() < 2) return;
    const QJsonObject result1 = renderer.sentMessages.first().value(QStringLiteral("result")).toObject();
    TM_ASSERT_TRUE(result1.value(QStringLiteral("mode")).toString() == QStringLiteral("fullscreen"), "advertised+supported mode should be accepted");
    TM_ASSERT_TRUE(modeChanged && changedMode == QStringLiteral("fullscreen"), "displayModeChanged signal should fire");
    const QJsonObject notification = renderer.sentMessages.at(1);
    TM_ASSERT_TRUE(notification.value(QStringLiteral("method")).toString()
                       == QStringLiteral("ui/notifications/host-context-changed"),
                   "accepted display mode should notify host-context change");
    TM_ASSERT_TRUE(notification.value(QStringLiteral("params")).toObject()
                       .value(QStringLiteral("displayMode")).toString() == QStringLiteral("fullscreen"),
                   "host-context notification should contain the actual display mode");

    // 请求 App 未声明的 inline → 拒绝，返回当前实际模式（fullscreen）
    renderer.sentMessages.clear();
    renderer.handler(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 3},
        {QStringLiteral("method"), QStringLiteral("ui/request-display-mode")},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("inline")}}}
    });
    const QJsonObject result2 = renderer.sentMessages.first().value(QStringLiteral("result")).toObject();
    TM_ASSERT_TRUE(result2.value(QStringLiteral("mode")).toString() == QStringLiteral("fullscreen"), "mode not advertised by App should fall back to current mode");
}

// notifications/message → appLogMessage 信号
void test_qt_mcp_app_bridge_app_log_signal() {
    MockAppRenderer renderer;
    mcp_qt::McpAppBridge bridge;
    bridge.attach(&renderer, nullptr);
    bridge.start();

    QJsonObject gotParams;
    QObject::connect(&bridge, &mcp_qt::McpAppBridge::appLogMessage,
                     [&](const QJsonObject& p) { gotParams = p; });
    renderer.handler(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), QStringLiteral("notifications/message")},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("level"), QStringLiteral("info")},
                                               {QStringLiteral("data"), QStringLiteral("hello")}}}
    });
    TM_ASSERT_TRUE(gotParams.value(QStringLiteral("data")).toString() == QStringLiteral("hello"), "appLogMessage signal should carry params");
}

// teardown 等待响应：发出带 id 的请求，App 响应后回调 ok=true
void test_qt_mcp_app_bridge_teardown_waits_response() {
    MockAppRenderer renderer;
    mcp_qt::McpAppBridge bridge;
    bridge.attach(&renderer, nullptr);
    bridge.start();
    renderer.handler(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), QStringLiteral("ui/notifications/initialized")},
        {QStringLiteral("params"), QJsonObject{}}
    });

    bool called = false;
    bool okResult = false;
    bridge.teardownResource(QStringLiteral("bye"), [&](bool ok, const QJsonObject&, const QJsonObject&) {
        called = true; okResult = ok;
    });

    TM_ASSERT_EQ(renderer.sentMessages.size(), 1, "should send teardown request");
    if (renderer.sentMessages.isEmpty()) return;
    const QJsonObject req = renderer.sentMessages.first();
    TM_ASSERT_TRUE(req.value(QStringLiteral("method")).toString() == QStringLiteral("ui/resource-teardown"), "method should be ui/resource-teardown");
    const qint64 id = req.value(QStringLiteral("id")).toVariant().toLongLong();
    TM_ASSERT_TRUE(id >= 1, "teardown request should carry an id");

    // App 响应
    renderer.handler(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("result"), QJsonObject{}}
    });
    TM_ASSERT_TRUE(called, "callback should fire on App response");
    TM_ASSERT_TRUE(okResult, "ok should be true on successful response");
}

// teardown 超时：App 未响应，2s 后回调 ok=false
void test_qt_mcp_app_bridge_teardown_timeout() {
    MockAppRenderer renderer;
    mcp_qt::McpAppBridge bridge;
    bridge.attach(&renderer, nullptr);
    bridge.start();
    renderer.handler(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), QStringLiteral("ui/notifications/initialized")},
        {QStringLiteral("params"), QJsonObject{}}
    });

    bool called = false;
    bool okResult = true;
    bridge.teardownResource(QStringLiteral("bye"), [&](bool ok, const QJsonObject&, const QJsonObject&) {
        called = true; okResult = ok;
    });
    waitEvents(2100);  // 不响应，等待超时
    TM_ASSERT_TRUE(called, "timeout should fire the callback");
    TM_ASSERT_FALSE(okResult, "timeout should report ok=false");
}
