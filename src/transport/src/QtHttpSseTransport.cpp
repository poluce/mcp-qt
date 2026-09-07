#include "mcp_qt_transport/QtHttpSseTransport.h"
#include "mcp_qt_transport/McpIoContext.h"

#include "QtHttpSseWorker.h"

#include <QMetaObject>
#include <QThread>

namespace mcp_qt {

class QtHttpSseTransport::Impl {
public:
    explicit Impl(const std::string& baseUrl)
        : url(baseUrl) {}

    std::string url;
    std::string protocolVersion{"2025-11-25"};
    std::function<void(const std::string&)> onMessage;
    std::function<void()> onClose;
    std::function<void(const std::string&)> onError;
    TokenProvider tokenProvider;
    AuthRetryHandler authRetryHandler;
    QtHttpRequestConfig requestConfig;
    QtHttpSseWorker* worker{nullptr};
    bool running{false};
};

QtHttpSseTransport::QtHttpSseTransport(const std::string& baseUrl)
    : m_impl(std::make_unique<Impl>(baseUrl)) {}

QtHttpSseTransport::~QtHttpSseTransport() {
    close();
}

bool QtHttpSseTransport::start() {
    if (m_impl->running) {
        return false;
    }
    // worker 移入共享 I/O 线程（终态架构 §2.1），不再每 transport 一条线程。
    m_impl->worker = new QtHttpSseWorker(QString::fromStdString(m_impl->url));
    m_impl->worker->moveToThread(McpIoContext::shared()->thread());
    m_impl->worker->setProtocolVersion(QString::fromStdString(m_impl->protocolVersion));
    m_impl->worker->setTokenProvider(m_impl->tokenProvider);
    m_impl->worker->setAuthRetryHandler(m_impl->authRetryHandler);
    m_impl->worker->setRequestConfig(m_impl->requestConfig);

    // 回调经 queued 连接投递回本对象所在线程（session 单线程运行在 client 线程）
    QObject::connect(m_impl->worker, &QtHttpSseWorker::messageReceived, [this](const QString& msg) {
        if (m_impl->onMessage) m_impl->onMessage(msg.toStdString());
    });
    QObject::connect(m_impl->worker, &QtHttpSseWorker::transportError, [this](const QString& err) {
        if (m_impl->onError) m_impl->onError(err.toStdString());
    });
    QObject::connect(m_impl->worker, &QtHttpSseWorker::transportClosed, [this]() {
        if (m_impl->onClose) m_impl->onClose();
    });

    McpIoContext::shared()->post([w = m_impl->worker]() { w->startStream(); });
    m_impl->running = true;
    return true;
}

void QtHttpSseTransport::close() {
    if (!m_impl->running) {
        return;
    }
    m_impl->running = false;

    if (m_impl->worker) {
        auto* w = m_impl->worker;
        m_impl->worker = nullptr;
        // 同线程守卫：close 可能从 I/O 线程回调路径触发（如 session onClose），
        // 此时 BlockingQueuedConnection 会与自身死锁，必须直接调用。
        if (McpIoContext::shared()->isCurrentThread()) {
            w->stopStream();
            w->deleteLater();
        } else {
            QMetaObject::invokeMethod(w, &QtHttpSseWorker::stopStream, Qt::BlockingQueuedConnection);
            w->deleteLater();
        }
    }
}

bool QtHttpSseTransport::send(const std::string& message) {
    if (!m_impl->running || !m_impl->worker) {
        return false;
    }
    bool accepted = false;
    if (McpIoContext::shared()->isCurrentThread()) {
        accepted = m_impl->worker->postMessage(QString::fromStdString(message));
    } else {
        QMetaObject::invokeMethod(
            m_impl->worker,
            [&]() { accepted = m_impl->worker->postMessage(QString::fromStdString(message)); },
            Qt::BlockingQueuedConnection
        );
    }
    return accepted;
}

void QtHttpSseTransport::setOnMessage(std::function<void(const std::string&)> callback) { m_impl->onMessage = std::move(callback); }
void QtHttpSseTransport::setOnClose(std::function<void()> callback) { m_impl->onClose = std::move(callback); }
void QtHttpSseTransport::setOnError(std::function<void(const std::string&)> callback) { m_impl->onError = std::move(callback); }
void QtHttpSseTransport::setProtocolVersion(const std::string& version) {
    m_impl->protocolVersion = version;
    if (m_impl->running && m_impl->worker) {
        QMetaObject::invokeMethod(m_impl->worker, [worker = m_impl->worker, version]() {
            worker->setProtocolVersion(QString::fromStdString(version));
        });
    }
}
void QtHttpSseTransport::setTokenProvider(TokenProvider provider) {
    m_impl->tokenProvider = provider;
    if (m_impl->running && m_impl->worker) {
        QMetaObject::invokeMethod(m_impl->worker, [worker = m_impl->worker, provider]() {
            worker->setTokenProvider(provider);
        });
    }
}

void QtHttpSseTransport::setAuthRetryHandler(AuthRetryHandler handler) {
    m_impl->authRetryHandler = handler;
    if (m_impl->running && m_impl->worker) {
        QMetaObject::invokeMethod(m_impl->worker, [worker = m_impl->worker, handler]() {
            worker->setAuthRetryHandler(handler);
        });
    }
}

void QtHttpSseTransport::setRequestConfig(const QtHttpRequestConfig& config) {
    m_impl->requestConfig = config;
    if (m_impl->running && m_impl->worker) {
        QMetaObject::invokeMethod(m_impl->worker, [worker = m_impl->worker, config]() {
            worker->setRequestConfig(config);
        });
    }
}

QtHttpRequestConfig QtHttpSseTransport::requestConfig() const {
    return m_impl->requestConfig;
}

} // namespace mcp_qt
