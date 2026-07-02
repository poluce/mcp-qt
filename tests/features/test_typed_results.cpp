#include "mcp_qt_client/McpQtClient.h"
#include "mcp_qt_client/McpQtToolResult.h"
#include "tests/common.h"
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>

// 此测试文件由 tests_qt 目标编译执行，用于兑现计划文档中
// `tests/features/test_typed_results.cpp` 的真实测试落点要求。

static std::shared_ptr<mcp_qt::McpQtClient> createTypedResultClient(std::shared_ptr<MockTransport> transport) {
    auto client = mcp_qt::McpQtClient::createForTest();

    transport->onSendCallback = [transport](const std::string& msg) {
        auto json = nlohmann::json::parse(msg);
        if (json["method"] == "initialize") {
            int64_t id = json["id"].get<int64_t>();
            nlohmann::json resp = {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"result", {
                    {"protocolVersion", "2025-11-25"},
                    {"capabilities", {
                        {"tools", {{"listChanged", true}}},
                        {"resources", {{"subscribe", true}}}
                    }},
                    {"serverInfo", {
                        {"name", "mock-server"},
                        {"version", "1.0.0"}
                    }}
                }}
            };
            std::lock_guard<std::mutex> lock(transport->m_state->mutex);
            if (transport->m_state->onMessage) {
                transport->m_state->onMessage(resp.dump());
            }
        }
    };

    bool ok = client->connectToTransportAndWait(transport, "test-client", "1.0.0", 1000);
    if (!ok) return nullptr;
    return client;
}

void test_qt_typed_results() {
    auto transport = std::make_shared<MockTransport>();
    auto client = createTypedResultClient(transport);
    TM_ASSERT_TRUE(client != nullptr, "MockClient should be initialized");

    {
        transport->onSendCallback = [transport](const std::string& msg) {
            auto json = nlohmann::json::parse(msg);
            if (json["method"] == "tools/call") {
                int64_t id = json["id"].get<int64_t>();

                QByteArray pngMagic = QByteArray("\x89\x50\x4E\x47", 4);
                QString b64 = QString::fromUtf8(pngMagic.toBase64());

                nlohmann::json resp = {
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", {
                        {"content", {
                            {{"type", "text"}, {"text", "Add success"}},
                            {{"type", "image"}, {"mimeType", "image/png"}, {"data", b64.toStdString()}},
                            {{"type", "custom-widget"}, {"widgetId", "w-100"}}
                        }},
                        {"structuredContent", {
                            {"count", 42}
                        }}
                    }}
                };

                std::lock_guard<std::mutex> lock(transport->m_state->mutex);
                if (transport->m_state->onMessage) {
                    transport->m_state->onMessage(resp.dump());
                }
            }
        };

        auto result = client->callToolTyped("add", QJsonObject{{"a", 1}});

        TM_ASSERT_FALSE(result.isError, "Result should not be error");
        TM_ASSERT_EQ(result.content.size(), 3, "Should parse 3 content items");
        TM_ASSERT_TRUE(result.content[0].kind == mcp_qt::McpQtContentKind::Text, "First kind Text");
        TM_ASSERT_EQ(result.content[0].text.toStdString(), "Add success", "Text content value");
        TM_ASSERT_TRUE(result.content[1].kind == mcp_qt::McpQtContentKind::Image, "Second kind Image");
        TM_ASSERT_EQ(result.content[1].mimeType.toStdString(), "image/png", "Image mimeType");
        TM_ASSERT_EQ(result.content[1].binary.size(), 4, "Decoded image binary size");
        TM_ASSERT_EQ(result.content[1].binary[0], static_cast<char>(0x89), "PNG Magic 1");
        TM_ASSERT_TRUE(result.content[2].kind == mcp_qt::McpQtContentKind::Unknown, "Third kind Unknown");
        TM_ASSERT_EQ(result.content[2].raw.value("widgetId").toString().toStdString(), "w-100", "Preserved raw field");
        TM_ASSERT_EQ(result.structuredContent.value("count").toInt(), 42, "Structured content");
        TM_ASSERT_FALSE(result.raw.isEmpty(), "Raw JSON must be preserved");
    }

    {
        transport->onSendCallback = [transport](const std::string& msg) {
            auto json = nlohmann::json::parse(msg);
            if (json["method"] == "tools/call") {
                int64_t id = json["id"].get<int64_t>();
                nlohmann::json errResp = {
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"error", {
                        {"code", -32000},
                        {"message", "Invalid arguments to tool add"}
                    }}
                };
                std::lock_guard<std::mutex> lock(transport->m_state->mutex);
                if (transport->m_state->onMessage) {
                    transport->m_state->onMessage(errResp.dump());
                }
            }
        };

        auto result = client->callToolTyped("add", QJsonObject{{"a", -99}});
        TM_ASSERT_TRUE(result.isError, "Should capture tool error");
        TM_ASSERT_EQ(result.errorString.toStdString(), "Invalid arguments to tool add", "Error string propagation");
    }
}
