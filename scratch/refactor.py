import os

header_path = r"f:\B_My_Document\GitHub\mcp-cpp-agent\mcp_qt_client\include\mcp_qt_client\McpQtClient.h"
cpp_path = r"f:\B_My_Document\GitHub\mcp-cpp-agent\mcp_qt_client\src\McpQtClient.cpp"
anysearch_path = r"f:\B_My_Document\GitHub\mcp-cpp-agent\examples\anysearch_qt\main.cpp"
context7_path = r"f:\B_My_Document\GitHub\mcp-cpp-agent\examples\context7_qt\main.cpp"

# 1. Update Header
with open(header_path, 'r', encoding='utf-8') as f:
    header = f.read()

header = header.replace('static Ptr connectHttp(const QString& serverUrl,', 'static Ptr connectHttpAndWait(const QString& serverUrl,')
header = header.replace('static Ptr connectStdio(const QString& command,', 'static Ptr connectStdioAndWait(const QString& command,')
header = header.replace('static Ptr connectWithOAuth(const OAuthConfig& oauth,', 'static Ptr connectWithOAuthAndWait(const OAuthConfig& oauth,')
header = header.replace('bool connectToTransport(std::shared_ptr<mcp::IMcpTransport> transport,', 'bool connectToTransportAndWait(std::shared_ptr<mcp::IMcpTransport> transport,')
header = header.replace('bool doInitialize(const QString& clientName,', 'bool doInitializeAndWait(const QString& clientName,')

async_decls = """
    /// HTTP/SSE 连接 (纯异步，需监听 connected() 和 errorOccurred() 信号)
    static Ptr connectHttpAsync(const QString& serverUrl,
                                const QString& clientName = QStringLiteral("mcp-qt-client"),
                                const QString& clientVersion = QStringLiteral("1.0.0"));

    /// Stdio 子进程连接 (纯异步)
    static Ptr connectStdioAsync(const QString& command, const QStringList& args = {},
                                 const QString& clientName = QStringLiteral("mcp-qt-client"),
                                 const QString& clientVersion = QStringLiteral("1.0.0"));

    /// HTTP/SSE + OAuth (纯异步)
    static Ptr connectWithOAuthAsync(const OAuthConfig& oauth,
                                     const QString& clientName = QStringLiteral("mcp-qt-client"),
                                     const QString& clientVersion = QStringLiteral("1.0.0"));
"""
if "connectHttpAsync" not in header:
    header = header.replace('static Ptr connectHttpAndWait(', async_decls + '\n    static Ptr connectHttpAndWait(')

async_connectToTransport = """
    /// 连接到已有 transport（纯异步）
    void connectToTransportAsync(std::shared_ptr<mcp::IMcpTransport> transport,
                                 const QString& clientName, const QString& clientVersion);
"""
if "connectToTransportAsync" not in header:
    header = header.replace('bool connectToTransportAndWait(', async_connectToTransport + '\n    bool connectToTransportAndWait(')

async_doInitialize = """
    void doInitializeAsync(const QString& clientName, const QString& clientVersion);
    void setupTransportCommon(std::shared_ptr<mcp::IMcpTransport> transport);
"""
if "doInitializeAsync" not in header:
    header = header.replace('bool doInitializeAndWait(', async_doInitialize + '\n    bool doInitializeAndWait(')

header = header.replace('std::shared_ptr<McpQtClient> buildAndConnect(QString* errorString = nullptr);', 'std::shared_ptr<McpQtClient> buildAndConnectAndWait(QString* errorString = nullptr);\n    std::shared_ptr<McpQtClient> buildAndConnectAsync();')

with open(header_path, 'w', encoding='utf-8') as f:
    f.write(header)

# 2. Update CPP
with open(cpp_path, 'r', encoding='utf-8') as f:
    cpp = f.read()

