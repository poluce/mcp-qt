#include "LlmBackends.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QDebug>
#include <iostream>

namespace mcp_agent {

// ==========================================
// MockLlmBackend
// ==========================================

void MockLlmBackend::requestDecision(
    const QList<LlmMessage>& history,
    const QJsonArray& availableTools,
    std::function<void(bool success, LlmDecision decision, QString err)> callback
) {
    Q_UNUSED(availableTools);

    // 延迟 500ms 模拟大模型思考延迟
    QTimer::singleShot(500, [history, callback]() {
        // 查找最初的用户 Task
        QString task;
        for (const auto& msg : history) {
            if (msg.role == "user") {
                task = msg.content;
                break;
            }
        }

        LlmDecision dec;

        // 0. MCP Apps 演示：任务含 dashboard / mcp app → 调用 show_dashboard 触发界面渲染
        if (task.contains("dashboard", Qt::CaseInsensitive) || task.contains("mcp app", Qt::CaseInsensitive)) {
            int toolMsgCount = 0;
            for (const auto& msg : history) {
                if (msg.role == "tool") toolMsgCount++;
            }
            if (toolMsgCount == 0) {
                dec.thought = "User wants to see the MCP Apps dashboard. I will call 'mcp-apps-mock_show_dashboard' tool.";
                dec.isToolCall = true;
                dec.toolName = "mcp-apps-mock_show_dashboard";
                dec.toolArguments["region"] = "华东-1";
            } else {
                dec.thought = "Dashboard has been rendered. Outputting final answer.";
                dec.isToolCall = false;
                dec.finalAnswer = "【Mock ReAct】已渲染 MCP Apps 仪表盘。";
            }
            callback(true, dec, "");
            return;
        }

        // 1. 如果任务是常规搜索且不需要截图
        if (task.contains("search", Qt::CaseInsensitive) && !task.contains("screenshot", Qt::CaseInsensitive)) {
            // 查看历史中有几条工具返回消息
            int toolMsgCount = 0;
            LlmMessage lastToolMsg;
            for (const auto& msg : history) {
                if (msg.role == "tool") {
                    toolMsgCount++;
                    lastToolMsg = msg;
                }
            }

            if (toolMsgCount == 0) {
                dec.thought = "User wants to search items. I will call 'mock-search-server_search_items' tool with query 'test'.";
                dec.isToolCall = true;
                dec.toolName = "mock-search-server_search_items";
                dec.toolArguments["query"] = "test";
            } else {
                dec.thought = "Search is completed. I have obtained the search results from tool. I will output final answer.";
                dec.isToolCall = false;
                dec.finalAnswer = QString("【Mock ReAct】已为您搜索到结果：%1").arg(lastToolMsg.content);
            }
            callback(true, dec, "");
            return;
        }

        // 2. 如果任务是复杂的链式任务（搜索并截图）
        if (task.contains("search", Qt::CaseInsensitive) && task.contains("screenshot", Qt::CaseInsensitive)) {
            QList<LlmMessage> toolMessages;
            for (const auto& msg : history) {
                if (msg.role == "tool") {
                    toolMessages.append(msg);
                }
            }

            if (toolMessages.size() == 0) {
                dec.thought = "Step 1: First, search for AI news using search_items tool.";
                dec.isToolCall = true;
                dec.toolName = "mock-search-server_search_items";
                dec.toolArguments["query"] = "AI news";
            } else if (toolMessages.size() == 1) {
                dec.thought = QString("Step 2: Search succeeded: '%1'. Now take a browser screenshot to capture the page.").arg(toolMessages[0].content);
                dec.isToolCall = true;
                dec.toolName = "browser_take_screenshot";
                dec.toolArguments["filename"] = "ai_news_result.png";
                dec.toolArguments["type"] = "png";
                dec.toolArguments["scale"] = "css";
            } else {
                dec.thought = "Step 3: Screenshot saved successfully. Both actions are completed. Outputting final answer.";
                dec.isToolCall = false;
                dec.finalAnswer = QString("【Mock ReAct 链式执行结果】\n1. 搜索数据：%1\n2. 网页截图状态：%2\n任务已圆满完成！")
                                  .arg(toolMessages[0].content)
                                  .arg(toolMessages[1].content);
            }
            callback(true, dec, "");
            return;
        }

        // 3. 其它兜底模拟
        dec.thought = "Unrecognized task query. Falling back to default response.";
        dec.isToolCall = false;
        dec.finalAnswer = QString("【Mock 提示】大模型识别到任务：'%1'。离线模拟器目前对 'search' 或 'search ... screenshot' 任务提供完整的 ReAct 多轮自反思链式执行演示。").arg(task);
        callback(true, dec, "");
    });
}

// ==========================================
// OpenAiLlmBackend
// ==========================================

static QString cleanAsciiOnly(const QString& input) {
    QString out;
    out.reserve(input.length());
    for (QChar c : input) {
        char16_t val = c.unicode();
        if (val >= 0x21 && val <= 0x7E) {
            out.append(c);
        }
    }
    return out;
}

OpenAiLlmBackend::OpenAiLlmBackend(QString apiUrl, QString apiKey, QString modelName, QObject* parent)
    : QObject(parent), m_apiUrl(cleanAsciiOnly(apiUrl)), m_apiKey(cleanAsciiOnly(apiKey)), m_modelName(cleanAsciiOnly(modelName)) {
    m_network = new QNetworkAccessManager(this);
}

void OpenAiLlmBackend::requestDecision(
    const QList<LlmMessage>& history,
    const QJsonArray& availableTools,
    std::function<void(bool success, LlmDecision decision, QString err)> callback
) {
    QJsonObject body;
    body["model"] = m_modelName;

    // 1. 组装对话历史
    QJsonArray messages;
    for (const auto& msg : history) {
        QJsonObject mObj;
        mObj["role"] = msg.role;
        
        // 如果是 assistant 执行了工具调用，拼装 tool_calls，避开 HTTP 400 Bad Request
        if (msg.role == QStringLiteral("assistant") && !msg.toolCallId.isEmpty()) {
            mObj["content"] = msg.content;
            if (!msg.reasoningContent.isEmpty()) {
                mObj["reasoning_content"] = msg.reasoningContent;
            }
            
            QJsonArray toolCallsArr;
            QJsonObject toolCallObj;
            toolCallObj["id"] = msg.toolCallId;
            toolCallObj["type"] = QStringLiteral("function");
            
            QJsonObject funcObj;
            funcObj["name"] = msg.toolName;
            funcObj["arguments"] = QString::fromUtf8(QJsonDocument(msg.toolArguments).toJson(QJsonDocument::Compact));
            
            toolCallObj["function"] = funcObj;
            toolCallsArr.append(toolCallObj);
            
            mObj["tool_calls"] = toolCallsArr;
        } else {
            mObj["content"] = msg.content;
            if (msg.role == QStringLiteral("assistant") && !msg.reasoningContent.isEmpty()) {
                mObj["reasoning_content"] = msg.reasoningContent;
            }
        }
        
        if (msg.role == QStringLiteral("tool") && !msg.toolCallId.isEmpty()) {
            mObj["tool_call_id"] = msg.toolCallId;
        }
        if (!msg.name.isEmpty()) {
            mObj["name"] = msg.name;
        }
        messages.append(mObj);
    }
    body["messages"] = messages;

    // 2. 将 MCP SDK 暴露的可用工具动态翻译给大模型
    if (!availableTools.isEmpty()) {
        body["tools"] = availableTools;
    }

    QNetworkRequest request(m_apiUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QString("Bearer %1").arg(m_apiKey).toUtf8());

    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkReply* reply = m_network->post(request, payload);
    connect(reply, &QNetworkReply::finished, this, [reply, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            QString errorBody = QString::fromUtf8(reply->readAll());
            callback(false, LlmDecision{}, reply->errorString() + "\nResponse: " + errorBody);
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        QJsonObject root = doc.object();
        QJsonArray choices = root["choices"].toArray();
        if (choices.isEmpty()) {
            callback(false, LlmDecision{}, "LLM returned empty choices");
            return;
        }

        QJsonObject message = choices[0].toObject()["message"].toObject();
        LlmDecision dec;
        dec.thought = message["content"].toString();
        if (message.contains("reasoning_content")) {
            dec.reasoningContent = message["reasoning_content"].toString();
        }

        QJsonArray toolCalls = message["tool_calls"].toArray();
        if (!toolCalls.isEmpty()) {
            QJsonObject tc = toolCalls[0].toObject();
            dec.isToolCall = true;
            dec.toolCallId = tc["id"].toString(); // 🌟 保存大模型生成的唯一 tool_call_id
            dec.toolName = tc["function"].toObject()["name"].toString();
            QString argsStr = tc["function"].toObject()["arguments"].toString();
            
            QJsonDocument argsDoc = QJsonDocument::fromJson(argsStr.toUtf8());
            dec.toolArguments = argsDoc.object();
        } else {
            dec.isToolCall = false;
            dec.finalAnswer = message["content"].toString();
        }

        callback(true, dec, "");
    });
}

} // namespace mcp_agent
