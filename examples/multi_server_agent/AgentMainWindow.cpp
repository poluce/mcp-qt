#include "AgentMainWindow.h"
#include "LlmBackends.h"

#include <mcp_qt_client/McpJsonConfigLoader.h>
#include <mcp_qt_client/McpToolRouter.h>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonArray>
#include <QCoreApplication>
#include <QScrollBar>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QProgressDialog>
#include <QDialog>
#include <QDesktopServices>
#include <QUrl>
#include <QVBoxLayout>
#include <iostream>

#include <mutex>
#include <QFile>

// 全局日志文件路径定义
QString g_logFilePath;

// 🌟 全局日志长连接文件指针与其互斥锁、更新函数声明（定义在 main.cpp 中，全局命名空间）
extern QFile* g_logFile;
extern std::mutex g_logMutex;
extern void updateGlobalLogFile(const QString& path);

namespace mcp_agent {

// 🌟 辅助读取配置文件中的默认日志路径
static QString readLogFileFromConfig(const QString& configPath) {
    if (configPath.isEmpty() || !QFileInfo::exists(configPath)) {
        return "";
    }
    QFile file(configPath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QByteArray data = file.readAll();
        file.close();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isObject()) {
            QJsonObject root = doc.object();
            return root["logFile"].toString();
        }
    }
    return "";
}

// 🌟 辅助将日志路径持久化写回配置文件中
static void writeLogFileToConfig(const QString& configPath, const QString& logFilePath) {
    if (configPath.isEmpty() || !QFileInfo::exists(configPath)) {
        return;
    }
    QFile file(configPath);
    QJsonObject root;
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QByteArray data = file.readAll();
        file.close();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isObject()) {
            root = doc.object();
        }
    }

    // 插入或更新 logFile 字段
    root["logFile"] = logFilePath;

    // 覆盖写回配置文件
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QJsonDocument newDoc(root);
        file.write(newDoc.toJson(QJsonDocument::Indented));
        file.close();
    }
}

AgentMainWindow::AgentMainWindow(QWidget* parent)
    : QMainWindow(parent) 
{
    m_network = new QNetworkAccessManager(this);
    m_host = new mcp_qt::McpHost(this); // 🌟 初始化全局单例 m_host

    initUi();
    applyTheme();
    setupMcpAppRenderer();

    // 默认搜寻本地配置文件，提供极佳体验
    QString defaultCfg = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../examples/multi_server_agent/examples_config.json");
    if (!QFileInfo::exists(defaultCfg)) {
        defaultCfg = QDir(QDir::currentPath()).absoluteFilePath("examples/multi_server_agent/examples_config.json");
    }
    if (QFileInfo::exists(defaultCfg)) {
        m_configPathEdit->setText(QDir::cleanPath(defaultCfg));
        
        // 自动读取默认配置里的日志保存路径并填入
        QString savedLogFile = readLogFileFromConfig(m_configPathEdit->text());
        if (!savedLogFile.isEmpty()) {
            g_logFilePath = savedLogFile;
            m_logPathEdit->setText(QDir::toNativeSeparators(savedLogFile));
        }

        loadAndConnectServers(m_configPathEdit->text());
    }

    // 如果命令行已经显式传递了日志路径（优先级最高），则覆盖填充
    if (!g_logFilePath.isEmpty()) {
        m_logPathEdit->setText(QDir::toNativeSeparators(g_logFilePath));
    }

    // 🌟 在构造函数里绑定所有 m_host 相关的长期信号，防止重复连接
    connect(m_host, &mcp_qt::McpHost::serverStateChanged, this, [this]() {
        updateServerList();
    });

    connect(m_host, &mcp_qt::McpHost::hostReady, this, [this](bool success, const QString& msg) {
        if (!m_isRunning && !m_sessionActive) {
            m_runBtn->setEnabled(true);
            m_runBtn->setText(QStringLiteral("⚡ 启动 Agent 任务"));
        }
        if (!success) {
            appendLogHtml(QString("<div style='color:red;'>%1</div>").arg(msg.toHtmlEscaped()));
        } else {
            m_serverLogConsole->append(m_host->getDiagnosticReport());
        }
    });

    connect(m_host, &mcp_qt::McpHost::errorOccurred, this, [this](const QString& name, const mcp_qt::McpError& err) {
        appendLogHtml(QString("<div style='color:red;'>[Error] %1: %2</div>").arg(name, err.message));
    });

    connect(m_host, &mcp_qt::McpHost::inputRequired, this, [this](const QString& srvName, const QString& reqId, const QJsonObject& inputRequests, const QString& requestState, mcp_qt::MrtrReplyCallback cb) {
        appendLogHtml(QString("<div style='color:#e67e22;'><b>[MRTR 2026-07-28 多轮交互]</b> 节点 %1 请求补充输入 (ReqID: %2)</div>").arg(srvName, reqId));
        // 演示：遍历规范 InputRequests map，构造按 key 组织的 InputResponses；
        // requestState 由库在重发时原样回显（客户端 MUST NOT 解析/修改）
        QJsonObject responses;
        const QJsonObject requests = inputRequests;
        for (auto it = requests.begin(); it != requests.end(); ++it) {
            const QJsonObject req = it.value().toObject();
            if (req.value(QStringLiteral("method")).toString() == QStringLiteral("elicitation/create")) {
                const QJsonObject params = req.value(QStringLiteral("params")).toObject();
                const QJsonObject schema = params.value(QStringLiteral("requestedSchema")).toObject();
                QJsonObject content;
                const QJsonObject props = schema.value(QStringLiteral("properties")).toObject();
                for (auto p = props.begin(); p != props.end(); ++p) {
                    content.insert(p.key(), QStringLiteral("demo-secret"));
                }
                responses.insert(it.key(), QJsonObject{{QStringLiteral("action"), QStringLiteral("accept")}, {QStringLiteral("content"), content}});
            }
        }
        Q_UNUSED(requestState);
        cb(responses);
    });
}

