#include "tests/common.h"
#include "mcp_qt_client/McpLogger.h"
#include <QFile>
#include <QTemporaryDir>
#include <QCoreApplication>

// ============================================================================
// McpLogger：统一日志门面（全局级别控制 + 文件落盘）
// ============================================================================

void test_qt_logger_level_filtering() {
    // 默认 Info：Debug 被过滤
    mcp_qt::McpLogger::setGlobalLevel(mcp_qt::McpLogLevel::Info);
    TM_ASSERT_TRUE(mcp_qt::McpLogger::globalLevel() == mcp_qt::McpLogLevel::Info,
                   "global level should be Info");

    // 级别名
    TM_ASSERT_EQ(mcp_qt::McpLogger::levelName(mcp_qt::McpLogLevel::Debug).toStdString(), std::string("Debug"),
                 "level name Debug");
    TM_ASSERT_EQ(mcp_qt::McpLogger::levelName(mcp_qt::McpLogLevel::Error).toStdString(), std::string("Error"),
                 "level name Error");

    // 切到 Error：Warning 也被过滤（不崩溃即可，输出由级别控制）
    mcp_qt::McpLogger::setGlobalLevel(mcp_qt::McpLogLevel::Error);
    mcp_qt::McpLogger::warning(QStringLiteral("should be filtered at Error level"), QStringLiteral("test"));
    mcp_qt::McpLogger::error(QStringLiteral("should be emitted at Error level"), QStringLiteral("test"));

    // 恢复默认
    mcp_qt::McpLogger::setGlobalLevel(mcp_qt::McpLogLevel::Info);
}

void test_qt_logger_file_output() {
    QTemporaryDir tmpDir;
    TM_ASSERT_TRUE(tmpDir.isValid(), "temp dir should be valid");
    const QString logPath = tmpDir.filePath(QStringLiteral("mcp-qt.log"));

    // 启用文件输出
    TM_ASSERT_TRUE(mcp_qt::McpLogger::setLogFile(logPath), "setLogFile should succeed");

    // 写几条日志（含模块名）
    mcp_qt::McpLogger::info(QStringLiteral("logger test info"), QStringLiteral("test-module"));
    mcp_qt::McpLogger::warning(QStringLiteral("logger test warning"), QStringLiteral("test-module"));
    mcp_qt::McpLogger::error(QStringLiteral("logger test error"), QStringLiteral("test-module"));

    // 关闭文件（flush）
    mcp_qt::McpLogger::closeLogFile();

    // 验证文件内容
    QFile f(logPath);
    TM_ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text), "log file should be readable");
    const QByteArray content = f.readAll();
    f.close();

    TM_ASSERT_TRUE(content.contains("logger test info"), "file should contain info line");
    TM_ASSERT_TRUE(content.contains("logger test warning"), "file should contain warning line");
    TM_ASSERT_TRUE(content.contains("logger test error"), "file should contain error line");
    TM_ASSERT_TRUE(content.contains("[Info]"), "line should carry level tag");
    TM_ASSERT_TRUE(content.contains("[test-module]"), "line should carry module tag");
    TM_ASSERT_TRUE(content.contains("2026-"), "line should carry timestamp");

    // 追加模式：再写一条，文件应追加而非覆盖
    TM_ASSERT_TRUE(mcp_qt::McpLogger::setLogFile(logPath), "reopen log file");
    mcp_qt::McpLogger::info(QStringLiteral("second write"), QStringLiteral("test-module"));
    mcp_qt::McpLogger::closeLogFile();

    QFile f2(logPath);
    TM_ASSERT_TRUE(f2.open(QIODevice::ReadOnly | QIODevice::Text), "log file should be readable again");
    const QByteArray content2 = f2.readAll();
    f2.close();
    TM_ASSERT_TRUE(content2.contains("second write"), "append mode should keep previous lines");
    TM_ASSERT_TRUE(content2.contains("logger test info"), "append mode should not overwrite");

    // 控制台开关（不崩溃即可）
    mcp_qt::McpLogger::setConsoleEnabled(false);
    TM_ASSERT_TRUE(!mcp_qt::McpLogger::consoleEnabled(), "console should be disabled");
    mcp_qt::McpLogger::info(QStringLiteral("console off test"), QStringLiteral("test-module"));
    mcp_qt::McpLogger::setConsoleEnabled(true);
    TM_ASSERT_TRUE(mcp_qt::McpLogger::consoleEnabled(), "console should be re-enabled");
}
