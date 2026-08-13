#include "tests/common.h"
#include "mcp_qt_apps/McpAppContentAdapter.h"
#include "mcp_qt_apps/McpAppCompatibility.h"
#include "mcp_qt_apps/McpAppSupport.h"

#include <QJsonArray>

namespace {

class MarkerAdapter final : public mcp_qt::IMcpAppContentAdapter {
public:
    QString id() const override { return QStringLiteral("marker"); }
    bool matches(const mcp_qt::McpAppContent& content) const override
    {
        return content.html.contains(QStringLiteral("data-adapt"));
    }
    void adapt(mcp_qt::McpAppContent& content) const override
    {
        content.html.replace(QStringLiteral("data-adapt"), QStringLiteral("adapted"));
    }
};

} // namespace

void test_mcp_app_content_adapter_registry_is_generic()
{
    mcp_qt::McpAppContentAdapterRegistry registry;
    registry.add(std::make_shared<MarkerAdapter>());

    const mcp_qt::McpAppContent untouched = registry.adapt({
        QStringLiteral("<html>plain</html>"), QJsonObject{}, QUrl{}, {}});
    TM_ASSERT_TRUE(untouched.html == QStringLiteral("<html>plain</html>"),
                   "non-matching content must remain unchanged");
    TM_ASSERT_TRUE(untouched.appliedAdapters.isEmpty(),
                   "non-matching adapter must not be reported");

    const mcp_qt::McpAppContent adapted = registry.adapt({
        QStringLiteral("<html data-adapt></html>"), QJsonObject{}, QUrl{}, {}});
    TM_ASSERT_TRUE(adapted.html.contains(QStringLiteral("adapted")),
                   "registered adapter should transform matching content");
    TM_ASSERT_TRUE(adapted.appliedAdapters == QStringList{QStringLiteral("marker")},
                   "registry should report applied adapter ids");
}

void test_mcp_app_cesium_adapter_is_isolated_and_declares_csp()
{
    auto registry = mcp_qt::createStandardMcpAppCompatibilityAdapters();
    const QString sourceBase = QStringLiteral(
        "https://cesium.com/downloads/cesiumjs/releases/1.123/Build/Cesium");
    mcp_qt::McpAppContent content{
        QStringLiteral("<!doctype html><html><head></head><body><script src=\"%1/Cesium.js\"></script></body></html>")
            .arg(sourceBase),
        QJsonObject{{QStringLiteral("csp"), QJsonObject{
            {QStringLiteral("resourceDomains"), QJsonArray{QStringLiteral("https://cesium.com")}}
        }}},
        QUrl{}, {}};

    const mcp_qt::McpAppContent adapted = registry->adapt(content);
    const QString mirrorBase = QStringLiteral(
        "https://cdn.jsdelivr.net/npm/cesium@1.123/Build/Cesium");
    TM_ASSERT_TRUE(!adapted.html.contains(sourceBase),
                   "Cesium downloads URL should be removed from adapted HTML");
    TM_ASSERT_TRUE(adapted.html.contains(mirrorBase + QStringLiteral("/Cesium.js")),
                   "Cesium assets should use the CORS-enabled mirror");
    TM_ASSERT_TRUE(adapted.html.contains(
        QStringLiteral("window.CESIUM_BASE_URL=\"") + mirrorBase + QStringLiteral("\"")),
        "Cesium base URL must be bootstrapped before its module executes");
    TM_ASSERT_TRUE(adapted.appliedAdapters.contains(QStringLiteral("cesium-downloads-cors")),
                   "Cesium compatibility must be attributable to an isolated adapter");

    const QJsonArray domains = adapted.uiMeta.value(QStringLiteral("csp")).toObject()
                                   .value(QStringLiteral("resourceDomains")).toArray();
    TM_ASSERT_TRUE(domains.contains(QStringLiteral("https://cdn.jsdelivr.net")),
                   "adapter-added mirror must be explicitly declared in CSP metadata");
    const QString csp = mcp_qt::McpAppSupport::buildCsp(adapted.uiMeta);
    TM_ASSERT_TRUE(csp.contains(QStringLiteral("https://cdn.jsdelivr.net")),
                   "effective CSP must authorize the adapter-added mirror");

    const mcp_qt::McpAppContent plain = registry->adapt({
        QStringLiteral("<html><script src=\"https://example.com/app.js\"></script></html>"),
        QJsonObject{}, QUrl{}, {}});
    TM_ASSERT_TRUE(plain.appliedAdapters.isEmpty(),
                   "unrelated MCP Apps must not be changed by the Cesium adapter");
    TM_ASSERT_TRUE(plain.html.contains(QStringLiteral("https://example.com/app.js")),
                   "unrelated MCP App HTML must remain intact");
}