cpp = cpp.replace('McpQtClientBuilder::buildAndConnect(', 'McpQtClientBuilder::buildAndConnectAndWait(')
cpp = cpp.replace('c->connectToTransport(', 'c->connectToTransportAndWait(')
cpp = cpp.replace('McpQtClient::connectHttp(', 'McpQtClient::connectHttpAndWait(')
cpp = cpp.replace('McpQtClient::connectStdio(', 'McpQtClient::connectStdioAndWait(')
cpp = cpp.replace('McpQtClient::connectWithOAuth(', 'McpQtClient::connectWithOAuthAndWait(')
cpp = cpp.replace('bool McpQtClient::connectToTransport(', 'bool McpQtClient::connectToTransportAndWait(')
cpp = cpp.replace('return doInitialize(', 'return doInitializeAndWait(')
cpp = cpp.replace('bool McpQtClient::doInitialize(', 'bool McpQtClient::doInitializeAndWait(')

# Inject setupTransportCommon to replace duplicate code in connectToTransportAndWait
old_setup = '''bool McpQtClient::connectToTransportAndWait(std::shared_ptr<mcp::IMcpTransport> t,const QString& name,const QString& ver,int to, QString* err){
    m_session=std::make_shared<mcp::McpClientSession>(t);
    m_session->init();'''

new_setup = '''bool McpQtClient::connectToTransportAndWait(std::shared_ptr<mcp::IMcpTransport> t,const QString& name,const QString& ver,int to, QString* err){
    setupTransportCommon(t);
    if(!m_session->start()){ if(err)*err="Failed to start transport"; emit errorOccurred(*err); return false; }
    return doInitializeAndWait(name,ver,to,err);
}

void McpQtClient::setupTransportCommon(std::shared_ptr<mcp::IMcpTransport> t) {
    m_session = std::make_shared<mcp::McpClientSession>(t);
    m_session->init();'''

end_setup = '''    for (const auto& pc : m_pendingCapabilities) {
        m_session->registerCapabilities(_nl(QJsonObject{{pc.name, pc.config}}));
    }
    m_pendingCapabilities.clear();

    if(!m_session->start()){ if(err)*err="Failed to start transport"; emit errorOccurred(*err); return false; }
    return doInitializeAndWait(name,ver,to,err);
}'''

if "setupTransportCommon" not in cpp:
    cpp = cpp.replace(old_setup, new_setup)
    cpp = cpp.replace(end_setup, '''    for (const auto& pc : m_pendingCapabilities) {
        m_session->registerCapabilities(_nl(QJsonObject{{pc.name, pc.config}}));
    }
    m_pendingCapabilities.clear();
}''')


