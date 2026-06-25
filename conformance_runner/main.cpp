#include <iostream>
#include <memory>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <cassert>
#include "mcp_core/ConsoleStdioTransport.h"
#include "mcp_core/McpClientSession.h"

// Memory transport mock class to dynamically simulate server behaviors locally
class MockTransport : public mcp::IMcpTransport {
public:
    std::string lastSentMessage;
    std::function<void(const std::string&)> onSendCallback;
    std::function<void(const std::string&)> m_onMessage;
    std::function<void()> m_onClose;

    bool send(const std::string& message) override {
        lastSentMessage = message;
        if (onSendCallback) {
            onSendCallback(message);
        }
        return true;
    }
    void setOnMessage(std::function<void(const std::string&)> callback) override {
        m_onMessage = std::move(callback);
    }
    void setOnClose(std::function<void()> callback) override {
        m_onClose = std::move(callback);
    }
    void setOnError(std::function<void(const std::string&)>) override {}
    bool start() override { return true; }
    void close() override { if (m_onClose) m_onClose(); }

    void pushServerMessage(const std::string& msg) {
        if (m_onMessage) {
            m_onMessage(msg);
        }
    }
};

// Automate lifecycle scenario assertions in a closed-loop local test suite
void runLocalLifecycleTests() {
    std::cout << "========================================\n";
    std::cout << "  C++ MCP SDK Lifecycle Local Test Suite\n";
    std::cout << "========================================\n\n";

    // ----------------------------------------------------
    // Scenario 1: tools/list before initialize (Should be intercepted)
    // ----------------------------------------------------
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        bool interceptCorrect = false;
        session->listTools([&](const std::vector<mcp::McpTool>&, const mcp::json& error) {
            if (!error.empty() && error.contains("code") && error["code"] == -32002) {
                interceptCorrect = true;
            }
        });

        assert(interceptCorrect && "Scenario 1 Failed: Sending tools/list before initialization should be intercepted locally.");
        assert(transport->lastSentMessage.empty() && "Scenario 1 Failed: Stdio packet should not be sent out on interception.");
        std::cout << "[✓] Scenario 1: Intercept business request before initialization\n";
    }

    // ----------------------------------------------------
    // Scenario 2: Normal initialize
    // ----------------------------------------------------
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        bool initializedNotificationSent = false;
        transport->onSendCallback = [&](const std::string& msg) {
            mcp::json j = mcp::json::parse(msg);
            if (j.contains("method") && j["method"] == "notifications/initialized") {
                initializedNotificationSent = true;
            }
        };

        bool initSuccess = false;
        session->initialize("test-client", "1.0.0", [&](bool success, const mcp::json&) {
            initSuccess = success;
        });

        mcp::json mockResponse = {
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"result", {
                {"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION},
                {"capabilities", mcp::json::object()},
                {"serverInfo", {{"name", "mock-server"}, {"version", "1.0.0"}}}
            }}
        };
        transport->pushServerMessage(mockResponse.dump());

        assert(initSuccess && "Scenario 2 Failed: Normal initialize handshake failed.");
        assert(initializedNotificationSent && "Scenario 2 Failed: initialized notification was not sent.");
        assert(session->state() == mcp::SessionState::Initialized && "Scenario 2 Failed: state was not updated to Initialized.");
        std::cout << "[✓] Scenario 2: Normal initialize handshake and notification\n";
    }

    // ----------------------------------------------------
    // Scenario 3: Duplicate initialize (Should be blocked)
    // ----------------------------------------------------
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        session->initialize("test-client", "1.0.0", [](bool, const mcp::json&){});

        bool repeatIntercept = false;
        session->initialize("test-client", "1.0.0", [&](bool success, const mcp::json& error) {
            if (!success && error.contains("code") && error["code"] == -32600) {
                repeatIntercept = true;
            }
        });

        assert(repeatIntercept && "Scenario 3 Failed: Duplicate initialization calls should be intercepted.");
        std::cout << "[✓] Scenario 3: Prevent duplicate initialize calls\n";
    }

    // ----------------------------------------------------
    // Scenario 4: Server returns unsupported protocolVersion / Mismatch
    // ----------------------------------------------------
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        bool initializedNotificationSent = false;
        transport->onSendCallback = [&](const std::string& msg) {
            mcp::json j = mcp::json::parse(msg);
            if (j.contains("method") && j["method"] == "notifications/initialized") {
                initializedNotificationSent = true;
            }
        };

        bool initSuccess = true; 
        session->initialize("test-client", "1.0.0", [&](bool success, const mcp::json&) {
            initSuccess = success;
        });

        // Simulate server returning mismatched version "2024-11-05" instead of "2025-11-25"
        mcp::json mockResponse = {
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"result", {
                {"protocolVersion", "2024-11-05"},
                {"capabilities", mcp::json::object()},
                {"serverInfo", {{"name", "old-mock-server"}, {"version", "1.0.0"}}}
            }}
        };
        transport->pushServerMessage(mockResponse.dump());

        assert(!initSuccess && "Scenario 4 Failed: Handshake should fail on version mismatch.");
        assert(!initializedNotificationSent && "Scenario 4 Failed: initialized notification must not be sent on mismatch.");
        assert(session->state() == mcp::SessionState::Uninitialized && "Scenario 4 Failed: state should rollback to Uninitialized.");
        std::cout << "[✓] Scenario 4: Unmatched protocolVersion negotiation and rollback\n";
    }

    // ----------------------------------------------------
    // Scenario 5: Normal tools/list after initialized
    // ----------------------------------------------------
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        session->initialize("test-client", "1.0.0", [](bool, const mcp::json&){});
        mcp::json mockResponse = {
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"result", {
                {"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION},
                {"capabilities", mcp::json::object()},
                {"serverInfo", {{"name", "mock"}, {"version", "1"}}}
            }}
        };
        transport->pushServerMessage(mockResponse.dump());

        bool toolsGot = false;
        session->listTools([&](const std::vector<mcp::McpTool>& tools, const mcp::json&) {
            if (tools.size() == 1 && tools[0].name == "test_tool") {
                toolsGot = true;
            }
        });

        mcp::json toolsListResponse = {
            {"jsonrpc", "2.0"},
            {"id", 2},
            {"result", {
                {"tools", mcp::json::array({
                    {{"name", "test_tool"}, {"description", "testDesc"}, {"inputSchema", mcp::json::object()}}
                })}
            }}
        };
        transport->pushServerMessage(toolsListResponse.dump());

        assert(toolsGot && "Scenario 5 Failed: Cannot query tools/list after initialization.");
        std::cout << "[✓] Scenario 5: Business tools/list query after initialized\n";
    }

    std::cout << "\n========================================\n";
    std::cout << "  🎉 🎉 🎉 All Lifecycle self-tests PASSED!\n";
    std::cout << "========================================\n";
}

