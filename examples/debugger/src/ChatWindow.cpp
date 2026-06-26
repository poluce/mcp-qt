#include "ChatWindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QSplitter>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>

ChatWindow::ChatWindow(AgentController* controller, ToolManager* toolManager, QWidget* parent)
    : QMainWindow(parent)
    , m_controller(controller)
    , m_toolManager(toolManager)
{
    setupUi();

    connect(m_controller, &AgentController::logMessage, this, &ChatWindow::appendLog);
    connect(m_controller, &AgentController::statusChanged, this, &ChatWindow::updateStatus);
    connect(m_toolManager, &ToolManager::toolsChanged, this, &ChatWindow::updateToolList);

    // Preset some sensible defaults
    m_editStdioProgram->setText("node");
    m_editStdioArgs->setText(""); 
    m_editHttpUrl->setText("http://localhost:3000/sse");
}

void ChatWindow::setupUi() {
    setWindowTitle("C++ MCP 客户端调试器 (C++17 & Qt6)");
    resize(1000, 700);

    // Premium Slate Dark QSS Stylesheet
    setStyleSheet(
        "QMainWindow { background-color: #1e1e24; color: #f0f0f5; }"
        "QGroupBox { border: 2px solid #3a3a4a; border-radius: 8px; margin-top: 12px; font-weight: bold; color: #a0a0c0; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; }"
        "QLineEdit, QTextEdit, QPlainTextEdit, QComboBox { background-color: #2b2b36; border: 1px solid #4a4a5a; border-radius: 4px; padding: 6px; color: #e0e0e8; font-family: 'Consolas', 'Courier New', monospace; }"
        "QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, QComboBox:focus { border: 1px solid #6c5ce7; }"
        "QPushButton { background-color: #6c5ce7; color: white; border: none; border-radius: 4px; padding: 8px 16px; font-weight: bold; }"
        "QPushButton:hover { background-color: #5b4cc4; }"
        "QPushButton:pressed { background-color: #483ca3; }"
        "QPushButton:disabled { background-color: #4a4a5a; color: #888898; }"
        "QLabel { color: #d0d0d8; }"
    );

    QWidget* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    QVBoxLayout* mainLayout = new QVBoxLayout(centralWidget);

    QHBoxLayout* topLayout = new QHBoxLayout();
    m_lblStatus = new QLabel("状态: 未连接", this);
    m_lblStatus->setStyleSheet("font-size: 14px; font-weight: bold; color: #ff7675;");
    topLayout->addWidget(m_lblStatus);
    topLayout->addStretch();
    mainLayout->addLayout(topLayout);

    QSplitter* splitter = new QSplitter(Qt::Horizontal, this);
    mainLayout->addWidget(splitter);

    QWidget* leftWidget = new QWidget(this);
    QVBoxLayout* leftLayout = new QVBoxLayout(leftWidget);
    leftLayout->setContentsMargins(0, 0, 0, 0);

    // 1. Connection Group
    QGroupBox* connGroup = new QGroupBox("1. 服务端连接配置", this);
    QGridLayout* connLayout = new QGridLayout(connGroup);

    m_radioStdio = new QRadioButton("Stdio (子进程管道重定向)", this);
    m_radioStdio->setChecked(true);
    connLayout->addWidget(m_radioStdio, 0, 0, 1, 2);

    connLayout->addWidget(new QLabel("启动程序/命令:", this), 1, 0);
    m_editStdioProgram = new QLineEdit(this);
    connLayout->addWidget(m_editStdioProgram, 1, 1);

    connLayout->addWidget(new QLabel("运行参数:", this), 2, 0);
    m_editStdioArgs = new QLineEdit(this);
    connLayout->addWidget(m_editStdioArgs, 2, 1);

    m_radioHttp = new QRadioButton("HTTP / SSE (服务端推送事件)", this);
    connLayout->addWidget(m_radioHttp, 3, 0, 1, 2);

    connLayout->addWidget(new QLabel("SSE 终结点地址:", this), 4, 0);
    m_editHttpUrl = new QLineEdit(this);
    connLayout->addWidget(m_editHttpUrl, 4, 1);

    QHBoxLayout* btnConnLayout = new QHBoxLayout();
    m_btnConnect = new QPushButton("连接", this);
    m_btnDisconnect = new QPushButton("断开连接", this);
    m_btnDisconnect->setEnabled(false);
    m_btnShutdown = new QPushButton("安全停机", this);
    m_btnShutdown->setEnabled(false);
    btnConnLayout->addWidget(m_btnConnect);
    btnConnLayout->addWidget(m_btnDisconnect);
    btnConnLayout->addWidget(m_btnShutdown);
    connLayout->addLayout(btnConnLayout, 5, 0, 1, 2);

    leftLayout->addWidget(connGroup);

    // 2. Tool Invoker Group
    QGroupBox* toolGroup = new QGroupBox("2. 工具执行器", this);
    QVBoxLayout* toolLayout = new QVBoxLayout(toolGroup);

    toolLayout->addWidget(new QLabel("选择工具:", this));
    m_comboTools = new QComboBox(this);
    toolLayout->addWidget(m_comboTools);

    m_lblToolDesc = new QLabel("描述: -", this);
    m_lblToolDesc->setWordWrap(true);
    m_lblToolDesc->setStyleSheet("color: #a0a0a0; font-style: italic;");
    toolLayout->addWidget(m_lblToolDesc);

    toolLayout->addWidget(new QLabel("调用参数 (JSON 格式):", this));
    m_editToolArgs = new QTextEdit(this);
    m_editToolArgs->setPlaceholderText("{\n  \"参数1\": \"值\"\n}");
    toolLayout->addWidget(m_editToolArgs);

    m_btnCallTool = new QPushButton("执行工具", this);
    m_btnCallTool->setEnabled(false);
    toolLayout->addWidget(m_btnCallTool);

    leftLayout->addWidget(toolGroup);

    // 3. Resources & Prompts Group
    QGroupBox* extraGroup = new QGroupBox("3. 资源与提示词测试", this);
    QHBoxLayout* extraLayout = new QHBoxLayout(extraGroup);
    m_btnListResources = new QPushButton("获取资源列表", this);
    m_btnListResources->setEnabled(false);
    m_btnListPrompts = new QPushButton("获取提示词列表", this);
    m_btnListPrompts->setEnabled(false);
    extraLayout->addWidget(m_btnListResources);
    extraLayout->addWidget(m_btnListPrompts);
    leftLayout->addWidget(extraGroup);

    splitter->addWidget(leftWidget);

    // 4. Right Log Panel
    QGroupBox* logGroup = new QGroupBox("4. 日志控制台 (JSON-RPC 协议流量)", this);
    QVBoxLayout* logLayout = new QVBoxLayout(logGroup);

    m_txtLog = new QPlainTextEdit(this);
    m_txtLog->setReadOnly(true);
    logLayout->addWidget(m_txtLog);

    splitter->addWidget(logGroup);

    // Connect local triggers
    connect(m_btnConnect, &QPushButton::clicked, this, &ChatWindow::handleConnect);
    connect(m_btnDisconnect, &QPushButton::clicked, this, &ChatWindow::handleDisconnect);
    connect(m_btnShutdown, &QPushButton::clicked, this, &ChatWindow::handleShutdown);
    connect(m_btnCallTool, &QPushButton::clicked, this, &ChatWindow::handleCallTool);
    connect(m_btnListResources, &QPushButton::clicked, this, &ChatWindow::handleListResources);
    connect(m_btnListPrompts, &QPushButton::clicked, this, &ChatWindow::handleListPrompts);
    connect(m_comboTools, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ChatWindow::updateToolDescription);
}

