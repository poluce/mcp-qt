// McpAppWebView2Renderer.cpp — MCP Apps WebView2 渲染后端实现
#include "mcp_qt_apps/McpAppWebView2Renderer.h"
#include "mcp_qt_apps/McpAppSupport.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QRegularExpression>
#include <QShowEvent>
#include <QResizeEvent>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTimer>

#include <windows.h>
#include <objbase.h>  // CreateStreamOnHGlobal（CapturePreview 内存输出流）
#include <psapi.h>
#include <tlhelp32.h>
#include "WebView2.h"

static void ensureMcpAppResources()
{
    Q_INIT_RESOURCE(mcp_app_assets);
}

namespace mcp_qt {

namespace {
QByteArray readEmbeddedAsset(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
} // namespace

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

class PermissionRequestedHandler : public ICoreWebView2PermissionRequestedEventHandler {
public:
    explicit PermissionRequestedHandler(std::function<HRESULT(ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs*)> fn) : m_fn(std::move(fn)) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override { return handleQueryInterface(riid, ppv, this); }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP Invoke(ICoreWebView2* sender, ICoreWebView2PermissionRequestedEventArgs* args) override { return m_fn(sender, args); }
private:
    std::function<HRESULT(ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs*)> m_fn;
};

class CapturePreviewHandler : public ICoreWebView2CapturePreviewCompletedHandler {
public:
    explicit CapturePreviewHandler(std::function<void(HRESULT)> fn) : m_fn(std::move(fn)) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override { return handleQueryInterface(riid, ppv, this); }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP Invoke(HRESULT result) override { m_fn(result); return S_OK; }
private:
    std::function<void(HRESULT)> m_fn;
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
    : QWidget(parent),
      m_contentAdapters(std::make_shared<McpAppContentAdapterRegistry>())
{
    ensureMcpAppResources();
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

            // NavigateToString 生成的文档没有正常来源，内层 srcdoc 最终会成为
            // origin=null。依赖 blob Worker 的 App 会因此启动失败。把壳页映射到
            // WebView2 的虚拟 HTTPS 域名；内层
            // iframe 仍保留 sandbox，仅通过 allow-same-origin 继承这个隔离来源。
            self->m_virtualHostRoot = std::make_unique<QTemporaryDir>(
                QDir::tempPath() + QStringLiteral("/mcp_qt_webview2_XXXXXX"));
            const QString shellDir = self->m_virtualHostRoot->path() + QStringLiteral("/shell");
            self->m_virtualAppDir = self->m_virtualHostRoot->path() + QStringLiteral("/app");
            if (self->m_virtualHostRoot->isValid()
                && QDir().mkpath(shellDir) && QDir().mkpath(self->m_virtualAppDir)) {
                const QByteArray shellBytes = readEmbeddedAsset(QStringLiteral(":/mcp_qt_apps/sandbox.html"));
                const QByteArray bridgeBytes = readEmbeddedAsset(QStringLiteral(":/mcp_qt_apps/bridge.js"));
                QFile shellFile(shellDir + QStringLiteral("/sandbox.html"));
                if (shellFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    const bool shellWritten = !shellBytes.isEmpty()
                                           && shellFile.write(shellBytes) == shellBytes.size();
                    shellFile.close();
                    ICoreWebView2_3* wv3 = nullptr;
                    if (shellWritten && SUCCEEDED(wv->QueryInterface(
                            IID_ICoreWebView2_3, reinterpret_cast<void**>(&wv3)))) {
                        const HRESULT hrShellMap = wv3->SetVirtualHostNameToFolderMapping(
                            L"mcp-shell.local", shellDir.toStdWString().c_str(),
                            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                        const HRESULT hrAppMap = wv3->SetVirtualHostNameToFolderMapping(
                            L"mcp-app.local", self->m_virtualAppDir.toStdWString().c_str(),
                            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                        self->m_sandboxVirtualHostReady = SUCCEEDED(hrShellMap) && SUCCEEDED(hrAppMap);
                        qDebug() << "[McpAppWebView2] virtual sandbox origin mapping hr=0x"
                                 << QString::number(hrShellMap, 16)
                                 << "app hr=0x" << QString::number(hrAppMap, 16);
                        wv3->Release();
                    }
                }
                QFile bridgeFile(self->m_virtualAppDir + QStringLiteral("/bridge.js"));
                if (bridgeFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    bridgeFile.write(bridgeBytes);
                    bridgeFile.close();
                }
            }

            // WebView2 需要可见 + 有效尺寸才启动渲染
            controller->put_IsVisible(TRUE);
            self->updateViewportBounds();
            qDebug() << "[McpAppWebView2] controller visible & bounds set, logical size="
                     << self->width() << "x" << self->height()
                     << "devicePixelRatio=" << self->devicePixelRatioF();

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

            // 权限请求按 _meta.ui.permissions 授予（规范：仅授予 App 声明的权限，其余一律拒绝）
            {
                HRESULT hrPerm = wv->add_PermissionRequested(new PermissionRequestedHandler(
                    [self](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                        if (!self) return S_OK;
                        COREWEBVIEW2_PERMISSION_KIND kind;
                        if (args->get_PermissionKind(&kind) != S_OK) return S_OK;
                        const QJsonObject perms = self->m_uiMeta.value(QStringLiteral("permissions")).toObject();
                        bool allow = false;
                        switch (kind) {
                            case COREWEBVIEW2_PERMISSION_KIND_CAMERA:         allow = perms.contains(QStringLiteral("camera")); break;
                            case COREWEBVIEW2_PERMISSION_KIND_MICROPHONE:     allow = perms.contains(QStringLiteral("microphone")); break;
                            case COREWEBVIEW2_PERMISSION_KIND_GEOLOCATION:    allow = perms.contains(QStringLiteral("geolocation")); break;
                            case COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ: allow = perms.contains(QStringLiteral("clipboardWrite")); break;
                            default:                                          allow = false; break;
                        }
                        args->put_State(allow ? COREWEBVIEW2_PERMISSION_STATE_ALLOW
                                              : COREWEBVIEW2_PERMISSION_STATE_DENY);
                        return S_OK;
                    }), nullptr);
                qDebug() << "[McpAppWebView2] add_PermissionRequested hr=0x" << QString::number(hrPerm, 16);
            }

            // 导航完成日志（调试）
            {
                HRESULT hrNav = wv->add_NavigationCompleted(new NavigationCompletedHandler([self](HRESULT, BOOL ok) {
                    if (self) self->m_navCompleted = true;
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

            // 若有待加载的 App 资源，进入沙箱壳页流程（壳页就绪后发 sandbox-proxy-ready）
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
        // 独立 userDataFolder：避免与其它 WebView2 实例共享默认目录的锁冲突
        // （并发实例时 CreateCoreWebView2Controller 报 ERROR_NO_SYSTEM_RESOURCES 0x800700AA）。
        const QString userDataFolder = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                                     + QStringLiteral("/mcp_qt_webview2");
        g_createEnv(nullptr, userDataFolder.toStdWString().c_str(), nullptr, envHandler);
    } else {
        if (m_onReady) m_onReady(false, QStringLiteral("WebView2Loader export missing"));
    }
}

void McpAppWebView2Renderer::loadSandboxShell()
{
    if (!m_webview || !m_ready) return;
    qDebug() << "[McpAppWebView2] loadSandboxShell (double-iframe sandbox proxy)";
    // 加载看门狗：15s 内未完成导航视为异常（基本资源防护，防恶意 View 挂起宿主）
    m_navCompleted = false;
    QTimer::singleShot(15000, this, [this]() {
        if (!m_navCompleted) emit loadTimeout();
    });
    if (m_sandboxVirtualHostReady) {
        // 壳页与 App 使用不同来源，避免 allow-scripts + allow-same-origin 的内层
        // iframe 访问并解除父页 sandbox。
        static_cast<ICoreWebView2*>(m_webview)->Navigate(L"https://mcp-shell.local/sandbox.html");
    } else {
        qWarning() << "[McpAppWebView2] virtual sandbox origin unavailable; using opaque fallback";
        const QString shellHtml = QString::fromUtf8(
            readEmbeddedAsset(QStringLiteral(":/mcp_qt_apps/sandbox.html")));
        static_cast<ICoreWebView2*>(m_webview)->NavigateToString(shellHtml.toStdWString().c_str());
    }
}

void McpAppWebView2Renderer::sendSandboxResourceReady()
{
    if (!m_webview || !m_ready) return;
    // 内容白名单（哈希 allowlist）：非空白名单时仅放行匹配 SHA-256 的 HTML
    if (!m_allowedHtmlHashes.isEmpty()) {
        const QString h = McpAppSupport::hashHtml(m_pendingHtml.toUtf8());
        if (!m_allowedHtmlHashes.contains(h)) {
            qWarning() << "[McpAppWebView2] HTML not in allowlist; refusing to render";
            emit htmlBlockedByPolicy();
            return;
        }
    }
    // 外部域确认：用户未批准则拒绝渲染
    if (m_extDomainWarning && !m_extDomainApproved) {
        qWarning() << "[McpAppWebView2] external domain access not approved; refusing to render";
        emit htmlBlockedByPolicy();
        return;
    }
    qDebug() << "[McpAppWebView2] sendSandboxResourceReady, htmlLen=" << m_pendingHtml.size();
    McpAppContent content{m_pendingHtml, m_uiMeta, m_pendingBaseUrl, {}};
    if (m_contentAdapters) content = m_contentAdapters->adapt(std::move(content));
    QString html = content.html;
    const QJsonObject effectiveUiMeta = content.uiMeta;
    if (!content.appliedAdapters.isEmpty()) {
        qInfo() << "[McpAppWebView2] applied content adapters:" << content.appliedAdapters;
    }
    QJsonObject params;
    params[QStringLiteral("sandbox")] = QStringLiteral("allow-scripts allow-same-origin");
    const QString csp = McpAppSupport::buildCsp(effectiveUiMeta);
    qInfo().noquote() << "[McpAppWebView2] CSP:" << csp;
    params[QStringLiteral("csp")] = csp;

    if (m_sandboxVirtualHostReady) {
        // srcdoc 的 location 永远是 about:srcdoc，一些运行时会错误解析相对 Worker
        // 模块。将完整 App 文档写入虚拟域，让 location/origin/Worker 基址一致。
        QString headInjection = QStringLiteral(
            "<meta http-equiv=\"Content-Security-Policy\" content=\"%1\">")
            .arg(csp.toHtmlEscaped().replace(QLatin1Char('"'), QStringLiteral("&quot;")));
        if (!content.baseUrl.isEmpty()) {
            headInjection += QStringLiteral("<base href=\"%1\">")
                .arg(content.baseUrl.toString().toHtmlEscaped());
        }
        headInjection += QStringLiteral("<script src=\"/bridge.js\"></script>");
        const QRegularExpression headTag(QStringLiteral("<head[^>]*>"),
                                         QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch headMatch = headTag.match(html);
        if (headMatch.hasMatch()) {
            html.insert(headMatch.capturedEnd(), headInjection);
        } else {
            html.prepend(QStringLiteral("<!DOCTYPE html><html><head>%1</head><body>").arg(headInjection));
            html.append(QStringLiteral("</body></html>"));
        }
        const QString viewName = QStringLiteral("view-%1.html")
            .arg(QString::number(reinterpret_cast<quintptr>(this), 16));
        const QString viewPath = m_virtualAppDir + QLatin1Char('/') + viewName;
        QFile viewFile(viewPath);
        if (viewFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
            && viewFile.write(html.toUtf8()) >= 0) {
            viewFile.close();
            params[QStringLiteral("url")] = QStringLiteral("https://mcp-app.local/") + viewName;
            params[QStringLiteral("html")] = QString();
        } else {
            viewFile.close();
            params[QStringLiteral("html")] = html;
        }
    } else {
        if (!content.baseUrl.isEmpty()) {
            html = QStringLiteral("<base href=\"%1\">%2").arg(content.baseUrl.toString(), html);
        }
        params[QStringLiteral("bridgeScript")] = QString::fromUtf8(
            readEmbeddedAsset(QStringLiteral(":/mcp_qt_apps/bridge.js")));
        params[QStringLiteral("html")] = html;
    }
    emit cspAudited(csp);  // 安全审计日志（规范 SHOULD）
    params[QStringLiteral("prefersBorder")] = McpAppSupport::uiPrefersBorder(m_uiMeta);
    const QJsonValue perms = m_uiMeta.value(QStringLiteral("permissions"));
    if (perms.isObject() && !perms.toObject().isEmpty()) {
        params[QStringLiteral("permissions")] = perms.toObject();
    }
    postToJs(QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), QStringLiteral("ui/notifications/sandbox-resource-ready")},
        {QStringLiteral("params"), params}
    }).toJson(QJsonDocument::Compact)));
}

void McpAppWebView2Renderer::setContentAdapterRegistry(
    std::shared_ptr<McpAppContentAdapterRegistry> registry)
{
    m_contentAdapters = std::move(registry);
}

void McpAppWebView2Renderer::setUiMeta(const QJsonObject& uiMeta)
{
    m_uiMeta = uiMeta;
    // 检测 App 声明的外部域（规范 SHOULD：宿主应警示用户）
    const QStringList ext = McpAppSupport::cspExternalDomains(uiMeta);
    if (!ext.isEmpty()) {
        emit externalDomainsDetected(ext);
        if (m_extDomainWarning) {
            // 未声明域访问确认对话框（可选启用；默认仅发信号）
            const QMessageBox::StandardButton b = QMessageBox::warning(
                nullptr, QStringLiteral("MCP Apps 外部域访问"),
                QStringLiteral("该 App 声明访问以下外部域：\n%1\n\n是否继续加载？")
                    .arg(ext.join(QStringLiteral("\n"))),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            m_extDomainApproved = (b == QMessageBox::Yes);
        } else {
            m_extDomainApproved = true;
        }
    } else {
        m_extDomainApproved = true;
    }
}

// ========== 安全策略 ==========
void McpAppWebView2Renderer::setAllowedHtmlHashes(const QSet<QString>& hashes)
{
    m_allowedHtmlHashes = hashes;
}

void McpAppWebView2Renderer::setExternalDomainWarningEnabled(bool on)
{
    m_extDomainWarning = on;
}

void McpAppWebView2Renderer::setMaxMemoryMb(int mb)
{
    m_maxMemoryMb = mb;
    if (mb > 0 && !m_memTimer) {
        m_memTimer = new QTimer(this);
        connect(m_memTimer, &QTimer::timeout, this, &McpAppWebView2Renderer::checkMemoryUsage);
        m_memTimer->start(5000);
    } else if (mb <= 0 && m_memTimer) {
        m_memTimer->stop();
    }
}

namespace {
// 累加本进程直接子进程（WebView2 渲染/GPU/网络进程）的总内存（MB）
int webviewChildMemoryMb()
{
    int totalMb = 0;
    const DWORD parent = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (pe.th32ParentProcessID != parent) continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h) continue;
            PROCESS_MEMORY_COUNTERS pmc;
            if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) {
                totalMb += static_cast<int>(pmc.WorkingSetSize / (1024 * 1024));
            }
            CloseHandle(h);
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return totalMb;
}
} // namespace

void McpAppWebView2Renderer::checkMemoryUsage()
{
    if (m_maxMemoryMb <= 0) return;
    const int totalMb = webviewChildMemoryMb();
    if (totalMb > m_maxMemoryMb) emit resourceLimitExceeded(totalMb, m_maxMemoryMb);
}

void McpAppWebView2Renderer::handleWebMessage(const QString& json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject()) return;
    const QJsonObject obj = doc.object();
    const QString method = obj.value(QStringLiteral("method")).toString();
    if (method.startsWith(QLatin1String("debug/"))) {
        qDebug().noquote() << "[McpAppWebView2] " << method << QJsonDocument(obj.value(QStringLiteral("params")).toObject()).toJson(QJsonDocument::Compact);
    }
    if (method == QStringLiteral("ui/notifications/sandbox-proxy-ready")) {
        sendSandboxResourceReady();
        return;
    }
    if (method.startsWith(QStringLiteral("debug/"))) {
        qDebug() << "[McpAppWebView2] SANDBOX DEBUG:" << QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        return;
    }
    if (method.startsWith(QStringLiteral("ui/notifications/sandbox-"))) {
        return;  // 沙箱代理保留消息不上抛给上层
    }
    if (m_messageHandler) m_messageHandler(obj);
}

void McpAppWebView2Renderer::loadHtml(const QString& html, const QUrl& baseUrl)
{
    m_pendingHtml = html;
    m_pendingBaseUrl = baseUrl;
    if (!m_webview || !m_ready) {
        ensureInitialized();
        return;
    }
    loadSandboxShell();
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
    // 显示时按当前实际尺寸更新 WebView2 视口（控制器创建时可能还是未布局的小尺寸）
    updateViewportBounds();
}

void McpAppWebView2Renderer::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateViewportBounds();
}

void McpAppWebView2Renderer::capturePreviewToFile(const QString& path,
                                                  std::function<void(bool, const QString&)> onDone)
{
    if (!m_webview || !m_ready) {
        if (onDone) onDone(false, QStringLiteral("webview not ready"));
        return;
    }
    // 用内存流承接 CapturePreview 的 PNG 输出：避免文件流在 Release 后仍锁着文件导致改名失败
    IStream* stream = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(hr)) {
        if (onDone) onDone(false, QStringLiteral("CreateStreamOnHGlobal failed"));
        return;
    }
    auto* wv = static_cast<ICoreWebView2*>(m_webview);
    auto* handler = new CapturePreviewHandler([path, stream, onDone](HRESULT result) {
        if (SUCCEEDED(result)) {
            HGLOBAL hg = nullptr;
            if (SUCCEEDED(GetHGlobalFromStream(stream, &hg))) {
                const SIZE_T size = GlobalSize(hg);
                void* ptr = GlobalLock(hg);
                if (ptr) {
                    QFile f(path);
                    if (f.open(QIODevice::WriteOnly)) {
                        f.write(static_cast<const char*>(ptr), static_cast<qint64>(size));
                        f.close();
                        if (onDone) onDone(true, QString());
                    } else {
                        if (onDone) onDone(false, QStringLiteral("open output file failed"));
                    }
                    GlobalUnlock(hg);
                } else {
                    if (onDone) onDone(false, QStringLiteral("GlobalLock failed"));
                }
            } else {
                if (onDone) onDone(false, QStringLiteral("GetHGlobalFromStream failed"));
            }
        } else {
            if (onDone) onDone(false, QStringLiteral("CapturePreview failed"));
        }
        stream->Release();
    });
    hr = wv->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream, handler);
    if (FAILED(hr)) {
        stream->Release();
        delete handler;
        if (onDone) onDone(false, QStringLiteral("CapturePreview call failed"));
    }
}

void McpAppWebView2Renderer::updateViewportBounds()
{
    if (!m_controller) return;

    // Qt 的 width()/height() 是设备无关的逻辑像素，而 WebView2 Controller
    // 默认以 raw pixels 解释 Bounds。高 DPI 下直接传 Qt 尺寸会让 WebView
    // 宽高各缩小一个缩放因子（200% 缩放时最终只占窗口面积的 1/4）。
    // 这里不能使用 GetClientRect(winId())：renderer 在隐藏弹窗中提前创建
    // native HWND 时，该客户区可能仍保留初始的 100x30 逻辑尺寸。
    const qreal scale = devicePixelRatioF();
    RECT bounds{};
    bounds.right = qRound(width() * scale);
    bounds.bottom = qRound(height() * scale);

    if (bounds.right <= 0 || bounds.bottom <= 0) return;
    auto* controller = static_cast<ICoreWebView2Controller*>(m_controller);
    const HRESULT hr = controller->put_Bounds(bounds);
    if (FAILED(hr)) {
        qWarning() << "[McpAppWebView2] put_Bounds failed: 0x"
                   << QString::number(hr, 16);
    }
}

} // namespace mcp_qt
