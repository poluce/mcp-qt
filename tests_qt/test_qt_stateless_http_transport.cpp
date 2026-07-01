#include "mcp_qt_transport/QtStatelessHttpTransport.h"
#include "tests/common.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <functional>
#include <memory>

static void waitEvents(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

class MockStatelessServer : public QObject {
public:
    QTcpServer server;
    QByteArray lastRequestData;
    int requestCount{0};
    std::function<QByteArray(const QByteArray&)> handler;

    MockStatelessServer() {
        connect(&server, &QTcpServer::newConnection, this, &MockStatelessServer::handleConnection);
        server.listen(QHostAddress::LocalHost, 0);
    }

    quint16 port() const { return server.serverPort(); }

    void handleConnection() {
        QTcpSocket* socket = server.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, [this, socket]() {
            QByteArray request = socket->readAll();
            lastRequestData = request;
            requestCount++;
            
            QByteArray responseBody;
            if (handler) {
                int bodyIdx = request.indexOf("\r\n\r\n");
                QByteArray reqBody = bodyIdx != -1 ? request.mid(bodyIdx + 4) : QByteArray();
                responseBody = handler(reqBody);
            } else {
                responseBody = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}";
            }
            
            QByteArray response = "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: application/json\r\n"
                                  "Content-Length: " + QByteArray::number(responseBody.size()) + "\r\n"
                                  "Connection: close\r\n\r\n" + responseBody;
            socket->write(response);
            socket->disconnectFromHost();
        });
    }
};

void test_qt_stateless_http_transport_basic() {
    int argc = 0;
    char* argv[] = {nullptr};
    std::unique_ptr<QCoreApplication> app;
    if (!QCoreApplication::instance()) {
        app = std::make_unique<QCoreApplication>(argc, argv);
    }

    MockStatelessServer server;
    server.handler = [](const QByteArray& reqBody) {
        TM_ASSERT_TRUE(reqBody.contains("hello world"), "Request body must contain payload");
        return QByteArray("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"reply\":\"hi\"}}");
    };

    QString url = QString("http://127.0.0.1:%1/rpc").arg(server.port());
    mcp_qt::QtStatelessHttpTransport transport(url);

    std::string receivedMsg;
    transport.setOnMessage([&](const std::string& msg) {
        receivedMsg = msg;
    });

    TM_ASSERT_TRUE(transport.start(), "Transport should start");
    
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"test\",\"params\":\"hello world\"}";
    bool sent = transport.send(payload);
    TM_ASSERT_TRUE(sent, "Transport should send");

    waitEvents(100);

    TM_ASSERT_EQ(server.requestCount, 1, "Server should receive exactly 1 request");
    TM_ASSERT_TRUE(server.lastRequestData.contains("POST /rpc HTTP/1.1"), "Should send POST");
    TM_ASSERT_TRUE(server.lastRequestData.contains("Content-Type: application/json"), "Should set content type");
    
    TM_ASSERT_EQ(receivedMsg, std::string("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"reply\":\"hi\"}}"), "Should parse response body");
}

void test_qt_stateless_http_transport_headers() {
    int argc = 0;
    char* argv[] = {nullptr};
    std::unique_ptr<QCoreApplication> app;
    if (!QCoreApplication::instance()) {
        app = std::make_unique<QCoreApplication>(argc, argv);
    }

    MockStatelessServer server;
    QString url = QString("http://127.0.0.1:%1/rpc").arg(server.port());
    mcp_qt::QtStatelessHttpTransport transport(url);

    QMap<QByteArray, QByteArray> headers;
    headers.insert("Authorization", "Bearer token123");
    headers.insert("X-Custom", "value");
    transport.setCustomHeaders(headers);
    transport.start();

    transport.send("{}");
    waitEvents(100);

    TM_ASSERT_EQ(server.requestCount, 1, "Server should receive exactly 1 request");
    TM_ASSERT_TRUE(server.lastRequestData.contains("Authorization: Bearer token123"), "Should send auth header");
    TM_ASSERT_TRUE(server.lastRequestData.contains("X-Custom: value"), "Should send custom header");
    TM_ASSERT_TRUE(server.lastRequestData.contains("Content-Type: application/json"), "Should preserve content type");
}
