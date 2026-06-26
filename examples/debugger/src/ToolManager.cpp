#include "ToolManager.h"

ToolManager::ToolManager(QObject* parent)
    : QObject(parent) {}

void ToolManager::updateTools(const QList<mcp::McpTool>& tools) {
    m_tools = tools;
    emit toolsChanged();
}

QStringList ToolManager::toolNames() const {
    QStringList names;
    for (const auto& tool : m_tools) {
        names.append(QString::fromStdString(tool.name));
    }
    return names;
}

QString ToolManager::toolDescription(const QString& name) const {
    for (const auto& tool : m_tools) {
        if (QString::fromStdString(tool.name) == name) {
            return QString::fromStdString(tool.description);
        }
    }
    return QString();
}

mcp::McpTool ToolManager::getTool(const QString& name) const {
    for (const auto& tool : m_tools) {
        if (QString::fromStdString(tool.name) == name) {
            return tool;
        }
    }
    return mcp::McpTool();
}
