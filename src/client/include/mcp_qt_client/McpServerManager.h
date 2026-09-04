#pragma once

#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QStringList>
#include <QTimer>
#include <atomic>
#include <memory>
#include <mcp_qt_client/McpQtClient.h>
#include <mcp_qt_client/IMcpConfigLoader.h>

class QProcessEnvironment;

namespace mcp_qt {

enum class McpServerState {
    Pending,
    Connecting,
    Ready,
    Error
};

/**
 * @brief 多服务器生命周期管理器 (McpServerManager)
 *
 * 职责：
 * 1. 读取并解析 mcp_servers.json 配置文件。
 * 2. 循环调用 McpQtClientBuilder 实例化并连接多个客户端。
 * 3. 存储并管理各客户端的生命周期。
 * 4. 连接即预热：服务器上线后自动拉取工具列表并缓存。
 * 5. 统一聚合各 client 的信号并重新分发。
 */
class McpServerManager : public QObject {
    Q_OBJECT
public:
    explicit McpServerManager(QObject* parent = nullptr);
    ~McpServerManager() override;

    bool loadServers(std::shared_ptr<IMcpConfigLoader> loader);
    bool loadServers(const QList<McpServerConfig>& configs);

    bool startServer(const McpServerConfig& cfg);
    void stopServer(const QString& serverName);

    void registerClient(const QString& serverName, std::shared_ptr<McpQtClient> client);
    void unregisterClient(const QString& serverName);

    std::shared_ptr<McpQtClient> client(const QString& serverName) const;
    QHash<QString, std::shared_ptr<McpQtClient>> clients() const;
    QStringList serverNames() const;
    McpServerState serverState(const QString& serverName) const;

    /// 所有已注册服务器是否都已完成工具预热（或已离线）
    bool isAllToolsReady() const;

    void closeAll(int timeoutMs = 5000);

    /// 启动心跳保活检测（定期 ping 所有已连接客户端，检测断连并触发自动重连）
    /// @param intervalMs  检测间隔毫秒数（默认 30 秒）
    void startHeartbeat(int intervalMs = 30000);

    /// 停止心跳保活检测
    void stopHeartbeat();

signals:
    void clientConnected(const QString& serverName);
    void clientDisconnected(const QString& serverName);
    void clientErrorOccurred(const QString& serverName, const mcp_qt::McpError& error);
    void clientToolsChanged(const QString& serverName, const std::vector<mcp_qt::McpQtTool>& newTools);
    void clientPromptsChanged(const QString& serverName);
    void clientStateChanged(const QString& serverName, mcp_qt::McpServerState state);

    /// 某个服务器由于 MRTR 需要用户补充输入（2026-07-28 规范 InputRequests map + requestState）
    void clientInputRequired(const QString& serverName, const QString& requestId,
                             const QJsonObject& inputRequests, const QString& requestState,
                             mcp_qt::MrtrReplyCallback replyCallback);
    /// 某个服务器的工具预热完成（首次连接后的 fetchAllToolsAsync 完成）
    void clientToolsReady(const QString& serverName, int toolCount);
    /// 所有已注册服务器的工具预热全部完成
    void allToolsReady();

private:
    static QString interpolateEnv(const QString& value);
    static QString interpolateEnv(const QString& value, const QProcessEnvironment& env);
    static void configureBuilder(McpQtClientBuilder& builder, const McpServerConfig& cfg);
    void processHttpServerConfig(const McpServerConfig& cfg);
    /// 协议自动探测：protocolVersion 未显式指定时，POST server/discover(2026-07-28)
    /// 试探服务器——支持则用无状态模式，否则回退旧协议(2025-11-25)。
    void probeProtocolVersion(const McpServerConfig& cfg,
                              std::function<void(const McpServerConfig&)> done);
    void setupClientSignals(const QString& serverName, const std::shared_ptr<McpQtClient>& client);
    void warmupClientTools(const QString& serverName, const std::shared_ptr<McpQtClient>& client);
    void updateServerState(const QString& serverName, McpServerState state);

    QHash<QString, std::shared_ptr<McpQtClient>> m_clients;
    QHash<QString, McpServerState> m_serverStates;
    QTimer* m_heartbeatTimer{nullptr};
};

} // namespace mcp_qt
