#pragma once

#include <QString>
#include <QStringList>

namespace mcp_qt {

/// 统一日志级别（全局开关）
enum class McpLogLevel {
    Debug = 0,   // 调试：详细流程
    Info = 1,    // 信息：常规事件
    Warning = 2, // 警告：异常但可恢复
    Error = 3    // 错误：失败
};

/**
 * @brief 统一日志门面：全局级别控制 + 文件落盘 + 控制台输出。
 *
 * 解决两个可观测性缺口：
 * 1. 各模块各自 qDebug/qWarning，无全局开关 → setGlobalLevel() 统一控制；
 * 2. 日志只到控制台/回调，不落盘 → setLogFile() 追加写入文件。
 *
 * 用法：
 * @code
 *   mcp_qt::McpLogger::setGlobalLevel(mcp_qt::McpLogLevel::Info);
 *   mcp_qt::McpLogger::setLogFile("mcp-qt.log");
 *   mcp_qt::McpLogger::warning("McpServerManager", "MCP client error for xxx: ...");
 * @endcode
 *
 * 线程安全：文件写入由内部 mutex 保护，可在任意线程调用。
 */
class McpLogger {
public:
    /// 设置全局日志级别（低于该级别的日志被丢弃）。默认 Info。
    static void setGlobalLevel(McpLogLevel level);
    static McpLogLevel globalLevel();

    /// 启用文件输出（追加模式）。返回 false 表示打开失败。
    /// 传空路径关闭文件输出。
    static bool setLogFile(const QString& filePath);
    /// 关闭文件输出（flush 并关闭）。
    static void closeLogFile();

    /// 控制台输出开关（默认开）。关闭后仅写文件。
    static void setConsoleEnabled(bool enabled);
    static bool consoleEnabled();

    // ========== 统一入口（带可选模块名） ==========
    static void debug(const QString& message, const QString& module = QString());
    static void info(const QString& message, const QString& module = QString());
    static void warning(const QString& message, const QString& module = QString());
    static void error(const QString& message, const QString& module = QString());

    /// 级别名（Debug/Info/Warning/Error）
    static QString levelName(McpLogLevel level);
};

} // namespace mcp_qt

// 便捷宏：MCP_LOG_DEBUG("msg") / MCP_LOG_WARN("module", "msg") 等
#define MCP_LOG_DEBUG(msg) mcp_qt::McpLogger::debug(msg)
#define MCP_LOG_INFO(msg) mcp_qt::McpLogger::info(msg)
#define MCP_LOG_WARN(msg) mcp_qt::McpLogger::warning(msg)
#define MCP_LOG_ERROR(msg) mcp_qt::McpLogger::error(msg)
#define MCP_LOG_DEBUG_MOD(module, msg) mcp_qt::McpLogger::debug(msg, module)
#define MCP_LOG_INFO_MOD(module, msg) mcp_qt::McpLogger::info(msg, module)
#define MCP_LOG_WARN_MOD(module, msg) mcp_qt::McpLogger::warning(msg, module)
#define MCP_LOG_ERROR_MOD(module, msg) mcp_qt::McpLogger::error(msg, module)
