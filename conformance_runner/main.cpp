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

int main(int argc, char* argv[]) {
    bool isConformance = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--scenario") {
            isConformance = true;
            break;
        }
    }

    if (isConformance) {
        // Standard Stdio Conformance Flow (Scenario verification)
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
        // Local lifecycle & tools scenario testing suite
        runLocalLifecycleTests();
        runLocalToolsTests();
    }
    return 0;
}