void AgentMainWindow::initUi() {
    setWindowTitle(QStringLiteral("MCP Qt ReAct Agent 浏览器探索工具"));
    resize(1000, 750);

    auto* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    auto* mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(15, 15, 15, 15);
    mainLayout->setSpacing(12);

    // ==========================================
    // 1. 配置区
    // ==========================================
    auto* configLayout = new QHBoxLayout();
    auto* cfgLabel = new QLabel(QStringLiteral("MCP 配置文件:"), this);
    m_configPathEdit = new QLineEdit(this);
    m_configPathEdit->setPlaceholderText(QStringLiteral("请输入或选择 MCP 配置文件 examples_config.json 的路径..."));
    m_browseBtn = new QPushButton(QStringLiteral("浏览文件"), this);
    m_refreshBtn = new QPushButton(QStringLiteral("刷新/重载"), this);
    configLayout->addWidget(cfgLabel);
    configLayout->addWidget(m_configPathEdit);
    configLayout->addWidget(m_browseBtn);
    configLayout->addWidget(m_refreshBtn);
    mainLayout->addLayout(configLayout);

    // 新增：日志文件输出指定
    auto* logFileLayout = new QHBoxLayout();
    auto* logLabel = new QLabel(QStringLiteral("日志输出文件:"), this);
    // 保持与配置文件Label等宽
    cfgLabel->setMinimumWidth(85);
    logLabel->setMinimumWidth(85);

    m_logPathEdit = new QLineEdit(this);
    m_logPathEdit->setPlaceholderText(QStringLiteral("（可选）请输入或选择一个本地文件路径，程序运行日志将同步追加写入该文件中..."));
    m_logBrowseBtn = new QPushButton(QStringLiteral("选择位置"), this);
    logFileLayout->addWidget(logLabel);
    logFileLayout->addWidget(m_logPathEdit);
    logFileLayout->addWidget(m_logBrowseBtn);
    mainLayout->addLayout(logFileLayout);

    // ==========================================
    // 2. 大模型配置区
    // ==========================================
    auto* llmGrid = new QGridLayout();
    llmGrid->setHorizontalSpacing(10);
    llmGrid->setVerticalSpacing(8);

    auto* modeLabel = new QLabel(QStringLiteral("大模型模式:"), this);
    m_modeCombo = new QComboBox(this);
    m_modeCombo->addItem(QStringLiteral("离线 Mock 模拟器"));
    m_modeCombo->addItem(QStringLiteral("在线 OpenAI 兼容 API"));

    auto* urlLabel = new QLabel(QStringLiteral("接口地址 (API URL):"), this);
    m_apiUrlEdit = new QLineEdit(this);
    m_apiUrlEdit->setText(QStringLiteral("https://api.deepseek.com/v1/chat/completions"));
    m_apiUrlEdit->setEnabled(false);

    auto* keyLabel = new QLabel(QStringLiteral("API 密钥 (API Key):"), this);
    m_apiKeyEdit = new QLineEdit(this);
    m_apiKeyEdit->setEchoMode(QLineEdit::Password);
    m_apiKeyEdit->setPlaceholderText(QStringLiteral("sk-xxxxxx"));
    m_apiKeyEdit->setEnabled(false);

    auto* modelLabel = new QLabel(QStringLiteral("模型名称 (Model):"), this);
    
    // 🌟 将下拉框和获取模型按钮放在横向布局中，保持紧凑整齐
    auto* modelLayout = new QHBoxLayout();
    modelLayout->setSpacing(6);
    modelLayout->setContentsMargins(0, 0, 0, 0);

    m_modelCombo = new QComboBox(this);
    m_modelCombo->setEditable(true); // 支持下拉选择，也支持键盘手动录入任何自定义模型名
    m_modelCombo->addItem(QStringLiteral("deepseek-v4-flash"));
    m_modelCombo->addItem(QStringLiteral("deepseek-chat"));
    m_modelCombo->addItem(QStringLiteral("deepseek-reasoner"));
    m_modelCombo->addItem(QStringLiteral("gpt-4o-mini"));
    m_modelCombo->addItem(QStringLiteral("gpt-4o"));
    m_modelCombo->addItem(QStringLiteral("llama3"));
    m_modelCombo->setEnabled(false);

    m_fetchModelsBtn = new QPushButton(QStringLiteral("同步模型列表"), this);
    m_fetchModelsBtn->setEnabled(false);
    m_fetchModelsBtn->setMaximumWidth(120);

    modelLayout->addWidget(m_modelCombo, 1);
    modelLayout->addWidget(m_fetchModelsBtn);

    llmGrid->addWidget(modeLabel, 0, 0);
    llmGrid->addWidget(m_modeCombo, 0, 1);
    llmGrid->addWidget(urlLabel, 0, 2);
    llmGrid->addWidget(m_apiUrlEdit, 0, 3);
    
    llmGrid->addWidget(keyLabel, 1, 0);
    llmGrid->addWidget(m_apiKeyEdit, 1, 1);
    llmGrid->addWidget(modelLabel, 1, 2);
    llmGrid->addLayout(modelLayout, 1, 3);

    mainLayout->addLayout(llmGrid);

    // 🌟 自动从系统环境变量中尝试提取默认的 API 密钥回显到输入框中，DEEPSEEK_API_KEY 优先
    QString defaultKey = qEnvironmentVariable("DEEPSEEK_API_KEY");
    if (defaultKey.isEmpty()) {
        defaultKey = qEnvironmentVariable("OPENAI_API_KEY");
    }
    if (!defaultKey.isEmpty()) {
        m_apiKeyEdit->setText(defaultKey);
    }

    // ==========================================
    // 3. 主核心显示区
    // ==========================================
    auto* displayLayout = new QHBoxLayout();
    displayLayout->setSpacing(12);

    // 左侧：服务器状态 + 服务端日志
    auto* leftLayout = new QVBoxLayout();
    auto* srvLabel = new QLabel(QStringLiteral("MCP 服务端状态:"), this);
    m_serverListWidget = new QListWidget(this);
    m_serverListWidget->setMaximumWidth(220);
    leftLayout->addWidget(srvLabel);
    leftLayout->addWidget(m_serverListWidget);

    auto* logConsoleLabel = new QLabel(QStringLiteral("服务端日志 (stderr):"), this);
    m_serverLogConsole = new QTextEdit(this);
    m_serverLogConsole->setReadOnly(true);
    m_serverLogConsole->setMaximumHeight(180);
    m_serverLogConsole->setPlaceholderText(QStringLiteral("服务端子进程的 stderr 输出将显示在这里..."));
    leftLayout->addWidget(logConsoleLabel);
    leftLayout->addWidget(m_serverLogConsole);
    displayLayout->addLayout(leftLayout, 1);

    // 右侧：QSplitter 分屏（ReAct 执行看板 + MCP App 渲染）
    m_rightSplitter = new QSplitter(Qt::Vertical, this);

    auto* blackboardBox = new QWidget(this);
    auto* bbLayout = new QVBoxLayout(blackboardBox);
    bbLayout->setContentsMargins(0, 0, 0, 0);
    auto* blackboardLabel = new QLabel(QStringLiteral("ReAct 执行过程看板:"), this);
    m_logBlackboard = new QTextEdit(this);
    m_logBlackboard->setReadOnly(true);
    m_logBlackboard->setHtml(QStringLiteral("<h3 style='color: #8e8e93; font-family: Segoe UI, Microsoft YaHei;'>系统空闲中。请在下方输入您的任务指令并点击启动...</h3>"));
    bbLayout->addWidget(blackboardLabel);
    bbLayout->addWidget(m_logBlackboard);
    m_rightSplitter->addWidget(blackboardBox);

    // MCP App 渲染容器（默认隐藏，工具返回 MCP Apps UI 时展开）
    m_mcpAppContainer = new QWidget(this);
    auto* appLayout = new QVBoxLayout(m_mcpAppContainer);
    appLayout->setContentsMargins(0, 0, 0, 0);
    auto* appLabel = new QLabel(QStringLiteral("MCP App 渲染（工具返回的交互界面）:"), this);
    appLayout->addWidget(appLabel);
    m_rightSplitter->addWidget(m_mcpAppContainer);
    m_rightSplitter->setStretchFactor(0, 3);
    m_rightSplitter->setStretchFactor(1, 2);
    m_mcpAppContainer->hide();
    displayLayout->addWidget(m_rightSplitter, 3);

    mainLayout->addLayout(displayLayout, 1);

    // ==========================================
    // 4. 发射指令区
    // ==========================================
    auto* runLayout = new QHBoxLayout();
    m_taskInputEdit = new QLineEdit(this);
    m_taskInputEdit->setPlaceholderText(QStringLiteral("在此输入您想让 Agent 执行的任务（例如：\"search for test\" 或 \"search for AI news and take a screenshot\"）..."));
    
    m_runBtn = new QPushButton(QStringLiteral("⚡ 启动 Agent 任务"), this);
    m_runBtn->setMinimumWidth(150);

    m_resetSessionBtn = new QPushButton(QStringLiteral("🧹 新建对话"), this);
    m_resetSessionBtn->setMinimumWidth(100);
    m_resetSessionBtn->setEnabled(false);
    m_resetSessionBtn->setStyleSheet(QStringLiteral("background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #8e8e93, stop:1 #636366);"));

    runLayout->addWidget(m_taskInputEdit);
    runLayout->addWidget(m_runBtn);
    runLayout->addWidget(m_resetSessionBtn);
    mainLayout->addLayout(runLayout);

    // ==========================================
    // 信号与槽的联结
    // ==========================================
    connect(m_browseBtn, &QPushButton::clicked, this, &AgentMainWindow::handleBrowseConfig);
    connect(m_refreshBtn, &QPushButton::clicked, this, [this]() {
        QString path = m_configPathEdit->text().trimmed();
        if (!path.isEmpty()) {
            loadAndConnectServers(path);
        }
    });
    connect(m_logBrowseBtn, &QPushButton::clicked, this, &AgentMainWindow::handleBrowseLogFile);
    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &AgentMainWindow::handleModeChanged);
    connect(m_runBtn, &QPushButton::clicked, this, &AgentMainWindow::handleRunTask);
    connect(m_taskInputEdit, &QLineEdit::returnPressed, this, &AgentMainWindow::handleRunTask);
    connect(m_fetchModelsBtn, &QPushButton::clicked, this, &AgentMainWindow::handleFetchModels);
    connect(m_resetSessionBtn, &QPushButton::clicked, this, &AgentMainWindow::handleResetSession);

    connect(m_modelCombo, &QComboBox::currentTextChanged, this, [this](){}); // To fix any warnings if needed, but the important is below
    connect(m_serverListWidget, &QListWidget::itemDoubleClicked, this, &AgentMainWindow::handleServerDoubleClicked);

    // 🌟 默认选中“在线 OpenAI 兼容 API”模式，开启全部配置框输入权限
    m_modeCombo->setCurrentIndex(1);
}

