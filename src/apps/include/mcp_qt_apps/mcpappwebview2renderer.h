#pragma once
// McpAppWebView2Renderer.h — MCP Apps WebView2 渲染后端（Windows）
//
// 基于 Microsoft WebView2（Edge WebView2 Runtime，Windows 10/11 系统预装）。
// 通过动态加载 WebView2Loader.dll 接入，避免对 MSVC 构建的静态库 ABI 依赖，
// 使 MinGW 工具链可直接使用。
//
// 特性：
//   - QWidget 宿主（内含 HWND，WebView2 控制器绑定为子窗口）
//   - 注入 AppBridge 桥接脚本：window.mcpAppHost.postMessage(json) / 监听宿主消息
//   - JS → C++：window.chrome.webview.postMessage → WebMessageReceived → 回调
//   - C++ → JS：PostWebMessageAsJson
//   - 隔离进程渲染（WebView2 原生沙箱语义）

#include "IMcpAppRenderer.h"
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QWidget>
#include <functional>
#include <memory>
#include <vector>

// 前置声明 WebView2 COM 接口（避免污染头文件）
struct ICoreWebView2;
struct ICoreWebView2Controller;
struct ICoreWebView2Environment;

namespace mcp_qt {

/**
 * @brief WebView2 渲染后端。
 *
 * 用法：
 *   auto renderer = std::make_shared<McpAppWebView2Renderer>();
 *   auto w = renderer->hostWidget();   // 嵌入你的布局
 *   renderer->setAppMessageHandler([this](const QJsonObject& m){ ... });
 *   renderer->loadHtml(html, baseUrl);
 */
class McpAppWebView2Renderer : public QWidget, public IMcpAppRenderer {
    Q_OBJECT
public:
    explicit McpAppWebView2Renderer(QWidget* parent = nullptr);
    ~McpAppWebView2Renderer() override;

signals:
    /// 沙箱 CSP 已构造并注入（安全审计日志，规范 Host SHOULD 记录）。
    void cspAudited(const QString& csp);
    /// 检测到 App 声明访问外部域（_meta.ui.csp 的 connect/resource/frame/base 域），宿主应警示用户。
    void externalDomainsDetected(const QStringList& domains);
    /// 沙箱页面加载超时（15s 内未完成导航，基本资源防护）。
    void loadTimeout();
    /// HTML 被安全策略拦截（内容白名单不匹配 / 外部域警告被拒绝），未渲染。
    void htmlBlockedByPolicy();
    /// WebView2 子进程总内存超限（totalMb > maxMb，资源限制）。
    void resourceLimitExceeded(int totalMb, int maxMb);

public:

    // IMcpAppRenderer
    QWidget* hostWidget() override { return this; }
    void loadHtml(const QString& html, const QUrl& baseUrl) override;
    void sendMessageToApp(const QJsonObject& message) override;
    void setAppMessageHandler(std::function<void(const QJsonObject&)> handler) override;
    void setPermissionPolicy(const std::vector<QString>& allowedTools,
                             const std::vector<QString>& allowedCapabilities) override;
    void setUiMeta(const QJsonObject& uiMeta) override;

    /**
     * @brief 异步初始化 WebView2（首次调用自动创建环境/控制器）。
     * @param onReady 初始化完成回调（成功 true）
     */
    void initializeAsync(std::function<void(bool ok, const QString& error)> onReady = nullptr);

    /**
     * @brief 当前是否已初始化（可导航）。
     */
    bool isReady() const { return m_ready; }

    // ---- 安全策略（资源限制 / 内容白名单 / 外部域警告）----
    /// 设置 WebView2 子进程总内存上限（MB）。0=禁用。超限发射 resourceLimitExceeded。
    void setMaxMemoryMb(int mb);
    /// 设置 HTML 内容 SHA-256 白名单；非空白名单时仅允许白名单内的 HTML 渲染（哈希 allowlist）。
    void setAllowedHtmlHashes(const QSet<QString>& hashes);
    /// 启用外部域访问确认对话框（默认关闭，仅发 externalDomainsDetected 信号）。
    void setExternalDomainWarningEnabled(bool on);

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void ensureInitialized();
    void loadSandboxShell();
    void sendSandboxResourceReady();
    void handleWebMessage(const QString& json);
    void postToJs(const QString& jsonString);
    void checkMemoryUsage();

    // WebView2 COM 状态
    void* m_environment{nullptr};     // ICoreWebView2Environment*
    void* m_controller{nullptr};      // ICoreWebView2Controller*
    void* m_webview{nullptr};         // ICoreWebView2*
    bool m_ready{false};
    bool m_initStarted{false};
    bool m_navCompleted{false};       // 最近一次沙箱导航是否完成（看门狗用）
    QString m_pendingHtml;
    QUrl m_pendingBaseUrl;
    QJsonObject m_uiMeta;             // 最近一次 setUiMeta（_meta.ui：csp/permissions）

    std::function<void(const QJsonObject&)> m_messageHandler;
    std::function<void(bool, const QString&)> m_onReady;

    // 安全策略状态
    QSet<QString> m_allowedHtmlHashes;   // HTML SHA-256 白名单（空 = 不过滤）
    bool m_extDomainWarning{false};      // 外部域访问是否弹确认框
    bool m_extDomainApproved{true};      // 外部域是否已获用户确认
    int m_maxMemoryMb{0};                // WebView2 子进程内存上限（MB），0=禁用
    QTimer* m_memTimer{nullptr};         // 内存监控定时器
};

} // namespace mcp_qt
