#include "tests/common.h"
#include <iostream>
#include <QCoreApplication>

// 声明 Qt 传输层的所有测试函数
void test_qt_sse_parser_reads_multiline_data();
void test_qt_sse_parser_reads_retry();
void test_qt_sse_parser_reads_crlf_events();
void test_qt_transport_constructs();
void test_qt_transport_shutdown_is_idempotent();
void test_qt_transport_rejects_send_before_start();
void test_qt_transport_keeps_core_session_api_shape();
void test_qt_transport_updates_protocol_version_runtime();
void test_qt_transport_auth_retry();
void test_qt_transport_post_event_stream_parsing();
void test_qt_transport_post_json_with_data_field();
void test_qt_transport_auth_retry_failure_stops_reconnect();
void test_qt_transport_updates_token_provider_runtime();
void test_qt_transport_post_auth_failure_blocks_message();
void test_qt_connect_stdio_integration();
void test_qt_http_request_config();
void test_qt_typed_results();
void test_qt_resource_router();
void test_qt_tools_model();
void test_qt_transport_recovery();

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    TmTestRunner::instance().startTestSuite("Qt HttpSse Transport Tests");

    TM_RUN_TEST(test_qt_sse_parser_reads_multiline_data);
    TM_RUN_TEST(test_qt_sse_parser_reads_retry);
    TM_RUN_TEST(test_qt_sse_parser_reads_crlf_events);
    TM_RUN_TEST(test_qt_transport_constructs);
    TM_RUN_TEST(test_qt_transport_shutdown_is_idempotent);
    TM_RUN_TEST(test_qt_transport_rejects_send_before_start);
    TM_RUN_TEST(test_qt_transport_keeps_core_session_api_shape);
    TM_RUN_TEST(test_qt_transport_updates_protocol_version_runtime);
    TM_RUN_TEST(test_qt_transport_auth_retry);
    TM_RUN_TEST(test_qt_transport_post_event_stream_parsing);
    TM_RUN_TEST(test_qt_transport_post_json_with_data_field);
    TM_RUN_TEST(test_qt_transport_auth_retry_failure_stops_reconnect);
    TM_RUN_TEST(test_qt_transport_updates_token_provider_runtime);
    TM_RUN_TEST(test_qt_transport_post_auth_failure_blocks_message);
    // TM_RUN_TEST(test_qt_connect_stdio_integration);
    TM_RUN_TEST(test_qt_http_request_config);
    TM_RUN_TEST(test_qt_typed_results);
    TM_RUN_TEST(test_qt_resource_router);
    TM_RUN_TEST(test_qt_tools_model);
    TM_RUN_TEST(test_qt_transport_recovery);
    TM_RUN_TEST(test_qt_stateless_http_transport_basic);
    TM_RUN_TEST(test_qt_stateless_http_transport_headers);

    TmTestRunner::instance().printSummary();
    return TmTestRunner::instance().hasFailed() ? 1 : 0;
}
