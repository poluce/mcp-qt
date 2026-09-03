#pragma once

#include "mcp_qt_client/McpServerManager.h"
#include "mcp_qt_client/McpToolRouter.h"
#include "mcp_qt_client/McpPromptRouter.h"
#include "mcp_qt_client/McpResourceRouter.h"
#include "mcp_qt_client/McpDiagnosticReporter.h"
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QJsonArray>
#include <QMap>
#include <functional>

namespace mcp_qt {

class McpHost : public QObject {
    Q_OBJECT
public:
    explicit McpHost(QObject* parent = nullptr);
    ~McpHost() override;

    // ================= 1. Configuration & Lifecycle =================
    bool loadConfigFromFile(const QString& configFilePath);
    bool loadConfigFromJson(const QJsonObject& jsonObj);
    void clearConfig();
    
    // Starts enabled servers asynchronously. Emits hostReady when done or timeout occurs.
    void start(int timeoutMs = 30000); 
    void stop();
    
    // 一键重启已加载的所有服务
    void restart(int timeoutMs = 30000);
    // 热重载配置文件并重新拉起服务
    bool reloadConfigAndRestart(int timeoutMs = 30000);

    // ================= 2. Server Management =================
    /// 已注册（已连接）的服务器名——与 McpServerManager::serverNames() 语义一致。
    /// 注意：未启用/未 start 的服务器不在此列；如需配置列表（含未启用）用 configuredServerNames()。
    QStringList serverNames() const;
    /// 配置中声明的全部服务器名（含未启用的），与 serverNames() 的"已注册"语义区分。
    QStringList configuredServerNames() const;
    void setServerEnabled(const QString& serverName, bool enabled, bool persist = true);
    void removeServerConfig(const QString& serverName, bool persist = true);
    /// 登记/更新配置（同名覆盖去重）。仅登记，不启动、不持久化——start() 时才连接。
    /// 需要"即改即生效 + 持久化"用 addOrUpdateServerConfig()。
    void addServerConfig(const McpServerConfig& config);
    /// 更新配置并自动重启该服务器（stop + start），可选持久化到配置文件。
    void addOrUpdateServerConfig(const McpServerConfig& config, bool persist = true);
    /// 重启指定服务器（从已加载配置重新拉起）。
    void restartServer(const QString& serverName);
    bool isServerEnabled(const QString& serverName) const;
    
    McpServerState serverState(const QString& serverName) const;
    QString serverErrorMessage(const QString& serverName) const;
    int serverToolCount(const QString& serverName) const;
    std::shared_ptr<McpQtClient> client(const QString& serverName) const;

    // ================= 3. Unified Routing =================
    QJsonArray exportAllToolsToLlm(McpQtClient::LlmFormat format = McpQtClient::LlmFormat::OpenAI) const;
    
    void callToolAsync(const QString& toolName, const QJsonObject& args, std::function<void(McpResult)> callback);
    
    McpToolRouter* toolRouter() const { return m_toolRouter; }
    McpPromptRouter* promptRouter() const { return m_promptRouter; }
    McpResourceRouter* resourceRouter() const { return m_resourceRouter; }
    McpServerManager* manager() const { return m_manager; }

    // ================= 4. Diagnostics =================
    QString getDiagnosticReport() const;
    McpDiagnosticReporter* reporter() const { return m_reporter; }

signals:
    void hostReady(bool success, const QString& summaryMsg);
    void serverStateChanged(const QString& serverName, mcp_qt::McpServerState state);
    void globalToolsChanged();
    void globalPromptsChanged();
    void globalResourcesChanged();
    void errorOccurred(const QString& serverName, const mcp_qt::McpError& error);
    void inputRequired(const QString& serverName, const QString& requestId,
                       const QJsonObject& inputRequests, const QString& requestState,
                       mcp_qt::MrtrReplyCallback replyCallback);

private:
    void handleAllToolsReady();
    void handleStartupTimeout();
    void checkReadyCondition();
    void finishStartup(bool success, const QString& summaryMsg);
    bool loadConfigs(const QList<McpServerConfig>& configs);

    bool persistServerProperty(const QString& serverName, const QString& key, const QJsonValue& value);
    bool persistServerObject(const QString& serverName, const QJsonObject& obj);
    bool persistRemoveServer(const QString& serverName);
    QJsonObject serializeServerConfig(const McpServerConfig& cfg) const;
    // 读-改-写 mcpServers 配置：allowMissing 时文件缺失则从空对象开始；mutate 返回 false 不写回。
    bool readWriteConfig(bool allowMissing, const std::function<bool(QJsonObject&)>& mutate);

    McpServerManager* m_manager;
    McpToolRouter* m_toolRouter;
    McpPromptRouter* m_promptRouter;
    McpResourceRouter* m_resourceRouter;
    McpDiagnosticReporter* m_reporter;

    QTimer* m_watchdogTimer;
    bool m_isStarting{false};
    
    QString m_lastConfigPath;
    QMap<QString, bool> m_enabledServers;
    QList<McpServerConfig> m_loadedConfigs;
};

} // namespace mcp_qt