void runLocalToolsTests() {
    std::cout << "\n========================================\n";
    std::cout << "  C++ MCP SDK Tools Local Test Suite\n";
    std::cout << "========================================\n\n";

    // ----------------------------------------------------
    // Scenario 1: Tool Name Format Validation
    // ----------------------------------------------------
    {
        assert(mcp::isValidToolName("calculate_add"));
        assert(mcp::isValidToolName("get-system-info"));
        assert(mcp::isValidToolName("test.tool"));
        assert(mcp::isValidToolName("12345"));
        
        assert(!mcp::isValidToolName("")); // Empty
        assert(!mcp::isValidToolName("tools list")); // Space
        assert(!mcp::isValidToolName("calculate*add")); // Asterisk
        assert(!mcp::isValidToolName("tool/call")); // Slash
        assert(!mcp::isValidToolName(std::string(129, 'a'))); // Length > 128
        std::cout << "[✓] Scenario 1: Tool Name constraints and regex matching\n";
    }

    // ----------------------------------------------------
    // Scenario 2: Paginated listTools Discovery
    // ----------------------------------------------------
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();
        
        // Mock init state
        session->initialize("test", "1", [](bool, const mcp::json&){});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        // First page listTools (no cursor)
        bool page1Success = false;
        std::string page1NextCursor;
        session->listTools("", [&](const std::vector<mcp::McpTool>& tools, const std::string& nextCursor, const mcp::json& error) {
            if (error.empty() && tools.size() == 2 && nextCursor == "page_2") {
                if (tools[0].name == "calculate_add" && tools[1].name == "get_system_time") {
                    page1Success = true;
                    page1NextCursor = nextCursor;
                }
            }
        });

        // Mock Server responses for page 1
        mcp::json firstPageResp = {
            {"jsonrpc", "2.0"},
            {"id", 2},
            {"result", {
                {"tools", mcp::json::array({
                    {{"name", "calculate_add"}, {"description", "add"}, {"inputSchema", mcp::json::object()}},
                    {{"name", "get_system_time"}, {"description", "time"}, {"inputSchema", {{"type", "object"}, {"properties", mcp::json::object()}}}}
                })},
                {"nextCursor", "page_2"}
            }}
        };
        transport->pushServerMessage(firstPageResp.dump());
        assert(page1Success && "Scenario 2 Failed: First page listTools with cursor failed.");

        // Second page listTools (with page1NextCursor)
        bool page2Success = false;
        session->listTools(page1NextCursor, [&](const std::vector<mcp::McpTool>& tools, const std::string& nextCursor, const mcp::json& error) {
            if (error.empty() && tools.size() == 2 && nextCursor.empty()) {
                if (tools[0].name == "get_system_info" && tools[1].name == "trigger_exception") {
                    page2Success = true;
                }
            }
        });

        mcp::json secondPageResp = {
            {"jsonrpc", "2.0"},
            {"id", 3},
            {"result", {
                {"tools", mcp::json::array({
                    {{"name", "get_system_info"}, {"description", "info"}, {"inputSchema", mcp::json::object()}},
                    {{"name", "trigger_exception"}, {"description", "exc"}, {"inputSchema", mcp::json::object()}}
                })}
            }}
        };
        transport->pushServerMessage(secondPageResp.dump());
        assert(page2Success && "Scenario 2 Failed: Second page listTools with cursor failed.");
        std::cout << "[✓] Scenario 2: Paginated tool listing and cursor passing\n";
    }

    // ----------------------------------------------------
    // Scenario 3: tools/call missing params, tool not found, and isError exception handling
    // ----------------------------------------------------
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();
        
        session->initialize("test", "1", [](bool, const mcp::json&){});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        // 3.1: Parameter missing standard error
        bool paramErrorSuccess = false;
        session->callTool("calculate_add", mcp::json::object(), [&](const mcp::json&, const mcp::json& error) {
            if (!error.empty() && error.contains("code") && error["code"] == -32602) {
                paramErrorSuccess = true;
            }
        });
        mcp::json errParamResp = {
            {"jsonrpc", "2.0"},
            {"id", 2},
            {"error", {{"code", -32602}, {"message", "Missing required arguments: a or b"}}}
        };
        transport->pushServerMessage(errParamResp.dump());
        assert(paramErrorSuccess && "Scenario 3.1 Failed: parameter missing should return standard error.");

        // 3.2: Tool not found standard error
        bool toolNotFoundSuccess = false;
        session->callTool("unknown_tool", mcp::json::object(), [&](const mcp::json&, const mcp::json& error) {
            if (!error.empty() && error.contains("code") && error["code"] == -32601) {
                toolNotFoundSuccess = true;
            }
        });
        mcp::json errNotFoundResp = {
            {"jsonrpc", "2.0"},
            {"id", 3},
            {"error", {{"code", -32601}, {"message", "Tool not found"}}}
        };
        transport->pushServerMessage(errNotFoundResp.dump());
        assert(toolNotFoundSuccess && "Scenario 3.2 Failed: calling non-existent tool should return -32601.");

        // 3.3: Execution isError returns true (Application level error)
        bool execErrorSuccess = false;
        session->callTool("trigger_exception", mcp::json::object(), [&](const mcp::json& result, const mcp::json& error) {
            if (error.empty() && result.contains("isError") && result["isError"] == true) {
                execErrorSuccess = true;
            }
        });
        mcp::json isErrorResp = {
            {"jsonrpc", "2.0"},
            {"id", 4},
            {"result", {
                {"content", mcp::json::array({{{"type", "text"}, {"text", "db failed"}}})},
                {"isError", true}
            }}
        };
        transport->pushServerMessage(isErrorResp.dump());
        assert(execErrorSuccess && "Scenario 3.3 Failed: execution failure isError mapping failed.");
        std::cout << "[✓] Scenario 3: Standard tools/call exceptions and application-level isError mapping\n";
    }

    std::cout << "\n========================================\n";
    std::cout << "  🎉 🎉 🎉 All Tools self-tests PASSED!\n";
    std::cout << "========================================\n";
}

