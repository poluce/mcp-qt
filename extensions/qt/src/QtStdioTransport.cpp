#include "mcp_qt/QtStdioTransport.h"
#include <QDebug>

namespace mcp {

QtStdioTransport::QtStdioTransport(const QString& program, const QStringList& arguments, QObject* parent)
    : QObject(parent)
    , m_program(program)
    , m_arguments(arguments)
    , m_process(new QProcess(this))
{
    connect(m_process, &QProcess::readyReadStandardOutput, this, &QtStdioTransport::handleReadyReadStandardOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, &QtStdioTransport::handleReadyReadStandardError);
    connect(m_process, &QProcess::finished, this, &QtStdioTransport::handleProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &QtStdioTransport::handleProcessError);
}

QtStdioTransport::~QtStdioTransport() {
    close();
}

bool QtStdioTransport::send(const std::string& message) {
    if (m_process->state() != QProcess::Running) {
        return false;
    }
    QByteArray data = QByteArray::fromStdString(message) + "\n";
    qint64 written = m_process->write(data);
    return (written == data.size());
}

void QtStdioTransport::setOnMessage(std::function<void(const std::string&)> callback) {
    m_onMessage = std::move(callback);
}

void QtStdioTransport::setOnClose(std::function<void()> callback) {
    m_onClose = std::move(callback);
}

void QtStdioTransport::setOnError(std::function<void(const std::string&)> callback) {
    m_onError = std::move(callback);
}

bool QtStdioTransport::start() {
    if (m_process->state() != QProcess::NotRunning) {
        return false;
    }
    m_process->start(m_program, m_arguments);
    return m_process->waitForStarted(5000);
}

void QtStdioTransport::close() {
    if (m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        if (!m_process->waitForFinished(2000)) {
            m_process->kill();
        }
    }
}

void QtStdioTransport::handleReadyReadStandardOutput() {
    while (m_process->canReadLine()) {
        QByteArray line = m_process->readLine().trimmed();
        if (line.isEmpty()) continue;
        
        std::string rawStr = line.toStdString();
        if (rawStr.front() == '{') {
            if (m_onMessage) {
                m_onMessage(rawStr);
            }
        } else {
            if (m_onError) {
                m_onError("[服务端非法在 stdout 输出日志]: " + rawStr);
            }
        }
    }
}

void QtStdioTransport::handleReadyReadStandardError() {
    QByteArray errData = m_process->readAllStandardError();
    if (!errData.isEmpty() && m_onError) {
        m_onError("[Server Stderr]: " + errData.trimmed().toStdString());
    }
}

void QtStdioTransport::handleProcessFinished(int exitCode) {
    Q_UNUSED(exitCode);
    if (m_onClose) {
        m_onClose();
    }
}

void QtStdioTransport::handleProcessError(QProcess::ProcessError error) {
    Q_UNUSED(error);
    if (m_onError) {
        m_onError("Process error: " + m_process->errorString().toStdString());
    }
}

} // namespace mcp
