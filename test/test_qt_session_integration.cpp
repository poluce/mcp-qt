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

void test_qt_session_exact_request_id_cancel() {
    // 请求方法返回精确 id：取消只影响目标请求，不影响并发中的其它请求
    auto transport = std::make_shared<MockTransport>();
    auto session = mcp::McpClientSession::connect(transport);

    bool cb1 = false, cb2 = false;
    int64_t id1 = session->sendRequest("tools/list", nlohmann::json::object(),
                                       [&](const nlohmann::json&, const nlohmann::json&) { cb1 = true; });
    int64_t id2 = session->sendRequest("prompts/list", nlohmann::json::object(),
                                       [&](const nlohmann::json&, const nlohmann::json&) { cb2 = true; });
    TM_ASSERT_TRUE(id1 > 0 && id2 > 0 && id1 != id2, "each request should return a distinct id");

    // 取消 id1：cb1 收到 cancelled 错误，cb2 不受影响
    session->cancelRequest(id1);
    TM_ASSERT_TRUE(cb1, "cancelled request callback should fire with error");
    TM_ASSERT_FALSE(cb2, "other in-flight request should be unaffected");

    // 响应 id2：cb2 正常完成
    nlohmann::json resp = {{"jsonrpc", "2.0"}, {"id", id2}, {"result", nlohmann::json::object()}};
    {
        std::lock_guard<std::mutex> lock(transport->m_state->mutex);
        if (transport->m_state->onMessage) {
            transport->m_state->onMessage(resp.dump());
        }
    }
    TM_ASSERT_TRUE(cb2, "uncancelled request should complete normally");
}
