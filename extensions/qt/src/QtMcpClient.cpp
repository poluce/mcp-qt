#include "mcp_qt/QtMcpClient.h"
#include <QJsonDocument>
#include <QJsonObject>

namespace mcp {

QtMcpClient::QtMcpClient(std::shared_ptr<IMcpTransport> transport, QObject* parent)
    : QObject(parent)
    , m_transport(std::move(transport))
{
    // Register types for use in Signals/Slots
    qRegisterMetaType<mcp::McpTool>("mcp::McpTool");
    qRegisterMetaType<QList<mcp::McpTool>>("QList<mcp::McpTool>");

    m_session = std::make_shared<McpClientSession>(m_transport);
    m_session->init();

    // Bind transport-level events to emit signals safely on the UI thread
    m_transport->setOnClose([this]() {
        QMetaObject::invokeMethod(this, [this]() {
            emit disconnected();
        }, Qt::QueuedConnection);
    });

    m_transport->setOnError([this](const std::string& err) {
        QMetaObject::invokeMethod(this, [this, err]() {
            emit errorOccurred(QString::fromStdString(err));
        }, Qt::QueuedConnection);
    });

    // Start request timeout checker timer (runs every 1 second)
    m_timeoutTimer = new QTimer(this);
    connect(m_timeoutTimer, &QTimer::timeout, this, &QtMcpClient::onCheckTimeouts);
    m_timeoutTimer->start(1000);
}

QtMcpClient::~QtMcpClient() {
    close();
}

void QtMcpClient::start() {
    if (m_session->start()) {
        emit connectionOpened();
    } else {
        emit errorOccurred("Failed to start transport connection");
    }
}

void QtMcpClient::close() {
    if (m_timeoutTimer) {
        m_timeoutTimer->stop();
    }
    m_session->close();
}

void QtMcpClient::initializeClient(const QString& clientName, const QString& clientVersion) {
    m_session->initialize(clientName.toStdString(), clientVersion.toStdString(),
                          [this](bool success, const json& serverInfo) {
        QMetaObject::invokeMethod(this, [this, success, serverInfo]() {
            QString infoStr = QString::fromStdString(serverInfo.dump());
            emit initialized(success, infoStr);
        }, Qt::QueuedConnection);
    });
}

void QtMcpClient::listTools() {
    m_session->listTools([this](const std::vector<McpTool>& tools, const json& error) {
        QMetaObject::invokeMethod(this, [this, tools, error]() {
            QString errStr;
            if (!error.empty()) {
                errStr = QString::fromStdString(error.dump());
            }

            QList<McpTool> qTools;
            for (const auto& t : tools) {
                qTools.append(t);
            }
            emit toolsListed(qTools, errStr);
        }, Qt::QueuedConnection);
    });
}

void QtMcpClient::callTool(const QString& name, const QString& argumentsJson) {
    json args = json::object();
    if (!argumentsJson.isEmpty()) {
        try {
            args = json::parse(argumentsJson.toStdString());
        } catch (const std::exception& e) {
            emit toolCalled(name, "", "Arguments JSON parse error: " + QString::fromStdString(e.what()));
            return;
        }
    }

    m_session->callTool(name.toStdString(), args, [this, name](const json& result, const json& error) {
        QMetaObject::invokeMethod(this, [this, name, result, error]() {
            QString resStr = QString::fromStdString(result.dump());
            QString errStr;
            if (!error.empty()) {
                errStr = QString::fromStdString(error.dump());
            }
            emit toolCalled(name, resStr, errStr);
        }, Qt::QueuedConnection);
    });
}

void QtMcpClient::shutdownClient() {
    m_session->shutdown([this](bool success) {
        QMetaObject::invokeMethod(this, [this, success]() {
            emit shutdownCompleted(success);
        }, Qt::QueuedConnection);
    });
}

void QtMcpClient::listResources() {
    m_session->listResources([this](const json& result, const json& error) {
        QMetaObject::invokeMethod(this, [this, result, error]() {
            QString resStr = QString::fromStdString(result.dump());
            QString errStr = error.empty() ? "" : QString::fromStdString(error.dump());
            emit resourcesListed(resStr, errStr);
        }, Qt::QueuedConnection);
    });
}

void QtMcpClient::readResource(const QString& uri) {
    m_session->readResource(uri.toStdString(), [this, uri](const json& result, const json& error) {
        QMetaObject::invokeMethod(this, [this, uri, result, error]() {
            QString resStr = QString::fromStdString(result.dump());
            QString errStr = error.empty() ? "" : QString::fromStdString(error.dump());
            emit resourceRead(uri, resStr, errStr);
        }, Qt::QueuedConnection);
    });
}

void QtMcpClient::listPrompts() {
    m_session->listPrompts([this](const json& result, const json& error) {
        QMetaObject::invokeMethod(this, [this, result, error]() {
            QString resStr = QString::fromStdString(result.dump());
            QString errStr = error.empty() ? "" : QString::fromStdString(error.dump());
            emit promptsListed(resStr, errStr);
        }, Qt::QueuedConnection);
    });
}

void QtMcpClient::getPrompt(const QString& name, const QString& argumentsJson) {
    json args = json::object();
    if (!argumentsJson.isEmpty()) {
        try {
            args = json::parse(argumentsJson.toStdString());
        } catch (const std::exception& e) {
            emit promptGot(name, "", "Arguments JSON parse error: " + QString::fromStdString(e.what()));
            return;
        }
    }

    m_session->getPrompt(name.toStdString(), args, [this, name](const json& result, const json& error) {
        QMetaObject::invokeMethod(this, [this, name, result, error]() {
            QString resStr = QString::fromStdString(result.dump());
            QString errStr = error.empty() ? "" : QString::fromStdString(error.dump());
            emit promptGot(name, resStr, errStr);
        }, Qt::QueuedConnection);
    });
}

void QtMcpClient::onCheckTimeouts() {
    if (m_session) {
        m_session->checkRequestTimeouts();
    }
}

} // namespace mcp