void runLocalResourcesAndPromptsTests() {
    std::cout << "\n==================================================\n";
    std::cout << "  C++ MCP SDK Resources & Prompts Local Test Suite\n";
    std::cout << "==================================================\n\n";

    // ----------------------------------------------------
    // Scenario 1: Paginated listResources Discovery
    // ----------------------------------------------------
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();
        
        session->initialize("test", "1", [](bool, const mcp::json&){});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        // First page listResources (no cursor)
        bool page1Success = false;
        std::string page1NextCursor;
        session->listResources("", [&](const mcp::json& result, const std::string& nextCursor, const mcp::json& error) {
            if (error.empty() && result.contains("resources") && result["resources"].is_array() && result["resources"].size() == 2 && nextCursor == "res_page_2") {
                page1Success = true;
                page1NextCursor = nextCursor;
            }
        });

        mcp::json page1Resp = {
            {"jsonrpc", "2.0"},
            {"id", 2},
            {"result", {
                {"resources", mcp::json::array({
                    {{"uri", "file:///logs/system.log"}, {"name", "sysLog"}, {"mimeType", "text/plain"}},
                    {{"uri", "file:///configs/app.json"}, {"name", "appJson"}, {"mimeType", "application/json"}}
                })},
                {"nextCursor", "res_page_2"}
            }}
        };
        transport->pushServerMessage(page1Resp.dump());
        assert(page1Success && "Resources Scenario 1 Failed: first page resources listing failed.");

        // Second page listResources (with cursor)
        bool page2Success = false;
        session->listResources(page1NextCursor, [&](const mcp::json& result, const std::string& nextCursor, const mcp::json& error) {
            if (error.empty() && result.contains("resources") && result["resources"].size() == 1 && nextCursor.empty()) {
                page2Success = true;
            }
        });

        mcp::json page2Resp = {
            {"jsonrpc", "2.0"},
            {"id", 3},
            {"result", {
                {"resources", mcp::json::array({
                    {{"uri", "file:///assets/logo.png"}, {"name", "logo"}, {"mimeType", "image/png"}}
                })}
            }}
        };
        transport->pushServerMessage(page2Resp.dump());
        assert(page2Success && "Resources Scenario 1 Failed: second page resources listing failed.");
        std::cout << "[✓] Scenario 1: Paginated resource listing and cursor parsing\n";
    }

    // ----------------------------------------------------
    // Scenario 2: resources/read Permission Denied, Large File, and Binary data with MIME type
    // ----------------------------------------------------
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();
        
        session->initialize("test", "1", [](bool, const mcp::json&){});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        // 2.1: Permission Denied error code -32000
        bool permissionDeniedSuccess = false;
        session->readResource("file:///configs/admin.json", [&](const mcp::json&, const mcp::json& error) {
            if (!error.empty() && error.contains("code") && error["code"] == -32000) {
                permissionDeniedSuccess = true;
            }
        });
        mcp::json errPermissionResp = {
            {"jsonrpc", "2.0"},
            {"id", 2},
            {"error", {{"code", -32000}, {"message", "Permission Denied"}}}
        };
        transport->pushServerMessage(errPermissionResp.dump());
        assert(permissionDeniedSuccess && "Resources Scenario 2.1 Failed: admin config should return permission denied.");

        // 2.2: Large file mock check (2MB)
        bool hugeFileSuccess = false;
        session->readResource("file:///logs/huge.log", [&](const mcp::json& result, const mcp::json& error) {
            if (error.empty() && result.contains("contents") && result["contents"].is_array()) {
                std::string text = result["contents"][0]["text"].get<std::string>();
                if (text.length() == 2 * 1024 * 1024) {
                    hugeFileSuccess = true;
                }
            }
        });
        mcp::json hugeFileResp = {
            {"jsonrpc", "2.0"},
            {"id", 3},
            {"result", {
                {"contents", mcp::json::array({{
                    {"uri", "file:///logs/huge.log"},
                    {"mimeType", "text/plain"},
                    {"text", std::string(2 * 1024 * 1024, 'H')}
                }})}
            }}
        };
        transport->pushServerMessage(hugeFileResp.dump());
        assert(hugeFileSuccess && "Resources Scenario 2.2 Failed: large resource content parsing failed.");

        // 2.3: Binary base64 data and MIME check
        bool binaryFileSuccess = false;
        session->readResource("file:///assets/logo.png", [&](const mcp::json& result, const mcp::json& error) {
            if (error.empty() && result.contains("contents")) {
                auto contentItem = result["contents"][0];
                if (contentItem.contains("blob") && contentItem["mimeType"] == "image/png") {
                    binaryFileSuccess = true;
                }
            }
        });
        mcp::json binaryResp = {
            {"jsonrpc", "2.0"},
            {"id", 4},
            {"result", {
                {"contents", mcp::json::array({{
                    {"uri", "file:///assets/logo.png"},
                    {"mimeType", "image/png"},
                    {"blob", "iVBORw0KGgoAAAANSUhEUgAAAAUA"}
                }})}
            }}
        };
        transport->pushServerMessage(binaryResp.dump());
        assert(binaryFileSuccess && "Resources Scenario 2.3 Failed: binary data or MIME mapping failed.");
        std::cout << "[✓] Scenario 2: Resource reading boundaries (403 Permission Denied, 2MB Large file, Binary base64 blob with MIME)\n";
    }

    // ----------------------------------------------------
    // Scenario 3: Resource Subscriptions and updated Notification
    // ----------------------------------------------------
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();
        
        session->initialize("test", "1", [](bool, const mcp::json&){});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        // 3.1: Subscribe to resource
        bool subscribeSuccess = false;
        session->subscribeResource("file:///logs/system.log", [&](bool success, const mcp::json& error) {
            if (success && error.empty()) {
                subscribeSuccess = true;
            }
        });
        mcp::json subResp = {{"jsonrpc", "2.0"}, {"id", 2}, {"result", mcp::json::object()}};
        transport->pushServerMessage(subResp.dump());
        assert(subscribeSuccess && "Resources Scenario 3.1 Failed: resource subscription failed.");

        // 3.2: Listen to notifications/resources/updated notification
        bool notificationReceived = false;
        session->registerNotificationHandler("notifications/resources/updated", [&](const mcp::json& params) {
            if (params.contains("uri") && params["uri"] == "file:///logs/system.log") {
                notificationReceived = true;
            }
        });

        // Server pushes resources update notification
        mcp::json notifyMsg = {
            {"jsonrpc", "2.0"},
            {"method", "notifications/resources/updated"},
            {"params", {{"uri", "file:///logs/system.log"}}}
        };
        transport->pushServerMessage(notifyMsg.dump());
        assert(notificationReceived && "Resources Scenario 3.2 Failed: subscription notification not received.");

        // 3.3: Unsubscribe from resource
        bool unsubscribeSuccess = false;
        session->unsubscribeResource("file:///logs/system.log", [&](bool success, const mcp::json& error) {
            if (success && error.empty()) {
                unsubscribeSuccess = true;
            }
        });
        mcp::json unsubResp = {{"jsonrpc", "2.0"}, {"id", 3}, {"result", mcp::json::object()}};
        transport->pushServerMessage(unsubResp.dump());
        assert(unsubscribeSuccess && "Resources Scenario 3.3 Failed: resource unsubscription failed.");
        std::cout << "[✓] Scenario 3: Resource subscription, unsubscription, and updated notifications\n";
    }

    // ----------------------------------------------------
    // Scenario 4: Prompts listing, get, missing argument, and type validation
    // ----------------------------------------------------
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();
        
        session->initialize("test", "1", [](bool, const mcp::json&){});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        // 4.1: Paginated prompt listing
        bool promptListSuccess = false;
        std::string promptNextCursor;
        session->listPrompts("", [&](const mcp::json& result, const std::string& nextCursor, const mcp::json& error) {
            if (error.empty() && result.contains("prompts") && result["prompts"].size() == 1 && nextCursor == "prompt_page_2") {
                promptListSuccess = true;
                promptNextCursor = nextCursor;
            }
        });
        mcp::json pList1Resp = {
            {"jsonrpc", "2.0"},
            {"id", 2},
            {"result", {
                {"prompts", mcp::json::array({{{"name", "code_review"}, {"description", "review"}}})},
                {"nextCursor", "prompt_page_2"}
            }}
        };
        transport->pushServerMessage(pList1Resp.dump());
        assert(promptListSuccess && "Prompts Scenario 4.1 Failed: prompt list first page failed.");

        // 4.2: prompts/get Missing argument error
        bool argMissingSuccess = false;
        session->getPrompt("code_review", mcp::json::object(), [&](const mcp::json&, const mcp::json& error) {
            if (!error.empty() && error.contains("code") && error["code"] == -32602) {
                argMissingSuccess = true;
            }
        });
        mcp::json errMissingResp = {
            {"jsonrpc", "2.0"},
            {"id", 3},
            {"error", {{"code", -32602}, {"message", "Missing required argument: code"}}}
        };
        transport->pushServerMessage(errMissingResp.dump());
        assert(argMissingSuccess && "Prompts Scenario 4.2 Failed: missing argument should return -32602.");

        // 4.3: prompts/get Argument type mismatch error
        bool typeMismatchSuccess = false;
        session->getPrompt("code_review", {{"code", 12345}}, [&](const mcp::json&, const mcp::json& error) {
            if (!error.empty() && error.contains("code") && error["code"] == -32602) {
                typeMismatchSuccess = true;
            }
        });
        mcp::json errTypeResp = {
            {"jsonrpc", "2.0"},
            {"id", 4},
            {"error", {{"code", -32602}, {"message", "Argument must be string"}}}
        };
        transport->pushServerMessage(errTypeResp.dump());
        assert(typeMismatchSuccess && "Prompts Scenario 4.3 Failed: type mismatch should return -32602.");
        std::cout << "[✓] Scenario 4: Prompt list discovery, missing parameters, and argument type checks\n";
    }

    // ----------------------------------------------------
    // Scenario 5: Rich prompt contents (text, image, embedded resource) and listChanged Notification
    // ----------------------------------------------------
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();
        
        session->initialize("test", "1", [](bool, const mcp::json&){});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        // 5.1: Multi-media prompt result (text, image, resource contents)
        bool richPromptSuccess = false;
        session->getPrompt("rich_prompt", mcp::json::object(), [&](const mcp::json& result, const mcp::json& error) {
            if (error.empty() && result.contains("messages")) {
                auto contentArr = result["messages"][0]["content"];
                if (contentArr.size() == 3) {
                    if (contentArr[0]["type"] == "text" &&
                        contentArr[1]["type"] == "image" &&
                        contentArr[2]["type"] == "resource") {
                        richPromptSuccess = true;
                    }
                }
            }
        });
        mcp::json richPromptResp = {
            {"jsonrpc", "2.0"},
            {"id", 2},
            {"result", {
                {"description", "rich"},
                {"messages", mcp::json::array({{
                    {"role", "assistant"},
                    {"content", mcp::json::array({
                        {{"type", "text"}, {"text", "analysis"}},
                        {{"type", "image"}, {"data", "Base64..."}, {"mimeType", "image/jpeg"}},
                        {{"type", "resource"}, {"resource", {{"uri", "file:///logs/system.log"}, {"text", "[Embedded Logs]"}}}}
                    })}
                }})}
            }}
        };
        transport->pushServerMessage(richPromptResp.dump());
        assert(richPromptSuccess && "Prompts Scenario 5.1 Failed: rich media prompt content parsing failed.");

        // 5.2: prompts/listChanged Notification
        bool promptsChangedReceived = false;
        session->registerNotificationHandler("notifications/prompts/list-changed", [&](const mcp::json&) {
            promptsChangedReceived = true;
        });

        mcp::json notifyMsg = {
            {"jsonrpc", "2.0"},
            {"method", "notifications/prompts/list-changed"},
            {"params", mcp::json::object()}
        };
        transport->pushServerMessage(notifyMsg.dump());
        assert(promptsChangedReceived && "Prompts Scenario 5.2 Failed: prompts list-changed notification failed.");
        std::cout << "[✓] Scenario 5: Prompt content formats (text, image, embedded resources) and list-changed notifications\n";
    }

    std::cout << "\n==================================================\n";
    std::cout << "  🎉 🎉 🎉 All Resources & Prompts self-tests PASSED!\n";
    std::cout << "==================================================\n";
}