void ChatWindow::handleConnect() {
    if (m_radioStdio->isChecked()) {
        QString prog = m_editStdioProgram->text().trimmed();
        QString argsRaw = m_editStdioArgs->text().trimmed();
        QStringList args = argsRaw.isEmpty() ? QStringList() : argsRaw.split(' ', Qt::SkipEmptyParts);
        if (prog.isEmpty()) {
            QMessageBox::warning(this, "输入错误", "请指定要启动的程序或命令。");
            return;
        }
        m_controller->connectToStdioServer(prog, args);
    } else {
        QString sse = m_editHttpUrl->text().trimmed();
        if (sse.isEmpty()) {
            QMessageBox::warning(this, "输入错误", "请指定 SSE 终结点 URL 地址。");
            return;
        }
        m_controller->connectToHttpServer(sse);
    }

    m_btnConnect->setEnabled(false);
    m_btnDisconnect->setEnabled(true);
}

void ChatWindow::handleDisconnect() {
    if (m_controller->mcpClient()) {
        m_controller->mcpClient()->close();
    }
    m_btnConnect->setEnabled(true);
    m_btnDisconnect->setEnabled(false);
    m_btnShutdown->setEnabled(false);
    m_btnCallTool->setEnabled(false);
    m_btnListResources->setEnabled(false);
    m_btnListPrompts->setEnabled(false);
}

