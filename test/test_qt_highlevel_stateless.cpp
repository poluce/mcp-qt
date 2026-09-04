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
                     [&signalReceived](const QString& reqId, const QJsonObject& inputRequests, const QString& requestState, mcp_qt::MrtrReplyCallback replyCb) {
        signalReceived = true;
        TM_ASSERT_TRUE(!reqId.isEmpty(), "request ID should not be empty");
        // 规范语义：requestState 原样透传，客户端只负责回显
        Q_UNUSED(requestState);
        replyCb(QJsonObject{{QStringLiteral("token"), QStringLiteral("abc-123")}});
    });

    TM_ASSERT_TRUE(client->nameSpace().isEmpty(), "default namespace is empty");
}
