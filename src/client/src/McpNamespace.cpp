#include "mcp_qt_client/McpNamespace.h"

namespace mcp_qt {

QPair<QString, QString> McpNamespace::parseNamespacedName(const QStringList& serverNames, const QString& namespaced) {
    for (const QString& serverName : serverNames) {
        const QString prefix = serverName + QStringLiteral("_");
        if (namespaced.startsWith(prefix)) {
            return {serverName, namespaced.mid(prefix.length())};
        }
    }
    return {};
}

QPair<QString, QString> McpNamespace::parseNamespacedUri(const QStringList& serverNames, const QString& namespacedUri) {
    for (const QString& serverName : serverNames) {
        const QString prefix = QStringLiteral("mcp-") + serverName + QStringLiteral("-");
        if (namespacedUri.startsWith(prefix)) {
            return {serverName, namespacedUri.mid(prefix.length())};
        }
    }
    return {};
}

} // namespace mcp_qt
