#pragma once
#include <string>
#include <functional>
#include <memory>
#include <iostream>
#include <sstream>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include "mcp_core/IMcpTransport.h"
#include "mcp_core/McpClientSession.h"

// ============================================================================
// TmTestRunner - Light-weight Test Runner Framework
// ============================================================================
class TmTestRunner {
public:
    static TmTestRunner& instance() {
        static TmTestRunner inst;
        return inst;
    }

    void startTestSuite(const std::string& suiteName) {
        std::cout << "\n[" << suiteName << "] Running scenario tests...\n";
        m_currentSuite = suiteName;
    }

    void startTestCase(const std::string& caseName) {
        m_currentCase = caseName;
        m_currentCasePassed = true;
        m_totalCasesRun++;
    }

    void recordPass() {
        m_assertsPassed++;
    }

    void recordFailure(const std::string& file, int line, const std::string& message) {
        m_currentCasePassed = false;
        m_assertsFailed++;
        std::cerr << "  [✗] Failure in " << file << ":" << line << "\n"
                  << "      Message: " << message << "\n";
    }

    void endTestCase() {
        if (m_currentCasePassed) {
            m_casesPassed++;
            std::cout << "  [✓] " << m_currentCase << "\n";
        } else {
            m_casesFailed++;
            std::cout << "  [✗] " << m_currentCase << " FAILED!\n";
        }
    }

    void printSummary() const {
        std::cout << "\n==================================================\n";
        std::cout << "  Test Runner Summary:\n";
        std::cout << "  - Total Test Cases Run: " << m_totalCasesRun << "\n";
        std::cout << "  - Passed Test Cases   : " << m_casesPassed << "\n";
        std::cout << "  - Failed Test Cases   : " << m_casesFailed << "\n";
        std::cout << "  - Asserts (Pass/Fail) : " << m_assertsPassed << "/" << m_assertsFailed << "\n";
        std::cout << "==================================================\n";
    }

    bool hasFailed() const {
        return m_casesFailed > 0;
    }

    // Helper toString overloads
    template <typename T>
    static std::string toString(const T& val) {
        std::stringstream ss;
        ss << val;
        return ss.str();
    }

    static std::string toString(const nlohmann::json& val) {
        return val.dump();
    }

    static std::string toString(bool val) {
        return val ? "true" : "false";
    }

    static std::string toString(mcp::SessionState state) {
        switch (state) {
            case mcp::SessionState::Uninitialized: return "Uninitialized";
            case mcp::SessionState::Initializing: return "Initializing";
            case mcp::SessionState::Initialized: return "Initialized";
            case mcp::SessionState::Shutdown: return "Shutdown";
        }
        return "Unknown";
    }

private:
    TmTestRunner() = default;
    std::string m_currentSuite;
    std::string m_currentCase;
    bool m_currentCasePassed = true;

    int m_totalCasesRun = 0;
    int m_casesPassed = 0;
    int m_casesFailed = 0;
    int m_assertsPassed = 0;
    int m_assertsFailed = 0;
};

// ============================================================================
// Custom Assertion Macros
// ============================================================================
#define TM_ASSERT_TRUE(cond, msg) \
    if (!(cond)) { \
        TmTestRunner::instance().recordFailure(__FILE__, __LINE__, std::string("Expected true but got false. Info: ") + (msg)); \
    } else { \
        TmTestRunner::instance().recordPass(); \
    }

#define TM_ASSERT_FALSE(cond, msg) \
    if (cond) { \
        TmTestRunner::instance().recordFailure(__FILE__, __LINE__, std::string("Expected false but got true. Info: ") + (msg)); \
    } else { \
        TmTestRunner::instance().recordPass(); \
    }

#define TM_ASSERT_EQ(actual, expected, msg) \
    if ((actual) != (expected)) { \
        TmTestRunner::instance().recordFailure(__FILE__, __LINE__, \
            std::string("Values not equal. Info: ") + (msg) + \
            "\n      [Actual  ]: " + TmTestRunner::toString(actual) + \
            "\n      [Expected]: " + TmTestRunner::toString(expected)); \
    } else { \
        TmTestRunner::instance().recordPass(); \
    }

#define TM_ASSERT_STR_CONTAINS(str, pattern, msg) \
    if ((str).find(pattern) == std::string::npos) { \
        TmTestRunner::instance().recordFailure(__FILE__, __LINE__, \
            std::string("Substring not found. Info: ") + (msg) + \
            "\n      [String ]: " + (str) + \
            "\n      [Pattern]: " + (pattern)); \
    } else { \
        TmTestRunner::instance().recordPass(); \
    }

#define TM_RUN_TEST(func) \
    TmTestRunner::instance().startTestCase(#func); \
    try { \
        func(); \
    } catch (const std::exception& e) { \
        TmTestRunner::instance().recordFailure(__FILE__, __LINE__, std::string("Unhandled exception: ") + e.what()); \
    } catch (...) { \
        TmTestRunner::instance().recordFailure(__FILE__, __LINE__, "Unhandled unknown exception"); \
    } \
    TmTestRunner::instance().endTestCase();

// ============================================================================
// Enhanced MockTransport - Memory transport mock class
// ============================================================================
class MockTransport : public mcp::IMcpTransport {
public:
    std::string lastSentMessage;
    std::function<void(const std::string&)> onSendCallback;
    std::function<void(const std::string&)> m_onMessage;
    std::function<void()> m_onClose;
    std::function<void(const std::string&)> m_onError;
    
    std::mutex m_mockMutex;
    std::vector<std::thread> m_threads;
    bool m_closed = false;

    ~MockTransport() override {
        for (auto& t : m_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    bool send(const std::string& message) override {
        std::lock_guard<std::mutex> lock(m_mockMutex);
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
    
    void setOnError(std::function<void(const std::string&)> callback) override {
        m_onError = std::move(callback);
    }
    
    bool start() override { 
        m_closed = false;
        return true; 
    }
    
    void close() override { 
        {
            std::lock_guard<std::mutex> lock(m_mockMutex);
            if (m_closed) return;
            m_closed = true;
        }
        if (m_onClose) m_onClose(); 
    }

    void pushServerMessage(const std::string& msg) {
        if (m_onMessage) {
            m_onMessage(msg);
        }
    }

    void pushServerMessageAsync(const std::string& msg, int delayMs = 5) {
        m_threads.push_back(std::thread([this, msg, delayMs]() {
            if (delayMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }
            std::function<void(const std::string&)> cb;
            {
                std::lock_guard<std::mutex> lock(m_mockMutex);
                cb = m_onMessage;
            }
            if (cb) {
                cb(msg);
            }
        }));
    }

    void triggerError(const std::string& err) {
        if (m_onError) {
            m_onError(err);
        }
    }
};

// Declarations of modular tests
void test_initialize();
void test_json_rpc();
void test_capabilities();
void test_error_response();

void test_stdio_transport();
void test_http_transport();
void test_process_lifecycle();

void test_tools();
void test_resources();
void test_prompts();
void test_notifications();
void test_ping();
void test_resource_templates();
void test_complete();
void test_elicitation();
void test_tool_annotations();

void test_with_filesystem_server();
void test_with_anysearch_mcp();
void test_with_inspector_cases();
