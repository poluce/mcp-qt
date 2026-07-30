#pragma once
#include <QString>
#include <QStringList>
#include <QMap>

namespace mcp_qt {

struct McpServerConfig {
    QString serverName;
    bool disabled{false};
    
    // For Stdio
    QString command;
    QStringList args;
    
    // For HTTP/SSE
    QString url;
    QString type; // "http" or "stateless_http"
    QString nameSpace;
    
    // Common
    QMap<QString, QString> env;
    QMap<QString, QString> headers;
    QString protocolVersion;
};

} // namespace mcp_qt
