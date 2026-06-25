#include <QCoreApplication>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDebug>
#include <iostream>
#include <cstdlib>
#include "mcp_qt/QtHttpTransport.h"

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

    // 我们使用 127.0.0.1 端口 3000 进行本地测试
    QUrl sseUrl("http://127.0.0.1:3000/mcp");
    auto* transport = new mcp::QtHttpTransport(sseUrl, &app);

    int step = 0;
    bool toolsListSuccess = false;
    bool disconnectTriggered = false;
    bool reconnectSucceeded = false;
    bool originVerificationPassed = false;

    // 1. 测试 Origin 校验 (防 DNS Rebinding)
    // 我们发送一个带有外部恶意 Origin 的请求，期望服务器返回 403 Forbidden
    auto* nam = new QNetworkAccessManager(&app);
    QNetworkRequest originReq(sseUrl);
    originReq.setRawHeader("Origin", "http://evil.com");
    originReq.setRawHeader("MCP-Protocol-Version", "2025-11-25");
    QNetworkReply* originReply = nam->get(originReq);

    QObject::connect(originReply, &QNetworkReply::finished, [&]() {
        if (originReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 403) {
            std::cout << "[✓] Origin Verification Passed: Server rejected external origin with 403." << std::endl;
            originVerificationPassed = true;
        } else {
            std::cerr << "[✗] Origin Verification Failed: Server returned status code " 
                      << originReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() << std::endl;
        }
        originReply->deleteLater();
    });

    // 连接各种回调
    transport->setOnMessage([&](const std::string& message) {
        std::cout << "[Client] Received message: " << message << std::endl;
        if (step == 1) {
            // 这是 tools/list 响应
            if (message.find("calculate_add") != std::string::npos) {
                toolsListSuccess = true;
                std::cout << "[✓] Tools/List response verified successfully via SSE." << std::endl;

                // 2. 触发意外断开测试
                step = 2;
                std::cout << "[Client] Sending trigger_disconnect request to server..." << std::endl;
                transport->send("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"test/trigger_disconnect\"}");
                disconnectTriggered = true;
            }
        } else if (step == 3) {
            // 重连后收到消息
            if (message.find("calculate_add") != std::string::npos) {
                std::cout << "[✓] Received message after reconnecting successfully!" << std::endl;
                reconnectSucceeded = true;
                
                // 所有测试完成，检查结果
                if (toolsListSuccess && disconnectTriggered && reconnectSucceeded && originVerificationPassed) {
                    std::cout << "\n========================================\n";
                    std::cout << "  🎉 🎉 🎉 HTTP/SSE Transport Tests PASSED!\n";
                    std::cout << "========================================\n" << std::endl;
                    std::exit(0);
                } else {
                    std::cerr << "[✗] Some tests failed to complete properly." << std::endl;
                    std::exit(1);
                }
            }
        }
    });

    transport->setOnError([&](const std::string& error) {
        std::cout << "[Client Error/Warning] " << error << std::endl;
        // 如果我们处于 step == 2 且触发了断线，我们将等待重连，所以这是预期的警告
        if (step == 2 && error.find("disconnected unexpectedly") != std::string::npos) {
            std::cout << "[Client] Client caught unexpected disconnect. Waiting for auto-reconnect..." << std::endl;
            step = 3;
            // 延时 3.5 秒发送新请求以验证重连后的通道
            QTimer::singleShot(3500, [&]() {
                std::cout << "[Client] Reconnected? Sending another tools/list request to verify..." << std::endl;
                transport->send("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}");
            });
        }
    });

    // 启动 Transport
    if (!transport->start()) {
        std::cerr << "[✗] Failed to start HTTP transport." << std::endl;
        return 1;
    }

    // 延时 1.5 秒，等待 SSE 握手成功并拿到 endpoint 之后发送 tools/list
    QTimer::singleShot(1500, [&]() {
        if (step == 0) {
            step = 1;
            std::cout << "[Client] Sending tools/list request..." << std::endl;
            transport->send("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}");
        }
    });

    // 守护定时器，防挂起超时 (15 秒)
    QTimer::singleShot(15000, [&]() {
        std::cerr << "[✗] Test timeout. Not all scenarios succeeded." << std::endl;
        std::exit(1);
    });

    return app.exec();
}
