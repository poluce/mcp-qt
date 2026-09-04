#pragma once
// McpAppSupport.h — MCP Apps (io.modelcontextprotocol/ui) 协议层
//
// 负责 MCP Apps 扩展在客户端侧的协议职责：
//   1. clientCapabilities 声明 extensions.io.modelcontextprotocol/ui 支持
//   2. 工具 _meta.ui.resourceUri 识别与 ui:// 资源获取
//   3. text/html;profile=mcp-app MIME 内容识别
//   4. AppBridge 消息协议（postMessage JSON-RPC 方言：ui/initialize 等）
//
// 渲染层与协议层解耦：本类只处理"数据与消息"，不负责 HTML 渲染。
// 渲染由 IMcpAppRenderer（可插拔）提供。

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QUrl>
#include <functional>
#include <memory>

class QNetworkAccessManager;

namespace mcp_qt {

// MCP Apps 的 MIME 类型（SEP-2133 / ext-apps）
inline constexpr const char* kMcpAppMimeType = "text/html;profile=mcp-app";
// ui:// 资源 URI 协议前缀
inline constexpr const char* kUiScheme = "ui:";

/**
 * @brief 工具是否声明了 MCP Apps UI 资源（inputSchema._meta.ui.resourceUri）
 */
bool toolDeclaresMcpApp(const QJsonObject& toolSchema);

/**
 * @brief 获取工具声明的 UI 资源 URI（inputSchema._meta.ui.resourceUri）
 */
QUrl toolUiResourceUri(const QJsonObject& toolSchema);

/**
 * @brief MCP Apps 客户端支持类：能力声明、资源获取、消息封装。
 *
 * 不依赖具体渲染器。渲染器通过 setMessageHandler 消费消息，通过
 * sendMessageToApp 发回 AppBridge 消息。
 */
class McpAppSupport {
public:
    McpAppSupport();
    ~McpAppSupport();

    McpAppSupport(const McpAppSupport&) = delete;
    McpAppSupport& operator=(const McpAppSupport&) = delete;

    /**
     * @brief 返回应合并到 clientCapabilities.extensions 的声明。
     *        { "io.modelcontextprotocol/ui": { "mimeTypes": ["text/html;profile=mcp-app"] } }
     */
    QJsonObject clientCapabilitiesExtension() const;

    /**
     * @brief 通过底层网络获取 ui:// 资源（异步）。
     * @param baseUrl      服务器基地址（用于把 ui:// 相对地址解析为 HTTP 地址）
     * @param uiUri        工具声明的 ui:// 资源地址
     * @param onResult     回调 (html, error)
     */
    void fetchUiResource(const QUrl& baseUrl, const QUrl& uiUri,
                         std::function<void(const QString& html, const QString& error)> onResult) const;

    /**
     * @brief 把 ui:// 资源地址解析为可请求的 HTTP URL。
     *        规范约定 ui:// 资源由服务器在工具描述中提供可解析地址；
     *        解析失败返回空 QUrl。
     */
    QUrl resolveUiUrl(const QUrl& baseUrl, const QUrl& uiUri) const;

    // ---- AppBridge 消息协议（postMessage JSON-RPC 方言）----

    /**
     * @brief 构造 App 初始化请求（ui/initialize）。
     * @param requestedCapabilities App 声明需要的能力（tools 等）
     */
    static QJsonObject buildInitializeRequest(const QJsonObject& requestedCapabilities);

    /**
     * @brief 构造对 App 请求的响应（jsonrpc result）。
     */
    static QJsonObject buildResponse(int64_t id, const QJsonObject& result);

    /**
     * @brief 构造对 App 请求的错误响应。
     */
    static QJsonObject buildError(int64_t id, int code, const QString& message);

    /**
     * @brief 解析 App 发来的消息（JSON-RPC 请求/通知）。
     * @param message 从渲染器收到的原始 JSON 对象
     * @param outMethod 输出方法名（如 ui/initialize、tools/call）
     * @param outId 输出 id（请求才有）
     * @param outParams 输出参数
     * @return true 解析成功且是 JSON-RPC 消息
     */
    static bool parseAppMessage(const QJsonObject& message,
                                QString* outMethod, qint64* outId, QJsonObject* outParams);

    // ---- 沙箱安全构造（2026-01-26 规范 MUST 级）----

    /**
     * @brief 限制性默认 CSP（规范 Restrictive Default；ui.csp 省略时 MUST 使用）。
     *        未声明的指令经 CSP default-src fallback 全部落到 'none'。
     */
    static QString buildDefaultCsp();

    /**
     * @brief 依据 _meta.ui 构造完整 CSP。
     *        - 无 csp 子对象 → buildDefaultCsp()（规范 MUST 默认）
     *        - 有 csp → 按 connectDomains/resourceDomains/frameDomains/baseUriDomains
     *          扩展（规范 CSP Construction from Metadata）
     * @param uiMeta 资源/工具声明的 _meta.ui 对象
     */
    static QString buildCsp(const QJsonObject& uiMeta);

    /**
     * @brief 依据 _meta.ui.permissions 构造内层 iframe 的 allow 属性（Permission Policy）。
     *        camera/microphone/geolocation/clipboardWrite → camera/microphone/geolocation/clipboard-write。
     * @param uiMeta 资源/工具声明的 _meta.ui 对象
     */
    static QString buildAllowAttribute(const QJsonObject& uiMeta);

    // ---- 资源元数据辅助（MCP Apps 2026-01-26）----

    /// 解析 _meta.ui.prefersBorder（App 是否偏好宿主显示沙箱边框）。缺省 true（安全默认：标识沙箱边界）。
    static bool uiPrefersBorder(const QJsonObject& uiMeta);
    /// 解析 _meta.ui.domain（App 专用源标识，格式由各 Host 定义）。未声明返回空。
    static QString uiDomain(const QJsonObject& uiMeta);
    /// 从 _meta.ui.csp 提取外部域名（connect/resource/frame/base 域），用于审计与用户警告。
    static QStringList cspExternalDomains(const QJsonObject& uiMeta);
    /// 计算 HTML 内容 SHA-256（十六进制小写），宿主可据此实现基于哈希的 allow/blocklist。
    static QString hashHtml(const QByteArray& html);
};

} // namespace mcp_qt
