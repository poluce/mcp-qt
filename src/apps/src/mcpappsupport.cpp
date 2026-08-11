// McpAppSupport.cpp — MCP Apps (io.modelcontextprotocol/ui) 协议层实现
#include "mcp_qt_apps/McpAppSupport.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>

namespace mcp_qt {

namespace {
// 从 tool inputSchema 取 _meta（2026-07-28 的 _meta.ui.resourceUri 位于 inputSchema 顶层 _meta）
QJsonObject schemaMeta(const QJsonObject& toolSchema) {
    return toolSchema.value(QStringLiteral("_meta")).toObject();
}
} // namespace

bool toolDeclaresMcpApp(const QJsonObject& toolSchema) {
    return !toolUiResourceUri(toolSchema).isEmpty();
}

QUrl toolUiResourceUri(const QJsonObject& toolSchema) {
    const QJsonObject meta = schemaMeta(toolSchema);
    if (!meta.contains(QStringLiteral("ui")) || !meta.value(QStringLiteral("ui")).isObject()) {
        return {};
    }
    const QJsonObject ui = meta.value(QStringLiteral("ui")).toObject();
    if (!ui.contains(QStringLiteral("resourceUri"))) return {};
    const QString uri = ui.value(QStringLiteral("resourceUri")).toString();
    if (uri.isEmpty()) return {};
    return QUrl(uri);
}

McpAppSupport::McpAppSupport() = default;
McpAppSupport::~McpAppSupport() = default;

QJsonObject McpAppSupport::clientCapabilitiesExtension() const {
    // SEP-2133 / ext-apps：客户端声明支持渲染 text/html;profile=mcp-app
    return QJsonObject{
        {QStringLiteral("io.modelcontextprotocol/ui"), QJsonObject{
            {QStringLiteral("mimeTypes"), QJsonArray{QStringLiteral("text/html;profile=mcp-app")}}
        }}
    };
}

QUrl McpAppSupport::resolveUiUrl(const QUrl& baseUrl, const QUrl& uiUri) const {
    if (uiUri.isEmpty()) return {};
    // 已是 http(s) 直接使用
    if (uiUri.scheme() == QStringLiteral("http") || uiUri.scheme() == QStringLiteral("https")) {
        return uiUri;
    }
    // ui:// 资源：尝试相对 baseUrl 解析（服务器把 UI 资源挂在同一 HTTP 根下）
    if (!baseUrl.isEmpty()) {
        return baseUrl.resolved(uiUri);
    }
    return {};
}

void McpAppSupport::fetchUiResource(const QUrl& baseUrl, const QUrl& uiUri,
                                    std::function<void(const QString&, const QString&)> onResult) const {
    const QUrl target = resolveUiUrl(baseUrl, uiUri);
    if (target.isEmpty()) {
        if (onResult) onResult(QString(), QStringLiteral("Cannot resolve ui:// resource"));
        return;
    }
    auto nam = new QNetworkAccessManager;
    QNetworkRequest req(target);
    req.setRawHeader("Accept", "text/html;profile=mcp-app, text/html");
    QNetworkReply* reply = nam->get(req);
    QObject::connect(reply, &QNetworkReply::finished, [reply, nam, onResult]() {
        reply->deleteLater();
        nam->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            if (onResult) onResult(QString(), reply->errorString());
            return;
        }
        const QByteArray data = reply->readAll();
        if (onResult) onResult(QString::fromUtf8(data), QString());
    });
}

QJsonObject McpAppSupport::buildInitializeRequest(const QJsonObject& requestedCapabilities) {
    return QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 1},
        {QStringLiteral("method"), QStringLiteral("ui/initialize")},
        {QStringLiteral("params"), QJsonObject{
            {QStringLiteral("capabilities"), requestedCapabilities}
        }}
    };
}

QJsonObject McpAppSupport::buildResponse(int64_t id, const QJsonObject& result) {
    return QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("result"), result}
    };
}

QJsonObject McpAppSupport::buildError(int64_t id, int code, const QString& message) {
    return QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("error"), QJsonObject{
            {QStringLiteral("code"), code},
            {QStringLiteral("message"), message}
        }}
    };
}

bool McpAppSupport::parseAppMessage(const QJsonObject& message,
                                    QString* outMethod, qint64* outId, QJsonObject* outParams) {
    if (message.isEmpty() || !message.contains(QStringLiteral("method"))) return false;
    if (outMethod) *outMethod = message.value(QStringLiteral("method")).toString();
    if (outId) *outId = message.value(QStringLiteral("id")).toVariant().toLongLong();
    if (outParams) *outParams = message.value(QStringLiteral("params")).toObject();
    return true;
}

// ============================================================================
// 沙箱安全构造（MCP Apps 2026-01-26 规范 MUST 级）
// ============================================================================

