#pragma once

#include <QMainWindow>
#include <QLineEdit>
#include <QTextEdit>
#include <QPushButton>
#include <QComboBox>
#include <QListWidget>
#include <QLabel>
#include <QSplitter>
#include <memory>
#include "AgentSession.h"
#include "mcp_qt_client/McpHost.h"
#include "mcp_qt_apps/McpAppWebView2Renderer.h"
#include "mcp_qt_apps/McpAppBridge.h"

namespace mcp_agent {

class AgentMainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit AgentMainWindow(QWidget* parent = nullptr);
    ~AgentMainWindow() override = default;

private slots:
    void handleBrowseConfig();
    void handleBrowseLogFile();
    void handleModeChanged(int index);
    void handleRunTask();
    void handleStepProgress(const QString& type, const QString& content);
    void handleMcpAppContent(const QString& html, const QString& toolName);
    void handleSessionFinished(int exitCode);
    void handleFetchModels();
    void handleResetSession();
    void handleServerDoubleClicked(QListWidgetItem* item);

private:
    void initUi();
    void applyTheme();
    void updateServerList();
    void appendLogHtml(const QString& html);
    void loadAndConnectServers(const QString& configPath);
    void setupMcpAppRenderer();
    void showMcpAppPanel(bool visible);

    // 控件定义
    QLineEdit* m_configPathEdit{nullptr};
    QPushButton* m_browseBtn{nullptr};
    QPushButton* m_refreshBtn{nullptr};
    QLineEdit* m_logPathEdit{nullptr};
    QPushButton* m_logBrowseBtn{nullptr};
    
    QComboBox* m_modeCombo{nullptr};
    QLineEdit* m_apiUrlEdit{nullptr};
    QLineEdit* m_apiKeyEdit{nullptr};
    QComboBox* m_modelCombo{nullptr};
    QPushButton* m_fetchModelsBtn{nullptr};
    
    QListWidget* m_serverListWidget{nullptr};
    QTextEdit* m_serverLogConsole{nullptr};  // 服务端 stderr 日志面板
    QTextEdit* m_logBlackboard{nullptr};

    QLineEdit* m_taskInputEdit{nullptr};
    QPushButton* m_runBtn{nullptr};
    QPushButton* m_resetSessionBtn{nullptr}; // 🌟 新增：重置/新建对话按钮

    // MCP Apps 渲染（内嵌 WebView2）
    mcp_qt::McpAppWebView2Renderer* m_mcpAppRenderer{nullptr};
    std::shared_ptr<mcp_qt::McpAppBridge> m_mcpAppBridge;
    QSplitter* m_rightSplitter{nullptr};   // 右列：ReAct 看板 + MCP App 分屏
    QWidget* m_mcpAppContainer{nullptr};   // MCP App 渲染容器（带标题栏）
    bool m_mcpAppVisible{false};

    // 运行时成员
    mcp_qt::McpHost* m_host{nullptr};
    std::shared_ptr<ILlmBackend> m_llmBackend;
    AgentSession* m_session{nullptr};
    QNetworkAccessManager* m_network{nullptr};

    bool m_isRunning{false};
    bool m_sessionActive{false}; // 🌟 新增：会话是否保持活跃状态
};

} // namespace mcp_agent

extern QString g_logFilePath;