void runLocalErrorTests() {
    std::cout << "\n==================================================\n";
    std::cout << "  C++ MCP SDK Error Handling & Defensiveness Local Test Suite\n";
    std::cout << "==================================================\n\n";

    // ----------------------------------------------------
    // Scenario 1: JSON Parsing error (Robustness check)
    // ----------------------------------------------------
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        try {
            transport->pushServerMessage("{invalid json: error");
            transport->pushServerMessage("");
            transport->pushServerMessage("   ");
            transport->pushServerMessage("[]");
        } catch (...) {
            assert(false && "Scenario 1 Failed: Incoming malformed JSON or empty message crashed the client.");
        }
        std::cout << "[✓] Scenario 1: Malformed JSON packet parsing defensiveness\n";
    }

    // ----------------------------------------------------
    // Scenario 2: Server-side unknown method request (Auto reply Method not found)
    // ----------------------------------------------------
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        bool methodNotFoundSent = false;
        transport->onSendCallback = [&](const std::string& msg) {
            try {
                mcp::json j = mcp::json::parse(msg);
                if (j.contains("error") && j["error"]["code"] == -32601) {
                    methodNotFoundSent = true;
                }
            } catch (...) {}
        };

        mcp::json unknownReq = {
            {"jsonrpc", "2.0"},
            {"id", 888},
            {"method", "custom/unsupportedMethod"},
            {"params", mcp::json::object()}
        };
        transport->pushServerMessage(unknownReq.dump());

        assert(methodNotFoundSent && "Scenario 2 Failed: Client did not reply with Method not found (-32601) error.");
        std::cout << "[✓] Scenario 2: Handle server-side unknown method requests\n";
    }

    // ----------------------------------------------------
    // Scenario 3: Unknown / Mismatched Response ID (Fault isolation)
    // ----------------------------------------------------
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        try {
            mcp::json unknownResp = {
                {"jsonrpc", "2.0"},
                {"id", 99999},
                {"result", {{"status", "ignored"}}}
            };
            transport->pushServerMessage(unknownResp.dump());

            mcp::json invalidIdResp = {
                {"jsonrpc", "2.0"},
                {"id", mcp::json::array({1, 2})},
                {"result", {{"status", "ignored"}}}
            };
            transport->pushServerMessage(invalidIdResp.dump());
        } catch (...) {
            assert(false && "Scenario 3 Failed: Client crashed on unknown or malformed response id.");
        }
        std::cout << "[✓] Scenario 3: Ignore unknown or type-mismatched response IDs (Crash prevention)\n";
    }

    // ----------------------------------------------------
    // Scenario 4: Request Timeout check and Lazy cleanup
    // ----------------------------------------------------
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        session->initialize("test", "1", [](bool, const mcp::json&){});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        bool timeoutTriggered = false;
        session->listTools([&](const std::vector<mcp::McpTool>&, const mcp::json& error) {
            if (!error.empty() && error.contains("code") && error["code"] == -32001) {
                timeoutTriggered = true;
            }
        });

        assert(!timeoutTriggered && "Scenario 4 Failed: Timeout triggered prematurely.");

        session->checkRequestTimeouts(std::chrono::milliseconds(0));

        assert(timeoutTriggered && "Scenario 4 Failed: Request was not timed out properly.");
        std::cout << "[✓] Scenario 4: Scan and invoke callbacks for timed out requests\n";
    }

    // ----------------------------------------------------
    // Scenario 5: Active Cancellation and notification
    // ----------------------------------------------------
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        session->initialize("test", "1", [](bool, const mcp::json&){});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        bool cancelledCallbackTriggered = false;
        bool cancelledNotificationSent = false;
        int64_t reqId = 0;

        transport->onSendCallback = [&](const std::string& msg) {
            try {
                mcp::json j = mcp::json::parse(msg);
                if (j.contains("method") && j["method"] == "notifications/cancelled") {
                    if (j.contains("params") && j["params"]["requestId"] == reqId) {
                        cancelledNotificationSent = true;
                    }
                }
            } catch (...) {}
        };

        reqId = session->sendRequest("tools/call", {{"name", "calculate_add"}}, [&](const mcp::json&, const mcp::json& error) {
            if (!error.empty() && error.contains("code") && error["code"] == -32000) {
                cancelledCallbackTriggered = true;
            }
        });

        session->cancelRequest(reqId);

        assert(cancelledCallbackTriggered && "Scenario 5 Failed: Cancelled callback was not triggered with cancel code.");
        assert(cancelledNotificationSent && "Scenario 5 Failed: notifications/cancelled protocol message not sent.");
        std::cout << "[✓] Scenario 5: Active cancellation locally and remote protocol notification\n";
    }

    // ----------------------------------------------------
    // Scenario 6: Server crash / Connection Interrupted (连接中断清理)
    // ----------------------------------------------------
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        session->initialize("test", "1", [](bool, const mcp::json&){});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        bool connectionErrorTriggered = false;
        session->listTools([&](const std::vector<mcp::McpTool>&, const mcp::json& error) {
            if (!error.empty() && error.contains("code") && error["code"] == -32603) {
                if (error.contains("message") && error["message"].get<std::string>().find("Connection interrupted") != std::string::npos) {
                    connectionErrorTriggered = true;
                }
            }
        });

        // 模拟 Transport 断开 / Server 崩溃
        transport->close();

        assert(connectionErrorTriggered && "Scenario 6 Failed: Connection interruption should trigger callback cleanup.");
        std::cout << "[✓] Scenario 6: Cleanup pending requests on server crash or connection interruption\n";
    }

    // ----------------------------------------------------
    // Scenario 7: Schema validation failed / Invalid params (类型错误与参数校验失败)
    // ----------------------------------------------------
    {
        auto transport = std::make_shared<MockTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);
        session->init();
        session->start();

        session->initialize("test", "1", [](bool, const mcp::json&){});
        mcp::json initResp = {
            {"jsonrpc", "2.0"}, {"id", 1},
            {"result", {{"protocolVersion", mcp::McpClientSession::MCP_PROTOCOL_VERSION}, {"capabilities", mcp::json::object()}, {"serverInfo", mcp::json::object()}}}
        };
        transport->pushServerMessage(initResp.dump());

        bool schemaErrorTriggered = false;
        session->callTool("calculate_add", {{"a", "invalid_number_type"}, {"b", 5}}, [&](const mcp::json&, const mcp::json& error) {
            if (!error.empty() && error.contains("code") && error["code"] == -32602) {
                schemaErrorTriggered = true;
            }
        });

        mcp::json schemaErrResp = {
            {"jsonrpc", "2.0"},
            {"id", 2},
            {"error", {{"code", -32602}, {"message", "Invalid params: a and b must be numbers"}}}
        };
        transport->pushServerMessage(schemaErrResp.dump());

        assert(schemaErrorTriggered && "Scenario 7 Failed: Invalid parameter type should return -32602.");
        std::cout << "[✓] Scenario 7: Validate schema validation failure and invalid params\n";
    }

    std::cout << "\n==================================================\n";
    std::cout << "  🎉 🎉 🎉 All Error Handling self-tests PASSED!\n";
    std::cout << "==================================================\n";
}

int main(int argc, char* argv[]) {
    bool isConformance = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--scenario") {
            isConformance = true;
            break;
        }
    }

    if (isConformance) {
        auto transport = std::make_shared<mcp::ConsoleStdioTransport>();
        auto session = std::make_shared<mcp::McpClientSession>(transport);

        std::mutex mtx;
        std::condition_variable cv;
        bool finished = false;

        session->init();
        if (!session->start()) {
            std::cerr << "Failed to start console stdio transport." << std::endl;
            return 1;
        }

        session->initialize("mcp-conformance-client-cpp", "1.0.0", [&](bool success, const mcp::json& serverInfo) {
            if (!success) {
                std::lock_guard<std::mutex> lock(mtx);
                finished = true;
                cv.notify_one();
                return;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            
            std::lock_guard<std::mutex> lock(mtx);
            finished = true;
            cv.notify_one();
        });

        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(8), [&]{ return finished; });
        session->close();
    } else {
        runLocalLifecycleTests();
        runLocalToolsTests();
        runLocalResourcesAndPromptsTests();
        runLocalErrorTests();
    }
    return 0;
}
