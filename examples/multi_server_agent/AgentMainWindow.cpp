#include "AgentMainWindow.h"
#include "LlmBackends.h"
#include "LlmCredentialResolver.h"
#include "mcp_qt_apps/McpAppCompatibility.h"

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
#include <QGuiApplication>
#include <QScreen>
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

// 对话渲染辅助（静态，左对齐角色化记录；定义在 handleStepProgress 前）
static void appendUserMessage(QTextEdit*, const QString&);
static void appendAssistantMessage(QTextEdit*, const QString&);
static void appendThinking(QTextEdit*, const QString&);
static void appendToolCall(QTextEdit*, const QString&, const QString&);
static void appendToolResult(QTextEdit*, const QString&);

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

AgentMainWindow::AgentMainWindow(QWidget* parent, const QString& initialConfigPath)
    : QMainWindow(parent) 
{
    m_network = new QNetworkAccessManager(this);
    m_host = new mcp_qt::McpHost(this); // 🌟 初始化全局单例 m_host
    m_registry = new AgentRegistry(this);
    m_reconciler = new AgentReconciler(m_host, m_registry, this);
    connect(m_registry, &AgentRegistry::changed, this, [this]() {
        updateAllServerList();
        updateCurrentAgentMcpCombo();
    });

    initUi();
    applyTheme();
    setupMcpAppRenderer();

    // 默认搜寻本地配置文件，提供极佳体验
    QString defaultCfg = initialConfigPath.trimmed();
    if (defaultCfg.isEmpty()) {
        defaultCfg = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../examples/multi_server_agent/examples_config.json");
    }
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
        updateAllServerList();
        updateCurrentAgentMcpCombo();
    });

    connect(m_host, &mcp_qt::McpHost::hostReady, this, [this](bool success, const QString& msg) {
        m_hostReady = success;
        if (m_runningAgents.isEmpty()) {
            m_runBtn->setEnabled(true);
            m_runBtn->setText(QStringLiteral("⚡ 发送"));
        }
        if (!success) {
            appendLogHtml(QString("<div style='color:red;'>%1</div>").arg(msg.toHtmlEscaped()));
        } else {
            m_serverLogConsole->append(m_host->getDiagnosticReport());
            maybeRunPendingTask();  // submitTask 注入的待办任务 → 模拟用户输入 + 点发送
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

    // 🌟 自动从系统环境变量中尝试提取默认的 API 密钥回显到输入框中（DEEPSEEK_API_KEY / DEEPSEEK / OPENAI_API_KEY）
    const QString defaultKey = resolveLlmApiKey();
    if (!defaultKey.isEmpty()) {
        m_apiKeyEdit->setText(defaultKey);
    }

    // ==========================================
    // 3. 主核心显示区（左侧：标签页 服务端日志/MCP 总服务；右侧：ReAct 看板 + 底部输入）
    // ==========================================
    auto* displayLayout = new QHBoxLayout();
    displayLayout->setSpacing(12);

    // 左侧：QTabWidget（服务端日志 / MCP 总服务）
    m_leftTabs = new QTabWidget(this);
    m_leftTabs->setMaximumWidth(300);

    m_serverLogConsole = new QTextEdit(this);
    m_serverLogConsole->setReadOnly(true);
    m_serverLogConsole->setPlaceholderText(QStringLiteral("服务端子进程的 stderr 输出将显示在这里..."));
    m_leftTabs->addTab(m_serverLogConsole, QStringLiteral("服务端日志"));

    m_allServerList = new QListWidget(this);
    m_leftTabs->addTab(m_allServerList, QStringLiteral("MCP 总服务"));

    displayLayout->addWidget(m_leftTabs, 1);

    // 右侧：ReAct 看板（标题右侧 = agent + MCP 下拉；底部 = 输入框）
    auto* rightPanel = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(8);

    auto* blackboardHeader = new QHBoxLayout();
    auto* blackboardLabel = new QLabel(QStringLiteral("ReAct 执行过程看板:"), this);
    m_agentCombo = new QComboBox(this);
    m_agentCombo->setPlaceholderText(QStringLiteral("选择 Agent"));
    m_currentAgentMcpCombo = new QComboBox(this);
    blackboardHeader->addWidget(blackboardLabel);
    blackboardHeader->addStretch();
    blackboardHeader->addWidget(m_agentCombo);
    auto* mcpLabel = new QLabel(QStringLiteral("MCP:"), this);
    blackboardHeader->addWidget(mcpLabel);
    blackboardHeader->addWidget(m_currentAgentMcpCombo);
    rightLayout->addLayout(blackboardHeader);

    // 看板 + MCP App 分屏（看板 = QStackedWidget，每 agent 一页，切换 agent 切换看板）
    m_rightSplitter = new QSplitter(Qt::Vertical, this);
    auto* blackboardBox = new QWidget(this);
    auto* bbLayout = new QVBoxLayout(blackboardBox);
    bbLayout->setContentsMargins(0, 0, 0, 0);
    m_blackboardStack = new QStackedWidget(this);
    bbLayout->addWidget(m_blackboardStack);
    m_rightSplitter->addWidget(blackboardBox);

    m_mcpAppContainer = new QWidget(this);
    auto* appLayout = new QVBoxLayout(m_mcpAppContainer);
    appLayout->setContentsMargins(0, 0, 0, 0);
    auto* appLabel = new QLabel(QStringLiteral("MCP App 渲染（工具返回的交互界面）:"), this);
    appLayout->addWidget(appLabel);
    m_rightSplitter->addWidget(m_mcpAppContainer);
    m_rightSplitter->setStretchFactor(0, 3);
    m_rightSplitter->setStretchFactor(1, 2);
    m_mcpAppContainer->hide();
    rightLayout->addWidget(m_rightSplitter, 1);

    // 输入框（看板底部）
    auto* runLayout = new QHBoxLayout();
    m_taskInputEdit = new QLineEdit(this);
    m_taskInputEdit->setPlaceholderText(QStringLiteral("输入任务，如「search for AI news」..."));
    m_runBtn = new QPushButton(QStringLiteral("⚡ 发送"), this);
    m_resetSessionBtn = new QPushButton(QStringLiteral("新建"), this);
    m_resetSessionBtn->setEnabled(false);
    runLayout->addWidget(m_taskInputEdit, 1);
    runLayout->addWidget(m_runBtn);
    runLayout->addWidget(m_resetSessionBtn);
    rightLayout->addLayout(runLayout);

    displayLayout->addWidget(rightPanel, 3);

    mainLayout->addLayout(displayLayout, 1);

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
    connect(m_agentCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        m_currentAgent = m_agentCombo->currentText();
        updateCurrentAgentMcpCombo();
        showAgentBlackboard(m_currentAgent);
        refreshControlState();
    });
    connect(m_runBtn, &QPushButton::clicked, this, &AgentMainWindow::handleRunTask);
    connect(m_taskInputEdit, &QLineEdit::returnPressed, this, &AgentMainWindow::handleRunTask);
    connect(m_fetchModelsBtn, &QPushButton::clicked, this, &AgentMainWindow::handleFetchModels);
    connect(m_resetSessionBtn, &QPushButton::clicked, this, &AgentMainWindow::handleResetSession);

    connect(m_modelCombo, &QComboBox::currentTextChanged, this, [this](){}); // To fix any warnings if needed, but the important is below
    connect(m_allServerList, &QListWidget::itemDoubleClicked, this, &AgentMainWindow::handleServerDoubleClicked);

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
    if (m_mcpAppRenderer) return;

    // MCP App 用独立弹窗展示（比内嵌面板大、可自由缩放）
    m_mcpAppDialog = new QDialog(this);
    m_mcpAppDialog->setWindowTitle(QStringLiteral("MCP App"));
    m_mcpAppDialog->resize(960, 720);
    // 初始尺寸作为下限：App 的 size-changed 请求只允许放大，不允许把弹窗缩得更小（避免太小）
    m_mcpAppDialog->setMinimumSize(960, 720);
    m_mcpAppDialog->setWindowFlags(m_mcpAppDialog->windowFlags() | Qt::WindowMaximizeButtonHint);
    auto* dlgLayout = new QVBoxLayout(m_mcpAppDialog);
    dlgLayout->setContentsMargins(0, 0, 0, 0);

    m_mcpAppRenderer = new mcp_qt::McpAppWebView2Renderer(m_mcpAppDialog);
    // Standard compatibility adapters are composed by the host application,
    // keeping the generic WebView renderer free of App-specific knowledge.
    m_mcpAppRenderer->setContentAdapterRegistry(
        mcp_qt::createStandardMcpAppCompatibilityAdapters());
    m_mcpAppRenderer->setUiMeta(QJsonObject{});  // 无 csp/permissions 声明 → 限制性默认
    dlgLayout->addWidget(m_mcpAppRenderer);

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

    // displayMode 协商 → 弹窗全屏/普通切换
    connect(m_mcpAppBridge.get(), &mcp_qt::McpAppBridge::displayModeChanged, this, [this](const QString& mode) {
        if (!m_mcpAppDialog) return;
        // 桌面端保留系统标题栏，始终给用户提供还原与关闭出口。
        if (mode == QLatin1String("fullscreen")) m_mcpAppDialog->showMaximized();
        else if (mode == QLatin1String("inline")) m_mcpAppDialog->showNormal();
    });
    // size-changed → 弹窗适配 App 尺寸
    connect(m_mcpAppBridge.get(), &mcp_qt::McpAppBridge::appSizeChanged, this, [this](int w, int h) {
        if (m_mcpAppDialog && m_mcpAppDialog->isVisible()) {
            m_mcpAppDialog->resize(qMax(480, w), qMax(360, h));
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

void AgentMainWindow::handleMcpAppContent(const QString& html, const QString& toolName,
                                          const QJsonObject& uiMeta, const QJsonObject& toolInput,
                                          const QJsonObject& toolResult) {
    if (!m_mcpAppRenderer || !m_mcpAppBridge) setupMcpAppRenderer();
    if (!m_mcpAppRenderer) return;

    // 从 namespaced 工具名（serverName_toolName）解析 serverName，attach 真实 client
    const auto pair = m_host->toolRouter()->parseToolName(toolName);
    const QString serverName = pair.first;
    // 每个新 App 资源都开始独立的 initialized 生命周期；即使 client 查找失败，
    // 也要重置上一 View 的状态，避免通知误发给旧页面。
    auto client = serverName.isEmpty() ? std::shared_ptr<mcp_qt::McpQtClient>()
                                       : m_host->client(serverName);
    m_mcpAppBridge->attach(m_mcpAppRenderer, client);

    // 应用服务器声明的 _meta.ui（含 csp/permissions），避免默认严格 CSP 拦掉外部 CDN
    m_mcpAppRenderer->setUiMeta(uiMeta);
    m_mcpAppRenderer->loadHtml(html, QUrl());
    // 此时 View 往往尚未 initialized；Bridge 会暂存并在握手完成后按顺序发送。
    m_mcpAppBridge->sendToolInput(toolInput);
    m_mcpAppBridge->sendToolResult(toolResult);

    // MCP App 用独立弹窗展示（比内嵌面板大、可自由缩放）
    if (m_mcpAppDialog) {
        m_mcpAppDialog->show();
        m_mcpAppDialog->raise();
        m_mcpAppDialog->activateWindow();
    }
    appendLogHtml(QStringLiteral("<p style='color:#0a7d33;'><b>◇ MCP App 已弹出：</b>%1</p>")
                      .arg(toolName.toHtmlEscaped()));

    // 自动化：--screenshot 传入时，MCP App 渲染后延迟截图，然后退出
    // 用 WebView2 CapturePreview 抓真实渲染内容（GDI grabWindow 对 DirectComposition 表面返回空白）
    // 三连拍（15s/35s/55s）：区分「CDN 瓦片经代理加载慢」与「相机/渲染未就位」两类黑屏
    if (!m_screenshotPath.isEmpty()) {
        QTimer::singleShot(15000, this, [this]() {
            if (!m_mcpAppRenderer) { qApp->quit(); return; }
            qInfo().noquote() << "[AutoTask] dialog=" << (m_mcpAppDialog ? m_mcpAppDialog->size() : QSize())
                              << "renderer=" << m_mcpAppRenderer->size();
            QTimer::singleShot(60000, qApp, &QCoreApplication::quit);  // 兜底：异步链路异常也退出
            auto shot = [this](const QString& path, int delayMs) {
                QTimer::singleShot(delayMs, this, [this, path]() {
                    m_mcpAppRenderer->capturePreviewToFile(path, [path](bool ok, const QString& err) {
                        if (ok) qInfo().noquote() << "[AutoTask] 截图已保存:" << path;
                        else qWarning().noquote() << "[AutoTask] 截图失败:" << path << err;
                    });
                });
            };
            shot(m_screenshotPath, 0);                         // t=15s（距渲染）
            shot(m_screenshotPath + QStringLiteral(".t35.png"), 20000);  // t=35s
            shot(m_screenshotPath + QStringLiteral(".t55.png"), 40000);  // t=55s
        });
    }
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

void AgentMainWindow::submitTask(const QString& task) {
    const QString t = task.trimmed();
    if (t.isEmpty()) return;
    m_pendingTask = t;
    maybeRunPendingTask();
}

void AgentMainWindow::setAutomatedToolCall(const QString& toolName, const QJsonObject& arguments)
{
    m_automatedToolName = toolName.trimmed();
    m_automatedToolArguments = arguments;
}

void AgentMainWindow::maybeRunPendingTask() {
    if (m_pendingTask.isEmpty() || !m_hostReady) return;  // 未就绪则等 hostReady 再触发
    QTimer::singleShot(800, this, [this]() {
        if (m_pendingTask.isEmpty()) return;
        m_taskInputEdit->setText(m_pendingTask);  // 模拟用户输入
        m_pendingTask.clear();
        handleRunTask();                            // 模拟点击「发送」
    });
}

void AgentMainWindow::handleRunTask() {
    const QString agent = m_agentCombo->currentText();
    if (agent.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("警告"), QStringLiteral("请先选择一个 Agent！"));
        return;
    }
    if (m_runningAgents.contains(agent)) return; // 该 agent 正在跑任务，忽略重复点击

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

    const bool isActive = m_activeSessionAgents.contains(agent);
    QString resolvedKey;  // 解析后的 API Key（环境变量优先）
    if (!isActive) {
        if (!m_automatedToolName.isEmpty()) {
            m_llmBackend = std::make_shared<ScriptedToolLlmBackend>(
                m_automatedToolName, m_automatedToolArguments);
        } else if (m_modeCombo->currentIndex() == 1) {
            // 首轮：构建 LLM 驱动（始终优先读环境变量 DEEPSEEK_API_KEY / OPENAI_API_KEY，免手动输入）
            QString apiUrl = m_apiUrlEdit->text().trimmed();
            QString model = m_modelCombo->currentText().trimmed();
            resolvedKey = resolveLlmApiKey();
            if (resolvedKey.isEmpty()) resolvedKey = m_apiKeyEdit->text().trimmed();
            if (resolvedKey.isEmpty()) {
                qWarning().noquote() << "[AgentMainWindow] 未配置 API Key（环境变量与输入框均无），本次回退离线 Mock 演示模式";
                m_llmBackend = std::make_shared<MockLlmBackend>();
            } else {
                m_llmBackend = std::make_shared<OpenAiLlmBackend>(apiUrl, resolvedKey, model, this);
            }
        } else {
            m_llmBackend = std::make_shared<MockLlmBackend>();
        }

        // 首轮全局清空日志文件
        if (m_activeSessionAgents.isEmpty() && !g_logFilePath.isEmpty()) {
            std::lock_guard<std::mutex> lock(g_logMutex);
            if (g_logFile) {
                g_logFile->close();
                g_logFile->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text | QIODevice::Unbuffered);
            }
        }
    }

    // 取（或建）该 agent 的会话与看板，并切到它的看板
    AgentSession* sess = sessionForAgent(agent);
    showAgentBlackboard(agent);
    QTextEdit* blackboard = blackboardForAgent(agent);

    if (isActive) {
        // 多轮：直接追加对话，保留该 agent 的看板与会话状态
        appendUserMessage(blackboard, task);
        m_runningAgents.insert(agent);
        refreshControlState();
        sess->continueConversation(task);
        return;
    }

    // 首轮：清空该 agent 的看板并初始化
    blackboard->clear();
    appendLogHtml(QStringLiteral("<div style='color:#8e8e93; font-size:12px; margin:4px 0;'>⚡ ReAct 执行环路初始化中...</div>"));
    appendUserMessage(blackboard, task);

    // 限定该 agent 的可见工具面 = 其服务器集合
    const QStringList servers = m_registry ? m_registry->serversFor(agent) : QStringList();
    sess->setServerFilter(servers);

    AgentRunOptions options;
    options.configPath = configPath;
    options.task = task;
    options.timeoutMs = 15000; // 15秒超时
    options.useRealLlm = (m_modeCombo->currentIndex() == 1);
    options.apiUrl = m_apiUrlEdit->text().trimmed();
    options.apiKey = resolvedKey;
    options.modelName = m_modelCombo->currentText().trimmed();

    m_runningAgents.insert(agent);
    refreshControlState();
    sess->start(options);
    updateAllServerList();
}

void AgentMainWindow::updateAllServerList() {
    m_allServerList->clear();
    if (!m_host) return;

    const QStringList referenced = m_registry ? m_registry->allServers() : QStringList();
    // 显示全部配置的服务器（含未启用），未引用/未连接状态分别标记
    for (const QString& name : m_host->configuredServerNames()) {
        // 未被任何 agent 引用 → 对账器预期禁用，标灰提示
        if (!referenced.contains(name)) {
            auto* item = new QListWidgetItem(QString("⚪ %1 (未引用)").arg(name));
            item->setData(Qt::UserRole, name);
            item->setForeground(QBrush(QColor("#8e8e93")));
            m_allServerList->addItem(item);
            continue;
        }
        auto state = m_host->serverState(name);
        bool ready = (state == mcp_qt::McpServerState::Ready);
        bool connecting = (state == mcp_qt::McpServerState::Connecting);
        QString label;
        if (ready) {
            label = QString("🟢 %1 (%2 tools)").arg(name).arg(m_host->serverToolCount(name));
        } else if (connecting) {
            label = QString("⌛ %1 (connecting)").arg(name);
        } else {
            label = QString("🔴 %1").arg(name);
        }
        auto* item = new QListWidgetItem(label);
        item->setData(Qt::UserRole, name);
        item->setForeground(QBrush(ready ? QColor("#28a745") : QColor("#dc3545")));
        m_allServerList->addItem(item);
    }
}

void AgentMainWindow::updateCurrentAgentMcpCombo() {
    if (!m_currentAgentMcpCombo || !m_registry) return;
    m_currentAgentMcpCombo->clear();
    const QString agent = m_agentCombo->currentText();
    if (agent.isEmpty()) return;
    m_currentAgentMcpCombo->addItems(m_registry->serversFor(agent));
}

// ============================================================================
// 对话渲染：左对齐角色化记录（用户 / 助手 / 工具调用 / 工具结果），无头像。
// 助手回答走 Qt 内置 Markdown 解析（QTextDocument::setMarkdown）。
// ============================================================================
static QString renderMarkdownToHtml(const QString& md) {
    QTextDocument doc;
    doc.setMarkdown(md);
    QString html = doc.toHtml();
    const int b1 = html.indexOf(QStringLiteral("<body"));
    const int b2 = html.indexOf(QLatin1Char('>'), b1);
    const int e1 = html.lastIndexOf(QStringLiteral("</body>"));
    if (b1 >= 0 && e1 > b2) {
        return html.mid(b2 + 1, e1 - b2 - 1);
    }
    return html;
}

static QString esc(const QString& s) {
    return s.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
}

static void appendUserMessage(QTextEdit* bb, const QString& text) {
    if (!bb) return;
    bb->append(QStringLiteral(
        "<br><div style='margin:4px 0;'>"
        "  <div style='color:#2f6fed; font-size:11px; font-weight:600; margin-bottom:2px;'>👤 用户</div>"
        "  <div style='background:#eef3ff; border:1px solid #d6e2ff; border-radius:6px; padding:8px 12px; "
        "           font-family:Segoe UI, Microsoft YaHei; font-size:13px; color:#1c2128;'>%1</div>"
        "</div>").arg(esc(text)));
}

static void appendAssistantMessage(QTextEdit* bb, const QString& markdown) {
    if (!bb) return;
    bb->append(QStringLiteral(
        "<br><div style='margin:4px 0;'>"
        "  <div style='color:#1f9d57; font-size:11px; font-weight:600; margin-bottom:2px;'>🤖 助手</div>"
        "  <div style='background:#f2f6f3; border:1px solid #dcebe0; border-radius:6px; padding:8px 12px; "
        "           font-family:Segoe UI, Microsoft YaHei; font-size:13px; color:#1c2128;'>%1</div>"
        "</div>").arg(renderMarkdownToHtml(markdown)));
}

static void appendThinking(QTextEdit* bb, const QString& text) {
    if (!bb) return;
    bb->append(QStringLiteral(
        "<br><div style='margin:4px 0; color:#8e8e93; font-size:12px; border-left:3px solid #d1d5db; padding-left:8px;'>💭 %1</div>"
    ).arg(esc(text)));
}

static void appendToolCall(QTextEdit* bb, const QString& name, const QString& args) {
    if (!bb) return;
    const QString nameSafe = name.toHtmlEscaped();
    const QString argsHtml = args.isEmpty() ? QString() : QStringLiteral("<br>") + esc(args);
    bb->append(QStringLiteral(
        "<br><div style='margin:4px 0;'>"
        "  <div style='color:#34c759; font-size:11px; font-weight:600; margin-bottom:2px;'>🔧 工具调用</div>"
        "  <div style='background:#f2fbf4; border:1px solid #d9f0de; border-radius:6px; padding:8px 12px; "
        "           font-family:Consolas, monospace; font-size:12px; color:#155724;'><b>%1</b>%2</div>"
        "</div>").arg(nameSafe, argsHtml));
}

static void appendToolResult(QTextEdit* bb, const QString& result) {
    if (!bb) return;
    bb->append(QStringLiteral(
        "<br><div style='margin:4px 0;'>"
        "  <div style='color:#af52de; font-size:11px; font-weight:600; margin-bottom:2px;'>👁️ 工具结果</div>"
        "  <div style='background:#faf4fc; border:1px solid #ecdaf2; border-radius:6px; padding:8px 12px; "
        "           font-family:Consolas, monospace; font-size:12px; color:#4a154b;'>%1</div>"
        "</div>").arg(esc(result)));
}

void AgentMainWindow::handleStepProgress(QTextEdit* blackboard, const QString& type, const QString& content) {
    if (!blackboard) return;

    if (type == "thought") {
        appendThinking(blackboard, content);
    } else if (type == "act") {
        // content 形如 "toolName\nargs" 或纯 "toolName"
        const QString trimmed = content.trimmed();
        QString name = trimmed;
        QString args;
        const int nl = trimmed.indexOf(QLatin1Char('\n'));
        if (nl >= 0) { name = trimmed.left(nl).trimmed(); args = trimmed.mid(nl + 1).trimmed(); }
        appendToolCall(blackboard, name, args);
    } else if (type == "observation") {
        appendToolResult(blackboard, content);
    } else if (type == "answer") {
        appendAssistantMessage(blackboard, content);  // Markdown 解析
    }
}

void AgentMainWindow::handleSessionFinished(const QString& agent, int exitCode) {
    m_runningAgents.remove(agent);
    QTextEdit* blackboard = m_agentBlackboards.value(agent);

    if (exitCode == 0) {
        // 任务运行成功，该 agent 进入多轮活跃状态
        m_activeSessionAgents.insert(agent);
    } else {
        // 执行失败，重置该 agent 的会话（下次运行重建）
        m_activeSessionAgents.remove(agent);
        if (blackboard) {
            blackboard->append(QStringLiteral("<br><h4 style='color: #dc3545; font-family: Segoe UI, Microsoft YaHei;'>❌ ReAct Agent 任务执行失败或超时。</h4>"));
        }
        if (AgentSession* sess = m_agentSessions.take(agent)) {
            sess->deleteLater();
        }
    }
    if (agent == m_currentAgent) {
        refreshControlState();
    }
}

void AgentMainWindow::handleResetSession() {
    const QString agent = m_agentCombo->currentText();
    if (m_runningAgents.contains(agent)) return; // 该 agent 正在跑任务，不能重置

    m_activeSessionAgents.remove(agent);
    if (AgentSession* sess = m_agentSessions.take(agent)) {
        sess->deleteLater();
    }
    QTextEdit* blackboard = m_agentBlackboards.value(agent);
    if (blackboard) {
        blackboard->clear();
        blackboard->setHtml(QStringLiteral("<h3 style='color: #8e8e93; font-family: Segoe UI, Microsoft YaHei;'>会话已重置。新对话已就绪，请输入您的任务指令并点击发送...</h3>"));
    }
    m_taskInputEdit->clear();
    refreshControlState();
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
    QTextEdit* bb = m_agentBlackboards.value(m_currentAgent);
    if (!bb && m_blackboardStack) {
        bb = blackboardForAgent(m_currentAgent);
    }
    if (!bb) return;
    bb->append(html);
    bb->verticalScrollBar()->setValue(bb->verticalScrollBar()->maximum());
}

QTextEdit* AgentMainWindow::blackboardForAgent(const QString& agent) {
    QTextEdit* bb = m_agentBlackboards.value(agent);
    if (bb) return bb;
    bb = new QTextEdit(m_blackboardStack);
    bb->setReadOnly(true);
    bb->setHtml(QStringLiteral("<h3 style='color: #8e8e93; font-family: Segoe UI, Microsoft YaHei;'>系统空闲中。请在下方输入您的任务指令...</h3>"));
    m_agentBlackboards[agent] = bb;
    m_blackboardStack->addWidget(bb);
    return bb;
}

void AgentMainWindow::showAgentBlackboard(const QString& agent) {
    QTextEdit* bb = blackboardForAgent(agent);
    if (bb) m_blackboardStack->setCurrentWidget(bb);
}

AgentSession* AgentMainWindow::sessionForAgent(const QString& agent) {
    AgentSession* sess = m_agentSessions.value(agent);
    if (sess) return sess;

    sess = new AgentSession(m_host, m_llmBackend, this);
    m_agentSessions[agent] = sess;
    QTextEdit* blackboard = blackboardForAgent(agent);

    // 该 agent 的进度 → 其自己的看板（后台继续运行，切换 agent 不中断）
    connect(sess->executor(), &LlmAgentExecutor::stepProgress, this,
            [this, agent, blackboard](const QString& type, const QString& content) {
                handleStepProgress(blackboard, type, content);
            });
    // 工具返回 MCP Apps UI → 内嵌渲染
    connect(sess->executor(), &LlmAgentExecutor::mcpAppContentAvailable, this,
            &AgentMainWindow::handleMcpAppContent);
    // 会话结束 → 该 agent 的收尾
    connect(sess, &AgentSession::finished, this,
            [this, agent](int code) { handleSessionFinished(agent, code); });

    return sess;
}

void AgentMainWindow::refreshControlState() {
    const QString agent = m_agentCombo->currentText();
    const bool running = m_runningAgents.contains(agent);
    const bool active = m_activeSessionAgents.contains(agent);

    m_runBtn->setEnabled(!running);
    m_runBtn->setText(running ? QStringLiteral("⏳ 运行中...") : QStringLiteral("⚡ 发送"));
    m_taskInputEdit->setEnabled(!running);
    m_configPathEdit->setEnabled(!running && !active);
    m_browseBtn->setEnabled(!running && !active);
    m_logPathEdit->setEnabled(!running && !active);
    m_logBrowseBtn->setEnabled(!running && !active);
    m_modeCombo->setEnabled(!running && !active);
    m_modelCombo->setEnabled(!running && !active);
    m_fetchModelsBtn->setEnabled(!running && !active);
    m_resetSessionBtn->setEnabled(!running && active);
    m_resetSessionBtn->setStyleSheet(active
        ? QStringLiteral("background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #ff3b30, stop:1 #ff2d55);")
        : QStringLiteral("background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #8e8e93, stop:1 #636366);"));
}

void AgentMainWindow::loadAndConnectServers(const QString& configPath) {
    if (!m_host) return;

    appendLogHtml("<b>[System]</b> 尝试加载 MCP 配置: " + configPath);

    m_allServerList->clear();
    m_host->stop();
    m_host->clearConfig();

    if (!m_host->loadConfigFromFile(configPath)) {
        appendLogHtml(QStringLiteral("<div style='color: red;'>加载配置文件失败！</div>"));
        return;
    }

    // 解析配置里的 "agents" 段（agent → 服务器列表）
    QMap<QString, QStringList> agents;
    QFile cfgFile(configPath);
    if (cfgFile.open(QIODevice::ReadOnly)) {
        const QJsonObject agentsObj = QJsonDocument::fromJson(cfgFile.readAll()).object()
                                          .value(QStringLiteral("agents")).toObject();
        for (auto it = agentsObj.begin(); it != agentsObj.end(); ++it) {
            QStringList servers;
            for (const auto& v : it.value().toArray()) servers << v.toString();
            agents.insert(it.key(), servers);
        }
    }

    m_agentCombo->clear();
    for (const QString& agent : agents.keys()) m_agentCombo->addItem(agent);
    if (m_agentCombo->count() > 0) m_agentCombo->setCurrentIndex(0);

    if (agents.isEmpty()) {
        // 兼容单 host：无 agents 段 → 启动全部（loadConfigFromFile 默认全启用）
        m_host->start(30000);
        return;
    }

    // 多 agent：一次灌入注册表（单次 changed → 一次对账）。reconcile 把 McpHost 的
    // 启用集对账到「所有 agent 服务器集合的并集」，start() 只启动被引用的服务器，
    // 重叠服务器在并集中只出现一次 → 只建一条连接。
    m_registry->setAgents(agents);
    m_host->start(30000);
    updateAllServerList();
    updateCurrentAgentMcpCombo();
    appendLogHtml(QString("<b>[System]</b> 多 agent 模式：%1 个 agent，期望启动服务器 = %2")
                      .arg(agents.size())
                      .arg(m_registry->allServers().join(QStringLiteral(", "))));
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
