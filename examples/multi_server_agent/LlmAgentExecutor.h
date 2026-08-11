#pragma once

#include "ILlmBackend.h"
#include <mcp_qt_client/McpQtClient.h>
#include <QObject>
#include <QList>
#include <memory>

namespace mcp_agent {

class LlmAgentExecutor : public QObject {
    Q_OBJECT
public:
    explicit LlmAgentExecutor(std::shared_ptr<ILlmBackend> backend, QObject* parent = nullptr);
    ~LlmAgentExecutor() override = default;

    /**
     * @brief 设置工具调度器（负责分发并真正执行外部 MCP 客户端的 Tool 调用）
     */
    void setToolDispatcher(
        std::function<void(const QString& name, const QJsonObject& args, std::function<void(mcp_qt::McpResult)> callback)> dispatcher
    );

    /**
     * @brief 设置诊断上下文信息（仅在发生错误时组装详细排查日志使用）
     */
    void setDiagnosticContext(const QString& apiUrl, const QString& apiKey, const QString& modelName);

    /**
     * @brief 开始运行 ReAct 任务环路
     * 
     * @param task 用户指令
     * @param availableTools 从各大 MCP 客户端获取并整合的可用工具 Schema 列表
     * @param onFinish 最终完成或失败时的回调
     */
    void run(
        const QString& task,
        const QJsonArray& availableTools,
        std::function<void(bool success, QString finalAnswer)> onFinish
    );

    void continueRun(
        const QString& task,
        const QJsonArray& availableTools,
        std::function<void(bool success, QString finalAnswer)> onFinish
    );

signals:
    void stepProgress(const QString& type, const QString& content);
    /// 工具返回 MCP Apps UI 内容（text/html;profile=mcp-app），供宿主内嵌 WebView2 渲染。
    void mcpAppContentAvailable(const QString& html, const QString& toolName);

private:
    void nextStep(
        const QJsonArray& availableTools,
        std::function<void(bool success, QString finalAnswer)> onFinish
    );

    std::shared_ptr<ILlmBackend> m_backend;
    QList<LlmMessage> m_history;
    std::function<void(const QString& name, const QJsonObject& args, std::function<void(mcp_qt::McpResult)>)> m_toolDispatcher;

    int m_maxSteps{200};
    int m_currentStep{0};

    QString m_diagApiUrl;
    QString m_diagApiKey;
    QString m_diagModelName;
};

} // namespace mcp_agent
