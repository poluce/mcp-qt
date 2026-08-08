// McpAppWebView2Renderer.cpp — MCP Apps WebView2 渲染后端实现
#include "mcp_qt_apps/McpAppWebView2Renderer.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QShowEvent>
#include <QResizeEvent>
#include <QDebug>

#include <windows.h>
#include "WebView2.h"

namespace mcp_qt {

// ================== COM 回调接口实现（MinGW 下显式实现 vtable）==================
namespace wv2_detail {

// MinGW 无 __uuidof：使用 IID_IUnknown 匹配即可满足 WebView2 对回调接口的 QI 需求
inline HRESULT handleQueryInterface(REFIID riid, void** ppv, void* self) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown) { *ppv = self; return S_OK; }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

class EnvCreatedHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
public:
    explicit EnvCreatedHandler(std::function<void(HRESULT, ICoreWebView2Environment*)> fn) : m_fn(std::move(fn)) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override { return handleQueryInterface(riid, ppv, this); }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP Invoke(HRESULT result, ICoreWebView2Environment* env) override { m_fn(result, env); return S_OK; }
private:
    std::function<void(HRESULT, ICoreWebView2Environment*)> m_fn;
};

class ControllerCreatedHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
public:
    explicit ControllerCreatedHandler(std::function<void(HRESULT, ICoreWebView2Controller*)> fn) : m_fn(std::move(fn)) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override { return handleQueryInterface(riid, ppv, this); }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP Invoke(HRESULT result, ICoreWebView2Controller* controller) override { m_fn(result, controller); return S_OK; }
private:
    std::function<void(HRESULT, ICoreWebView2Controller*)> m_fn;
};

class WebMessageHandler : public ICoreWebView2WebMessageReceivedEventHandler {
public:
    explicit WebMessageHandler(std::function<HRESULT(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*)> fn) : m_fn(std::move(fn)) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override { return handleQueryInterface(riid, ppv, this); }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP Invoke(ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) override { return m_fn(sender, args); }
private:
    std::function<HRESULT(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*)> m_fn;
};

class NavigationCompletedHandler : public ICoreWebView2NavigationCompletedEventHandler {
public:
    explicit NavigationCompletedHandler(std::function<void(HRESULT, BOOL)> fn) : m_fn(std::move(fn)) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override { return handleQueryInterface(riid, ppv, this); }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP Invoke(ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) override {
        BOOL ok = FALSE;
        args->get_IsSuccess(&ok);
        m_fn(S_OK, ok);
        return S_OK;
    }
private:
    std::function<void(HRESULT, BOOL)> m_fn;
};

class SourceChangedHandler : public ICoreWebView2SourceChangedEventHandler {
public:
    explicit SourceChangedHandler(std::function<void()> fn) : m_fn(std::move(fn)) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override { return handleQueryInterface(riid, ppv, this); }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP Invoke(ICoreWebView2* sender, ICoreWebView2SourceChangedEventArgs* args) override { m_fn(); return S_OK; }
private:
    std::function<void()> m_fn;
};

// 动态加载 WebView2Loader.dll（避免链接 MSVC 静态库）
using CreateEnvFn = HRESULT(STDMETHODCALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
CreateEnvFn g_createEnv = nullptr;
bool loadWebView2LoaderOnce() {
    static bool loaded = []() {
        HMODULE h = LoadLibraryW(L"WebView2Loader.dll");
        if (!h) h = LoadLibraryW(L"WebView2Loader_x64.dll");
        if (!h) { qWarning() << "[McpAppWebView2] WebView2Loader.dll not found"; return false; }
        g_createEnv = (CreateEnvFn)GetProcAddress(h, "CreateCoreWebView2EnvironmentWithOptions");
        return g_createEnv != nullptr;
    }();
    return loaded;
}

} // namespace wv2_detail

using namespace wv2_detail;