void AgentMainWindow::applyTheme() {
    // 现代浅色主题 (Modern Light)：统一色板、卡片圆角、hover 反馈、清晰层级
    QString qss = R"(
        QMainWindow, QWidget {
            background-color: #f5f7fa;
            color: #1f2329;
            font-family: "Segoe UI", "Microsoft YaHei";
        }
        QLabel {
            color: #30343a;
            font-size: 12px;
            font-weight: 600;
        }
        QLineEdit, QComboBox, QTextEdit, QListWidget {
            background-color: #ffffff;
            border: 1px solid #d8dde3;
            border-radius: 6px;
            padding: 6px;
            color: #1f2329;
            selection-background-color: #dbeafe;
            selection-color: #1f2329;
        }
        QLineEdit:focus, QComboBox:focus, QTextEdit:focus {
            border: 1px solid #3b82f6;
        }
        QLineEdit:disabled, QComboBox:disabled {
            background-color: #eef1f4;
            color: #9aa2ab;
        }
        QComboBox::drop-down {
            border: none;
            width: 22px;
        }
        QPushButton {
            background-color: #3b82f6;
            border: none;
            border-radius: 6px;
            color: #ffffff;
            padding: 7px 16px;
            font-weight: 600;
        }
        QPushButton:hover {
            background-color: #2f74e0;
        }
        QPushButton:pressed {
            background-color: #2563eb;
        }
        QPushButton:disabled {
            background-color: #d3d9df;
            color: #8a929b;
        }
        QPushButton[secondary="true"] {
            background-color: #6b7280;
        }
        QPushButton[secondary="true"]:hover {
            background-color: #5b6470;
        }
        QListWidget {
            padding: 4px;
        }
        QListWidget::item {
            padding: 6px;
            border-radius: 4px;
        }
        QListWidget::item:hover {
            background-color: #eef2f7;
        }
        QListWidget::item:selected {
            background-color: #dbeafe;
            color: #1f2329;
        }
        QTextEdit {
            padding: 8px;
        }
        QSplitter::handle {
            background-color: #e2e6ea;
        }
        QSplitter::handle:vertical {
            height: 2px;
        }
        QScrollBar:vertical {
            background: transparent;
            width: 10px;
        }
        QScrollBar::handle:vertical {
            background: #c9cfd6;
            border-radius: 5px;
            min-height: 30px;
        }
        QScrollBar::handle:vertical:hover {
            background: #adb5bd;
        }
    )";
    setStyleSheet(qss);
}

