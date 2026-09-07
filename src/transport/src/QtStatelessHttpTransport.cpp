#include "mcp_qt_transport/QtStatelessHttpTransport.h"
#include "mcp_qt_transport/McpIoContext.h"
#include "QtStatelessHttpWorker.h"

#include <QMetaObject>
#include <QThread>

namespace mcp_qt {

QtStatelessHttpTransport::QtStatelessHttpTransport(const QString& endpointUrl, QObject* parent)
    : QObject(parent), m_endpointUrl(endpointUrl) {
    m_headers.insert("Content-Type", "application/json");
}

QtStatelessHttpTransport::~QtStatelessHttpTransport() {
    close();
}

bool QtStatelessHttpTransport::start() {
    if (m_isRunning) return true;
    m_isRunning = true;

    // worker 在调用线程构造（仅配置，无网络对象），移入共享 I/O 线程后
    // 经 post 执行 start()：QNAM 在 I/O 线程内惰性创建。
    m_worker = new QtStatelessHttpWorker(m_endpointUrl);
    m_worker->setProtocolVersion(m_protocolVersion);
    m_worker->setExtraRequestHeaders(m_extraRequestHeaders);
    m_worker->setCustomHeaders(m_headers);
    m_worker->setProxy(m_proxy);
    m_worker->setTokenProvider(m_tokenProvider);
    m_worker->setAuthRetryHandler(m_authRetryHandler);
    m_worker->moveToThread(McpIoContext::shared()->thread());

    // 回调经 queued 连接投递回本对象所在线程（session 单线程运行在 client 线程）
    connect(m_worker, &QtStatelessHttpWorker::messageReceived, this, [this](const QString& msg) {
        if (m_onMessage) m_onMessage(msg.toStdString());
    });
    connect(m_worker, &QtStatelessHttpWorker::transportError, this, [this](const QString& err) {
        if (m_onError) m_onError(err.toStdString());
    });
    connect(m_worker, &QtStatelessHttpWorker::transportClosed, this, [this]() {
        if (m_onClose) m_onClose();
    });

    McpIoContext::shared()->post([w = m_worker]() { w->start(); });
    return true;
}

void QtStatelessHttpTransport::close() {
    if (!m_isRunning) return;
    m_isRunning = false;

    if (m_worker) {
        auto* w = m_worker;
        m_worker = nullptr;
        // 同线程守卫：close 可能从 I/O 线程回调路径触发，BlockingQueuedConnection 会自死锁
        if (McpIoContext::shared()->isCurrentThread()) {
            w->stop();
            w->deleteLater();
        } else {
            QMetaObject::invokeMethod(w, &QtStatelessHttpWorker::stop, Qt::BlockingQueuedConnection);
            w->deleteLater();
        }
    }
}

bool QtStatelessHttpTransport::send(const std::string& message) {
    if (!m_isRunning || !m_worker) return false;

    QString msg = QString::fromStdString(message);
    if (McpIoContext::shared()->isCurrentThread()) {
        m_worker->postMessage(msg);
    } else {
        QMetaObject::invokeMethod(m_worker, [w = m_worker, msg]() { w->postMessage(msg); },
                                  Qt::QueuedConnection);
    }
    return true;
}

void QtStatelessHttpTransport::setOnMessage(std::function<void(const std::string&)> callback) {
    m_onMessage = std::move(callback);
}

void QtStatelessHttpTransport::setOnClose(std::function<void()> callback) {
    m_onClose = std::move(callback);
}

void QtStatelessHttpTransport::setOnError(std::function<void(const std::string&)> callback) {
    m_onError = std::move(callback);
}

void QtStatelessHttpTransport::setProtocolVersion(const std::string& version) {
    m_protocolVersion = version;
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, [w = m_worker, version]() { w->setProtocolVersion(version); },
                                  Qt::QueuedConnection);
    }
}

void QtStatelessHttpTransport::setExtraRequestHeaders(const std::map<std::string, std::string>& headers) {
    m_extraRequestHeaders = headers;
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, [w = m_worker, headers]() { w->setExtraRequestHeaders(headers); },
                                  Qt::QueuedConnection);
    }
}

void QtStatelessHttpTransport::setCustomHeaders(const QMap<QByteArray, QByteArray>& headers) {
    m_headers = headers;
    if (!m_headers.contains("Content-Type")) {
        m_headers.insert("Content-Type", "application/json");
    }
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, [w = m_worker, headers = m_headers]() { w->setCustomHeaders(headers); },
                                  Qt::QueuedConnection);
    }
}

void QtStatelessHttpTransport::setProxy(const QNetworkProxy& proxy) {
    m_proxy = proxy;
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, [w = m_worker, proxy]() { w->setProxy(proxy); },
                                  Qt::QueuedConnection);
    }
}

void QtStatelessHttpTransport::setTokenProvider(TokenProvider provider) {
    m_tokenProvider = std::move(provider);
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, [w = m_worker, provider = m_tokenProvider]() { w->setTokenProvider(provider); },
                                  Qt::QueuedConnection);
    }
}

void QtStatelessHttpTransport::setAuthRetryHandler(AuthRetryHandler handler) {
    m_authRetryHandler = std::move(handler);
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, [w = m_worker, handler = m_authRetryHandler]() { w->setAuthRetryHandler(handler); },
                                  Qt::QueuedConnection);
    }
}

} // namespace mcp_qt
