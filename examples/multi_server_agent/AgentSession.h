#pragma once

#include "ILlmBackend.h"
#include "LlmAgentExecutor.h"
#include "mcp_qt_client/McpHost.h"

#include <QObject>
#include <QJsonObject>
#include <memory>

struct AgentRunOptions {
    QString configPath;
    QString task;
    QString serverFilter;
    int timeoutMs{30000};

    // LLM 配置
    bool useRealLlm{false};
    QString apiUrl;
    QString apiKey;
    QString modelName;
};

class AgentSession : public QObject {
    Q_OBJECT

public:
    AgentSession(mcp_qt::McpHost* host,
                 std::shared_ptr<mcp_agent::ILlmBackend> llmBackend,
                 QObject* parent = nullptr);

    void start(const AgentRunOptions& options);
    void continueConversation(const QString& task, const QString& serverFilter = "");
    mcp_agent::LlmAgentExecutor* executor() const { return m_executor; }

    /// 设置该 agent 可见的服务器集合（空 = 全量）。用于多 agent 场景按服务器隔离工具面。
    void setServerFilter(const QStringList& servers);

signals:
    void finished(int exitCode);

private:
    void runTask(const QString& task);
    void finishWithError(const QString& stage, const QString& message, const QString& suggestion = QString());
    void finishSuccessfully(const QString& message);

    mcp_qt::McpHost* m_host{nullptr};
    std::shared_ptr<mcp_agent::ILlmBackend> m_llmBackend;

    mcp_agent::LlmAgentExecutor* m_executor{nullptr};

    QStringList m_serverFilter;  // 该 agent 可见的服务器集合（serverName_ 前缀白名单）

    int m_timeoutMs{30000};
    bool m_finished{false};
    bool m_taskStarted{false};
    QTimer* m_watchdogTimer{nullptr};
};
