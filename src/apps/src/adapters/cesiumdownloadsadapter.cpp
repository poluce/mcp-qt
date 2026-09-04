#include "mcp_qt_apps/McpAppContentAdapter.h"
#include "CesiumDownloadsAdapter_p.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace mcp_qt::content_adapters {
namespace {

const QRegularExpression& cesiumDownloadsPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("https://cesium\\.com/downloads/cesiumjs/releases/([^/]+)/Build/Cesium"));
    return pattern;
}

QString jsonStringLiteral(const QString& value)
{
    const QByteArray array = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(array.mid(1, array.size() - 2));
}

void injectIntoHead(QString& html, const QString& markup)
{
    static const QRegularExpression headTag(QStringLiteral("<head[^>]*>"),
                                            QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = headTag.match(html);
    if (match.hasMatch()) html.insert(match.capturedEnd(), markup);
    else html.prepend(markup);
}

class CesiumDownloadsAdapter final : public IMcpAppContentAdapter {
public:
    QString id() const override { return QStringLiteral("cesium-downloads-cors"); }

    bool matches(const McpAppContent& content) const override
    {
        return cesiumDownloadsPattern().match(content.html).hasMatch();
    }

    void adapt(McpAppContent& content) const override
    {
        const QRegularExpressionMatch match = cesiumDownloadsPattern().match(content.html);
        if (!match.hasMatch()) return;

        const QString mirrorBase = QStringLiteral(
            "https://cdn.jsdelivr.net/npm/cesium@%1/Build/Cesium").arg(match.captured(1));
        content.html.replace(cesiumDownloadsPattern(), mirrorBase);

        // Cesium resolves Worker modules while Cesium.js executes. server-map sets
        // CESIUM_BASE_URL from onload, which is too late, so bootstrap it first.
        injectIntoHead(content.html,
            QStringLiteral("<script>window.CESIUM_BASE_URL=%1;</script>")
                .arg(jsonStringLiteral(mirrorBase)));

        QJsonObject csp = content.uiMeta.value(QStringLiteral("csp")).toObject();
        QJsonArray resourceDomains = csp.value(QStringLiteral("resourceDomains")).toArray();
        const QString mirrorOrigin = QStringLiteral("https://cdn.jsdelivr.net");
        bool present = false;
        for (const QJsonValue& value : resourceDomains) {
            if (value.toString() == mirrorOrigin) { present = true; break; }
        }
        if (!present) resourceDomains.append(mirrorOrigin);
        csp[QStringLiteral("resourceDomains")] = resourceDomains;
        content.uiMeta[QStringLiteral("csp")] = csp;
    }
};

} // namespace

std::shared_ptr<IMcpAppContentAdapter> createCesiumDownloadsAdapter()
{
    return std::make_shared<CesiumDownloadsAdapter>();
}

} // namespace mcp_qt::content_adapters
