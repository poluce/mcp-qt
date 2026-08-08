// McpAppSupport.cpp — MCP Apps (io.modelcontextprotocol/ui) 协议层实现
#include "mcp_qt_apps/McpAppSupport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

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

} // namespace mcp_qt
