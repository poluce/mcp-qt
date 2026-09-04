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
     * @brief 设置 MCP Apps 引用式资源获取器。
     *
     * 部分 MCP Apps 服务器（如 ext-apps 示例）工具结果只返回 _meta.viewUUID 引用，
     * HTML 在工具的 _meta.ui.resourceUri（ui:// 资源）。宿主需据此 resources/read 取回并渲染。
     */
    using AppResourceFetcher = std::function<void(const QString& toolName, std::function<void(const QString& html, const QJsonObject& uiMeta, const QString& error)> cb)>;
    void setAppResourceFetcher(AppResourceFetcher fetcher);

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
    /// uiMeta = 资源的 _meta.ui（含 csp/permissions），宿主据此构建沙箱 CSP。
    /// toolInput/toolResult 必须在 App initialized 后转发，驱动 View 展示本次工具调用的实际状态。
    void mcpAppContentAvailable(const QString& html, const QString& toolName,
                                const QJsonObject& uiMeta, const QJsonObject& toolInput,
                                const QJsonObject& toolResult);

private:
    void nextStep(
        const QJsonArray& availableTools,
        std::function<void(bool success, QString finalAnswer)> onFinish
    );

    std::shared_ptr<ILlmBackend> m_backend;
    QList<LlmMessage> m_history;
    std::function<void(const QString& name, const QJsonObject& args, std::function<void(mcp_qt::McpResult)>)> m_toolDispatcher;
    AppResourceFetcher m_appResourceFetcher;

    int m_maxSteps{200};
    int m_currentStep{0};

    QString m_diagApiUrl;
    QString m_diagApiKey;
    QString m_diagModelName;
};

} // namespace mcp_agent
