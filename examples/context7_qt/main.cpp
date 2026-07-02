#include <QCoreApplication>
#include <QTimer>
#include <QDebug>
#include <iostream>
#include <mcp_qt_client/McpQtClient.h>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace mcp_qt;

int main(int argc, char *argv[]) {
#ifdef Q_OS_WIN
    // 强制 Windows 控制台使用 UTF-8 编码输出
    SetConsoleOutputCP(CP_UTF8);
#endif
    QCoreApplication app(argc, argv);

    qInfo() << "Starting Context7 MCP server via Stdio...";
    
#ifdef Q_OS_WIN
    QString command = "npx.cmd";
#else
    QString command = "npx";
#endif

    QStringList args = {"-y", "@upstash/context7-mcp"};
    
    qInfo() << "Waiting for npx to start and initialize Context7... (this might take up to 60s if downloading)";
    
    // 我们这次使用全新的 Async 异步工厂，这样就能在握手开始前绑定信号了！
    auto client = McpQtClient::connectStdioAsync(command, args, "Context7Example", "1.0");
    if (!client) {
        qCritical() << "Failed to allocate Context7 MCP Server client!";
        return -1;
    }
    
    // 【关键】现在我们可以在初始化完成前，就能监听到 npx 传来的进度和日志了
    QObject::connect(client.get(), &McpQtClient::errorOccurred, [](const QString& message) {
        // npx 的标准错误通常包含下载进度或纯日志
        qDebug().noquote() << "[NPX Log/Progress] =>" << message.trimmed();
    });

    // 绑定连接成功事件
    QObject::connect(client.get(), &McpQtClient::connected, [client, &app]() {
        qInfo() << "\nContext7 MCP Server connected and initialized! Fetching tools...";
        
        try {
            auto tools = client->listTools(30000);
            
            qInfo() << "\n=== Available tools from Context7 ===";
            if (tools.empty()) {
                qWarning() << "No tools found! The server might not provide any tools.";
            } else {
                for (const auto& t : tools) {
                    std::cout << " - " << t.name.toStdString() << ":\n" 
                              << t.description.toStdString() << "\n\n";
                }
            }

            // 也顺便拉取一下 Resources，因为 Context7 主要可能提供的是 Resources
            auto resourcesObj = client->listResources(30000);
            QJsonArray resources = resourcesObj["resources"].toArray();
            qInfo() << "\n=== Available resources from Context7 ===";
            if (resources.isEmpty()) {
                qWarning() << "No resources found either.";
            } else {
                for (const auto& rVal : resources) {
                    QJsonObject r = rVal.toObject();
                    std::cout << " - " << r["name"].toString().toStdString() << " (" << r["uri"].toString().toStdString() << ")\n" 
                              << r["description"].toString().toStdString() << "\n\n";
                }
            }

            // 拉取 Prompts
            auto promptsObj = client->listPrompts(30000);
            QJsonArray prompts = promptsObj["prompts"].toArray();
            qInfo() << "\n=== Available prompts from Context7 ===";
            if (prompts.isEmpty()) {
                qWarning() << "No prompts found either.";
            } else {
                for (const auto& pVal : prompts) {
                    QJsonObject p = pVal.toObject();
                    std::cout << " - " << p["name"].toString().toStdString() << "\n" 
                              << p["description"].toString().toStdString() << "\n\n";
                }
            }
        } catch (const std::exception& e) {
            qCritical() << "Exception occurred while fetching data:" << e.what();
        }
        
        qInfo() << "Shutting down...";
        client->close();
        QCoreApplication::quit();
    });

    // 超时保护
    QTimer::singleShot(60000, [client, &app]() {
        if (!client->isConnected()) {
            qCritical() << "Initialization timed out after 60 seconds!";
            client->close();
            app.quit();
        }
    });

    return app.exec();
}
