#pragma once

#include "ILlmBackend.h"
#include <QObject>
#include <QNetworkAccessManager>

namespace mcp_agent {

class MockLlmBackend : public ILlmBackend {
public:
    MockLlmBackend() = default;
    ~MockLlmBackend() override = default;

    void requestDecision(
        const QList<LlmMessage>& history,
        const QJsonArray& availableTools,
        std::function<void(bool success, LlmDecision decision, QString err)> callback
    ) override;
};

/// Deterministic backend for MCP App smoke tests: call one configured tool once.
class ScriptedToolLlmBackend : public ILlmBackend {
public:
    ScriptedToolLlmBackend(QString toolName, QJsonObject arguments);

    void requestDecision(
        const QList<LlmMessage>& history,
        const QJsonArray& availableTools,
        std::function<void(bool success, LlmDecision decision, QString err)> callback
    ) override;

private:
    QString m_toolName;
    QJsonObject m_arguments;
};

class OpenAiLlmBackend : public QObject, public ILlmBackend {
    Q_OBJECT
public:
    OpenAiLlmBackend(QString apiUrl, QString apiKey, QString modelName, QObject* parent = nullptr);
    ~OpenAiLlmBackend() override = default;

    void requestDecision(
        const QList<LlmMessage>& history,
        const QJsonArray& availableTools,
        std::function<void(bool success, LlmDecision decision, QString err)> callback
    ) override;

private:
    QString m_apiUrl;
    QString m_apiKey;
    QString m_modelName;
    QNetworkAccessManager* m_network;
};

} // namespace mcp_agent