async_impls = """
std::shared_ptr<McpQtClient> McpQtClientBuilder::buildAndConnectAsync() {
    auto c = std::shared_ptr<McpQtClient>(new McpQtClient());
    c->m_transportType = m_transportType;
    c->m_url_or_cmd = m_url_or_cmd;
    c->m_args = m_args;
    c->m_clientName = m_clientName;
    c->m_clientVersion = m_clientVersion;
    c->m_timeoutMs = m_timeoutMs;
    c->m_reconnectPolicy = m_reconnectPolicy;

    if (m_transportType == 1) {
        c->m_httpHeaders = m_httpHeaders;
        c->m_proxy = m_proxy;
        auto t = std::make_shared<mcp_qt::QtHttpSseTransport>(m_url_or_cmd.toStdString());
        mcp_qt::QtHttpRequestConfig cfg;
        for (auto it = m_httpHeaders.constBegin(); it != m_httpHeaders.constEnd(); ++it) {
            cfg.defaultHeaders.insert(it.key().toUtf8(), it.value().toUtf8());
        }
        if (m_proxy) cfg.proxy = *m_proxy;
        t->setRequestConfig(cfg);
        c->connectToTransportAsync(t, m_clientName, m_clientVersion);
        return c;
    }
    if (m_transportType == 3) {
        c->m_httpHeaders = m_httpHeaders;
        c->m_proxy = m_proxy;
        auto t = std::make_shared<mcp_qt::QtStatelessHttpTransport>(m_url_or_cmd);
        QMap<QByteArray, QByteArray> headers;
        for (auto it = m_httpHeaders.constBegin(); it != m_httpHeaders.constEnd(); ++it) {
            headers.insert(it.key().toUtf8(), it.value().toUtf8());
        }
        t->setCustomHeaders(headers);
        if (m_proxy) t->setProxy(*m_proxy);
        c->connectToTransportAsync(t, m_clientName, m_clientVersion);
        return c;
    }
    if (m_transportType == 2) {
        std::vector<std::string> a;
        for (const auto& x : m_args) a.push_back(x.toStdString());
        auto t = std::make_shared<mcp_qt::QtProcessStdioTransport>(m_url_or_cmd.toStdString(), a);
        c->connectToTransportAsync(t, m_clientName, m_clientVersion);
        return c;
    }
    return nullptr;
}

McpQtClient::Ptr McpQtClient::connectHttpAsync(const QString& url, const QString& name, const QString& ver) {
    auto c = Ptr(new McpQtClient());
    c->m_transportType = 1;
    c->m_url_or_cmd = url;
    c->m_clientName = name;
    c->m_clientVersion = ver;
    auto t = std::make_shared<mcp_qt::QtHttpSseTransport>(url.toStdString());
    c->connectToTransportAsync(t, name, ver);
    return c;
}

McpQtClient::Ptr McpQtClient::connectStdioAsync(const QString& cmd, const QStringList& args, const QString& name, const QString& ver) {
    auto c = Ptr(new McpQtClient());
    c->m_transportType = 2;
    c->m_url_or_cmd = cmd;
    c->m_args = args;
    c->m_clientName = name;
    c->m_clientVersion = ver;
    std::vector<std::string> a;
    for(const auto& x : args) a.push_back(x.toStdString());
    auto t = std::make_shared<mcp_qt::QtProcessStdioTransport>(cmd.toStdString(), a);
    c->connectToTransportAsync(t, name, ver);
    return c;
}

McpQtClient::Ptr McpQtClient::connectWithOAuthAsync(const OAuthConfig& oa, const QString& name, const QString& ver) {
    auto c = Ptr(new McpQtClient());
    auto t = std::make_shared<mcp_qt::QtHttpSseTransport>(oa.serverUrl.toStdString());
    auto oc = c->m_oauth;
    t->setTokenProvider([oc]{return oc->getCurrentToken().accessToken;});
    t->setAuthRetryHandler([oc, oa](const std::string& wa)->bool{
        nlohmann::json ctx;
        if(!oa.clientId.isEmpty()) ctx["client_id"] = oa.clientId.toStdString();
        if(!oa.clientSecret.isEmpty()) ctx["client_secret"] = oa.clientSecret.toStdString();
        return _runOAuthQt(oa.serverUrl.toStdString(), ctx, wa, oc, oa.redirectUri);
    });
    c->connectToTransportAsync(t, name, ver);
    return c;
}

void McpQtClient::connectToTransportAsync(std::shared_ptr<mcp::IMcpTransport> t, const QString& name, const QString& ver) {
    setupTransportCommon(t);
    if (!m_session->start()) {
        QMetaObject::invokeMethod(this, [this]() {
            emit errorOccurred("Failed to start transport");
        }, Qt::QueuedConnection);
        return;
    }
    doInitializeAsync(name, ver);
}

void McpQtClient::doInitializeAsync(const QString& name, const QString& ver) {
    m_session->initialize(name.toStdString(), ver.toStdString(), [this](bool success, const nlohmann::json& errorJson) {
        if (!success) {
            QString errStr = "Initialization failure";
            if (!errorJson.empty()) errStr = QString::fromStdString(errorJson.dump());
            QMetaObject::invokeMethod(this, [this, errStr]() {
                emit errorOccurred(errStr);
            }, Qt::QueuedConnection);
        } else {
            QMetaObject::invokeMethod(this, [this]() {
                m_initialized = true;
                emit connected();
            }, Qt::QueuedConnection);
        }
    });
}
"""

if "connectHttpAsync" not in cpp:
    cpp = cpp.replace('} // namespace mcp_qt', async_impls + '\n} // namespace mcp_qt')

with open(cpp_path, 'w', encoding='utf-8') as f:
    f.write(cpp)

# 3. Update anysearch_qt
with open(anysearch_path, 'r', encoding='utf-8') as f:
    anysearch = f.read()

anysearch = anysearch.replace('connectHttp(', 'connectHttpAndWait(')
with open(anysearch_path, 'w', encoding='utf-8') as f:
    f.write(anysearch)

# 4. Update context7_qt
with open(context7_path, 'r', encoding='utf-8') as f:
    context7 = f.read()

context7 = context7.replace('connectStdio(', 'connectStdioAndWait(')
with open(context7_path, 'w', encoding='utf-8') as f:
    f.write(context7)

print("SUCCESS")
