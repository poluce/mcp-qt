#pragma once

#include <QMainWindow>
#include <QLineEdit>
#include <QTextEdit>
#include <QPushButton>
#include <QComboBox>
#include <QListWidget>
#include <QLabel>
#include <QSplitter>
#include <QTabWidget>
#include <QStackedWidget>
#include <QMap>
#include <QSet>
#include <memory>
#include "AgentSession.h"
#include "AgentRegistry.h"
#include "AgentReconciler.h"
#include "mcp_qt_client/McpHost.h"
#include "mcp_qt_apps/McpAppWebView2Renderer.h"
#include "mcp_qt_apps/McpAppBridge.h"

namespace mcp_agent {

class AgentMainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit AgentMainWindow(QWidget* parent = nullptr, const QString& initialConfigPath = {});
    ~AgentMainWindow() override = default;

    /// 外部注入任务：模拟用户在输入框输入文本并点击「发送」。
    /// 可在任意时机调用（服务器就绪后自动执行，未就绪则排队等待 hostReady）。
    void submitTask(const QString& task);
    /// 自动化：MCP App 渲染后把弹窗截图保存到该路径（并退出）
    void setScreenshotPath(const QString& path) { m_screenshotPath = path; }
    void setAutomatedToolCall(const QString& toolName, const QJsonObject& arguments);

private slots:
    void handleBrowseConfig();
    void handleBrowseLogFile();
    void handleModeChanged(int index);
    void handleRunTask();
    void handleStepProgress(QTextEdit* blackboard, const QString& type, const QString& content);
    void handleMcpAppContent(const QString& html, const QString& toolName,
                             const QJsonObject& uiMeta, const QJsonObject& toolInput,
                             const QJsonObject& toolResult);
    void handleSessionFinished(const QString& agent, int exitCode);
    void handleFetchModels();
    void handleResetSession();
    void handleServerDoubleClicked(QListWidgetItem* item);

private:
    void initUi();
    void applyTheme();
    void updateAllServerList();          // 刷新「MCP 总服务」tab（全部配置服务器 + 状态）
    void updateCurrentAgentMcpCombo();   // 刷新当前 agent 的 MCP 下拉
    void refreshControlState();          // 按当前 agent 状态刷新按钮/输入/配置区
    AgentSession* sessionForAgent(const QString& agent);   // 取或建该 agent 的会话
    QTextEdit* blackboardForAgent(const QString& agent);   // 取或建该 agent 的看板
    void showAgentBlackboard(const QString& agent);        // 切换到该 agent 的看板页
    void appendLogHtml(const QString& html);               // 追加到当前 agent 看板
    void loadAndConnectServers(const QString& configPath);
    void setupMcpAppRenderer();
    void showMcpAppPanel(bool visible);
    void maybeRunPendingTask();   // 待办任务在 host 就绪后自动填入输入框并发送

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
    
    QTabWidget* m_leftTabs{nullptr};     // 左侧：标签页（服务端日志 / MCP 总服务）
    QListWidget* m_allServerList{nullptr}; // 「MCP 总服务」tab：全部配置的服务器 + 状态
    QTextEdit* m_serverLogConsole{nullptr};  // 服务端 stderr 日志面板

    // 每 agent 一个看板 + 会话（切换 agent 时切换看板，状态各自保留，后台任务不中断）
    QStackedWidget* m_blackboardStack{nullptr};
    QMap<QString, QTextEdit*> m_agentBlackboards;
    QMap<QString, AgentSession*> m_agentSessions;
    QString m_currentAgent;
    QSet<QString> m_runningAgents;        // 正在跑任务的 agent
    QSet<QString> m_activeSessionAgents;  // 处于多轮活跃的 agent

    QLineEdit* m_taskInputEdit{nullptr};
    QPushButton* m_runBtn{nullptr};
    QPushButton* m_resetSessionBtn{nullptr}; // 🌟 新增：重置/新建对话按钮
    QComboBox* m_agentCombo{nullptr};        // 多 agent：选择运行哪个 agent（决定其可见工具面）
    QComboBox* m_currentAgentMcpCombo{nullptr}; // 当前 agent 的 MCP 下拉（随 agent 切换联动）

    // MCP Apps 渲染（内嵌 WebView2）
    mcp_qt::McpAppWebView2Renderer* m_mcpAppRenderer{nullptr};
    std::shared_ptr<mcp_qt::McpAppBridge> m_mcpAppBridge;
    QDialog* m_mcpAppDialog{nullptr};     // MCP App 弹窗（比内嵌面板大、可自由缩放）
    QSplitter* m_rightSplitter{nullptr};   // 右列：ReAct 看板 + MCP App 分屏
    QWidget* m_mcpAppContainer{nullptr};   // MCP App 渲染容器（带标题栏）
    bool m_mcpAppVisible{false};

    // 运行时成员
    mcp_qt::McpHost* m_host{nullptr};
    AgentRegistry* m_registry{nullptr};      // 多 agent：agent → 服务器集合
    AgentReconciler* m_reconciler{nullptr};  // 期望态对账：注册表 → McpHost 启用集合
    std::shared_ptr<ILlmBackend> m_llmBackend;
    QNetworkAccessManager* m_network{nullptr};

    QString m_pendingTask;      // submitTask 注入的待办任务（host 就绪后自动发送）
    bool m_hostReady{false};    // McpHost 是否已就绪（hostReady 回调置位）
    QString m_screenshotPath;   // --screenshot 传入的截图保存路径
    QString m_automatedToolName;
    QJsonObject m_automatedToolArguments;
};

} // namespace mcp_agent

extern QString g_logFilePath;
