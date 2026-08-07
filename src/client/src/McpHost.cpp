#include "mcp_qt_client/McpHost.h"
#include "mcp_qt_client/McpJsonConfigLoader.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <QSaveFile>
#include <QDebug>

namespace mcp_qt {

class FilteredConfigLoader : public IMcpConfigLoader {
public:
    FilteredConfigLoader(const QList<McpServerConfig>& configs) : m_configs(configs) {}
    QList<McpServerConfig> load() override {
        return m_configs;
    }
private:
    QList<McpServerConfig> m_configs;
};

McpHost::McpHost(QObject* parent) 
    : QObject(parent)
    , m_manager(new McpServerManager(this))
    , m_toolRouter(new McpToolRouter(m_manager, this))
    , m_promptRouter(new McpPromptRouter(m_manager, this))
    , m_resourceRouter(new McpResourceRouter(m_manager, this))
    , m_reporter(new McpDiagnosticReporter())
    , m_watchdogTimer(new QTimer(this))
{
    m_watchdogTimer->setSingleShot(true);
    connect(m_watchdogTimer, &QTimer::timeout, this, &McpHost::handleStartupTimeout);

    // Forward signals
    connect(m_manager, &McpServerManager::allToolsReady, this, &McpHost::handleAllToolsReady);
    
    connect(m_manager, &McpServerManager::clientErrorOccurred, this, [this](const QString& serverName, const mcp_qt::McpError& error) {
        emit errorOccurred(serverName, error);
        m_reporter->addError(serverName, error.message);
        if (m_isStarting) {
            checkReadyCondition();
        }
    });

    connect(m_manager, &McpServerManager::clientStateChanged, this, [this](const QString& serverName, McpServerState state) {
        emit serverStateChanged(serverName, state);
        if (m_isStarting) {
            checkReadyCondition();
        }
    });

    connect(m_manager, &McpServerManager::clientToolsChanged, this, [this]() {
        emit globalToolsChanged();
    });
    connect(m_manager, &McpServerManager::clientPromptsChanged, this, [this]() {
        emit globalPromptsChanged();
    });
    connect(m_manager, &McpServerManager::clientInputRequired, this, [this](const QString& serverName, const QString& reqId, const QJsonObject& inputRequests, const QString& requestState, mcp_qt::MrtrReplyCallback cb) {
        emit inputRequired(serverName, reqId, inputRequests, requestState, cb);
    });
}

McpHost::~McpHost() {
    delete m_reporter;
}

bool McpHost::loadConfigFromFile(const QString& configFilePath) {
    try {
        auto loader = McpJsonConfigLoader::fromFile(configFilePath);
        m_lastConfigPath = configFilePath; // 🌟 记录路径供热重载使用
        return loadConfigs(loader.load());
    } catch (const std::exception& e) {
        m_reporter->addError("Config", QString("Failed to load config file: %1").arg(e.what()));
        return false;
    }
}

bool McpHost::loadConfigFromJson(const QJsonObject& jsonObj) {
    try {
        McpJsonConfigLoader loader(jsonObj);
        return loadConfigs(loader.load());
    } catch (const std::exception& e) {
        m_reporter->addError("Config", QString("Failed to load config JSON: %1").arg(e.what()));
        return false;
    }
}

bool McpHost::loadConfigs(const QList<McpServerConfig>& configs) {
    for (const auto& cfg : configs) {
        addServerConfig(cfg);
    }
    return true;
}

void McpHost::addServerConfig(const McpServerConfig& config) {
    m_loadedConfigs.append(config);
    m_enabledServers.insert(config.serverName, !config.disabled);
}

void McpHost::clearConfig() {
    m_loadedConfigs.clear();
    m_enabledServers.clear();
}

void McpHost::start(int timeoutMs) {
    if (m_isStarting) return;
    
    m_reporter->clear();
    m_reporter->addExecutionLogLine("Starting MCP Host...");

    QList<McpServerConfig> activeConfigs;
    for (const auto& cfg : m_loadedConfigs) {
        if (m_enabledServers.value(cfg.serverName, false)) {
            activeConfigs.append(cfg);
        }
    }

    if (activeConfigs.isEmpty()) {
        m_reporter->addExecutionLogLine("No servers enabled or loaded.");
        emit hostReady(true, "No servers enabled.");
        return;
    }

    m_isStarting = true;
    m_watchdogTimer->start(timeoutMs);

    auto filterLoader = std::make_shared<FilteredConfigLoader>(activeConfigs);
    bool ok = m_manager->loadServers(filterLoader);
    if (!ok) {
        m_reporter->addError("Startup", "McpServerManager loadServers returned false.");
        finishStartup(false, "Failed to initiate server loading.");
    }
}

void McpHost::stop() {
    m_watchdogTimer->stop();
    m_isStarting = false;
    m_manager->closeAll();
}

void McpHost::restart(int timeoutMs) {
    stop();
    start(timeoutMs);
}

bool McpHost::reloadConfigAndRestart(int timeoutMs) {
    if (m_lastConfigPath.isEmpty()) {
        m_reporter->addError("Restart", "No configuration file to reload.");
        return false;
    }
    stop();
    clearConfig();
    bool ok = loadConfigFromFile(m_lastConfigPath);
    if (ok) {
        start(timeoutMs);
    }
    return ok;
}

QStringList McpHost::serverNames() const {
    QStringList names;
    for (const auto& cfg : m_loadedConfigs) {
        names.append(cfg.serverName);
    }
    return names;
}

void McpHost::setServerEnabled(const QString& serverName, bool enabled, bool persist) {
    if (m_enabledServers.value(serverName, false) == enabled) return;
    
    m_enabledServers[serverName] = enabled;
    for (int i = 0; i < m_loadedConfigs.size(); ++i) {
        if (m_loadedConfigs[i].serverName == serverName) {
            m_loadedConfigs[i].disabled = !enabled;
            if (enabled) {
                m_manager->startServer(m_loadedConfigs[i]);
            } else {
                m_manager->stopServer(serverName);
            }
            break;
        }
    }
    
    if (persist) persistServerProperty(serverName, QStringLiteral("disabled"), !enabled);
}

void McpHost::removeServerConfig(const QString& serverName, bool persist) {
    m_enabledServers.remove(serverName);
    for (int i = 0; i < m_loadedConfigs.size(); ++i) {
        if (m_loadedConfigs[i].serverName == serverName) {
            m_loadedConfigs.removeAt(i);
            break;
        }
    }
    m_manager->stopServer(serverName);
    if (persist) persistRemoveServer(serverName);
}

void McpHost::addOrUpdateServerConfig(const McpServerConfig& config, bool persist) {
    bool found = false;
    for (int i = 0; i < m_loadedConfigs.size(); ++i) {
        if (m_loadedConfigs[i].serverName == config.serverName) {
            m_loadedConfigs[i] = config;
            found = true;
            break;
        }
    }
    if (!found) {
        m_loadedConfigs.append(config);
    }
    m_enabledServers.insert(config.serverName, !config.disabled);
    
    m_manager->stopServer(config.serverName);
    if (!config.disabled) {
        m_manager->startServer(config);
    }
    
    if (persist) {
        persistServerObject(config.serverName, serializeServerConfig(config));
    }
}

bool McpHost::persistServerProperty(const QString& serverName, const QString& key, const QJsonValue& value) {
    if (m_lastConfigPath.isEmpty()) return false;
    QFile file(m_lastConfigPath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    QJsonObject root = doc.object();
    QJsonObject serversObj = root.contains(QStringLiteral("mcpServers")) ? root[QStringLiteral("mcpServers")].toObject() : root;
    
    if (serversObj.contains(serverName)) {
        QJsonObject srvObj = serversObj[serverName].toObject();
        srvObj[key] = value;
        serversObj[serverName] = srvObj;
        
        if (root.contains(QStringLiteral("mcpServers"))) {
            root[QStringLiteral("mcpServers")] = serversObj;
        } else {
            root = serversObj;
        }
        
        QSaveFile saveFile(m_lastConfigPath);
        if (saveFile.open(QIODevice::WriteOnly)) {
            saveFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
            return saveFile.commit();
        }
    }
    return false;
}

bool McpHost::persistServerObject(const QString& serverName, const QJsonObject& obj) {
    if (m_lastConfigPath.isEmpty()) return false;
    QFile file(m_lastConfigPath);
    QJsonDocument doc;
    if (file.open(QIODevice::ReadOnly)) {
        doc = QJsonDocument::fromJson(file.readAll());
        file.close();
    }

    QJsonObject root = doc.object();
    QJsonObject serversObj = root.contains(QStringLiteral("mcpServers")) ? root[QStringLiteral("mcpServers")].toObject() : root;
    
    serversObj[serverName] = obj;
    
    if (root.contains(QStringLiteral("mcpServers"))) {
        root[QStringLiteral("mcpServers")] = serversObj;
    } else {
        root = serversObj;
    }
    
    QSaveFile saveFile(m_lastConfigPath);
    if (saveFile.open(QIODevice::WriteOnly)) {
        saveFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        return saveFile.commit();
    }
    return false;
}

bool McpHost::persistRemoveServer(const QString& serverName) {
    if (m_lastConfigPath.isEmpty()) return false;
    QFile file(m_lastConfigPath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    QJsonObject root = doc.object();
    QJsonObject serversObj = root.contains(QStringLiteral("mcpServers")) ? root[QStringLiteral("mcpServers")].toObject() : root;
    
    serversObj.remove(serverName);
    
    if (root.contains(QStringLiteral("mcpServers"))) {
        root[QStringLiteral("mcpServers")] = serversObj;
    } else {
        root = serversObj;
    }
    
    QSaveFile saveFile(m_lastConfigPath);
    if (saveFile.open(QIODevice::WriteOnly)) {
        saveFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        return saveFile.commit();
    }
    return false;
}

QJsonObject McpHost::serializeServerConfig(const McpServerConfig& cfg) const {
    QJsonObject obj;
    obj[QStringLiteral("disabled")] = cfg.disabled;
    if (!cfg.command.isEmpty()) obj[QStringLiteral("command")] = cfg.command;
    if (!cfg.args.isEmpty()) {
        QJsonArray argsArr;
        for (const auto& arg : cfg.args) argsArr.append(arg);
        obj[QStringLiteral("args")] = argsArr;
    }
    if (!cfg.url.isEmpty()) obj[QStringLiteral("url")] = cfg.url;
    if (!cfg.type.isEmpty()) obj[QStringLiteral("type")] = cfg.type;
    if (!cfg.nameSpace.isEmpty()) obj[QStringLiteral("namespace")] = cfg.nameSpace;
    
    if (!cfg.env.isEmpty()) {
        QJsonObject envs;
        for (auto it = cfg.env.constBegin(); it != cfg.env.constEnd(); ++it) {
            envs[it.key()] = it.value();
        }
        obj[QStringLiteral("env")] = envs;
    }
    
    if (!cfg.headers.isEmpty()) {
        QJsonObject hdrs;
        for (auto it = cfg.headers.constBegin(); it != cfg.headers.constEnd(); ++it) {
            hdrs[it.key()] = it.value();
        }
        obj[QStringLiteral("headers")] = hdrs;
    }
    return obj;
}

bool McpHost::isServerEnabled(const QString& serverName) const {
    return m_enabledServers.value(serverName, false);
}

QString McpHost::serverErrorMessage(const QString& serverName) const {
    if (m_manager->serverState(serverName) == McpServerState::Error) {
        return "Server encountered an error. Check diagnostic report.";
    }
    return QString();
}

int McpHost::serverToolCount(const QString& serverName) const {
    auto c = m_manager->client(serverName);
    if (!c) return 0;
    return static_cast<int>(c->cachedTools().size());
}

std::shared_ptr<McpQtClient> McpHost::client(const QString& serverName) const {
    return m_manager->client(serverName);
}

McpServerState McpHost::serverState(const QString& serverName) const {
    return m_manager->serverState(serverName);
}

QJsonArray McpHost::exportAllToolsToLlm(McpQtClient::LlmFormat format) const {
    return m_toolRouter->exportAllToolsToLlmFormat(format);
}

void McpHost::callToolAsync(const QString& toolName, const QJsonObject& args, std::function<void(McpResult)> callback) {
    m_toolRouter->callToolAsync(toolName, args, callback);
}

QString McpHost::getDiagnosticReport() const {
    return m_reporter->renderText() + "\n\n" + m_reporter->renderExecutionLog();
}

void McpHost::handleAllToolsReady() {
    if (!m_isStarting) return;
    m_reporter->addExecutionLogLine("All enabled servers ready.");
    finishStartup(true, "All servers ready.");
}

void McpHost::checkReadyCondition() {
    if (!m_isStarting) return;

    // 检查所有启用的服务器是否都已到达终态
    bool allTerminal = true;
    bool hasError = false;

    for (const auto& cfg : m_loadedConfigs) {
        if (!m_enabledServers.value(cfg.serverName, false)) continue;

        McpServerState st = m_manager->serverState(cfg.serverName);
        if (st == McpServerState::Pending || st == McpServerState::Connecting) {
            allTerminal = false;
            break;
        }
        if (st == McpServerState::Error) {
            hasError = true;
        }
    }

    if (allTerminal) {
        if (hasError) {
            m_reporter->addExecutionLogLine("Startup finished but some servers encountered errors.");
            finishStartup(false, "Finished with errors.");
        } else {
            m_reporter->addExecutionLogLine("All enabled servers ready.");
            finishStartup(true, "All servers ready.");
        }
    }
}

void McpHost::handleStartupTimeout() {
    if (!m_isStarting) return;
    m_reporter->addError("Watchdog", "Timeout reached while waiting for servers to become ready.");
    finishStartup(false, "Timeout reached.");
}

void McpHost::finishStartup(bool success, const QString& summaryMsg) {
    m_isStarting = false;
    m_watchdogTimer->stop();
    emit hostReady(success, summaryMsg);
}

} // namespace mcp_qt
