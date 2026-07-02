#include "mcp_qt_client/McpQtClient.h"
#include "mcp_core/McpReconnectPolicy.h"
#include "tests/common.h"
#include <QEventLoop>
#include <QTimer>
#include <QSignalSpy>
#include <QList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>

// 辅助等待事件循环运转
static void waitEvents(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// 绝对安全、在后台子线程事件循环内运行的 Mock HTTP 服务器，规避主线程死等导致的死锁
class RecoveryMockServer : public QObject {
    Q_OBJECT
public:
    QTcpServer server;
    QList<QTcpSocket*> sockets;
    QList<std::string> receivedRequests;
    QMutex mutex; // 保护成员变量跨线程安全访问
    bool holdToolsListResponse{false};

    RecoveryMockServer() {
        QObject::connect(&server, &QTcpServer::newConnection, [this]() {
            QTcpSocket* socket = server.nextPendingConnection();
            if (!socket) return;
            {
                QMutexLocker lock(&mutex);
                sockets.append(socket);
            }

            QObject::connect(socket, &QTcpSocket::readyRead, [this, socket]() {
                if (socket->state() != QAbstractSocket::ConnectedState) return;

                QByteArray buffer = socket->property("receivedBuffer").toByteArray();
                buffer.append(socket->readAll());
                socket->setProperty("receivedBuffer", buffer);

                std::string req(buffer.constData(), buffer.size());

                if (req.find("GET ") != std::string::npos) {
                    {
                        QMutexLocker lock(&mutex);
                        receivedRequests.append(req);
                    }
                    socket->setProperty("receivedBuffer", QByteArray()); // 清空

                    socket->write("HTTP/1.1 200 OK\r\n"
                                  "Content-Type: text/event-stream\r\n"
                                  "Connection: keep-alive\r\n\r\n");
                    socket->write("event: endpoint\ndata: /post\n\n");
                    socket->flush();
                } else if (req.find("POST ") != std::string::npos) {
                    size_t bodyStart = req.find("\r\n\r\n");
                    if (bodyStart == std::string::npos) return;
                    if (req.find("}", bodyStart) == std::string::npos) return; // 还没齐，等待

                    {
                        QMutexLocker lock(&mutex);
                        receivedRequests.append(req);
                    }
                    socket->setProperty("receivedBuffer", QByteArray()); // 清空

                    // 提取 JSON-RPC method
                    std::string method = "";
                    size_t pos = req.find("\"method\":\"");
                    if (pos != std::string::npos) {
                        size_t start = pos + 10;
                        size_t end = req.find("\"", start);
                        if (end != std::string::npos) {
                            method = req.substr(start, end - start);
                        }
                    }

                    // 提取 ID
                    int64_t id = 0;
                    size_t idPos = req.find("\"id\":");
                    if (idPos != std::string::npos) {
                        id = std::stoll(req.substr(idPos + 5));
                    }

                    bool holdThisToolsList = false;
                    {
                        QMutexLocker lock(&mutex);
                        holdThisToolsList = (method == "tools/list" && holdToolsListResponse);
                    }
                    if (holdThisToolsList) {
                        return; // 故意悬挂该请求，等待稍后物理断线触发 replay
                    }

                    nlohmann::json result = nlohmann::json::object();
                    if (method == "initialize") {
                        result = {
                            {"protocolVersion", "2025-11-25"},
                            {"capabilities", {{"resources", {{"subscribe", true}}}}},
                            {"serverInfo", {{"name", "recovery-mock"}, {"version", "1.0.0"}}}
                        };
                    } else if (method == "tools/list") {
                        result = {
                            {"tools", {
                                {{"name", "recovered-tool"}, {"description", "after-reconnect"}, {"inputSchema", {{"type", "object"}}}}
                            }}
                        };
                    }

                    nlohmann::json resp = {
                        {"jsonrpc", "2.0"},
                        {"id", id},
                        {"result", result}
                    };

                    std::string respStr = resp.dump();
                    socket->write(std::string("HTTP/1.1 200 OK\r\n"
                                              "Content-Type: application/json\r\n"
                                              "Content-Length: " + std::to_string(respStr.size()) + "\r\n\r\n" + respStr).c_str());
                    socket->flush();
                    socket->disconnectFromHost();
                }
            });
        });
        server.listen(QHostAddress::LocalHost, 0);
    }

    ~RecoveryMockServer() {
        closeAllSockets();
        server.close();
    }

    Q_INVOKABLE void closeAllSockets() {
        QMutexLocker lock(&mutex);
        for (auto* s : sockets) {
            if (s) {
                s->close();
                s->deleteLater();
            }
        }
        sockets.clear();
    }

    quint16 port() const { return server.serverPort(); }
};

// 后台子线程运行的 Mock 包装器
class ThreadedRecoveryMockServer : public QThread {
public:
    RecoveryMockServer* mock{nullptr};
    quint16 mockPort{0};
    QMutex portMutex;
    QWaitCondition portReady;

    void run() override {
        RecoveryMockServer serverInstance;
        {
            QMutexLocker lock(&portMutex);
            mockPort = serverInstance.port();
            mock = &serverInstance;
            portReady.wakeAll();
        }
        exec(); // 开启子线程事件循环
        mock = nullptr;
    }
};

void test_qt_transport_recovery() {
    // 1. 验证指数避退策略延迟计算算法的精确性
    {
        mcp::McpReconnectPolicy policy;
        policy.initialDelayMs = 250;
        policy.maxDelayMs = 2000;
        policy.multiplier = 2.0;

        TM_ASSERT_EQ(policy.getDelayMs(1), 250, "1st retry should wait 250ms");
        TM_ASSERT_EQ(policy.getDelayMs(2), 500, "2nd retry should wait 500ms");
        TM_ASSERT_EQ(policy.getDelayMs(3), 1000, "3rd retry should wait 1000ms");
        TM_ASSERT_EQ(policy.getDelayMs(4), 2000, "4th retry should cap at maxDelayMs (2000ms)");
        TM_ASSERT_EQ(policy.getDelayMs(5), 2000, "5th retry should remain capped at 2000ms");
    }

    // 2. 端到端物理网络自愈测试：验证真实 HTTP/SSE 通道非预期断开后的完整恢复链路
    {
        ThreadedRecoveryMockServer threadMock;
        threadMock.start();

        // 等待后台 Mock Server 启动并就绪
        {
            QMutexLocker lock(&threadMock.portMutex);
            if (threadMock.mockPort == 0) {
                threadMock.portReady.wait(&threadMock.portMutex);
            }
        }
        quint16 port = threadMock.mockPort;

        // 建立真实的 HTTP SSE 连接 (m_transportType = 1)
        QString url = QString("http://127.0.0.1:%1/sse").arg(port);
        auto client = mcp_qt::McpQtClient::connectHttpAndWait(url, "test-client", "1.0.0", 1000);
        TM_ASSERT_TRUE(client != nullptr, "Client must successfully establish initial HTTP connection");

        // 立即设置极低退避延时重连策略，防患于未然
        mcp::McpReconnectPolicy policy;
        policy.enabled = true;
        policy.initialDelayMs = 20; // 20ms 退避
        policy.maxAttempts = 5;
        client->setReconnectPolicy(policy);

        // 注册带和不带 context 的两个通知处理器
        int countWithCtx = 0;
        client->registerNotificationHandler("notifications/custom-ctx", client.get(), [&countWithCtx](const QJsonObject&) {
            countWithCtx++;
        });

        int countNoCtx = 0;
        client->registerNotificationHandler("notifications/custom-no-ctx", [&countNoCtx](const QJsonObject&) {
            countNoCtx++;
        });

        // 订阅一个资源更新状态，非 callback 版本
        bool subOk = client->subscribeResource("file:///data/config.json");
        TM_ASSERT_TRUE(subOk, "Initial subscribeResource must succeed");

        // 构造一个将被断线打断的 replayable request：tools/list
        bool replayCallbackCalled = false;
        QJsonObject replayResult;
        QJsonObject replayError;
        {
            QMutexLocker lock(&threadMock.mock->mutex);
            threadMock.mock->holdToolsListResponse = true;
            threadMock.mock->receivedRequests.clear();
        }

        client->sendRequest("tools/list", QJsonObject{}, client.get(),
            [&](const QJsonObject& result, const QJsonObject& error) {
                replayCallbackCalled = true;
                replayResult = result;
                replayError = error;
            });
        waitEvents(50); // 确保首次 tools/list 已经发到服务端并被挂起

        // 绑定信号侦听器
        QSignalSpy reconnectingSpy(client.get(), &mcp_qt::McpQtClient::reconnecting);
        QSignalSpy reconnectedSpy(client.get(), &mcp_qt::McpQtClient::reconnected);
        QSignalSpy failedSpy(client.get(), &mcp_qt::McpQtClient::recoveryFailed);

        // 允许重连后的 tools/list 自动刷新与 replay 请求获得成功响应
        {
            QMutexLocker lock(&threadMock.mock->mutex);
            threadMock.mock->holdToolsListResponse = false;
        }

        // 在子线程中安全地强制切断所有现存的 TCP 套接字
        QMetaObject::invokeMethod(threadMock.mock, "closeAllSockets");

        // 事件循环运转，等待客户端检测到断开，进行 reconnecting -> 等待 20ms -> 发起物理重连 HTTP 与重新握手
        waitEvents(300);

        // 验证信号触发
        TM_ASSERT_EQ(reconnectingSpy.count(), 1, "reconnecting signal should emit upon real TCP connection cut");
        TM_ASSERT_EQ(reconnectedSpy.count(), 1, "reconnected signal should emit since client must reconnect successfully");
        TM_ASSERT_EQ(failedSpy.count(), 0, "recoveryFailed must not emit");

        // 额外等待 200ms 让 100ms 延迟派发的自愈恢复任务（通知、资源订阅）在事件循环中执行完毕
        waitEvents(200);

        // 验证重连后自动发送了 initialize 重新握手和重订阅请求
        bool reInitialized = false;
        bool reSubscribed = false;
        int toolListCount = 0;
        {
            QMutexLocker lock(&threadMock.mock->mutex);
            for (const auto& req : threadMock.mock->receivedRequests) {
                if (req.find("\"method\":\"initialize\"") != std::string::npos) {
                    reInitialized = true;
                }
                if (req.find("\"method\":\"resources/subscribe\"") != std::string::npos) {
                    reSubscribed = true;
                }
                if (req.find("\"method\":\"tools/list\"") != std::string::npos) {
                    ++toolListCount;
                }
            }
        }

        TM_ASSERT_TRUE(reInitialized, "Auto-reconnect must send a new initialize handshake over HTTP");
        TM_ASSERT_TRUE(reSubscribed, "Auto-reconnect must replay pending resources/subscribe over HTTP");
        TM_ASSERT_TRUE(toolListCount >= 3, "Recovery flow must include original tools/list, auto refresh tools/list, and replayed tools/list");
        TM_ASSERT_TRUE(replayCallbackCalled, "Replayable pending request callback should complete after recovery");
        TM_ASSERT_TRUE(replayError.isEmpty(), "Replayable tools/list should finish without error after recovery");
        TM_ASSERT_EQ(replayResult.value("tools").toArray().size(), 1, "Replayed tools/list should return recovered tools payload");

        // 在重新连上的物理 TCP 通道中（mock 的 sockets 列表），投递带 method 的通知，验证通知处理器自愈恢复！
        nlohmann::json notifyCtx = {
            {"jsonrpc", "2.0"},
            {"method", "notifications/custom-ctx"},
            {"params", {}}
        };
        nlohmann::json notifyNoCtx = {
            {"jsonrpc", "2.0"},
            {"method", "notifications/custom-no-ctx"},
            {"params", {}}
        };

        std::string notifyCtxStr = notifyCtx.dump();
        std::string notifyNoCtxStr = notifyNoCtx.dump();

        QMetaObject::invokeMethod(threadMock.mock, [mock = threadMock.mock, notifyCtxStr, notifyNoCtxStr]() {
            QMutexLocker lock(&mock->mutex);
            for (auto* s : mock->sockets) {
                if (s && s->state() == QAbstractSocket::ConnectedState) {
                    s->write(std::string("event: message\ndata: " + notifyCtxStr + "\n\n").c_str());
                    s->write(std::string("event: message\ndata: " + notifyNoCtxStr + "\n\n").c_str());
                    s->flush();
                }
            }
        }, Qt::QueuedConnection);

        waitEvents(100);

        // 断言重连后通知处理器的回调响应依然有效工作 (100% 成功自愈证据)
        TM_ASSERT_EQ(countWithCtx, 1, "Context handler should execute normally after HTTP self-healing");
        TM_ASSERT_EQ(countNoCtx, 1, "No-context handler should execute normally after HTTP self-healing");

        client->close();
        waitEvents(100);

        // 安全停止并等待后台 Mock 线程退出
        threadMock.quit();
        threadMock.wait();
    }
}

#include "test_qt_transport_recovery.moc"