// ========== MCP Apps 渲染（内嵌 WebView2 + AppBridge）==========
void AgentMainWindow::setupMcpAppRenderer() {
    if (m_mcpAppRenderer || !m_mcpAppContainer) return;
    m_mcpAppRenderer = new mcp_qt::McpAppWebView2Renderer(m_mcpAppContainer);
    m_mcpAppRenderer->setUiMeta(QJsonObject{});  // 无 csp/permissions 声明 → 限制性默认

    m_mcpAppBridge = std::make_shared<mcp_qt::McpAppBridge>();
    m_mcpAppBridge->attach(m_mcpAppRenderer, nullptr);  // 占位（渲染具体 App 时替换为该服务器 client）
    m_mcpAppBridge->setHostInfo(QStringLiteral("mcp-qt-multi-agent"), QStringLiteral("1.0.0"));
    m_mcpAppBridge->setOpenLinkHandler([](const QJsonObject& params, mcp_qt::McpAppBridge::UiRequestRespond respond) {
        const QUrl url(params.value(QStringLiteral("url")).toString());
        if (!QDesktopServices::openUrl(url)) {
            respond(QJsonObject{}, -1, QStringLiteral("open link failed"));
            return;
        }
        respond(QJsonObject{}, 0, QString());
    });
    m_mcpAppBridge->setDisplayModes({QStringLiteral("inline"), QStringLiteral("fullscreen")});
    m_mcpAppBridge->start();

    if (auto* appLayout = m_mcpAppContainer->layout()) {
        appLayout->addWidget(m_mcpAppRenderer->hostWidget());
    }

    // displayMode 协商通过 → 窗口全屏/普通切换
    connect(m_mcpAppBridge.get(), &mcp_qt::McpAppBridge::displayModeChanged, this, [this](const QString& mode) {
        if (mode == QLatin1String("fullscreen")) this->showFullScreen();
        else if (mode == QLatin1String("inline")) this->showNormal();
    });
    // size-changed → 弹性尺寸调整 App 面板
    connect(m_mcpAppBridge.get(), &mcp_qt::McpAppBridge::appSizeChanged, this, [this](int w, int h) {
        Q_UNUSED(w);
        if (m_mcpAppContainer && m_mcpAppVisible) {
            m_mcpAppContainer->setMinimumHeight(qMax(120, h));
            m_mcpAppContainer->adjustSize();
        }
    });

    m_mcpAppRenderer->initializeAsync([](bool ok, const QString& err) {
        if (!ok) qWarning() << "MCP App WebView2 init failed:" << err;
    });
}

void AgentMainWindow::showMcpAppPanel(bool visible) {
    if (!m_mcpAppContainer) return;
    m_mcpAppVisible = visible;
    m_mcpAppContainer->setVisible(visible);
    if (visible && m_rightSplitter) {
        const QList<int> sizes = m_rightSplitter->sizes();
        if (sizes.size() == 2) {
            const int total = sizes[0] + sizes[1];
            m_rightSplitter->setSizes({total * 3 / 5, total * 2 / 5});  // 看板 60% / App 40%
        }
    }
}

void AgentMainWindow::handleMcpAppContent(const QString& html, const QString& toolName) {
    if (!m_mcpAppRenderer || !m_mcpAppBridge) setupMcpAppRenderer();
    if (!m_mcpAppRenderer) return;

    // 从 namespaced 工具名（serverName_toolName）解析 serverName，attach 真实 client
    const auto pair = m_host->toolRouter()->parseToolName(toolName);
    const QString serverName = pair.first;
    if (!serverName.isEmpty()) {
        auto client = m_host->client(serverName);
        if (client) m_mcpAppBridge->attach(m_mcpAppRenderer, client);
    }

    m_mcpAppRenderer->loadHtml(html, QUrl());
    showMcpAppPanel(true);
    appendLogHtml(QStringLiteral("<p style='color:#0a7d33;'><b>◇ MCP App 已渲染：</b>%1</p>")
                      .arg(toolName.toHtmlEscaped()));
}

