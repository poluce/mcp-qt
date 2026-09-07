#pragma once

#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <functional>
#include <string>
#include <vector>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace mcp_qt {

/**
 * @brief QtProcessStdioTransport 的 I/O 线程工作对象（终态架构 §2.1）。
 *
 * QProcess 与子进程管理全部运行在 McpIoContext 的 I/O 线程上（QProcess 在
 * start() 内惰性创建，保证构造于 I/O 线程）；结果经信号投递回 transport
 * 所在线程（queued 连接）。Windows 上 CreateJobObject 保证子进程随父进程退出。
 */
class QtProcessStdioWorker : public QObject {
    Q_OBJECT
public:
    QtProcessStdioWorker(const std::string& command, const std::vector<std::string>& args);
    ~QtProcessStdioWorker() override;

    void setEnvironment(const std::unordered_map<std::string, std::string>& env);

    /// 在 I/O 线程上执行：创建 QProcess、注入环境、启动子进程。
    void start();
    /// 在 I/O 线程上执行：终止子进程，退出运行态。
    void stop();
    /// 在 I/O 线程上执行：向子进程 stdin 写一条 JSON-RPC 消息。
    void postMessage(const QString& message);

signals:
    void messageReceived(const QString& message);
    void transportError(const QString& error);
    void transportClosed();
    /// 子进程 stderr 输出（服务端日志），与 transportError（传输层故障）分离
    void serverLog(const QString& message);

private slots:
    void handleReadyReadStandardOutput();
    void handleReadyReadStandardError();
    void handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void handleProcessError(QProcess::ProcessError error);

private:
    std::string m_command;
    std::vector<std::string> m_args;
    std::unordered_map<std::string, std::string> m_env;
    QProcess* m_process{nullptr};  // I/O 线程内惰性创建
    std::string m_buffer;
    bool m_started{false};

#ifdef _WIN32
    void* m_jobObject{nullptr};
#endif
};

} // namespace mcp_qt
