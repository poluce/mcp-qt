#pragma once

#include "ILlmBackend.h"
#include "LlmAgentExecutor.h"
#include "mcp_qt_client/McpHost.h"
#include "mcp_qt_client/McpServerView.h"

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
    /// 底层连接仍全局共享（McpHost 统一注册），仅视图裁剪可见性（issue #9）。
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

    // 该 agent 的服务器视图：按可见服务器裁剪工具/提示词/资源（issue #9）
    std::unique_ptr<mcp_qt::McpServerView> m_view;

    int m_timeoutMs{30000};
    bool m_finished{false};
    bool m_taskStarted{false};
    QTimer* m_watchdogTimer{nullptr};
};