void AgentMainWindow::handleBrowseConfig() {
    QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Select MCP Server Config"), "", "*.json");
    if (!path.isEmpty()) {
        m_configPathEdit->setText(QDir::toNativeSeparators(path));
        
        // 切换了配置文件，自动尝试读取新配置文件里的 logFile
        QString savedLogFile = readLogFileFromConfig(path);
        if (!savedLogFile.isEmpty()) {
            g_logFilePath = savedLogFile;
            m_logPathEdit->setText(QDir::toNativeSeparators(savedLogFile));
        } else {
            // 如果新配置文件中没有指定，清空当前的日志路径，保持一致
            m_logPathEdit->clear();
            g_logFilePath.clear();
        }
        
        loadAndConnectServers(path);
    }
}

void AgentMainWindow::handleBrowseLogFile() {
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("选择日志保存位置"), "", "*.log");
    if (!path.isEmpty()) {
        m_logPathEdit->setText(QDir::toNativeSeparators(path));
        g_logFilePath = path; // 同步到全局变量
        updateGlobalLogFile(path); // 🌟 重新打开新的日志文件连接
    }
}

void AgentMainWindow::handleModeChanged(int index) {
    bool enableLlm = (index == 1);
    m_apiUrlEdit->setEnabled(enableLlm);
    m_apiKeyEdit->setEnabled(enableLlm);
    m_modelCombo->setEnabled(enableLlm);
    m_fetchModelsBtn->setEnabled(enableLlm);
}

void AgentMainWindow::handleRunTask() {
    if (m_isRunning) return;

    QString configPath = m_configPathEdit->text().trimmed();
    QString task = m_taskInputEdit->text().trimmed();
    
    // 同步输入框中的日志文件路径到全局变量，并物理更新文件句柄
    QString currentLogPath = m_logPathEdit->text().trimmed();
    if (currentLogPath != g_logFilePath) {
        g_logFilePath = currentLogPath;
        updateGlobalLogFile(g_logFilePath);
    }

    if (configPath.isEmpty() || !QFileInfo::exists(configPath)) {
        QMessageBox::warning(this, QStringLiteral("警告"), QStringLiteral("请先指定一个有效的 MCP 配置文件 JSON 路径！"));
        return;
    }
    if (task.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("警告"), QStringLiteral("任务指令描述不能为空！"));
        return;
    }
    m_taskInputEdit->clear();

    // 🌟 将日志输出位置保存到 examples_config.json 中
    writeLogFileToConfig(configPath, g_logFilePath);

    // 🌟 如果是启动第一轮，清空已有的日志文件
    if (!m_sessionActive && !g_logFilePath.isEmpty()) {
        // 通过以 Truncate 方式重开全局长连接，瞬间物理清空日志！
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_logFile) {
            g_logFile->close();
            g_logFile->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text | QIODevice::Unbuffered);
        }
    }

    m_isRunning = true;
    m_runBtn->setEnabled(false);
    m_runBtn->setText(QStringLiteral("⏳ 连接预热中..."));
    m_taskInputEdit->setEnabled(false);
    m_configPathEdit->setEnabled(false);
    m_browseBtn->setEnabled(false);
    m_logPathEdit->setEnabled(false);
    m_logBrowseBtn->setEnabled(false);
    m_modeCombo->setEnabled(false);
    m_modelCombo->setEnabled(false);
    m_fetchModelsBtn->setEnabled(false);
    m_resetSessionBtn->setEnabled(false); // 运行期间禁用重置

    // 🌟 多轮会话分支判断
    if (m_sessionActive && m_session) {
        // 已经是活跃的会话，直接追加对话运行，不清空看板和 Session！
        appendLogHtml(QStringLiteral("<hr style='border: 1px dashed rgba(0,0,0,0.1); margin: 15px 0;'/>"));
        appendLogHtml(QString(QStringLiteral("<h3 style='color: #007aff; font-family: Segoe UI, Microsoft YaHei; margin: 0 0 8px 0;'>💬 发送多轮对话: \"%1\"</h3>")).arg(task.toHtmlEscaped()));
        
        m_session->continueConversation(task);
        return;
    }

    // 初始化黑板日志
    m_logBlackboard->clear();
    appendLogHtml(QStringLiteral("<h2 style='color: #ff9500; font-family: Segoe UI, Microsoft YaHei; margin: 0 0 10px 0;'>⚡ ReAct 执行环路初始化中...</h2>"));

    // 实例化 LLM 驱动
    if (m_modeCombo->currentIndex() == 1) {
        QString apiUrl = m_apiUrlEdit->text().trimmed();
        QString apiKey = m_apiKeyEdit->text().trimmed();
        QString model = m_modelCombo->currentText().trimmed();

        if (apiKey.isEmpty()) {
            apiKey = qEnvironmentVariable("DEEPSEEK_API_KEY");
            if (apiKey.isEmpty()) {
                apiKey = qEnvironmentVariable("OPENAI_API_KEY");
            }
        }

        if (apiKey.isEmpty()) {
            // 未配置 API Key → 不阻塞，回退离线 Mock 演示（便于开箱即用体验）
            qWarning().noquote() << "[AgentMainWindow] 未配置 API Key，本次回退离线 Mock 演示模式";
            m_llmBackend = std::make_shared<MockLlmBackend>();
        } else {
            m_llmBackend = std::make_shared<OpenAiLlmBackend>(apiUrl, apiKey, model, this);
        }
    } else {
        m_llmBackend = std::make_shared<MockLlmBackend>();
    }
    m_session = new AgentSession(m_host, m_llmBackend, this);
    
    // 监听 ReAct 循环信号并着色展示
    connect(m_session->executor(), &LlmAgentExecutor::stepProgress, this, &AgentMainWindow::handleStepProgress);
    // 工具返回 MCP Apps UI → 内嵌渲染
    connect(m_session->executor(), &LlmAgentExecutor::mcpAppContentAvailable, this, &AgentMainWindow::handleMcpAppContent);
    connect(m_session, &AgentSession::finished, this, &AgentMainWindow::handleSessionFinished);

    AgentRunOptions options;
    options.configPath = configPath;
    options.task = task;
    options.timeoutMs = 15000; // 15秒超时
    options.useRealLlm = (m_modeCombo->currentIndex() == 1);
    options.apiUrl = m_apiUrlEdit->text().trimmed();
    options.apiKey = m_apiKeyEdit->text().trimmed();
    options.modelName = m_modelCombo->currentText().trimmed();

    m_session->start(options);
    updateServerList();
}

