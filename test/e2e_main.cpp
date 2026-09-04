// MCP Apps A4 端到端测试入口（独立目标：需 node 起 mock 服务器）
#include "tests/common.h"
#include <QCoreApplication>

void test_qt_mcp_apps_end_to_end();

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    TM_RUN_TEST(test_qt_mcp_apps_end_to_end);
    TmTestRunner::instance().printSummary();
    return TmTestRunner::instance().hasFailed() ? 1 : 0;
}
