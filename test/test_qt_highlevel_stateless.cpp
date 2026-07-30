#include "tests/common.h"
#include "mcp_qt_client/McpQtClient.h"
#include <QCoreApplication>
#include <QJsonObject>
#include <memory>

void test_qt_highlevel_builder_and_signals() {
    mcp_qt::McpQtClientBuilder builder;
    builder.setTransportStatelessHttp(QStringLiteral("http://127.0.0.1:8080/mcp"))
           .setClientInfo(QStringLiteral("test-agent"), QStringLiteral("2.0"))
           .setProtocolVersion(QStringLiteral("2026-07-28"))
           .setStatelessMode(true);

    auto client = builder.buildAndConnectAsync();
    TM_ASSERT_TRUE(client != nullptr, "client created from builder should not be null");
    TM_ASSERT_TRUE(client->isStatelessMode(), "client should reflect stateless mode from builder");

    bool signalReceived = false;
    QObject::connect(client.get(), &mcp_qt::McpQtClient::inputRequired,
                     [&signalReceived](const QString& reqId, const QJsonObject& schema, mcp_qt::MrtrReplyCallback replyCb) {
        signalReceived = true;
        TM_ASSERT_TRUE(!reqId.isEmpty(), "request ID should not be empty");
        TM_ASSERT_TRUE(schema.contains(QStringLiteral("properties")), "inputSchema should contain properties");
        replyCb(QJsonObject{{QStringLiteral("token"), QStringLiteral("abc-123")}});
    });

    TM_ASSERT_TRUE(client->nameSpace().isEmpty(), "default namespace is empty");
}
