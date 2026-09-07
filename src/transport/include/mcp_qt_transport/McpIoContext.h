#pragma once

#include <QObject>
#include <QThread>
#include <functional>

namespace mcp_qt {

/**
 * @brief 共享 I/O 线程上下文（终态架构 §2.1）。
 *
 * 所有 transport 的网络 I/O（QNAM 请求、SSE 流、QProcess 子进程）都运行在这条
 * 线程上；transport 回调经 queued 连接投递回调用线程，session 状态机单线程运行
 * 在 client 所在线程。单线程承载多连接是安全的：QNAM/QProcess 都是异步的。
 *
 * 默认共享实例（进程级，故意泄漏，随进程退出回收）；McpQtClientBuilder 可注入
 * 自定义实例（如测试隔离）。
 */
class McpIoContext : public QObject {
    Q_OBJECT
public:
    /// 进程级共享实例（懒启动）。
    static McpIoContext* shared();

    explicit McpIoContext(QObject* parent = nullptr);
    ~McpIoContext() override;

    QThread* thread() const { return m_thread; }
    bool isCurrentThread() const { return QThread::currentThread() == m_thread; }

    /// 把 fn 投递到 I/O 线程执行；已在 I/O 线程则直接执行。
    /// 要求线程已 start()，否则投递永不执行。
    void post(std::function<void()> fn);

    /// 启动 I/O 线程（shared 实例自动启动；自定义实例需显式调用）。
    void start();
    /// 停止 I/O 线程（quit + wait）。shared 实例不调用。
    void stop();

private:
    QThread* m_thread{nullptr};
};

} // namespace mcp_qt