void ChatWindow::handleShutdown() {
    if (m_controller->mcpClient()) {
        m_controller->mcpClient()->shutdownClient();
    }
}

void ChatWindow::handleCallTool() {
    QString toolName = m_comboTools->currentText();
    if (toolName.isEmpty()) return;

    QString argsJson = m_editToolArgs->toPlainText().trimmed();
    m_controller->mcpClient()->callTool(toolName, argsJson);
}

void ChatWindow::handleListResources() {
    if (m_controller->mcpClient()) {
        m_controller->mcpClient()->listResources();
    }
}

void ChatWindow::handleListPrompts() {
    if (m_controller->mcpClient()) {
        m_controller->mcpClient()->listPrompts();
    }
}

void ChatWindow::updateToolList() {
    m_comboTools->clear();
    QStringList names = m_toolManager->toolNames();
    m_comboTools->addItems(names);

    bool hasTools = !names.isEmpty();
    m_btnCallTool->setEnabled(hasTools && m_lblStatus->text().contains("已初始化"));
}

void ChatWindow::updateToolDescription() {
    QString name = m_comboTools->currentText();
    if (name.isEmpty()) {
        m_lblToolDesc->setText("描述: -");
        m_editToolArgs->setPlainText("");
        return;
    }

    QString desc = m_toolManager->toolDescription(name);
    m_lblToolDesc->setText("描述: " + desc);

    mcp::McpTool tool = m_toolManager->getTool(name);
    nlohmann::json schema = tool.inputSchema;
    nlohmann::json templateObj = nlohmann::json::object();

    if (schema.contains("properties") && schema["properties"].is_object()) {
        for (auto& el : schema["properties"].items()) {
            std::string propName = el.key();
            if (el.value().contains("type")) {
                std::string type = el.value()["type"];
                if (type == "string") {
                    templateObj[propName] = "";
                } else if (type == "number" || type == "integer") {
                    templateObj[propName] = 0;
                } else if (type == "boolean") {
                    templateObj[propName] = false;
                } else {
                    templateObj[propName] = nullptr;
                }
            }
        }
    }
    
    std::string templateStr = templateObj.dump(2);
    m_editToolArgs->setPlainText(QString::fromStdString(templateStr));
}

void ChatWindow::appendLog(const QString& msg) {
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    m_txtLog->appendPlainText(QString("[%1] %2").arg(timestamp, msg));
}

void ChatWindow::updateStatus(const QString& status) {
    m_lblStatus->setText("状态: " + status);
    if (status == "已初始化") {
        m_lblStatus->setStyleSheet("font-size: 14px; font-weight: bold; color: #1dd1a1;");
        m_btnCallTool->setEnabled(m_comboTools->count() > 0);
        m_btnShutdown->setEnabled(true);
        m_btnListResources->setEnabled(true);
        m_btnListPrompts->setEnabled(true);
    } else if (status.startsWith("正在连接")) {
        m_lblStatus->setStyleSheet("font-size: 14px; font-weight: bold; color: #feca57;");
    } else {
        m_lblStatus->setStyleSheet("font-size: 14px; font-weight: bold; color: #ff7675;");
        m_btnShutdown->setEnabled(false);
        m_btnListResources->setEnabled(false);
        m_btnListPrompts->setEnabled(false);
        m_btnDisconnect->setEnabled(false);
        m_btnConnect->setEnabled(true);
    }
}