void AgentMainWindow::updateServerList() {
    m_serverListWidget->clear();
    if (!m_host) return;

    for (const auto& name : m_host->serverNames()) {
        auto state = m_host->serverState(name);
        bool ready = (state == mcp_qt::McpServerState::Ready);
        bool connecting = (state == mcp_qt::McpServerState::Connecting);

        QString label;
        if (ready) {
            int count = m_host->serverToolCount(name);
            label = QString("🟢 %1 (%2 tools)").arg(name).arg(count);
        } else if (connecting) {
            label = QString("⌛ %1 (connecting)").arg(name);
        } else {
            label = QString("🔴 %1").arg(name);
        }

        auto* item = new QListWidgetItem(label);
        item->setData(Qt::UserRole, name);
        item->setForeground(QBrush(ready ? QColor("#28a745") : QColor("#dc3545")));
        m_serverListWidget->addItem(item);
    }
}

void AgentMainWindow::handleStepProgress(const QString& type, const QString& content) {
    QString html;
    QString safeContent = content.toHtmlEscaped().replace("\n", "<br>");
    QString timeStr = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");

    if (type == "thought") {
        html = QString(
            "<div style='background-color: rgba(0, 122, 255, 0.05); border-left: 4px solid #007aff; "
            "padding: 10px; margin: 6px 0; border-radius: 4px; font-family: Segoe UI, Microsoft YaHei;'>"
            "  <div style='color: #8e8e93; font-size: 10px; font-family: Consolas, monospace; margin-bottom: 4px;'>[%1] [大模型推理决策阶段]</div>"
            "  <b style='color: #007aff; font-size: 12px;'>💭 思考规划 (THOUGHT)</b>"
            "  <p style='color: #2c3e50; margin: 4px 0 0 0; line-height: 1.4; font-size: 13px;'>%2</p>"
            "</div>"
        ).arg(timeStr, safeContent);
    } 
    else if (type == "act") {
        html = QString(
            "<div style='background-color: rgba(52, 199, 89, 0.05); border-left: 4px solid #34c759; "
            "padding: 10px; margin: 6px 0; border-radius: 4px; font-family: Consolas, monospace;'>"
            "  <div style='color: #8e8e93; font-size: 10px; font-family: Consolas, monospace; margin-bottom: 4px;'>[%1] [工具执行调用阶段]</div>"
            "  <b style='color: #34c759; font-size: 12px;'>🚀 动作执行 (ACT - 工具调用)</b>"
            "  <p style='color: #155724; margin: 4px 0 0 0; line-height: 1.4; font-size: 12px;'>%2</p>"
            "</div>"
        ).arg(timeStr, safeContent);
    } 
    else if (type == "observation") {
        html = QString(
            "<div style='background-color: rgba(175, 82, 222, 0.05); border-left: 4px solid #af52de; "
            "padding: 10px; margin: 6px 0; border-radius: 4px; font-family: Consolas, monospace;'>"
            "  <div style='color: #8e8e93; font-size: 10px; font-family: Consolas, monospace; margin-bottom: 4px;'>[%1] [外部反馈接收阶段]</div>"
            "  <b style='color: #af52de; font-size: 12px;'>👁️ 状态观测 (OBSERVATION - 结果反馈)</b>"
            "  <p style='color: #4a154b; margin: 4px 0 0 0; line-height: 1.4; font-size: 12px;'>%2</p>"
            "</div>"
        ).arg(timeStr, safeContent);
    } 
    else if (type == "answer") {
        html = QString(
            "<div style='background-color: rgba(255, 149, 0, 0.06); border-left: 4px solid #ff9500; "
            "padding: 12px; margin: 8px 0; border-radius: 6px; border: 1px solid rgba(255, 149, 0, 0.2); "
            "font-family: Segoe UI, Microsoft YaHei;'>"
            "  <div style='color: #8e8e93; font-size: 10px; font-family: Consolas, monospace; margin-bottom: 4px;'>[%1] [任务目标达成终结]</div>"
            "  <b style='color: #ff9500; font-size: 13px;'>🏆 最终答案 (FINAL ANSWER)</b>"
            "  <p style='color: #212529; margin: 6px 0 0 0; line-height: 1.5; font-size: 13px; font-weight: bold;'>%2</p>"
            "</div>"
        ).arg(timeStr, safeContent);
    }

    appendLogHtml(html);
}

