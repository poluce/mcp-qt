#pragma once

#include <mcp_core/IMcpTransport.h>
#include <QObject>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace mcp_qt {

class QtProcessStdioWorker;

/**
 * @brief 子进程 stdio 传输（终态架构 §2.1）。
 *
 * QProcess 与子进程管理全部运行在 McpIoContext 共享 I/O 线程（内部 worker），
 * 回调经 queued 连接投递回本对象所在线程；本对象只是线程安全的配置/回调句柄。
 * Windows 上 CreateJobObject 保证子进程随父进程退出。
 */
class QtProcessStdioTransport : public QObject, public mcp::IMcpTransport {
    Q_OBJECT
public:
    QtProcessStdioTransport(const std::string& command, const std::vector<std::string>& args, QObject* parent = nullptr);
    ~QtProcessStdioTransport() override;

    void setEnvironment(const std::unordered_map<std::string, std::string>& env);

    bool start() override;
    void close() override;
    bool send(const std::string& message) override;
    void setOnMessage(std::function<void(const std::string&)> callback) override;
    void setOnClose(std::function<void()> callback) override;
    void setOnError(std::function<void(const std::string&)> callback) override;
    void setProtocolVersion(const std::string& version) override;

signals:
    /// 子进程 stderr 输出（服务端日志），与 onError（传输层故障）分离
    void serverLog(const QString& message);

private:
    std::string m_command;
    std::vector<std::string> m_args;
    std::unordered_map<std::string, std::string> m_env;

    std::function<void(const std::string&)> m_onMessage;
    std::function<void()> m_onClose;
    std::function<void(const std::string&)> m_onError;

    bool m_started{false};
    QtProcessStdioWorker* m_worker{nullptr};
};

} // namespace mcp_qt
