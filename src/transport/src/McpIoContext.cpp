#include "mcp_qt_transport/McpIoContext.h"

#include <QMetaObject>

namespace mcp_qt {

McpIoContext* McpIoContext::shared() {
    // 进程级单例：故意泄漏（QObject 静态析构顺序不可控，泄漏最安全）。
    static McpIoContext* s_shared = [] {
        auto* ctx = new McpIoContext();
        ctx->start();
        return ctx;
    }();
    return s_shared;
}

McpIoContext::McpIoContext(QObject* parent)
    : QObject(parent) {
    m_thread = new QThread();
    // 上下文对象自身生活在 I/O 线程上，post() 以它为 queued 投递目标。
    moveToThread(m_thread);
}

McpIoContext::~McpIoContext() {
    stop();
    delete m_thread;
}

void McpIoContext::start() {
    if (!m_thread->isRunning()) {
        m_thread->start();
    }
}

void McpIoContext::stop() {
    if (m_thread->isRunning()) {
        m_thread->quit();
        m_thread->wait();
    }
}

void McpIoContext::post(std::function<void()> fn) {
    if (isCurrentThread()) {
        fn();
        return;
    }
    QMetaObject::invokeMethod(this, [fn = std::move(fn)]() { fn(); }, Qt::QueuedConnection);
}

} // namespace mcp_qt