void AgentMainWindow::handleSessionFinished(int exitCode) {
    m_isRunning = false;
    m_runBtn->setEnabled(true);
    m_taskInputEdit->setEnabled(true);

    updateServerList();

    if (exitCode == 0) {
        // 🌟 任务运行成功，将会话置为“活跃多轮状态”
        m_sessionActive = true;
        m_resetSessionBtn->setEnabled(true);
        m_resetSessionBtn->setStyleSheet(QStringLiteral("background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #ff3b30, stop:1 #ff2d55);")); // 高亮粉红色

        // 更改启动按钮为“发送对话”
        m_runBtn->setText(QStringLiteral("💬 发送对话"));

        // 保持配置区的禁用锁定
        m_configPathEdit->setEnabled(false);
        m_browseBtn->setEnabled(false);
        m_logPathEdit->setEnabled(false);
        m_logBrowseBtn->setEnabled(false);
        m_modeCombo->setEnabled(false);
        m_modelCombo->setEnabled(false);
        m_fetchModelsBtn->setEnabled(false);

        QString statusHtml = QStringLiteral("<h4 style='color: #28a745; font-family: Segoe UI, Microsoft YaHei;'>✔ 本轮对话回答完毕，Agent 会话已保持挂起，您可在下方输入以继续多轮交谈。</h4>");
        appendLogHtml("<br>" + statusHtml);
    } else {
        // 执行失败，重置会话
        m_sessionActive = false;
        m_resetSessionBtn->setEnabled(false);
        m_resetSessionBtn->setStyleSheet(QStringLiteral("background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #8e8e93, stop:1 #636366);"));
        m_runBtn->setText(QStringLiteral("⚡ 启动 Agent 任务"));

        // 恢复配置区的启用
        m_configPathEdit->setEnabled(true);
        m_browseBtn->setEnabled(true);
        m_logPathEdit->setEnabled(true);
        m_logBrowseBtn->setEnabled(true);
        m_modeCombo->setEnabled(true);
        m_modelCombo->setEnabled(m_modeCombo->currentIndex() == 1);
        m_fetchModelsBtn->setEnabled(m_modeCombo->currentIndex() == 1);

        QString statusHtml = QStringLiteral("<h4 style='color: #dc3545; font-family: Segoe UI, Microsoft YaHei;'>❌ ReAct Agent 任务执行失败或超时。</h4>");
        appendLogHtml("<br>" + statusHtml);

        // 释放资源
        if (m_session) {
            m_session->deleteLater();
            m_session = nullptr;
        }
    }
}

void AgentMainWindow::handleResetSession() {
    if (m_isRunning) return;

    m_sessionActive = false;
    m_runBtn->setText(QStringLiteral("⚡ 启动 Agent 任务"));
    m_runBtn->setEnabled(true);
    m_taskInputEdit->setEnabled(true);
    m_taskInputEdit->clear();

    m_resetSessionBtn->setEnabled(false);
    m_resetSessionBtn->setStyleSheet(QStringLiteral("background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #8e8e93, stop:1 #636366);"));

    // 恢复配置区
    m_configPathEdit->setEnabled(true);
    m_browseBtn->setEnabled(true);
    m_logPathEdit->setEnabled(true);
    m_logBrowseBtn->setEnabled(true);
    m_modeCombo->setEnabled(true);
    m_modelCombo->setEnabled(m_modeCombo->currentIndex() == 1);
    m_fetchModelsBtn->setEnabled(m_modeCombo->currentIndex() == 1);

    // 释放 session
    if (m_session) {
        m_session->deleteLater();
        m_session = nullptr;
    }

    m_logBlackboard->clear();
    m_logBlackboard->setHtml(QStringLiteral("<h3 style='color: #8e8e93; font-family: Segoe UI, Microsoft YaHei;'>会话已重置。新对话已就绪，请输入您的任务指令并点击启动...</h3>"));
    
    qInfo().noquote() << "[AgentMainWindow] 用户手动清空重置了多轮对话会话。";
}

void AgentMainWindow::handleFetchModels() {
    QString apiUrl = m_apiUrlEdit->text().trimmed();
    QString apiKey = m_apiKeyEdit->text().trimmed();

    if (apiKey.isEmpty()) {
        apiKey = qEnvironmentVariable("OPENAI_API_KEY");
    }
    if (apiKey.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("警告"), QStringLiteral("请输入 API 密钥 (API Key) 或设置环境变量 OPENAI_API_KEY 后再同步！"));
        return;
    }

    // 智能化拼接 /models URL
    QString modelsUrl = apiUrl;
    if (modelsUrl.endsWith(QStringLiteral("/chat/completions"))) {
        modelsUrl.replace(QStringLiteral("/chat/completions"), QStringLiteral("/models"));
    } else if (modelsUrl.endsWith(QStringLiteral("/completions"))) {
        modelsUrl.replace(QStringLiteral("/completions"), QStringLiteral("/models"));
    } else {
        int idx = modelsUrl.lastIndexOf('/');
        if (idx != -1) {
            modelsUrl = modelsUrl.left(idx + 1) + QStringLiteral("models");
        }
    }

    m_fetchModelsBtn->setEnabled(false);
    m_fetchModelsBtn->setText(QStringLiteral("正在同步..."));

    QNetworkRequest request;
    request.setUrl(QUrl(modelsUrl));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QString("Bearer %1").arg(apiKey).toUtf8());

    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_fetchModelsBtn->setEnabled(true);
        m_fetchModelsBtn->setText(QStringLiteral("同步模型列表"));

        if (reply->error() != QNetworkReply::NoError) {
            QMessageBox::warning(this, QStringLiteral("获取模型失败"), 
                                 QStringLiteral("网络请求失败: %1").arg(reply->errorString()));
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        QJsonObject root = doc.object();
        QJsonArray modelsArray = root["data"].toArray();
        
        if (modelsArray.isEmpty()) {
            if (doc.isArray()) {
                modelsArray = doc.array();
            } else {
                QMessageBox::warning(this, QStringLiteral("获取模型失败"), 
                                     QStringLiteral("无法解析模型数据或返回列表为空。\n返回数据: %1").arg(QString::fromUtf8(data).left(200)));
                return;
            }
        }

        // 暂存用户原先选中或录入的名称，以备还原
        QString currentModel = m_modelCombo->currentText();

        m_modelCombo->clear();
        for (int i = 0; i < modelsArray.size(); ++i) {
            QJsonObject mObj = modelsArray[i].toObject();
            QString modelId = mObj["id"].toString();
            if (!modelId.isEmpty()) {
                m_modelCombo->addItem(modelId);
            }
        }

        if (!currentModel.isEmpty() && m_modelCombo->findText(currentModel) == -1) {
            m_modelCombo->addItem(currentModel);
        }
        m_modelCombo->setCurrentText(currentModel);

        QMessageBox::information(this, QStringLiteral("同步成功"), 
                                 QStringLiteral("已成功同步并更新了 %1 个可用模型！").arg(m_modelCombo->count()));
    });
}

