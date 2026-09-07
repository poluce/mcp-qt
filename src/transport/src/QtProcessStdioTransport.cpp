#include "mcp_qt_transport/QtProcessStdioTransport.h"
#include "mcp_qt_transport/McpIoContext.h"
#include "QtProcessStdioWorker.h"

#include <QMetaObject>
#include <QThread>

namespace mcp_qt {

QtProcessStdioTransport::QtProcessStdioTransport(const std::string& command, const std::vector<std::string>& args, QObject* parent)
    : QObject(parent), m_command(command), m_args(args) {}

QtProcessStdioTransport::~QtProcessStdioTransport() {
    close();
}

void QtProcessStdioTransport::setEnvironment(const std::unordered_map<std::string, std::string>& env) {
    m_env = env;
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, [w = m_worker, env]() { w->setEnvironment(env); },
                                  Qt::QueuedConnection);
    }
}

bool QtProcessStdioTransport::start() {
    if (m_started) return true;
    m_started = true;

    // worker 在调用线程构造（仅配置，无 QProcess），移入共享 I/O 线程后
    // 经 post 执行 start()：QProcess 在 I/O 线程内惰性创建。
    m_worker = new QtProcessStdioWorker(m_command, m_args);
    m_worker->setEnvironment(m_env);
    m_worker->moveToThread(McpIoContext::shared()->thread());

    // 回调经 queued 连接投递回本对象所在线程（session 单线程运行在 client 线程）
    connect(m_worker, &QtProcessStdioWorker::messageReceived, this, [this](const QString& msg) {
        if (m_onMessage) m_onMessage(msg.toStdString());
    });
    connect(m_worker, &QtProcessStdioWorker::transportError, this, [this](const QString& err) {
        if (m_onError) m_onError(err.toStdString());
    });
    connect(m_worker, &QtProcessStdioWorker::transportClosed, this, [this]() {
        if (m_onClose) m_onClose();
    });
    connect(m_worker, &QtProcessStdioWorker::serverLog, this, &QtProcessStdioTransport::serverLog);

    McpIoContext::shared()->post([w = m_worker]() { w->start(); });
    return true;
}

void QtProcessStdioTransport::close() {
    if (!m_started) return;
    m_started = false;

    if (m_worker) {
        auto* w = m_worker;
        m_worker = nullptr;
        // 同线程守卫：close 可能从 I/O 线程回调路径触发，BlockingQueuedConnection 会自死锁
        if (McpIoContext::shared()->isCurrentThread()) {
            w->stop();
            w->deleteLater();
        } else {
            QMetaObject::invokeMethod(w, &QtProcessStdioWorker::stop, Qt::BlockingQueuedConnection);
            w->deleteLater();
        }
    }
}

bool QtProcessStdioTransport::send(const std::string& message) {
    if (!m_started || !m_worker) return false;

    QString msg = QString::fromStdString(message);
    if (McpIoContext::shared()->isCurrentThread()) {
        m_worker->postMessage(msg);
    } else {
        QMetaObject::invokeMethod(m_worker, [w = m_worker, msg]() { w->postMessage(msg); },
                                  Qt::QueuedConnection);
    }
    return true;
}

void QtProcessStdioTransport::setOnMessage(std::function<void(const std::string&)> callback) {
    m_onMessage = std::move(callback);
}

void QtProcessStdioTransport::setOnClose(std::function<void()> callback) {
    m_onClose = std::move(callback);
}

void QtProcessStdioTransport::setOnError(std::function<void(const std::string&)> callback) {
    m_onError = std::move(callback);
}

void QtProcessStdioTransport::setProtocolVersion(const std::string&) {
    // Stdio doesn't negotiate protocol version at the transport level
}

} // namespace mcp_qt