QString McpAppSupport::buildDefaultCsp() {
    // 规范 Restrictive Default（ui.csp 省略时 Host MUST 使用）：
    // default-src 'none'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline';
    // img-src 'self' data:; media-src 'self' data:; connect-src 'none';
    // 未列出的指令（frame-src/object-src/base-uri 等）经 default-src 'none' fallback 全部禁用。
    return QStringLiteral(
        "default-src 'none'; "
        "script-src 'self' 'unsafe-inline'; "
        "style-src 'self' 'unsafe-inline'; "
        "img-src 'self' data:; "
        "media-src 'self' data:; "
        "connect-src 'none';");
}

namespace {
QStringList cspDomainList(const QJsonObject& csp, const char* key) {
    QStringList out;
    const QJsonValue v = csp.value(QLatin1String(key));
    if (v.isArray()) {
        for (const auto& d : v.toArray()) out << d.toString();
    }
    return out;
}
QString cspSpaced(const QStringList& l) {
    return l.isEmpty() ? QString() : l.join(QLatin1Char(' ')) + QLatin1Char(' ');
}
} // namespace

QString McpAppSupport::buildCsp(const QJsonObject& uiMeta) {
    const QJsonValue cspVal = uiMeta.value(QStringLiteral("csp"));
    if (!cspVal.isObject()) {
        return buildDefaultCsp();
    }
    const QJsonObject csp = cspVal.toObject();
    const QStringList connectDomains = cspDomainList(csp, "connectDomains");
    const QStringList resourceDomains = cspDomainList(csp, "resourceDomains");
    const QStringList frameDomains = cspDomainList(csp, "frameDomains");
    const QStringList baseUriDomains = cspDomainList(csp, "baseUriDomains");

    // 规范 CSP Construction from Metadata：未声明域绝不放行（frame/base 缺省最严格）。
    QString c;
    c += QStringLiteral("default-src 'none'; ");
    c += QStringLiteral("script-src 'self' 'unsafe-inline' ") + cspSpaced(resourceDomains) + QStringLiteral("; ");
    c += QStringLiteral("style-src 'self' 'unsafe-inline' ") + cspSpaced(resourceDomains) + QStringLiteral("; ");
    c += QStringLiteral("connect-src 'self' ") + cspSpaced(connectDomains) + QStringLiteral("; ");
    c += QStringLiteral("img-src 'self' data: ") + cspSpaced(resourceDomains) + QStringLiteral("; ");
    c += QStringLiteral("font-src 'self' ") + cspSpaced(resourceDomains) + QStringLiteral("; ");
    c += QStringLiteral("media-src 'self' data: ") + cspSpaced(resourceDomains) + QStringLiteral("; ");
    c += QStringLiteral("frame-src ") + (frameDomains.isEmpty() ? QStringLiteral("'none'") : frameDomains.join(QLatin1Char(' '))) + QStringLiteral("; ");
    c += QStringLiteral("object-src 'none'; ");
    c += QStringLiteral("base-uri ") + (baseUriDomains.isEmpty() ? QStringLiteral("'self'") : baseUriDomains.join(QLatin1Char(' '))) + QStringLiteral(";");
    return c;
}

QString McpAppSupport::buildAllowAttribute(const QJsonObject& uiMeta) {
    const QJsonValue pVal = uiMeta.value(QStringLiteral("permissions"));
    if (!pVal.isObject()) return {};
    const QJsonObject p = pVal.toObject();
    QStringList allow;
    if (p.contains(QStringLiteral("camera"))) allow << QStringLiteral("camera");
    if (p.contains(QStringLiteral("microphone"))) allow << QStringLiteral("microphone");
    if (p.contains(QStringLiteral("geolocation"))) allow << QStringLiteral("geolocation");
    if (p.contains(QStringLiteral("clipboardWrite"))) allow << QStringLiteral("clipboard-write");
    return allow.join(QLatin1Char(' '));
}

// ============================================================================
// 资源元数据辅助
// ============================================================================

bool McpAppSupport::uiPrefersBorder(const QJsonObject& uiMeta) {
    const QJsonValue v = uiMeta.value(QStringLiteral("prefersBorder"));
    if (v.isBool()) return v.toBool();
    return true;  // 安全默认：显示边框标识沙箱边界
}

QString McpAppSupport::uiDomain(const QJsonObject& uiMeta) {
    return uiMeta.value(QStringLiteral("domain")).toString();
}

QStringList McpAppSupport::cspExternalDomains(const QJsonObject& uiMeta) {
    QStringList out;
    const QJsonObject csp = uiMeta.value(QStringLiteral("csp")).toObject();
    auto collect = [&out, &csp](const char* key) {
        const QJsonValue v = csp.value(QLatin1String(key));
        if (v.isArray()) {
            for (const auto& d : v.toArray()) {
                const QString s = d.toString();
                if (!s.isEmpty() && !out.contains(s)) out << s;
            }
        }
    };
    collect("connectDomains");
    collect("resourceDomains");
    collect("frameDomains");
    collect("baseUriDomains");
    return out;
}

QString McpAppSupport::hashHtml(const QByteArray& html) {
    return QString::fromLatin1(QCryptographicHash::hash(html, QCryptographicHash::Sha256).toHex());
}

} // namespace mcp_qt