void AgentMainWindow::appendLogHtml(const QString& html) {
    m_logBlackboard->append(html);
    m_logBlackboard->verticalScrollBar()->setValue(m_logBlackboard->verticalScrollBar()->maximum());
}

void AgentMainWindow::loadAndConnectServers(const QString& configPath) {
    if (!m_host) return;

    appendLogHtml("<b>[System]</b> 尝试加载 MCP 配置: " + configPath);
    
    m_serverListWidget->clear();
    m_host->stop();
    m_host->clearConfig();
    
    if (!m_host->loadConfigFromFile(configPath)) {
        appendLogHtml(QStringLiteral("<div style='color: red;'>加载配置文件失败！</div>"));
    } else {
        m_host->start(30000);
    }
}

void AgentMainWindow::handleServerDoubleClicked(QListWidgetItem* item) {
    if (!item || !m_host) return;
    QString serverName = item->data(Qt::UserRole).toString();
    auto c = m_host->client(serverName);
    if (!c) return;

    QString info = QString("MCP 服务端详细信息: %1\n\n").arg(serverName);

    if (m_host->serverState(serverName) != mcp_qt::McpServerState::Ready) {
        info += "当前状态: " + m_host->serverErrorMessage(serverName) + "\n";
        QMessageBox::information(this, QStringLiteral("MCP 服务端信息"), info);
        return;
    }

    info += QString("服务器能力 (Capabilities):\n");
    info += QString("- 提示词 (Prompts): %1\n").arg(c->hasPromptsCapability() ? "支持" : "不支持");
    info += QString("- 资源读取 (Resources): %1\n").arg(c->hasResourcesCapability() ? "支持" : "不支持");
    
    const auto& tools = c->cachedTools();
    info += QString("- 包含工具 (Tools): %1 个\n").arg(tools.size());

    if (!tools.empty()) {
        info += QString("\n================ 工具列表 (Tools) ================\n");
        for (const auto& t : tools) {
            info += QString("🔧 工具名称: %1\n").arg(t.name);
            if (!t.description.isEmpty()) {
                QString desc = t.description;
                desc.replace("\n", "\n   ");
                info += QString("   描述: %1\n").arg(desc);
            }
            QJsonDocument doc(t.inputSchema);
            QString schemaStr = doc.toJson(QJsonDocument::Compact);
            if (schemaStr != "{}" && !schemaStr.isEmpty()) {
                info += QString("   参数声明: %1\n").arg(schemaStr);
            } else {
                info += QString("   参数声明: 无参数\n");
            }
            info += "\n";
        }
    }

    auto showInfoDialog = [this, serverName](const QString& text) {
        QDialog* dialog = new QDialog(this);
        dialog->setWindowTitle(QString("MCP 服务端详细信息: %1").arg(serverName));
        dialog->resize(700, 500);
        QVBoxLayout* layout = new QVBoxLayout(dialog);
        QTextEdit* textEdit = new QTextEdit(dialog);
        textEdit->setReadOnly(true);
        textEdit->setPlainText(text);
        
        QFont font("Consolas", 10);
        font.setStyleHint(QFont::Monospace);
        textEdit->setFont(font);

        layout->addWidget(textEdit);
        dialog->setLayout(layout);
        dialog->exec();
        dialog->deleteLater();
    };

    if (c->hasPromptsCapability()) {
        QProgressDialog* progress = new QProgressDialog(QStringLiteral("正在加载详细提示词列表..."), QStringLiteral("取消"), 0, 0, this);
        progress->setWindowModality(Qt::WindowModal);
        progress->show();

        c->listPromptsAsync("", [showInfoDialog, progress, info, serverName](const QJsonObject& result, const QString& next, const QString& err) {
            progress->deleteLater();
            QString finalInfo = info;
            if (!err.isEmpty()) {
                finalInfo += QString("\n获取 Prompts 失败: %1").arg(err);
            } else {
                QJsonArray promptsArray = result.value("prompts").toArray();
                finalInfo += QString("\n================ 提示词 (Prompts) 共 %1 个 ================\n").arg(promptsArray.size());
                for (int i=0; i<promptsArray.size(); ++i) {
                    QJsonObject p = promptsArray[i].toObject();
                    QString name = p.value("name").toString();
                    QString desc = p.value("description").toString();
                    finalInfo += QString("💡 【%1】\n   描述: %2\n").arg(name, desc.isEmpty() ? "暂无描述" : desc);
                    
                    QJsonArray argsArray = p.value("arguments").toArray();
                    if (!argsArray.isEmpty()) {
                        finalInfo += "   参数:\n";
                        for (int j=0; j<argsArray.size(); ++j) {
                            QJsonObject arg = argsArray[j].toObject();
                            QString argName = arg.value("name").toString();
                            bool req = arg.value("required").toBool(false);
                            finalInfo += QString("     - %1 %2\n").arg(argName, req ? "(必填)" : "(可选)");
                        }
                    }
                    finalInfo += "\n";
                }
            }
            showInfoDialog(finalInfo);
        });
    } else {
        showInfoDialog(info);
    }
}

} // namespace mcp_agent
