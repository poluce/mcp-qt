#include <QCoreApplication>
#include <QTimer>
#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QRegularExpression>
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

    qInfo() << "Starting Playwright MCP server via Stdio...";
    
#ifdef Q_OS_WIN
    QString command = "npx.cmd";
#else
    QString command = "npx";
#endif

    QStringList args;
    args << "-y" << "@playwright/mcp";
    
    qInfo() << "Waiting for npx to start and initialize Playwright... (this might take up to 60s if downloading)";
    
    // 我们这次使用全新的 Async 异步工厂，这样就能在握手开始前绑定信号了！
    auto client = McpQtClient::connectStdioAsync(command, args, "PlaywrightExample", "1.0");
    if (!client) {
        qCritical() << "Failed to allocate Playwright MCP Server client!";
        return -1;
    }
    
    // 【关键】现在我们可以在初始化完成前，就能监听到 npx 传来的进度和日志了
    QObject::connect(client.get(), &McpQtClient::errorOccurred, [](const QString& message) {
        // npx 的标准错误通常包含下载进度或纯日志
        qDebug().noquote() << "[NPX Log/Progress] =>" << message.trimmed();
    });

    // 绑定连接成功事件
    QObject::connect(client.get(), &McpQtClient::connected, [client, &app]() {
        qInfo() << "\nPlaywright MCP Server connected and initialized! Fetching tools...";
        
        try {
            auto tools = client->listTools(30000);
            
            qInfo() << "\n=== Available tools from Playwright ===";
            if (tools.empty()) {
                qWarning() << "No tools found! The server might not provide any tools.";
            } else {
                for (const auto& t : tools) {
                    std::cout << " - " << t.name.toStdString() << ":\n" 
                              << t.description.toStdString() << "\n\n";
                    if (t.name == "browser_take_screenshot") {
                        std::cout << ">>> " << t.name.toStdString() << " Schema:\n"
                                  << QJsonDocument(t.inputSchema).toJson(QJsonDocument::Indented).toStdString() << "\n\n";
                    }
                }
            }

            // 也顺便拉取一下 Resources，因为 Playwright 主要可能提供的是 Resources
            auto resourcesObj = client->listResources(30000);
            QJsonArray resources = resourcesObj["resources"].toArray();
            qInfo() << "\n=== Available resources from Playwright ===";
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
            qInfo() << "\n=== Available prompts from Playwright ===";
            if (prompts.isEmpty()) {
                qWarning() << "No prompts found either.";
            } else {
                for (const auto& pVal : prompts) {
                    QJsonObject p = pVal.toObject();
                    std::cout << " - " << p["name"].toString().toStdString() << "\n" 
                              << p["description"].toString().toStdString() << "\n\n";
                }
            }

            // =========================================================================
            // 🌟 浏览器自动化测试流程：打开 Google 搜寻今日日期 -> 截图结果 -> 关闭浏览器
            // =========================================================================
            if (!tools.empty()) {
                qInfo() << "\n>>> Starting Browser Automation Test Flow...";
                
                // 1. 导航到 google.com
                qInfo() << "Step 1: Navigating to google.com...";
                QJsonObject navArgs;
                navArgs["url"] = "https://www.google.com";
                auto navRes = client->callTool("browser_navigate", navArgs, 30000);
                if (navRes.isError) {
                    qCritical() << "Failed to navigate to google.com:" << navRes.errorString;
                } else {
                    qInfo() << "Successfully navigated to google.com!";
                    
                    // 2. 获取无障碍快照，寻找搜索框的 ref
                    qInfo() << "Step 2: Taking accessibility tree snapshot to find the search box...";
                    auto snapRes = client->callTool("browser_snapshot", QJsonObject{}, 30000);
                    if (snapRes.isError) {
                        qCritical() << "Failed to take page snapshot:" << snapRes.errorString;
                    } else {
                        QString treeText = snapRes.data.value("text").toString();
                        QJsonArray contents = snapRes.data.value("content").toArray();
                        if (treeText.isEmpty() && !contents.isEmpty()) {
                            treeText = contents.at(0).toObject().value("text").toString();
                        }
                        
                        qDebug() << "Page Snapshot (truncated):" << treeText.left(500);
                        
                        // 用正则表达式在树状文本中搜寻搜索框 combo/textbox
                        QRegularExpression re("(?:textbox|combobox).*?(?:Search|Google).*?\\[ref=(e\\d+)\\]", QRegularExpression::CaseInsensitiveOption);
                        QRegularExpressionMatch match = re.match(treeText);
                        QString searchRef;
                        if (match.hasMatch()) {
                            searchRef = match.captured(1);
                            qInfo() << "Found Google search box ref:" << searchRef;
                        } else {
                            // 备用方案：搜寻第一个出现的文本框
                            QRegularExpression backupRe("(?:textbox|combobox).*?\\[ref=(e\\d+)\\]");
                            QRegularExpressionMatch backupMatch = backupRe.match(treeText);
                            if (backupMatch.hasMatch()) {
                                searchRef = backupMatch.captured(1);
                                qInfo() << "Fallback: Found first input box ref:" << searchRef;
                            }
                        }
                        
                        if (searchRef.isEmpty()) {
                            searchRef = "e5"; // 最后的硬编码兜底
                            qWarning() << "Could not find search box ref dynamically. Falling back to default 'e5'";
                        }
                        
                        // 3. 在搜索框中输入“北京时间”并提交搜索
                        qInfo() << "Step 3: Typing '北京时间' into the search box...";
                        QJsonObject fillArgs;
                        fillArgs["target"] = searchRef;
                        fillArgs["text"] = QString::fromUtf8(u8"北京时间");
                        fillArgs["submit"] = true;
                        auto fillRes = client->callTool("browser_type", fillArgs, 30000);
                        if (fillRes.isError) {
                            qCritical() << "Failed to type search content:" << fillRes.errorString;
                        } else {
                            // 4. 按下 Enter 键触发搜索（双重保险，以防 submit=true 没有自动触发）
                            qInfo() << "Step 4: Pressing Enter to search...";
                            QJsonObject pressArgs;
                            pressArgs["key"] = "Enter";
                            auto pressRes = client->callTool("browser_press_key", pressArgs, 30000);
                            if (pressRes.isError) {
                                qCritical() << "Failed to press Enter:" << pressRes.errorString;
                            } else {
                                qInfo() << "Search triggered successfully! Waiting 3s for results to render...";
                                
                                // 用局部事件循环做非阻塞的 3 秒等待
                                QEventLoop delayLoop;
                                QTimer::singleShot(3000, &delayLoop, &QEventLoop::quit);
                                delayLoop.exec();
                                
                                // 5. 截图并保存以展示搜索结果
                                qInfo() << "Step 5: Taking a screenshot of the search results...";
                                QJsonObject shotArgs;
                                shotArgs["type"] = "png";
                                shotArgs["scale"] = "css";
                                auto shotRes = client->callTool("browser_take_screenshot", shotArgs, 30000);
                                if (shotRes.isError) {
                                    qCritical() << "Failed to take screenshot:" << shotRes.errorString;
                                } else {
                                     QString base64Data;
                                     QJsonArray shotContent = shotRes.data.value("content").toArray();
                                     if (!shotContent.isEmpty()) {
                                         for (const auto& itemVal : shotContent) {
                                             QJsonObject item = itemVal.toObject();
                                             if (item.value("type").toString() == "image" || item.contains("data")) {
                                                 base64Data = item.value("data").toString();
                                                 break;
                                             }
                                         }
                                     } else {
                                         base64Data = shotRes.data.value("screenshot").toString();
                                     }
                                    
                                    if (!base64Data.isEmpty()) {
                                        QByteArray imageBytes = QByteArray::fromBase64(base64Data.toLatin1());
                                        QFile file("google_search_result.png");
                                        if (file.open(QIODevice::WriteOnly)) {
                                            file.write(imageBytes);
                                            file.close();
                                            qInfo() << "SUCCESS: Screenshot saved to google_search_result.png!";
                                        } else {
                                            qCritical() << "Failed to open local file to save screenshot!";
                                        }
                                    } else {
                                        qWarning() << "Screenshot data was empty. Response:" << QJsonDocument(shotRes.data).toJson(QJsonDocument::Compact);
                                    }
                                }
                            }
                        }
                    }
                }
                
                // 6. 关闭页面
                qInfo() << "Step 6: Closing browser page...";
                client->callTool("browser_close", QJsonObject{}, 10000);
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
