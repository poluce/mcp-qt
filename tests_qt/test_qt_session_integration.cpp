#include "mcp_qt_transport/QtHttpSseTransport.h"
#include "mcp_core/McpClientSession.h"
#include "mcp_qt_client/McpQtClient.h"
#include "tests/common.h"
#include <QEventLoop>
#include <QTimer>

void test_qt_transport_keeps_core_session_api_shape() {
    auto transport = std::make_shared<mcp_qt::QtHttpSseTransport>("https://example.test/mcp");
    auto session = std::make_shared<mcp::McpClientSession>(transport);
    session->init();

    TM_ASSERT_TRUE(session->state() == mcp::SessionState::Uninitialized, "Core session should accept Qt transport");
}

void test_qt_connect_stdio_integration() {
    QStringList args;
    args << "-NoProfile" << "-NonInteractive" << "-Command"
         << R"(while (`$line = [Console]::ReadLine()) { if (`$line -match '"id":\s*([0-9]+)') { `$id = `$Matches[1]; [Console]::WriteLine('{"jsonrpc":"2.0","id":' + `$id + ',"result":{"protocolVersion":"2025-11-25","capabilities":{},"serverInfo":{"name":"mock-stdio","version":"1.0"}}}'); [Console]::Out.Flush() } })";

    QString err;
    auto client = mcp_qt::McpQtClient::connectStdioAndWait("powershell", args, "test-client", "1.0", 20000, &err);
    
    if (client == nullptr) {
        std::cout << "connectStdioAndWait failed with error: " << err.toStdString() << std::endl;
    }
    TM_ASSERT_TRUE(client != nullptr, "client should successfully start and initialize over stdio");
    if (client) {
        TM_ASSERT_TRUE(client->isConnected(), "client should be in connected state");
    }
    
    if (client) {
        client->close();
    }
}
