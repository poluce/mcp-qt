#include "mcp_qt/QtMcpToolBridge.h"

namespace mcp {

QtMcpToolBridge::QtMcpToolBridge(QObject* parent)
    : QObject(parent) {}

QJsonObject QtMcpToolBridge::toJson(const nlohmann::json& j) {
    std::string s = j.dump();
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(s));
    if (doc.isObject()) {
        return doc.object();
    }
    return QJsonObject();
}

nlohmann::json QtMcpToolBridge::fromJson(const QJsonObject& qj) {
    QJsonDocument doc(qj);
    std::string s = doc.toJson(QJsonDocument::Compact).toStdString();
    try {
        return nlohmann::json::parse(s);
    } catch (...) {
        return nlohmann::json::object();
    }
}

McpTool QtMcpToolBridge::toMcpTool(const QString& name, const QString& description, const QJsonObject& schema) {
    McpTool tool;
    tool.name = name.toStdString();
    tool.description = description.toStdString();
    tool.inputSchema = fromJson(schema);
    return tool;
}

} // namespace mcp
