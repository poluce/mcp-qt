#pragma once
#include <QObject>
#include <QProcess>
#include <QStringList>
#include "mcp_core/IMcpTransport.h"

namespace mcp {

/**
 * @brief Qt-based Stdio transport channel for MCP.
 * 
 * Spawns and manages an external process (the MCP Server) as a child process,
 * communicating via redirecting standard input/output with line-by-line JSON messages.
 */
class QtStdioTransport : public QObject, public IMcpTransport {
    Q_OBJECT
public:
    explicit QtStdioTransport(const QString& program, const QStringList& arguments = {}, QObject* parent = nullptr);
    ~QtStdioTransport() override;

    // IMcpTransport interface
    bool send(const std::string& message) override;
    void setOnMessage(std::function<void(const std::string&)> callback) override;
    void setOnClose(std::function<void()> callback) override;
    void setOnError(std::function<void(const std::string&)> callback) override;
    bool start() override;
    void close() override;

private slots:
    void handleReadyReadStandardOutput();
    void handleReadyReadStandardError();
    void handleProcessFinished(int exitCode);
    void handleProcessError(QProcess::ProcessError error);

private:
    QString m_program;
    QStringList m_arguments;
    QProcess* m_process;

    std::function<void(const std::string&)> m_onMessage;
    std::function<void()> m_onClose;
    std::function<void(const std::string&)> m_onError;

    QByteArray m_outputBuffer; 
};

} // namespace mcp
