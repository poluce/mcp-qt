#pragma once
#include <QObject>
#include <QList>
#include <QStringList>
#include "mcp_core/McpTool.h"

/**
 * @brief Manages the list of tools discovered from the MCP Server.
 * 
 * Exposes helper methods to query descriptions and metadata for UI display.
 */
class ToolManager : public QObject {
    Q_OBJECT
public:
    explicit ToolManager(QObject* parent = nullptr);
    ~ToolManager() override = default;

    void updateTools(const QList<mcp::McpTool>& tools);
    QStringList toolNames() const;
    QString toolDescription(const QString& name) const;
    mcp::McpTool getTool(const QString& name) const;

signals:
    void toolsChanged();

private:
    QList<mcp::McpTool> m_tools;
};
