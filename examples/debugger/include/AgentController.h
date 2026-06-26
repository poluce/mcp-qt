#pragma once
#include <QObject>
#include <memory>
#include "mcp_qt/QtMcpClient.h"
#include "ToolManager.h"

/**
 * @brief Coordinates the connection flow, initialization, and tool execution.
 * 
 * Intercepts signals from QtMcpClient and routes data to ToolManager and UI loggers.
 */
class AgentController : public QObject {
    Q_OBJECT
public:
    explicit AgentController(ToolManager* toolManager, QObject* parent = nullptr);
    ~AgentController() override;

    void connectToStdioServer(const QString& program, const QStringList& arguments = {});
    void connectToHttpServer(const QString& sseUrl);

    mcp::QtMcpClient* mcpClient() const { return m_client; }

signals:
    void logMessage(const QString& msg);
    void statusChanged(const QString& status);
    void toolExecutionResult(const QString& result);

private slots:
    void onConnectionOpened();
    void onInitialized(bool success, const QString& info);
    void onToolsListed(const QList<mcp::McpTool>& tools, const QString& error);
    void onToolCalled(const QString& toolName, const QString& resultJson, const QString& error);
    void onShutdownCompleted(bool success);
    void onResourcesListed(const QString& resultJson, const QString& error);
    void onResourceRead(const QString& uri, const QString& resultJson, const QString& error);
    void onPromptsListed(const QString& resultJson, const QString& error);
    void onPromptGot(const QString& promptName, const QString& resultJson, const QString& error);
    void onErrorOccurred(const QString& errorMessage);
    void onDisconnected();

private:
    ToolManager* m_toolManager;
    mcp::QtMcpClient* m_client = nullptr;
};
