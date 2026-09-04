#include "mcp_qt_client/McpLogger.h"

#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QDebug>
#include <QCoreApplication>
#include <mutex>

namespace mcp_qt {

namespace {
// 全局状态（mutex 保护）
std::mutex g_logMutex;
McpLogLevel g_level = McpLogLevel::Info;
bool g_consoleEnabled = true;
QFile* g_logFile = nullptr;
QTextStream* g_logStream = nullptr;

QString formatLine(McpLogLevel level, const QString& module, const QString& message) {
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz"));
    QString line = ts + QStringLiteral(" [") + McpLogger::levelName(level) + QStringLiteral("]");
    if (!module.isEmpty()) {
        line += QStringLiteral(" [") + module + QStringLiteral("]");
    }
    line += QStringLiteral(" ") + message;
    return line;
}
} // namespace

void McpLogger::setGlobalLevel(McpLogLevel level) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_level = level;
}

McpLogLevel McpLogger::globalLevel() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_level;
}

bool McpLogger::setLogFile(const QString& filePath) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
        g_logStream->flush();
        delete g_logStream;
        g_logStream = nullptr;
        delete g_logFile;
        g_logFile = nullptr;
    }
    if (filePath.isEmpty()) {
        return true;
    }
    auto* file = new QFile(filePath);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        delete file;
        return false;
    }
    g_logFile = file;
    g_logStream = new QTextStream(g_logFile);
    return true;
}

void McpLogger::closeLogFile() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logStream) {
        g_logStream->flush();
        delete g_logStream;
        g_logStream = nullptr;
    }
    if (g_logFile) {
        delete g_logFile;
        g_logFile = nullptr;
    }
}

void McpLogger::setConsoleEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_consoleEnabled = enabled;
}

bool McpLogger::consoleEnabled() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_consoleEnabled;
}

QString McpLogger::levelName(McpLogLevel level) {
    switch (level) {
        case McpLogLevel::Debug: return QStringLiteral("Debug");
        case McpLogLevel::Info: return QStringLiteral("Info");
        case McpLogLevel::Warning: return QStringLiteral("Warning");
        case McpLogLevel::Error: return QStringLiteral("Error");
    }
    return QStringLiteral("Unknown");
}

void McpLogger::debug(const QString& message, const QString& module) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_level > McpLogLevel::Debug) return;
    const QString line = formatLine(McpLogLevel::Debug, module, message);
    if (g_logStream) *g_logStream << line << Qt::endl;
    if (g_consoleEnabled) qDebug().noquote() << line;
}

void McpLogger::info(const QString& message, const QString& module) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_level > McpLogLevel::Info) return;
    const QString line = formatLine(McpLogLevel::Info, module, message);
    if (g_logStream) *g_logStream << line << Qt::endl;
    if (g_consoleEnabled) qInfo().noquote() << line;
}

void McpLogger::warning(const QString& message, const QString& module) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_level > McpLogLevel::Warning) return;
    const QString line = formatLine(McpLogLevel::Warning, module, message);
    if (g_logStream) *g_logStream << line << Qt::endl;
    if (g_consoleEnabled) qWarning().noquote() << line;
}

void McpLogger::error(const QString& message, const QString& module) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_level > McpLogLevel::Error) return;
    const QString line = formatLine(McpLogLevel::Error, module, message);
    if (g_logStream) *g_logStream << line << Qt::endl;
    if (g_consoleEnabled) qCritical().noquote() << line;
}

} // namespace mcp_qt
