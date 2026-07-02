#include <QCoreApplication>
#include <QTimer>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <iostream>
#include <mcp_qt_client/McpQtClient.h>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace mcp_qt;

int main(int argc, char *argv[]) {
#ifdef Q_OS_WIN
    // 强制 Windows 控制台使用 UTF-8 编码输出，防止 Emoji 和特殊破折号乱码
    SetConsoleOutputCP(CP_UTF8);
#endif
    QCoreApplication app(argc, argv);

    qInfo() << "Starting AnySearch MCP server via HTTP SSE...";
    
    // AnySearch 原生支持 Streamable HTTP/SSE，使用直接网络连接避免 npx 代理的控制台日志污染
    auto client = McpQtClient::connectHttpAndWait("https://api.anysearch.com/mcp", "AnySearchExample", "1.0", 60000);
    if (!client || !client->isConnected()) {
        qCritical() << "Failed to connect to AnySearch MCP Server!";
        return -1;
    }
    
    qInfo() << "Connected successfully! Initializing...";
    
    // 使用单次定时器在事件循环跑起来后执行业务逻辑
    QTimer::singleShot(100, [&]() {
        try {
            qInfo() << "Fetching tools...";
            auto tools = client->listTools(15000); // 15秒超时
            
            bool hasSearchTool = false;
            
            qInfo() << "Available tools:";
            for (const auto& t : tools) {
                std::cout << " - " << t.name.toStdString() << ":\n" << t.description.toStdString() << "\n\n";
                if (t.name == "search") {
                    hasSearchTool = true;
                }
            }
            
            if (!tools.empty()) {
                // 如果服务端返回了名为 search 的工具，优先使用；否则取第一个
                QString toolToCall = hasSearchTool ? "search" : tools.front().name;
                qInfo() << "Calling tool:" << toolToCall;
                
                std::vector<QString> testQueries = {
                    "What are the latest AI news today?",
                    QString::fromUtf8(u8"2024年人工智能领域有哪些重大突破？")
                };

                // 用一个共享指针（或引用捕获变量）来统计还有几个异步请求未完成
                auto pendingRequests = std::make_shared<int>(testQueries.size());

                for (const auto& q : testQueries) {
                    QJsonObject toolArgs;
                    toolArgs["query"] = q;
                    
                    std::cout << "\n[ASYNC] Dispatching search for: " << q.toStdString() << "...\n";
                    
                    // 🌟 使用刚刚新增的纯异步 API！
                    client->callToolAsync(toolToCall, toolArgs, [client, q, pendingRequests](mcp_qt::McpResult result) {
                        std::cout << "\n=============================================\n";
                        std::cout << "Async Result for [" << q.toStdString() << "]:\n";
                        if (result.isError) {
                            std::cout << "Error: " << result.errorString.toStdString() << std::endl;
                        } else {
                            std::cout << "Search Result:\n" 
                                      << QJsonDocument(result.data).toJson(QJsonDocument::Indented).toStdString() << std::endl;
                        }
                        
                        // 每次回调完成，计数器减 1
                        (*pendingRequests)--;
                        if (*pendingRequests == 0) {
                            qInfo() << "All async tasks finished. Shutting down...";
                            client->close();
                            QCoreApplication::quit();
                        }
                    }, nullptr);
                }
            } else {
                qWarning() << "No tools found from the server!";
                qInfo() << "Shutting down...";
                client->close();
                QCoreApplication::quit();
            }
        } catch (const std::exception& e) {
            qCritical() << "Exception occurred:" << e.what();
            client->close();
            QCoreApplication::quit();
        }
        
        // ⚠️ 警告：因为变成了异步调用，绝对不能在这里立马执行 quit()！
        // 程序的退出已经转移到了 callToolAsync 的回调内部去控制。
    });

    return app.exec();
}