McpAppWebView2Renderer::McpAppWebView2Renderer(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_NativeWindow, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

McpAppWebView2Renderer::~McpAppWebView2Renderer()
{
    if (m_webview) { static_cast<ICoreWebView2*>(m_webview)->Release(); }
    if (m_controller) { static_cast<ICoreWebView2Controller*>(m_controller)->Release(); }
    if (m_environment) { static_cast<ICoreWebView2Environment*>(m_environment)->Release(); }
}

void McpAppWebView2Renderer::initializeAsync(std::function<void(bool, const QString&)> onReady)
{
    m_onReady = std::move(onReady);
    ensureInitialized();
}

void McpAppWebView2Renderer::ensureInitialized()
{
    if (m_ready || m_initStarted) return;
    if (!loadWebView2LoaderOnce()) {
        if (m_onReady) m_onReady(false, QStringLiteral("WebView2Loader.dll not found"));
        return;
    }
    m_initStarted = true;

    HWND hwnd = reinterpret_cast<HWND>(winId());
    auto self = QPointer<McpAppWebView2Renderer>(this);

    auto envHandler = new EnvCreatedHandler([self, hwnd](HRESULT result, ICoreWebView2Environment* env) {
        if (!self) return;
        if (FAILED(result)) {
            self->m_initStarted = false;
            if (self->m_onReady) self->m_onReady(false, QStringLiteral("CreateEnvironment failed: 0x%1").arg(result, 0, 16));
            return;
        }
        self->m_environment = env;
        if (env) env->AddRef(); // raw 赋值不持有引用，显式 AddRef 防止环境被提前释放
        auto ctrlHandler = new ControllerCreatedHandler([self](HRESULT r2, ICoreWebView2Controller* controller) {
            if (!self) return;
            if (FAILED(r2)) {
                self->m_initStarted = false;
                if (self->m_onReady) self->m_onReady(false, QStringLiteral("CreateController failed: 0x%1").arg(r2, 0, 16));
                return;
            }
            self->m_controller = controller;
            if (controller) controller->AddRef(); // raw 赋值不持有引用，显式 AddRef（关键：防止回调返回后被释放）
            ICoreWebView2* wv = nullptr;
            controller->get_CoreWebView2(&wv);
            self->m_webview = wv; // get_CoreWebView2 返回已 AddRef，析构 Release 平衡
            self->m_ready = true;

            // WebView2 需要可见 + 有效尺寸才启动渲染
            controller->put_IsVisible(TRUE);
            {
                RECT b; b.left = 0; b.top = 0;
                b.right = self->width(); b.bottom = self->height();
                if (b.right < 100) b.right = 100;
                if (b.bottom < 100) b.bottom = 100;
                controller->put_Bounds(b);
            }
            qDebug() << "[McpAppWebView2] controller visible & bounds set, size=" << self->width() << "x" << self->height();

            // 宿主 -> App 通道（App 用 chrome.webview message 事件接收）
            // App -> 宿主 通道
            {
                HRESULT hrAdd = wv->add_WebMessageReceived(new WebMessageHandler([self](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                    if (!self) return S_OK;
                    LPWSTR msg = nullptr;
                    args->get_WebMessageAsJson(&msg);
                    if (msg) {
                        const QString json = QString::fromWCharArray(msg);
                        CoTaskMemFree(msg);
                        qDebug() << "[McpAppWebView2] WebMessageReceived: " << json.left(200);
                        QMetaObject::invokeMethod(self, [self, json]() {
                            self->handleWebMessage(json);
                        });
                    }
                    return S_OK;
                }), nullptr);
                qDebug() << "[McpAppWebView2] add_WebMessageReceived hr=0x" << QString::number(hrAdd, 16);
            }

            // 导航完成日志（调试）
            {
                HRESULT hrNav = wv->add_NavigationCompleted(new NavigationCompletedHandler([self](HRESULT, BOOL ok) {
                    qDebug() << "[McpAppWebView2] NavigationCompleted success=" << (ok ? "true" : "false");
                }), nullptr);
                qDebug() << "[McpAppWebView2] add_NavigationCompleted hr=0x" << QString::number(hrNav, 16);
            }

            // 进程失败 / 源变化诊断（定位导航卡点）
            {
                HRESULT hrSrc = wv->add_SourceChanged(new SourceChangedHandler([self]() {
                    if (!self) return;
                    LPWSTR src = nullptr;
                    ICoreWebView2* wv2 = static_cast<ICoreWebView2*>(self->m_webview);
                    if (wv2 && SUCCEEDED(wv2->get_Source(&src))) {
                        qDebug() << "[McpAppWebView2] SourceChanged -> " << QString::fromWCharArray(src);
                        CoTaskMemFree(src);
                    }
                }), nullptr);
                qDebug() << "[McpAppWebView2] add_SourceChanged hr=0x" << QString::number(hrSrc, 16);
            }

            // 注入 AppBridge 桥接脚本
            self->injectBridgeScript();

            // 若有待导航 HTML，继续
            if (!self->m_pendingHtml.isEmpty()) {
                const QString html = self->m_pendingHtml;
                const QUrl base = self->m_pendingBaseUrl;
                self->m_pendingHtml.clear();
                self->m_pendingBaseUrl = QUrl();
                self->loadHtml(html, base);
            }
            if (self->m_onReady) self->m_onReady(true, QString());
        });
        env->CreateCoreWebView2Controller(hwnd, ctrlHandler);
    });

    if (g_createEnv) {
        g_createEnv(nullptr, nullptr, nullptr, envHandler);
    } else {
        if (m_onReady) m_onReady(false, QStringLiteral("WebView2Loader export missing"));
    }
}

void McpAppWebView2Renderer::injectBridgeScript()
{
    if (!m_webview) return;
    // AppBridge：App 通过 window.mcpAppHost 与宿主通信（JSON-RPC 方言）
    const char* script =
        "if (!window.mcpAppHost) {"
        "  window.mcpAppHost = {"
        "    _l: [],"
        "    postMessage: function(m) { try { window.chrome.webview.postMessage(m); } catch(e){} },"
        "    addEventListener: function(fn) { this._l.push(fn); },"
        "    _dispatch: function(m) { this._l.forEach(function(fn){ try{ fn(m); }catch(e){} }); }"
        "  };"
        "  try { window.chrome.webview.addEventListener('message', function(e){ window.mcpAppHost._dispatch(e.data); }); } catch(e){}"
        "}";
    static_cast<ICoreWebView2*>(m_webview)->AddScriptToExecuteOnDocumentCreated(
        QString::fromUtf8(script).toStdWString().c_str(), nullptr);
}

void McpAppWebView2Renderer::handleWebMessage(const QString& json)
{
    if (m_messageHandler) {
        const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
        if (doc.isObject()) {
            m_messageHandler(doc.object());
        }
    }
}

void McpAppWebView2Renderer::loadHtml(const QString& html, const QUrl& baseUrl)
{
    if (!m_webview || !m_ready) {
        m_pendingHtml = html;
        m_pendingBaseUrl = baseUrl;
        ensureInitialized();
        return;
    }
    const QString base = baseUrl.toString();
    QString fullHtml = html;
    if (!base.isEmpty()) {
        // 注入 <base> 便于相对资源解析
        fullHtml = QStringLiteral("<base href=\"%1\">%2").arg(base, html);
    }
    qDebug() << "[McpAppWebView2] NavigateToString, len=" << fullHtml.size() << " webview=" << (m_webview ? "set" : "null");
    const HRESULT hr = static_cast<ICoreWebView2*>(m_webview)->NavigateToString(fullHtml.toStdWString().c_str());
    qDebug() << "[McpAppWebView2] NavigateToString hr=0x" << QString::number(hr, 16);
}

void McpAppWebView2Renderer::sendMessageToApp(const QJsonObject& message)
{
    if (!m_webview || !m_ready) return;
    postToJs(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

void McpAppWebView2Renderer::postToJs(const QString& jsonString)
{
    static_cast<ICoreWebView2*>(m_webview)->PostWebMessageAsJson(jsonString.toStdWString().c_str());
}

void McpAppWebView2Renderer::setAppMessageHandler(std::function<void(const QJsonObject&)> handler)
{
    m_messageHandler = std::move(handler);
}

void McpAppWebView2Renderer::setPermissionPolicy(const std::vector<QString>& allowedTools,
                                                 const std::vector<QString>& allowedCapabilities)
{
    // 简化实现：当前记录允许的工具集合，供宿主层在 tools/call 代理时校验。
    // 后续可扩展为 WebView2 Settings 上的权限细化。
    Q_UNUSED(allowedTools);
    Q_UNUSED(allowedCapabilities);
}

void McpAppWebView2Renderer::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    ensureInitialized();
}

void McpAppWebView2Renderer::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_controller) {
        RECT bounds;
        bounds.left = 0; bounds.top = 0;
        bounds.right = width(); bounds.bottom = height();
        static_cast<ICoreWebView2Controller*>(m_controller)->put_Bounds(bounds);
    }
}

} // namespace mcp_qt
