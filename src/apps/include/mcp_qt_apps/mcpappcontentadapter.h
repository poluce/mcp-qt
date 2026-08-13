#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <memory>
#include <vector>

namespace mcp_qt {

/**
 * Content passed through host-owned compatibility adapters before rendering.
 * Adapters may rewrite HTML and extend uiMeta, but must not weaken renderer
 * sandboxing directly.
 */
struct McpAppContent {
    QString html;
    QJsonObject uiMeta;
    QUrl baseUrl;
    QStringList appliedAdapters;
};

/**
 * Optional compatibility adapter for a specific MCP App/runtime combination.
 * The WebView renderer itself deliberately contains no App-specific rules.
 */
class IMcpAppContentAdapter {
public:
    virtual ~IMcpAppContentAdapter() = default;
    virtual QString id() const = 0;
    virtual bool matches(const McpAppContent& content) const = 0;
    virtual void adapt(McpAppContent& content) const = 0;
};

/** Ordered registry. Applications can add adapters without changing renderer code. */
class McpAppContentAdapterRegistry {
public:
    void add(std::shared_ptr<IMcpAppContentAdapter> adapter);
    McpAppContent adapt(McpAppContent content) const;

private:
    std::vector<std::shared_ptr<IMcpAppContentAdapter>> m_adapters;
};

} // namespace mcp_qt
