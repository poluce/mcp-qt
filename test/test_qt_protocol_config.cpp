#include "tests/common.h"
#include "mcp_qt_client/McpServerManager.h"
#include "mcp_qt_client/McpLogger.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QFile>
#include <QCoreApplication>
#include <QThread>

// ============================================================================
// 协议配置校验：type 与 protocolVersion 矛盾时输出警告（McpServerManager）
// ============================================================================

void test_qt_protocol_config_validation() {
    QTemporaryDir tmpDir;
    TM_ASSERT_TRUE(tmpDir.isValid(), "temp dir should be valid");
    const QString logPath = tmpDir.filePath(QStringLiteral("cfg-warn.log"));
    TM_ASSERT_TRUE(mcp_qt::McpLogger::setLogFile(logPath), "setLogFile should succeed");

    // 矛盾配置 1：type=stateless_http 但 protocolVersion=2025-11-25（应为 2026-07-28）
    mcp_qt::McpServerConfig c1;
    c1.serverName = QStringLiteral("s1");
    c1.type = QStringLiteral("stateless_http");
    c1.url = QStringLiteral("http://127.0.0.1:1/mcp");
    c1.protocolVersion = QStringLiteral("2025-11-25");

    // 矛盾配置 2：type=sse 但 protocolVersion=2026-07-28（无状态协议建议 stateless_http）
    mcp_qt::McpServerConfig c2;
    c2.serverName = QStringLiteral("s2");
    c2.type = QStringLiteral("sse");
    c2.url = QStringLiteral("http://127.0.0.1:1/mcp");
    c2.protocolVersion = QStringLiteral("2026-07-28");

    {
        // loadServers 同步调用 startServer → configureBuilder（警告同步输出），
        // 连接本身异步且指向不可达端口，随后 closeAll 清理。
        mcp_qt::McpServerManager manager;
        manager.loadServers({c1});
        QCoreApplication::processEvents();
        QThread::msleep(50);
        manager.closeAll(1000);
    }
    {
        mcp_qt::McpServerManager manager;
        manager.loadServers({c2});
        QCoreApplication::processEvents();
        QThread::msleep(50);
        manager.closeAll(1000);
    }

    mcp_qt::McpLogger::closeLogFile();

    QFile f(logPath);
    TM_ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text), "log file should be readable");
    const QByteArray content = f.readAll();
    f.close();

    TM_ASSERT_TRUE(content.contains("stateless_http 与 protocolVersion=2025-11-25 组合矛盾"),
                   "config1 should produce contradiction warning");
    TM_ASSERT_TRUE(content.contains("type=sse 与 protocolVersion=2026-07-28 组合"),
                   "config2 should produce combination warning");
}
