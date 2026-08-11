#include "LlmAgentExecutor.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <iostream>
#include <QDateTime>
#include "mcp_qt_apps/McpAppSupport.h"

namespace mcp_agent {

LlmAgentExecutor::LlmAgentExecutor(std::shared_ptr<ILlmBackend> backend, QObject* parent)
    : QObject(parent), m_backend(backend) {}

void LlmAgentExecutor::setToolDispatcher(
    std::function<void(const QString& name, const QJsonObject& args, std::function<void(mcp_qt::McpResult)> callback)> dispatcher
) {
    m_toolDispatcher = dispatcher;
}

void LlmAgentExecutor::setDiagnosticContext(const QString& apiUrl, const QString& apiKey, const QString& modelName) {
    m_diagApiUrl = apiUrl;
    m_diagApiKey = apiKey;
    m_diagModelName = modelName;
}

void LlmAgentExecutor::run(
    const QString& task,
    const QJsonArray& availableTools,
    std::function<void(bool success, QString finalAnswer)> onFinish
) {
    m_currentStep = 0;
    m_history.clear();

    // 预置系统提示词，描述 ReAct 的工作流程
    LlmMessage sysMsg;
    sysMsg.role = "system";
    sysMsg.content = "You are a smart assistant. Resolve the user's task step-by-step by utilizing available tools. Follow the ReAct pattern: Thought -> Act -> Observe -> Output Answer.";
    m_history.append(sysMsg);

    // 用户 Task
    LlmMessage userMsg;
    userMsg.role = "user";
    userMsg.content = task;
    m_history.append(userMsg);

    qInfo() << "=========================================================";
    qInfo() << "Starting ReAct loop for task:" << task;
    qInfo() << "=========================================================";

    nextStep(availableTools, onFinish);
}

void LlmAgentExecutor::continueRun(
    const QString& task,
    const QJsonArray& availableTools,
    std::function<void(bool success, QString finalAnswer)> onFinish
) {
    m_currentStep = 0;

    LlmMessage userMsg;
    userMsg.role = "user";
    userMsg.content = task;
    m_history.append(userMsg);

    qInfo() << "=========================================================";
    qInfo() << "Continuing ReAct loop for multi-turn task:" << task;
    qInfo() << "=========================================================";

    nextStep(availableTools, onFinish);
}

void LlmAgentExecutor::nextStep(
    const QJsonArray& availableTools,
    std::function<void(bool success, QString finalAnswer)> onFinish
) {
    if (m_currentStep >= m_maxSteps) {
        onFinish(false, QString("Exceeded maximum ReAct loop steps (%1 steps limit)").arg(m_maxSteps));
        return;
    }

    m_currentStep++;
    qInfo() << QString("[ReAct Loop] Step %1/%2: Requesting decision from LLM...").arg(m_currentStep).arg(m_maxSteps).toStdString().c_str();

    m_backend->requestDecision(m_history, availableTools, [this, availableTools, onFinish](bool success, LlmDecision dec, QString err) {
        if (!success) {
            QString detailedErr = QString(
                "大模型请求失败: %1\n"
                "【排查状态信息】\n"
                " - 发生时间: %2\n"
                " - 当前流程阶段: [LLM_请求决策]\n"
                " - 目标接口 URL: %3\n"
                " - 推理大模型名: %4\n"
                " - API 密钥状态: %5\n"
                " - 对话历史轮数: %6 轮交互\n"
                " - 诊断排查建议: 请检查您的网络连接与代理，确保接口能通；如使用在线大模型，请确保 API Key 正确且额度充足。"
            ).arg(err)
             .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz"))
             .arg(m_diagApiUrl.isEmpty() ? QStringLiteral("Offline Mock") : m_diagApiUrl)
             .arg(m_diagModelName.isEmpty() ? QStringLiteral("Mock-Engine") : m_diagModelName)
             .arg(m_diagApiKey.isEmpty() ? QStringLiteral("未配置(使用环境变量或离线)") : QStringLiteral("已配置(长度: %1)").arg(m_diagApiKey.length()))
             .arg(m_history.size());
            
            onFinish(false, detailedErr);
            return;
        }

        qInfo().noquote() << "[Thought]" << dec.thought;
        emit stepProgress(QStringLiteral("thought"), dec.thought);

        // 保存大模型在这一步的思考输出
        LlmMessage assistantMsg;
        assistantMsg.role = "assistant";
        assistantMsg.content = dec.thought;
        assistantMsg.reasoningContent = dec.reasoningContent;
        if (dec.isToolCall) {
            assistantMsg.toolCallId = dec.toolCallId;
            assistantMsg.toolName = dec.toolName;
            assistantMsg.toolArguments = dec.toolArguments;
        }
        m_history.append(assistantMsg);

        if (dec.isToolCall) {
            qInfo().noquote() << "[Act] Call tool:" << dec.toolName 
                              << "with arguments:" 
                              << QJsonDocument(dec.toolArguments).toJson(QJsonDocument::Compact);
            emit stepProgress(QStringLiteral("act"), QStringLiteral("Call tool: %1 with args: %2").arg(dec.toolName, QJsonDocument(dec.toolArguments).toJson(QJsonDocument::Compact)));

            if (!m_toolDispatcher) {
                onFinish(false, "Fatal Error: Tool dispatcher callback is not set");
                return;
            }

            m_toolDispatcher(dec.toolName, dec.toolArguments, [this, dec, availableTools, onFinish](mcp_qt::McpResult res) {
                // MCP Apps：工具返回 text/html;profile=mcp-app 内容 → 交给宿主内嵌渲染
                const QString appMime = QString::fromUtf8(mcp_qt::kMcpAppMimeType);
                for (const auto& c : res.contents) {
                    if (c.mimeType == appMime && !c.text.isEmpty()) {
                        emit mcpAppContentAvailable(c.text, dec.toolName);
                        break;
                    }
                }

                QString obs;
                if (res.isError) {
                    obs = "Error: " + res.errorString;
                } else {
                    obs = QJsonDocument(res.data).toJson(QJsonDocument::Compact);
                }

                qInfo().noquote() << "[Observation]" << obs;
                emit stepProgress(QStringLiteral("observation"), obs);

                // 将工具调用获得的观测结果 Observation 加入历史，以作为大模型下一步自反思的输入
                LlmMessage toolMsg;
                toolMsg.role = "tool";
                toolMsg.content = obs;
                toolMsg.toolCallId = dec.toolCallId; // 🌟 必须将大模型生成的相同 ID 绑定回去！
                toolMsg.name = dec.toolName;
                m_history.append(toolMsg);

                // 递归进行下一个 ReAct Step
                nextStep(availableTools, onFinish);
            });
        } else {
            // 大模型宣告找到最终答案
            qInfo().noquote() << "[Final Answer]" << dec.finalAnswer;
            emit stepProgress(QStringLiteral("answer"), dec.finalAnswer);
            onFinish(true, dec.finalAnswer);
        }
    });
}

} // namespace mcp_agent
