#include "mcp_qt_transport/QtHttpSseTransport.h"

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
    QThread* thread{nullptr};
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
    m_impl->thread = new QThread();
    m_impl->worker = new QtHttpSseWorker(QString::fromStdString(m_impl->url));
    m_impl->worker->moveToThread(m_impl->thread);
    m_impl->worker->setProtocolVersion(QString::fromStdString(m_impl->protocolVersion));
    m_impl->worker->setTokenProvider(m_impl->tokenProvider);
    m_impl->worker->setAuthRetryHandler(m_impl->authRetryHandler);

    QObject::connect(m_impl->thread, &QThread::started, m_impl->worker, &QtHttpSseWorker::startStream);
    QObject::connect(m_impl->worker, &QtHttpSseWorker::messageReceived, [this](const QString& msg) {
        if (m_impl->onMessage) m_impl->onMessage(msg.toStdString());
    });
    QObject::connect(m_impl->worker, &QtHttpSseWorker::transportError, [this](const QString& err) {
        if (m_impl->onError) m_impl->onError(err.toStdString());
    });
    QObject::connect(m_impl->worker, &QtHttpSseWorker::transportClosed, [this]() {
        if (m_impl->onClose) m_impl->onClose();
    });
    QObject::connect(m_impl->worker, &QtHttpSseWorker::transportClosed, m_impl->thread, &QThread::quit);
    QObject::connect(m_impl->thread, &QThread::finished, m_impl->worker, &QObject::deleteLater);

    QObject::connect(m_impl->thread, &QThread::finished, [this]() {
        if (m_impl->running) {
            m_impl->running = false;
            if (m_impl->thread) {
                m_impl->thread->deleteLater();
                m_impl->thread = nullptr;
            }
            m_impl->worker = nullptr;
        }
    });

    m_impl->thread->start();
    m_impl->running = true;
    return true;
}

void QtHttpSseTransport::close() {
    if (!m_impl->running) {
        return;
    }
    m_impl->running = false;
    
    QMetaObject::invokeMethod(m_impl->worker, &QtHttpSseWorker::stopStream, Qt::BlockingQueuedConnection);
    m_impl->thread->quit();
    m_impl->thread->wait();
    delete m_impl->thread;
    m_impl->thread = nullptr;
    m_impl->worker = nullptr;
}

bool QtHttpSseTransport::send(const std::string& message) {
    if (!m_impl->running || !m_impl->worker) {
        return false;
    }
    bool accepted = false;
    if (QThread::currentThread() == m_impl->thread) {
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

} // namespace mcp_qt
