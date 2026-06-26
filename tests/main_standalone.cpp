#include "tests/common.h"
#include <cstdlib>

void runAllLocalTests() {
    TmTestRunner::instance().startTestSuite("C++ MCP SDK Standalone Test Suite");

    // 1. Protocol Tests
    TmTestRunner::instance().startTestSuite("Protocol Tests");
    TM_RUN_TEST(test_initialize);
    TM_RUN_TEST(test_json_rpc);
    TM_RUN_TEST(test_capabilities);
    TM_RUN_TEST(test_error_response);

    // 2. Transport Tests (仅纯 C++17 测试)
    TmTestRunner::instance().startTestSuite("Transport Tests");
    TM_RUN_TEST(test_stdio_transport);
    TM_RUN_TEST(test_process_lifecycle);

    // 3. Feature Tests
    TmTestRunner::instance().startTestSuite("Feature Tests");
    TM_RUN_TEST(test_tools);
    TM_RUN_TEST(test_resources);
    TM_RUN_TEST(test_prompts);
    TM_RUN_TEST(test_notifications);
    TM_RUN_TEST(test_ping);
    TM_RUN_TEST(test_resource_templates);
    TM_RUN_TEST(test_complete);
    TM_RUN_TEST(test_elicitation);
    TM_RUN_TEST(test_tool_annotations);

    TmTestRunner::instance().printSummary();

    if (TmTestRunner::instance().hasFailed()) {
        std::cerr << "\n Some tests FAILED!\n";
        std::exit(1);
    } else {
        std::cout << "\n All Standalone Tests PASSED!\n";
    }
}

int main() {
    runAllLocalTests();
    return 0;
}
