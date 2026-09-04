#include "examples/multi_server_agent/AgentSession.h"
#include <mcp_qt_client/McpLogger.h>

#include <QJsonArray>
#include <QTimer>
#include <QJsonDocument>
#include <QDateTime>
#include <QPointer>
#include <iostream>

AgentSession::AgentSession(mcp_qt::McpHost* host,
                           std::shared_ptr<mcp_agent::ILlmBackend> llmBackend,
                           QObject* parent)
    : QObject(parent)
    , m_host(host)
    , m_llmBackend(llmBackend)
    , m_view(std::make_unique<mcp_qt::McpServerView>(host, this))
{
    m_executor = new mcp_agent::LlmAgentExecutor(m_llmBackend, this);

    // 工具调度：McpHost 自动解析 namespaced 名称并路由到对应 client
    m_executor->setToolDispatcher([this](const QString& toolName, const QJsonObject& args, std::function<void(mcp_qt::McpResult)> cb) {
        mcp_qt::McpLogger::info(QStringLiteral("Tool call: %1").arg(toolName), QStringLiteral("AgentSession"));
        m_host->callToolAsync(toolName, args, [this, toolName, cb](mcp_qt::McpResult result) {
            if (result.isError && m_host->reporter()) {
                m_host->reporter()->addError("tool/call", toolName + ": " + result.errorString);
            }
            cb(result);
        });
    });

    // MCP Apps 引用式：从工具声明的 ui.resourceUri 取回 HTML；不依赖工具结果私有字段。
    m_executor->setAppResourceFetcher([this](const QString& toolName, std::function<void(const QString& html, const QJsonObject& uiMeta, const QString& error)> cb) {
        const auto pair = m_host->toolRouter()->parseToolName(toolName);
        auto client = m_host->client(pair.first);
        if (!client) { cb({}, {}, QStringLiteral("no client for ") + pair.first); return; }

        QString resourceUri;
        for (const auto& t : client->cachedTools()) {
            if (t.name == pair.second) {
                resourceUri = t.meta.value(QStringLiteral("ui")).toObject().value(QStringLiteral("resourceUri")).toString();
                break;
            }
        }
        if (resourceUri.isEmpty()) {
            cb({}, {}, QStringLiteral("工具 %1 未声明 _meta.ui.resourceUri").arg(toolName));
            return;
        }

        client->readResourceAsync(resourceUri, [cb](const QJsonObject& result, const QString& error) {
            if (!error.isEmpty()) { cb({}, {}, error); return; }
            const QJsonArray contents = result.value(QStringLiteral("contents")).toArray();
            for (const auto& c : contents) {
                const QJsonObject o = c.toObject();
                if (o.value(QStringLiteral("mimeType")).toString().contains(QStringLiteral("mcp-app"))) {
                    const QJsonObject uiMeta = o.value(QStringLiteral("_meta")).toObject().value(QStringLiteral("ui")).toObject();
                    cb(o.value(QStringLiteral("text")).toString(), uiMeta, {});
                    return;
                }
            }
            cb({}, {}, QStringLiteral("ui 资源未包含 mcp-app 内容"));
        });
    });
}

// ============================================================================
// setServerFilter(): 限定该 agent 可见的服务器（McpServerView 视图裁剪，issue #9）
// ============================================================================
void AgentSession::setServerFilter(const QStringList& servers) {
    m_view->setVisibleServers(servers);
}

// ============================================================================
// start(): 启动任务
// ============================================================================
void AgentSession::start(const AgentRunOptions& options) {
    m_executor->setDiagnosticContext(options.apiUrl, options.apiKey, options.modelName);
    m_timeoutMs = options.timeoutMs;
    m_finished = false;
    m_taskStarted = false;

    // 因为 McpHost 已经在外部保证了准备就绪，所以直接启动 ReAct
    QTimer::singleShot(0, this, [this, task = options.task]() {
        if (!m_taskStarted) runTask(task);
    });
}

// ============================================================================
// runTask(): 直接从缓存读取工具，启动 ReAct（纯同步，零异步等待）
// ============================================================================
void AgentSession::runTask(const QString& task) {
    if (m_taskStarted) return;
    m_taskStarted = true;

    mcp_qt::McpLogger::info(QStringLiteral("runTask: %1").arg(task), QStringLiteral("AgentSession"));

    QJsonArray tools = m_view->exportAllToolsToLlmFormat();

    if (tools.isEmpty()) {
        finishWithError("tool/discovery", "No tools available", "Ensure MCP servers are online");
        return;
    }

    mcp_qt::McpLogger::info(QStringLiteral("启动 ReAct: %1 个工具（过滤后）").arg(tools.size()), QStringLiteral("AgentSession"));
    if (m_host->reporter()) m_host->reporter()->addInfo("tool/discovery", QStringLiteral("%1 tools loaded").arg(tools.size()));

    QPointer<AgentSession> safeThis(this);
    m_executor->run(task, tools, [safeThis](bool ok, QString answer) {
        if (!safeThis) return;
        ok ? safeThis->finishSuccessfully(answer) : safeThis->finishWithError("react/loop", answer, "Check ReAct step");
    });
}

// ============================================================================
// continueConversation(): 多轮对话（直接读缓存）
// ============================================================================
void AgentSession::continueConversation(const QString& task, const QString&) {
    m_finished = false;
    if (m_watchdogTimer) m_watchdogTimer->start(m_timeoutMs * 6);

    QJsonArray tools = m_view->exportAllToolsToLlmFormat();
    QPointer<AgentSession> safeThis(this);
    m_executor->continueRun(task, tools, [safeThis](bool ok, QString answer) {
        if (!safeThis) return;
        ok ? safeThis->finishSuccessfully(answer) : safeThis->finishWithError("react/loop", answer, "Check ReAct step");
    });
}

// ============================================================================
void AgentSession::finishWithError(const QString& stage, const QString& msg, const QString& sug) {
    if (m_finished) return;
    m_finished = true;
    if (m_watchdogTimer) m_watchdogTimer->stop();
    mcp_qt::McpLogger::warning(QStringLiteral("Error: %1").arg(msg), QStringLiteral("AgentSession"));
    if (m_host->reporter()) m_host->reporter()->addError(stage, msg, sug);
    emit finished(1);
}

void AgentSession::finishSuccessfully(const QString& msg) {
    if (m_finished) return;
    m_finished = true;
    if (m_watchdogTimer) m_watchdogTimer->stop();
    mcp_qt::McpLogger::info(QStringLiteral("完成: %1").arg(msg), QStringLiteral("AgentSession"));
    if (m_host->reporter()) {
        m_host->reporter()->addExecutionLogLine(msg);
        m_host->reporter()->addInfo("result/render", msg);
    }
    emit finished(0);
}
