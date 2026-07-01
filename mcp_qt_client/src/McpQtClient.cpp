#include "mcp_qt_client/McpQtClient.h"
#include "mcp_qt_client/McpResourceTemplatesModel.h"
#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QPointer>
#include <QUrl>
#include <QUrlQuery>
#include <QDateTime>

namespace mcp_qt {

// ============================================================================
// JSON / 编码
// ============================================================================
static nlohmann::json _nl(const QJsonObject& o){return nlohmann::json::parse(QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString());}
static QJsonObject _qj(const nlohmann::json& j){return QJsonDocument::fromJson(QByteArray::fromStdString(j.dump())).object();}

// ============================================================================
// QNAM 同步 HTTP（整个 Qt 客户端零 libcurl 依赖）
// ============================================================================
static QByteArray _get(const QString& u){
    QNetworkAccessManager n; QNetworkRequest r{QUrl{u}}; r.setRawHeader("Accept","application/json");
    QNetworkReply* p=n.get(r,QByteArray()); QEventLoop l; QObject::connect(p,&QNetworkReply::finished,&l,&QEventLoop::quit); l.exec();
    QByteArray b=p->readAll(); p->deleteLater(); return b;
}
static QByteArray _post(const QString& u, const QByteArray& b, const char* ct){
    QNetworkAccessManager n; QNetworkRequest r{QUrl{u}}; r.setHeader(QNetworkRequest::ContentTypeHeader,ct); r.setRawHeader("Accept","application/json");
    QNetworkReply* p=n.post(r,b); QEventLoop l; QObject::connect(p,&QNetworkReply::finished,&l,&QEventLoop::quit); l.exec();
    QByteArray rb=p->readAll(); p->deleteLater(); return rb;
}
// POST with custom headers (for token exchange with Basic Auth)
static QByteArray _postH(const QString& u, const QByteArray& b, const QMap<QByteArray,QByteArray>& extraHeaders){
    QNetworkAccessManager n; QNetworkRequest r{QUrl{u}};
    r.setHeader(QNetworkRequest::ContentTypeHeader,"application/x-www-form-urlencoded");
    r.setRawHeader("Accept","application/json");
    for(auto it=extraHeaders.begin();it!=extraHeaders.end();++it) r.setRawHeader(it.key(),it.value());
    QNetworkReply* p=n.post(r,b); QEventLoop l; QObject::connect(p,&QNetworkReply::finished,&l,&QEventLoop::quit); l.exec();
    QByteArray rb=p->readAll(); p->deleteLater(); return rb;
}

// ============================================================================
// OAuth（全 QNAM，零 libcurl）
// ============================================================================
static std::string _eRm(const std::string& w){
    for(auto* px:{"resource_metadata=\"","resource_metadata="}){size_t p=w.find(px);if(p==std::string::npos)continue;size_t s=p+strlen(px),e=(px[17]=='"')?w.find('"',s):w.find_first_of(", ",s);if(e==std::string::npos)e=w.length();return w.substr(s,e-s);}return"";
}
static std::string _bUrl(const std::string& u){size_t p=u.find("://");if(p==std::string::npos)return u;size_t q=u.find('/',p+3);return q!=std::string::npos?u.substr(0,q):u;}
static bool _dmQt(const QString& is,mcp::OAuthServerMetadata* o){
    QString b=is;if(!b.endsWith('/'))b+='/';QStringList u;u<<b+".well-known/oauth-authorization-server"<<b+".well-known/openid-configuration";
    int se=is.indexOf("://");if(se>=0){int ps=is.indexOf('/',se+3);if(ps>=0){QString o=is.left(ps),pp=is.mid(ps);if(!pp.isEmpty()&&pp!="/"){if(!pp.endsWith('/'))pp+='/';u<<o+"/.well-known/oauth-authorization-server"+pp<<o+"/.well-known/openid-configuration"+pp;}}}
    for(const auto& x:u){QByteArray d=_get(x);if(!d.isEmpty()){auto j=nlohmann::json::parse(d.toStdString(),nullptr,false);if(!j.is_discarded()&&j.contains("token_endpoint")){try{*o=mcp::OAuthServerMetadata::fromJson(j);return true;}catch(...){}}}}return false;
}

// 完整 OAuth 认证（全 QNAM，无 libcurl）
static bool _runOAuthQt(const std::string& sseUrl, const nlohmann::json& ctx,
                        const std::string& wwwAuth, std::shared_ptr<mcp::McpOAuthClient> oc,
                        const QString& redirectUri="http://localhost:3000/callback"){
    std::string pu=_eRm(wwwAuth); nlohmann::json pj;
    // 1) PRM 发现
    if(pu.empty()){std::string b=_bUrl(sseUrl);bool fd=false;
        for(const char* px:{"/.well-known/oauth-protected-resource/mcp","/.well-known/oauth-protected-resource","/.well-known/mcp-protected-resource-metadata"}){
            QByteArray d=_get(QString::fromStdString(b+px));auto j=nlohmann::json::parse(d.toStdString(),nullptr,false);
            if(!j.is_discarded()&&j.contains("authorization_servers")&&j["authorization_servers"].is_array()&&!j["authorization_servers"].empty()){pu=b+px;pj=std::move(j);fd=true;break;}
        }if(!fd)return false;
    }else{QByteArray ps=_get(QString::fromStdString(pu));pj=nlohmann::json::parse(ps.toStdString(),nullptr,false);}
    if(pj.is_discarded()||!pj.contains("authorization_servers"))return false;

    // 2) Metadata 发现
    QString iu=QString::fromStdString(pj["authorization_servers"][0].get<std::string>());
    if(pj.contains("oauthMetadataLocation")&&pj["oauthMetadataLocation"].is_string())iu=QString::fromStdString(_bUrl(sseUrl))+QString::fromStdString(pj["oauthMetadataLocation"].get<std::string>());
    mcp::OAuthServerMetadata mm;if(!_dmQt(iu,&mm))return false;

    // 3) 获取 client 凭据
    std::string cid=ctx.value("client_id",""),csc=ctx.value("client_secret","");bool pr=!cid.empty();
    if(cid.empty()&&!mm.registrationEndpoint.empty()){
        nlohmann::json rr={{"client_name","mcp-qt-client"},{"grant_types",{"authorization_code","refresh_token"}},{"redirect_uris",{redirectUri.toStdString()}},{"response_types",{"code"}},{"token_endpoint_auth_method","none"}};
        QByteArray rb=_post(QString::fromStdString(mm.registrationEndpoint),QByteArray::fromStdString(rr.dump()),"application/json");
        auto rj=nlohmann::json::parse(rb.toStdString(),nullptr,false);if(rj.is_discarded()||!rj.contains("client_id"))return false;
        cid=rj["client_id"].get<std::string>();csc=rj.value("client_secret","");
    }

    // 4) JWT-Bearer
    for(const auto& gt:mm.grantTypesSupported)if(gt=="urn:ietf:params:oauth:grant-type:jwt-bearer"&&ctx.contains("idp_id_token")&&!ctx["idp_id_token"].get<std::string>().empty()){
        std::string a=ctx["idp_id_token"].get<std::string>();
        QByteArray pf=QByteArray("grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion=")+QByteArray::fromStdString(a);
        QMap<QByteArray,QByteArray> hdrs;
        if(!csc.empty()){hdrs["Authorization"]="Basic "+QByteArray::fromStdString(cid+":"+csc).toBase64();}
        QByteArray resp=_postH(QString::fromStdString(mm.tokenEndpoint),pf,hdrs);
        auto tj=nlohmann::json::parse(resp.toStdString(),nullptr,false);if(!tj.is_discarded()&&tj.contains("access_token")){mcp::OAuthToken t;t.accessToken=tj["access_token"].get<std::string>();oc->setCurrentToken(t);return true;}return false;
    }

    // 5) Client Credentials
    bool isCC=!pr&&ctx.contains("client_id");
    if(isCC){
        QByteArray pf=QByteArray::fromStdString("grant_type=client_credentials&client_id="+cid+"&client_secret="+csc);
        QMap<QByteArray,QByteArray> hdrs;hdrs["Authorization"]="Basic "+QByteArray::fromStdString(cid+":"+csc).toBase64();
        QByteArray resp=_postH(QString::fromStdString(mm.tokenEndpoint),pf,hdrs);
        auto tj=nlohmann::json::parse(resp.toStdString(),nullptr,false);if(!tj.is_discarded()&&tj.contains("access_token")){mcp::OAuthToken t;t.accessToken=tj["access_token"].get<std::string>();oc->setCurrentToken(t);return true;}return false;
    }

    // 6) Authorization Code + PKCE
    std::vector<std::string> scs;std::string rs;size_t sp=wwwAuth.find("scope=\"");if(sp!=std::string::npos){size_t ss=sp+7,se=wwwAuth.find('"',ss);if(se!=std::string::npos)rs=wwwAuth.substr(ss,se-ss);}
    if(!rs.empty()){std::istringstream iss(rs);std::string s;while(iss>>s)scs.push_back(s);}
    else if(pj.contains("scopes_supported")&&pj["scopes_supported"].is_array()){for(auto& s:pj["scopes_supported"])scs.push_back(s.get<std::string>());}else if(!mm.scopesSupported.empty())scs=mm.scopesSupported;

    bool ub=pr;for(const auto& am:mm.tokenEndpointAuthMethodsSupported)if(am=="client_secret_basic"){ub=true;break;}
    auto ar=oc->buildAuthorizationUrl(mm,cid,redirectUri.toStdString(),scs,sseUrl);

    // 获取 auth code
    QNetworkRequest aq{QUrl{QString::fromStdString(ar.authorizationUrl)}};aq.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    QNetworkAccessManager an;QNetworkReply*ap=an.get(aq,QByteArray());QEventLoop al;QObject::connect(ap,&QNetworkReply::finished,&al,&QEventLoop::quit);al.exec();
    QString loc=QString::fromUtf8(ap->rawHeader("Location"));ap->deleteLater();if(loc.isEmpty())return false;
    QString cd;int cp=loc.indexOf("code=");if(cp>=0){int cs=cp+5,ce=loc.indexOf('&',cs);cd=loc.mid(cs,(ce>=0)?ce-cs:-1);}if(cd.isEmpty())return false;

    // Token exchange — QURLQuery 自动 URL-encode
    QUrlQuery q;
    if(!ub){q.addQueryItem("client_id",QString::fromStdString(cid));if(!csc.empty())q.addQueryItem("client_secret",QString::fromStdString(csc));}
    q.addQueryItem("code",cd);q.addQueryItem("code_verifier",QString::fromStdString(ar.codeVerifier));q.addQueryItem("grant_type","authorization_code");
    q.addQueryItem("redirect_uri",redirectUri);q.addQueryItem("resource",QString::fromStdString(sseUrl));
    QMap<QByteArray,QByteArray> th;
    if(ub&&!csc.empty())th["Authorization"]="Basic "+QByteArray::fromStdString(cid+":"+csc).toBase64();
    QByteArray tb=_postH(QString::fromStdString(mm.tokenEndpoint),q.query(QUrl::FullyEncoded).toUtf8(),th);
    auto tj=nlohmann::json::parse(tb.toStdString(),nullptr,false);
    if(tj.is_discarded()||!tj.contains("access_token"))return false;
    mcp::OAuthToken tk;tk.accessToken=tj["access_token"].get<std::string>();
    if(tj.contains("refresh_token"))tk.refreshToken=tj["refresh_token"].get<std::string>();
    tk.expiresIn=tj.value("expires_in",0);tk.obtainedAt=std::chrono::steady_clock::now();
    oc->setCurrentToken(tk);return true;
}

// ============================================================================
// Builder Implementation
// ============================================================================
McpQtClientBuilder& McpQtClientBuilder::setTransportHttp(const QString& url) { m_transportType = 1; m_url_or_cmd = url; return *this; }
McpQtClientBuilder& McpQtClientBuilder::setTransportStatelessHttp(const QString& url) { m_transportType = 3; m_url_or_cmd = url; return *this; }
McpQtClientBuilder& McpQtClientBuilder::setTransportStdio(const QString& command, const QStringList& args) { m_transportType = 2; m_url_or_cmd = command; m_args = args; return *this; }
McpQtClientBuilder& McpQtClientBuilder::setHttpHeaders(const QMap<QString, QString>& headers) { m_httpHeaders = headers; return *this; }
McpQtClientBuilder& McpQtClientBuilder::setHttpProxy(const QNetworkProxy& proxy) { m_proxy = proxy; return *this; }
McpQtClientBuilder& McpQtClientBuilder::setReconnectPolicy(const mcp::McpReconnectPolicy& policy) { m_reconnectPolicy = policy; return *this; }
McpQtClientBuilder& McpQtClientBuilder::setClientInfo(const QString& name, const QString& version) { m_clientName = name; m_clientVersion = version; return *this; }
McpQtClientBuilder& McpQtClientBuilder::setTimeout(int ms) { m_timeoutMs = ms; return *this; }

std::shared_ptr<McpQtClient> McpQtClientBuilder::buildAndConnect(QString* errorString) {
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
        if (m_proxy) {
            cfg.proxy = *m_proxy;
        }
        t->setRequestConfig(cfg);

        if (!c->connectToTransport(t, m_clientName, m_clientVersion, m_timeoutMs, errorString)) {
            return nullptr;
        }
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
        if (m_proxy) {
            t->setProxy(*m_proxy);
        }

        if (!c->connectToTransport(t, m_clientName, m_clientVersion, m_timeoutMs, errorString)) {
            return nullptr;
        }
        return c;
    }
    if (m_transportType == 2) {
        std::vector<std::string> a;
        for (const auto& x : m_args) {
            a.push_back(x.toStdString());
        }
        auto t = std::make_shared<mcp_qt::QtProcessStdioTransport>(m_url_or_cmd.toStdString(), a);
        if (!c->connectToTransport(t, m_clientName, m_clientVersion, m_timeoutMs, errorString)) {
            return nullptr;
        }
        return c;
    }
    if (errorString) *errorString = "No transport configured";
    return nullptr;
}

// ============================================================================
// McpQtClient
// ============================================================================

McpQtClient::McpQtClient(QObject* p):QObject(p),m_oauth(std::make_shared<mcp::McpOAuthClient>()){}
McpQtClient::~McpQtClient(){close();}

McpQtClient::Ptr McpQtClient::connectHttp(const QString& url,const QString& name,const QString& ver,int to, QString* err){
    auto c=Ptr(new McpQtClient());
    c->m_transportType = 1;
    c->m_url_or_cmd = url;
    c->m_clientName = name;
    c->m_clientVersion = ver;
    c->m_timeoutMs = to;
    auto t=std::make_shared<mcp_qt::QtHttpSseTransport>(url.toStdString());
    if(!c->connectToTransport(t,name,ver,to,err))return nullptr;return c;
}
McpQtClient::Ptr McpQtClient::connectStdio(const QString& cmd,const QStringList& args,const QString& name,const QString& ver,int to, QString* err){
    auto c=Ptr(new McpQtClient());
    c->m_transportType = 2;
    c->m_url_or_cmd = cmd;
    c->m_args = args;
    c->m_clientName = name;
    c->m_clientVersion = ver;
    c->m_timeoutMs = to;
    std::vector<std::string> a;for(const auto& x:args)a.push_back(x.toStdString());
    auto t=std::make_shared<mcp_qt::QtProcessStdioTransport>(cmd.toStdString(),a);
    if(!c->connectToTransport(t,name,ver,to,err))return nullptr;return c;
}
McpQtClient::Ptr McpQtClient::connectWithOAuth(const OAuthConfig& oa,const QString& name,const QString& ver,int to){
    auto c=Ptr(new McpQtClient());
    auto t=std::make_shared<mcp_qt::QtHttpSseTransport>(oa.serverUrl.toStdString());
    auto oc=c->m_oauth;
    t->setTokenProvider([oc]{return oc->getCurrentToken().accessToken;});
    // 通过 transport 的 auth retry handler 触发 OAuth（收到 POST 401 时才执行）
    t->setAuthRetryHandler([oc,oa](const std::string& wa)->bool{
        nlohmann::json ctx;
        if(!oa.clientId.isEmpty())ctx["client_id"]=oa.clientId.toStdString();
        if(!oa.clientSecret.isEmpty())ctx["client_secret"]=oa.clientSecret.toStdString();
        return _runOAuthQt(oa.serverUrl.toStdString(),ctx,wa,oc,oa.redirectUri);
    });
    if(!c->connectToTransport(t,name,ver,to))return nullptr;return c;
}

bool McpQtClient::connectToTransport(std::shared_ptr<mcp::IMcpTransport> t,const QString& name,const QString& ver,int to, QString* err){
    m_session=std::make_shared<mcp::McpClientSession>(t);
    m_session->init();
    if (m_trafficLogger) {
        setTrafficLogger(m_trafficLogger);
    }
    m_session->setOnError([this](const std::string& err) {
        QString errStr = QString::fromStdString(err);
        QMetaObject::invokeMethod(this, [this, errStr]() {
            emit errorOccurred(errStr);
            handleTransportFailure();
        }, Qt::QueuedConnection);
    });
    m_session->setNotificationCallback([this](const std::string& method, const nlohmann::json& params) {
        QString methodStr = QString::fromStdString(method);
        emit notificationReceived(methodStr, _qj(params));
        
        if (methodStr == "notifications/tools/list_changed") {
            QMetaObject::invokeMethod(this, &McpQtClient::toolsChanged, Qt::QueuedConnection);
        } else if (methodStr == "notifications/resources/list_changed") {
            QMetaObject::invokeMethod(this, &McpQtClient::resourcesChanged, Qt::QueuedConnection);
        } else if (methodStr == "notifications/prompts/list_changed") {
            QMetaObject::invokeMethod(this, &McpQtClient::promptsChanged, Qt::QueuedConnection);
        }
    });

    // 监听 session 的 onClose 以响应连接断开，不覆盖底层的 transport onClose
    // 使用 QueuedConnection 确保 disconnected 和自愈状态机安全运行在主线程，防范 QTimer 跨线程归属警告
    m_session->setOnClose([this]() {
        QMetaObject::invokeMethod(this, [this]() {
            emit disconnected();
            handleTransportFailure();
        }, Qt::QueuedConnection);
    });

    // 自动在 session 上安装资源更新路由拦截（免去 static 锁防重带来的跨实例及重连失效问题）
    m_session->registerNotificationHandler(
        "notifications/resources/updated",
        [this](const nlohmann::json& params) {
            QJsonObject qp = _qj(params);
            const QString uri = qp.value("uri").toString();
            if (!uri.isEmpty()) {
                m_resourceRouter.dispatch(uri, qp);
            }
        });

    // Apply saved handlers if any (this will also register their capabilities in m_session)
    if (m_savedSamplingHandler) {
        setSamplingHandler(m_savedSamplingContext.data(), m_savedSamplingHandler);
    }
    if (m_savedElicitationHandler) {
        setElicitationHandler(m_savedElicitationContext.data(), m_savedElicitationHandler);
    }
    if (m_savedRootsProvider) {
        setRootsProvider(m_savedRootsContext.data(), m_savedRootsProvider);
    }

    if(!m_session->start()){ if(err)*err="Failed to start transport"; emit errorOccurred(*err); return false; }
    return doInitialize(name,ver,to,err);
}
bool McpQtClient::doInitialize(const QString& name,const QString& ver,int to, QString* err){
    auto loop = std::make_shared<QEventLoop>();
    auto initOk = std::make_shared<bool>(false);
    m_session->initialize(name.toStdString(), ver.toStdString(), [loop, initOk](bool success, const nlohmann::json&) {
        *initOk = success;
        loop->quit();
    });
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, loop.get(), &QEventLoop::quit);
    timer.start(to);
    loop->exec();

    if(!*initOk){ if(err)*err="Initialization timeout or failure"; emit errorOccurred(*err); return false; }
    m_initialized=true;emit connected();return true;
}
bool McpQtClient::doOAuth(const OAuthConfig& oa){
    QNetworkAccessManager n;QNetworkRequest r{QUrl{oa.serverUrl}};QNetworkReply* p=n.get(r,QByteArray());
    QEventLoop l;QObject::connect(p,&QNetworkReply::finished,&l,&QEventLoop::quit);l.exec();
    QByteArray w=p->rawHeader("WWW-Authenticate");p->deleteLater();
    if(w.isEmpty())return true;
    nlohmann::json ctx;
    if(!oa.clientId.isEmpty())ctx["client_id"]=oa.clientId.toStdString();
    if(!oa.clientSecret.isEmpty())ctx["client_secret"]=oa.clientSecret.toStdString();
    return _runOAuthQt(oa.serverUrl.toStdString(),ctx,w.toStdString(),m_oauth,oa.redirectUri);
}

// ========== Server Info ==========
QJsonObject McpQtClient::serverInfo()const{return m_session?_qj(m_session->getServerVersion()):QJsonObject{};}
QJsonObject McpQtClient::serverCapabilities()const{return m_session?_qj(m_session->getServerCapabilities()):QJsonObject{};}
QString McpQtClient::negotiatedProtocolVersion()const{return m_session?QString::fromStdString(m_session->getNegotiatedProtocolVersion()):QString{};}
QString McpQtClient::instructions()const{return m_session?QString::fromStdString(m_session->getInstructions()):QString{};}

bool McpQtClient::hasToolsCapability() const { return serverCapabilities().contains("tools"); }
bool McpQtClient::hasPromptsCapability() const { return serverCapabilities().contains("prompts"); }
bool McpQtClient::hasResourcesCapability() const { return serverCapabilities().contains("resources"); }

// ========== Tools ==========
static std::vector<McpQtTool> _cvt(const std::vector<mcp::McpTool>& src) { std::vector<McpQtTool> r; for(const auto& t:src){ r.push_back({QString::fromStdString(t.name), QString::fromStdString(t.description), _qj(t.inputSchema)}); } return r; }
std::vector<McpQtTool> McpQtClient::listTools(int to){
    nlohmann::json e; auto r = m_session?_cvt(m_session->listToolsSync(std::chrono::milliseconds(to),&e)):std::vector<McpQtTool>{};
    for(const auto& t : r) m_toolSchemaCache[t.name] = t.inputSchema;
    return r;
}
std::vector<McpQtTool> McpQtClient::listTools(const QString& c,QString* n,int to){
    nlohmann::json e;std::string ns;auto r=m_session?m_session->listToolsSync(c.toStdString(),&ns,std::chrono::milliseconds(to),&e):std::vector<mcp::McpTool>{};if(n)*n=QString::fromStdString(ns);
    auto res = _cvt(r);
    for(const auto& t : res) m_toolSchemaCache[t.name] = t.inputSchema;
    return res;
}
std::vector<McpQtTool> McpQtClient::fetchAllTools(int to) {
    std::vector<McpQtTool> all; QString c;
    do { QString nc; auto r=listTools(c,&nc,to); all.insert(all.end(),r.begin(),r.end()); c=nc; } while(!c.isEmpty());
    return all;
}

void McpQtClient::listToolsAsync(const QString& cursor, std::function<void(const std::vector<McpQtTool>&, const QString&, const QString&)> callback) {
    if (!m_session) {
        if (callback) QMetaObject::invokeMethod(this, [=]() { callback({}, "", "No session"); }, Qt::QueuedConnection);
        return;
    }
    m_session->listTools(cursor.toStdString(), [this, callback](const std::vector<mcp::McpTool>& tools, const std::string& nextCursor, const nlohmann::json& error) {
        if (!callback) return;
        auto res = _cvt(tools);
        // Note: m_toolSchemaCache must be accessed from the thread it was created on, usually main thread
        // We will do it inside the invokeMethod
        QString errStr = error.empty() ? QString() : QString::fromStdString(error.dump());
        QString nc = QString::fromStdString(nextCursor);
        QMetaObject::invokeMethod(this, [this, res, nc, errStr, callback]() {
            for(const auto& t : res) m_toolSchemaCache[t.name] = t.inputSchema;
            callback(res, nc, errStr);
        }, Qt::QueuedConnection);
    });
}


std::unique_ptr<McpToolsModel> McpQtClient::createToolsModel(QObject* parent) {
    auto model = std::make_unique<McpToolsModel>(parent);
    model->setClient(this);
    m_toolsModels.append(model.get());
    return model;
}

static bool validateProperty(const QJsonValue& val, const QJsonObject& propSchema, QString* errorString) {
    // 1. Check type
    if (propSchema.contains("type") && propSchema["type"].isString()) {
        QString expectedType = propSchema["type"].toString();
        if (expectedType == "string" && !val.isString()) {
            if (errorString) *errorString = "Expected string";
            return false;
        }
        if (expectedType == "number" && !val.isDouble()) {
            if (errorString) *errorString = "Expected number";
            return false;
        }
        if (expectedType == "integer" && (!val.isDouble() || val.toDouble() != static_cast<int>(val.toDouble()))) {
            if (errorString) *errorString = "Expected integer";
            return false;
        }
        if (expectedType == "boolean" && !val.isBool()) {
            if (errorString) *errorString = "Expected boolean";
            return false;
        }
        if (expectedType == "array" && !val.isArray()) {
            if (errorString) *errorString = "Expected array";
            return false;
        }
        if (expectedType == "object" && !val.isObject()) {
            if (errorString) *errorString = "Expected object";
            return false;
        }
    }

    // 2. Check enum
    if (propSchema.contains("enum") && propSchema["enum"].isArray()) {
        QJsonArray enumVals = propSchema["enum"].toArray();
        bool found = false;
        for (const auto& ev : enumVals) {
            if (ev == val) {
                found = true;
                break;
            }
        }
        if (!found) {
            if (errorString) *errorString = "Value not in enum list";
            return false;
        }
    }

    // 3. Check minimum / maximum
    if (val.isDouble()) {
        double num = val.toDouble();
        if (propSchema.contains("minimum") && propSchema["minimum"].isDouble()) {
            if (num < propSchema["minimum"].toDouble()) {
                if (errorString) *errorString = QString("Value %1 is less than minimum %2").arg(num).arg(propSchema["minimum"].toDouble());
                return false;
            }
        }
        if (propSchema.contains("maximum") && propSchema["maximum"].isDouble()) {
            if (num > propSchema["maximum"].toDouble()) {
                if (errorString) *errorString = QString("Value %1 is greater than maximum %2").arg(num).arg(propSchema["maximum"].toDouble());
                return false;
            }
        }
    }
    return true;
}

bool McpQtClient::validateToolSchemaLocally(const QString& name, const QJsonObject& arguments, QString* errorString) const {
    auto it = m_toolSchemaCache.find(name);
    if (it == m_toolSchemaCache.end()) return true;
    QJsonObject schema = it->second;
    
    // 1. Validate required fields
    if (schema.contains("required") && schema["required"].isArray()) {
        QJsonArray required = schema["required"].toArray();
        for (const auto& r : required) {
            QString reqField = r.toString();
            if (!arguments.contains(reqField)) {
                if (errorString) *errorString = QString("Validation Error: Missing required argument '%1'").arg(reqField);
                return false;
            }
        }
    }

    // 2. Validate properties
    if (schema.contains("properties") && schema["properties"].isObject()) {
        QJsonObject properties = schema["properties"].toObject();
        for (auto propIt = arguments.begin(); propIt != arguments.end(); ++propIt) {
            QString key = propIt.key();
            if (properties.contains(key) && properties[key].isObject()) {
                QJsonObject propSchema = properties[key].toObject();
                QString valError;
                if (!validateProperty(propIt.value(), propSchema, &valError)) {
                    if (errorString) *errorString = QString("Validation Error on '%1': %2").arg(key).arg(valError);
                    return false;
                }
            } else {
                // Check additionalProperties
                if (schema.contains("additionalProperties")) {
                    QJsonValue addProp = schema["additionalProperties"];
                    if (addProp.isBool() && !addProp.toBool()) {
                        if (errorString) *errorString = QString("Validation Error: Property '%1' is not allowed by additionalProperties=false").arg(key);
                        return false;
                    }
                }
            }
        }
    }
    return true;
}
McpResult McpQtClient::callTool(const QString& nm,const QJsonObject& a,int to){
    QString errStr; if(!validateToolSchemaLocally(nm, a, &errStr)) return {true, {}, errStr};
    nlohmann::json e;if(!m_session)return{true,{},"No session"}; auto r=m_session->callToolSync(nm.toStdString(),_nl(a),&e,std::chrono::milliseconds(to)); return {!e.empty(), _qj(r), e.empty()?QString{}:_qj(e).value("message").toString()};}
McpResult McpQtClient::callTool(const QString& nm,const QJsonObject& a,ProgressCallback onP,int to){
    QString errStr; if(!validateToolSchemaLocally(nm, a, &errStr)) return {true, {}, errStr};
    nlohmann::json e;if(!m_session)return{true,{},"No session"};auto pf=[onP](const nlohmann::json& pi){if(onP){float p=pi.value("progress",0.0f),t=pi.value("total",0.0f);onP(p,t,QString::fromStdString(pi.value("message","")));}};auto r=m_session->callToolSync(nm.toStdString(),_nl(a),&e,std::chrono::milliseconds(to),pf); return {!e.empty(), _qj(r), e.empty()?QString{}:_qj(e).value("message").toString()};}
void McpQtClient::callToolAsync(const QString& nm, const QJsonObject& a, std::function<void(McpResult)> cb, ProgressCallback onP) {
    callToolAsync(nm, a, this, cb, onP);
}
void McpQtClient::callToolAsync(const QString& nm, const QJsonObject& a, QObject* ctx, std::function<void(McpResult)> cb, ProgressCallback onP) {
    QString errStr;
    if(!validateToolSchemaLocally(nm, a, &errStr)) {
        if(ctx) {
            QPointer<QObject> pCtx(ctx);
            if (pCtx) {
                QMetaObject::invokeMethod(pCtx.data(), [cb, errStr](){ cb({true, {}, errStr}); }, Qt::QueuedConnection);
            }
        }
        else { cb({true, {}, errStr}); }
        return;
    }
    sendRequest("tools/call", QJsonObject{{"name",nm},{"arguments",a}}, ctx, [cb](const QJsonObject& r, const QJsonObject& e) {
        cb({!e.isEmpty(), r, e.isEmpty()?QString{}:e.value("message").toString()});
    }, onP);
}

// ========== Typed Tool Results ==========

/// 内部辅助：将工具调用的原始 QJsonObject 解析为 McpQtToolResult
static McpQtToolResult parseToolResult(const QJsonObject& raw, bool isError, const QString& errorString) {
    McpQtToolResult result;
    result.raw = raw;
    result.isError = isError;
    result.errorString = errorString;

    // structuredContent 字段（MCP 2025-11-25）
    if (raw.contains("structuredContent")) {
        result.structuredContent = raw.value("structuredContent").toObject();
    }

    const QJsonArray contentArray = raw.value("content").toArray();
    for (const QJsonValue& val : contentArray) {
        const QJsonObject item = val.toObject();
        McpQtContent content;
        content.raw = item;

        const QString type = item.value("type").toString();
        if (type == QStringLiteral("text")) {
            content.kind = McpQtContentKind::Text;
            content.text = item.value("text").toString();
        } else if (type == QStringLiteral("image")) {
            content.kind = McpQtContentKind::Image;
            content.mimeType = item.value("mimeType").toString();
            const QString b64 = item.value("data").toString();
            if (!b64.isEmpty()) {
                // 使用 Qt 6 严格模式：含无效字符时返回错误而非静默忽略
                auto decodeResult = QByteArray::fromBase64Encoding(
                    b64.toUtf8(), QByteArray::AbortOnBase64DecodingErrors);
                if (decodeResult.decodingStatus != QByteArray::Base64DecodingStatus::Ok) {
                    content.decodeError = QStringLiteral("Base64 decode failed for image data");
                } else {
                    content.binary = std::move(decodeResult.decoded);
                }
            }
        } else if (type == QStringLiteral("resource")) {
            content.kind = McpQtContentKind::Resource;
            content.mimeType = item.value("mimeType").toString();
            content.text = item.value("text").toString();
        } else {
            content.kind = McpQtContentKind::Unknown;
        }

        result.content.append(content);
    }

    return result;
}

McpQtToolResult McpQtClient::callToolTyped(const QString& nm, const QJsonObject& a, int to) {
    McpResult r = callTool(nm, a, to);
    return parseToolResult(r.data, r.isError, r.errorString);
}

void McpQtClient::callToolTypedAsync(const QString& nm, const QJsonObject& a,
                                     std::function<void(McpQtToolResult)> cb,
                                     int /*timeoutMs*/) {
    callToolAsync(nm, a, this, [cb](McpResult r) {
        cb(parseToolResult(r.data, r.isError, r.errorString));
    });
}

// ========== Resources ==========

QJsonObject McpQtClient::listResources(int to){nlohmann::json e;return m_session?_qj(m_session->listResourcesSync(std::chrono::milliseconds(to),&e)):QJsonObject{};}
QJsonObject McpQtClient::listResources(const QString& c,QString* n,int to){nlohmann::json e;std::string ns;auto r=m_session?_qj(m_session->listResourcesSync(c.toStdString(),&ns,std::chrono::milliseconds(to),&e)):QJsonObject{};if(n)*n=QString::fromStdString(ns);return r;}
QJsonObject McpQtClient::fetchAllResources(int to) {
    QJsonObject all; QJsonArray arr; QString c;
    do { QString nc; auto r=listResources(c,&nc,to); QJsonArray ta=r.value("resources").toArray(); for(const auto& x:ta) arr.append(x); c=nc; } while(!c.isEmpty());
    all["resources"] = arr; return all;
}

void McpQtClient::listResourcesAsync(const QString& cursor, std::function<void(const QJsonObject&, const QString&, const QString&)> callback) {
    if (!m_session) {
        if (callback) QMetaObject::invokeMethod(this, [=]() { callback(QJsonObject{}, "", "No session"); }, Qt::QueuedConnection);
        return;
    }
    m_session->listResources(cursor.toStdString(), [this, callback](const nlohmann::json& result, const std::string& nextCursor, const nlohmann::json& error) {
        if (!callback) return;
        QJsonObject res = _qj(result);
        QString errStr = error.empty() ? QString() : QString::fromStdString(error.dump());
        QString nc = QString::fromStdString(nextCursor);
        QMetaObject::invokeMethod(this, [res, nc, errStr, callback]() {
            callback(res, nc, errStr);
        }, Qt::QueuedConnection);
    });
}

QJsonObject McpQtClient::readResource(const QString& u,int to){nlohmann::json e;return m_session?_qj(m_session->readResourceSync(u.toStdString(),&e,std::chrono::milliseconds(to))):QJsonObject{};}

void McpQtClient::readResourceAsync(const QString& uri, std::function<void(const QJsonObject& result, const QString& error)> callback) {
    if (!m_session) {
        if (callback) QMetaObject::invokeMethod(this, [=]() { callback(QJsonObject{}, "No session"); }, Qt::QueuedConnection);
        return;
    }
    m_session->readResource(uri.toStdString(), [this, callback](const nlohmann::json& result, const nlohmann::json& error) {
        if (!callback) return;
        QJsonObject res = _qj(result);
        QString errStr = error.empty() ? QString() : QString::fromStdString(error.dump());
        QMetaObject::invokeMethod(this, [res, errStr, callback]() {
            callback(res, errStr);
        }, Qt::QueuedConnection);
    });
}
bool McpQtClient::subscribeResource(const QString& u,int to){
    if(!m_session) return false;
    nlohmann::json e;
    bool ok = m_session->subscribeResourceSync(u.toStdString(),&e,std::chrono::milliseconds(to));
    if (ok) {
        m_resourceRouter.subscribe(u, nullptr);
    }
    return ok;
}

void McpQtClient::subscribeResourceAsync(const QString& uri, std::function<void(bool success, const QString& error)> callback) {
    if (!m_session) {
        if (callback) QMetaObject::invokeMethod(this, [=]() { callback(false, "No session"); }, Qt::QueuedConnection);
        return;
    }
    m_session->subscribeResource(uri.toStdString(), [this, uri, callback](bool success, const nlohmann::json& error) {
        if (success) {
            QMetaObject::invokeMethod(this, [this, uri]() { m_resourceRouter.subscribe(uri, nullptr); }, Qt::QueuedConnection);
        }
        if (!callback) return;
        QString errStr = error.empty() ? QString() : QString::fromStdString(error.dump());
        QMetaObject::invokeMethod(this, [success, errStr, callback]() { callback(success, errStr); }, Qt::QueuedConnection);
    });
}

bool McpQtClient::unsubscribeResource(const QString& u,int to){
    m_resourceRouter.unsubscribeAll(u);
    nlohmann::json e;
    return m_session?m_session->unsubscribeResourceSync(u.toStdString(),&e,std::chrono::milliseconds(to)):false;
}

void McpQtClient::unsubscribeResourceAsync(const QString& uri, std::function<void(bool success, const QString& error)> callback) {
    m_resourceRouter.unsubscribeAll(uri);
    if (!m_session) {
        if (callback) QMetaObject::invokeMethod(this, [=]() { callback(false, "No session"); }, Qt::QueuedConnection);
        return;
    }
    m_session->unsubscribeResource(uri.toStdString(), [this, callback](bool success, const nlohmann::json& error) {
        if (!callback) return;
        QString errStr = error.empty() ? QString() : QString::fromStdString(error.dump());
        QMetaObject::invokeMethod(this, [success, errStr, callback]() { callback(success, errStr); }, Qt::QueuedConnection);
    });
}

int McpQtClient::subscribeResource(const QString& uri,
                                    std::function<void(const QString&, const QJsonObject&)> callback,
                                    int to) {
    if (!m_session) return -1;
    nlohmann::json e;
    bool ok = m_session->subscribeResourceSync(uri.toStdString(), &e, std::chrono::milliseconds(to));
    if (!ok) return -1;
    return m_resourceRouter.subscribe(uri, std::move(callback));
}

void McpQtClient::subscribeResourceAsync(const QString& uri,
                                         std::function<void(const QString&, const QJsonObject&)> onUpdate,
                                         std::function<void(int routerToken, const QString& error)> callback) {
    if (!m_session) {
        if (callback) QMetaObject::invokeMethod(this, [=]() { callback(-1, "No session"); }, Qt::QueuedConnection);
        return;
    }
    m_session->subscribeResource(uri.toStdString(), [this, uri, onUpdate, callback](bool success, const nlohmann::json& error) {
        QString errStr = error.empty() ? QString() : QString::fromStdString(error.dump());
        if (!success) {
            if (callback) QMetaObject::invokeMethod(this, [callback, errStr]() { callback(-1, errStr); }, Qt::QueuedConnection);
            return;
        }
        QMetaObject::invokeMethod(this, [this, uri, onUpdate, callback, errStr]() {
            int token = m_resourceRouter.subscribe(uri, onUpdate);
            if (callback) callback(token, errStr);
        }, Qt::QueuedConnection);
    });
}

bool McpQtClient::unsubscribeResourceByToken(const QString& uri, int routerToken, int to) {
    m_resourceRouter.unsubscribe(uri, routerToken);
    if (!m_resourceRouter.hasSubscribers(uri)) {
        nlohmann::json e;
        return m_session ? m_session->unsubscribeResourceSync(uri.toStdString(), &e, std::chrono::milliseconds(to)) : false;
    }
    return true;
}

void McpQtClient::unsubscribeResourceByTokenAsync(const QString& uri, int routerToken, std::function<void(bool success, const QString& error)> callback) {
    m_resourceRouter.unsubscribe(uri, routerToken);
    if (!m_resourceRouter.hasSubscribers(uri)) {
        if (!m_session) {
            if (callback) QMetaObject::invokeMethod(this, [=]() { callback(false, "No session"); }, Qt::QueuedConnection);
            return;
        }
        m_session->unsubscribeResource(uri.toStdString(), [this, callback](bool success, const nlohmann::json& error) {
            if (!callback) return;
            QString errStr = error.empty() ? QString() : QString::fromStdString(error.dump());
            QMetaObject::invokeMethod(this, [success, errStr, callback]() { callback(success, errStr); }, Qt::QueuedConnection);
        });
    } else {
        if (callback) QMetaObject::invokeMethod(this, [=]() { callback(true, ""); }, Qt::QueuedConnection);
    }
}



// ========== Resource Templates ==========
std::vector<mcp::McpResourceTemplate> McpQtClient::listResourceTemplates(int to){nlohmann::json e;return m_session?m_session->listResourceTemplatesSync(std::chrono::milliseconds(to),&e):std::vector<mcp::McpResourceTemplate>{};}
std::vector<mcp::McpResourceTemplate> McpQtClient::listResourceTemplates(const QString& c,QString* n,int to){nlohmann::json e;std::string ns;auto r=m_session?m_session->listResourceTemplatesSync(c.toStdString(),&ns,std::chrono::milliseconds(to),&e):std::vector<mcp::McpResourceTemplate>{};if(n)*n=QString::fromStdString(ns);return r;}
std::vector<mcp::McpResourceTemplate> McpQtClient::fetchAllResourceTemplates(int to) {
    std::vector<mcp::McpResourceTemplate> all; QString c;
    do { QString nc; auto r=listResourceTemplates(c,&nc,to); all.insert(all.end(),r.begin(),r.end()); c=nc; } while(!c.isEmpty());
    return all;
}

void McpQtClient::listResourceTemplatesAsync(const QString& cursor, std::function<void(const std::vector<mcp::McpResourceTemplate>&, const QString&, const QString&)> callback) {
    if (!m_session) {
        if (callback) QMetaObject::invokeMethod(this, [=]() { callback({}, "", "No session"); }, Qt::QueuedConnection);
        return;
    }
    m_session->listResourceTemplates(cursor.toStdString(), [this, callback](const std::vector<mcp::McpResourceTemplate>& templates, const std::string& nextCursor, const nlohmann::json& error) {
        if (!callback) return;
        QString errStr = error.empty() ? QString() : QString::fromStdString(error.dump());
        QString nc = QString::fromStdString(nextCursor);
        QMetaObject::invokeMethod(this, [templates, nc, errStr, callback]() {
            callback(templates, nc, errStr);
        }, Qt::QueuedConnection);
    });
}

std::unique_ptr<McpResourceTemplatesModel> McpQtClient::createResourceTemplatesModel(QObject* parent) {
    auto model = std::make_unique<McpResourceTemplatesModel>(parent);
    model->setClient(this);
    m_templatesModels.append(model.get());
    return model;
}

// ========== Prompts ==========
QJsonObject McpQtClient::listPrompts(int to){nlohmann::json e;return m_session?_qj(m_session->listPromptsSync(std::chrono::milliseconds(to),&e)):QJsonObject{};}
QJsonObject McpQtClient::listPrompts(const QString& c,QString* n,int to){nlohmann::json e;std::string ns;auto r=m_session?_qj(m_session->listPromptsSync(c.toStdString(),&ns,std::chrono::milliseconds(to),&e)):QJsonObject{};if(n)*n=QString::fromStdString(ns);return r;}
QJsonObject McpQtClient::fetchAllPrompts(int to) {
    QJsonObject all; QJsonArray arr; QString c;
    do { QString nc; auto r=listPrompts(c,&nc,to); QJsonArray ta=r.value("prompts").toArray(); for(const auto& x:ta) arr.append(x); c=nc; } while(!c.isEmpty());
    all["prompts"] = arr; return all;
}

void McpQtClient::listPromptsAsync(const QString& cursor, std::function<void(const QJsonObject&, const QString&, const QString&)> callback) {
    if (!m_session) {
        if (callback) QMetaObject::invokeMethod(this, [=]() { callback(QJsonObject{}, "", "No session"); }, Qt::QueuedConnection);
        return;
    }
    m_session->listPrompts(cursor.toStdString(), [this, callback](const nlohmann::json& result, const std::string& nextCursor, const nlohmann::json& error) {
        if (!callback) return;
        QJsonObject res = _qj(result);
        QString errStr = error.empty() ? QString() : QString::fromStdString(error.dump());
        QString nc = QString::fromStdString(nextCursor);
        QMetaObject::invokeMethod(this, [res, nc, errStr, callback]() {
            callback(res, nc, errStr);
        }, Qt::QueuedConnection);
    });
}

QJsonObject McpQtClient::getPrompt(const QString& nm,const QJsonObject& a,int to){nlohmann::json e;return m_session?_qj(m_session->getPromptSync(nm.toStdString(),_nl(a),&e,std::chrono::milliseconds(to))):QJsonObject{};}

void McpQtClient::getPromptAsync(const QString& name, const QJsonObject& arguments, std::function<void(const QJsonObject& result, const QString& error)> callback) {
    if (!m_session) {
        if (callback) QMetaObject::invokeMethod(this, [=]() { callback(QJsonObject{}, "No session"); }, Qt::QueuedConnection);
        return;
    }
    m_session->getPrompt(name.toStdString(), _nl(arguments), [this, callback](const nlohmann::json& result, const nlohmann::json& error) {
        if (!callback) return;
        QJsonObject res = _qj(result);
        QString errStr = error.empty() ? QString() : QString::fromStdString(error.dump());
        QMetaObject::invokeMethod(this, [res, errStr, callback]() {
            callback(res, errStr);
        }, Qt::QueuedConnection);
    });
}

// ========== Etc ==========
bool McpQtClient::ping(int to){return m_session?m_session->pingSync(std::chrono::milliseconds(to)):false;}

void McpQtClient::pingAsync(std::function<void(bool success, const QString& error)> callback) {
    if (!m_session) {
        if (callback) QMetaObject::invokeMethod(this, [=]() { callback(false, "No session"); }, Qt::QueuedConnection);
        return;
    }
    m_session->ping([this, callback](bool success, const nlohmann::json& error) {
        if (!callback) return;
        QString errStr = error.empty() ? QString() : QString::fromStdString(error.dump());
        QMetaObject::invokeMethod(this, [success, errStr, callback]() { callback(success, errStr); }, Qt::QueuedConnection);
    });
}

QJsonObject McpQtClient::complete(const QJsonObject& rf,const QJsonObject& ag,int to){nlohmann::json e;return m_session?_qj(m_session->completeSync(_nl(rf),_nl(ag),&e,std::chrono::milliseconds(to))):QJsonObject{};}

void McpQtClient::completeAsync(const QJsonObject& ref, const QJsonObject& argument, std::function<void(const QJsonObject& completion, const QString& error)> callback) {
    if (!m_session) {
        if (callback) QMetaObject::invokeMethod(this, [=]() { callback(QJsonObject{}, "No session"); }, Qt::QueuedConnection);
        return;
    }
    m_session->complete(_nl(ref), _nl(argument), [this, callback](const nlohmann::json& completion, const nlohmann::json& error) {
        if (!callback) return;
        QJsonObject comp = _qj(completion);
        QString errStr = error.empty() ? QString() : QString::fromStdString(error.dump());
        QMetaObject::invokeMethod(this, [comp, errStr, callback]() { callback(comp, errStr); }, Qt::QueuedConnection);
    });
}
bool McpQtClient::setLoggingLevel(const QString& lv,int to){if(!m_session)return false;nlohmann::json e;m_session->callToolSync("logging/setLevel",_nl(QJsonObject{{"level",lv}}),&e,std::chrono::milliseconds(to));return e.empty();}

// ========== 双向能力 ==========
void McpQtClient::setElicitationHandler(ElicitationHandler h){
    setElicitationHandler(this, h);
}
void McpQtClient::setElicitationHandler(QObject* ctx, ElicitationHandler h){
    m_savedElicitationHandler = h;
    m_savedElicitationContext = QPointer<QObject>(ctx);
    m_hasSavedElicitationContext = (ctx != nullptr);
    if(!m_session) return;
    
    registerCapability("experimental", QJsonObject{{"elicitation", QJsonObject()}});
    
    bool hasCtx = m_hasSavedElicitationContext;
    QPointer<QObject> pCtx = m_savedElicitationContext;
    m_session->setElicitationHandler([h, pCtx, hasCtx](const nlohmann::json& p, std::function<void(const nlohmann::json&, const nlohmann::json&)> cb){
        QJsonObject qp = _qj(p);
        auto localCb = [cb](const QJsonObject& res, const QJsonObject& err) {
            cb(_nl(res), _nl(err));
        };
        if (hasCtx) {
            if (pCtx) {
                QMetaObject::invokeMethod(pCtx.data(), [h, qp, localCb]() {
                    h(qp, localCb);
                }, Qt::QueuedConnection);
            } else {
                cb(nlohmann::json::object(), {{"code", -32000}, {"message", "Client context destroyed"}});
            }
        } else {
            h(qp, localCb);
        }
    });
}
void McpQtClient::setSamplingHandler(SamplingHandler h){
    setSamplingHandler(this, h);
}
void McpQtClient::setSamplingHandler(QObject* ctx, SamplingHandler h){
    m_savedSamplingHandler = h;
    m_savedSamplingContext = QPointer<QObject>(ctx);
    m_hasSavedSamplingContext = (ctx != nullptr);
    if(!m_session) return;
    
    registerCapability("sampling", QJsonObject());
    
    bool hasCtx = m_hasSavedSamplingContext;
    QPointer<QObject> pCtx = m_savedSamplingContext;
    m_session->setSamplingHandler([h, pCtx, hasCtx](const nlohmann::json& p, std::function<void(const nlohmann::json&, const nlohmann::json&)> cb){
        QJsonObject qp = _qj(p);
        auto localCb = [cb](const QJsonObject& res, const QJsonObject& err) {
            cb(_nl(res), _nl(err));
        };
        if (hasCtx) {
            if (pCtx) {
                QMetaObject::invokeMethod(pCtx.data(), [h, qp, localCb]() {
                    h(qp, localCb);
                }, Qt::QueuedConnection);
            } else {
                cb(nlohmann::json::object(), {{"code", -32000}, {"message", "Client context destroyed"}});
            }
        } else {
            h(qp, localCb);
        }
    });
}
void McpQtClient::setRootsProvider(RootsProvider p){
    setRootsProvider(this, p);
}
void McpQtClient::setRootsProvider(QObject* ctx, RootsProvider p){
    m_savedRootsProvider = p;
    m_savedRootsContext = QPointer<QObject>(ctx);
    m_hasSavedRootsContext = (ctx != nullptr);
    if(!m_session) return;
    
    registerCapability("roots", QJsonObject{{"listChanged", true}});
    
    bool hasCtx = m_hasSavedRootsContext;
    QPointer<QObject> pCtx = m_savedRootsContext;
    m_session->setRootsProvider([p, pCtx, hasCtx](std::function<void(const nlohmann::json&, const nlohmann::json&)> cb){
        auto localCb = [cb](const QJsonArray& res, const QJsonObject& err) {
            cb(_nl(QJsonObject{{"roots", res}})["roots"], _nl(err));
        };
        if (hasCtx) {
            if (pCtx) {
                QMetaObject::invokeMethod(pCtx.data(), [p, localCb]() {
                    p(localCb);
                }, Qt::QueuedConnection);
            } else {
                cb(nlohmann::json::array(), {{"code", -32000}, {"message", "Client context destroyed"}});
            }
        } else {
            p(localCb);
        }
    });
}
void McpQtClient::notifyRootsListChanged(){if(m_session)m_session->notifyRootsListChanged();}

// ========== 通知 ==========
void McpQtClient::registerNotificationHandler(const QString& m,std::function<void(const QJsonObject&)> h){
    m_savedNotificationHandlers.append(NotificationHandlerEntry{m, nullptr, h, false});
    if(!m_session) return;
    m_session->registerNotificationHandler(m.toStdString(), [h](const nlohmann::json& p) {
        h(_qj(p));
    });
}
void McpQtClient::registerNotificationHandler(const QString& m, QObject* ctx, std::function<void(const QJsonObject&)> h){
    m_savedNotificationHandlers.append(NotificationHandlerEntry{m, QPointer<QObject>(ctx), h, true});
    if(!m_session) return;
    QPointer<QObject> pCtx(ctx);
    m_session->registerNotificationHandler(m.toStdString(), [h, pCtx](const nlohmann::json& p) {
        QJsonObject qp = _qj(p);
        if (pCtx) {
            QMetaObject::invokeMethod(pCtx.data(), [h, qp]() {
                h(qp);
            }, Qt::QueuedConnection);
        }
    });
}
void McpQtClient::enableNotificationDebounce(const QString& m,int ms){if(m_session)m_session->enableNotificationDebounce(m.toStdString(),std::chrono::milliseconds(ms));}
void McpQtClient::sendNotification(const QString& m,const QJsonObject& p){if(m_session)m_session->sendNotification(m.toStdString(),_nl(p));}

// ========== 异步 ==========
int64_t McpQtClient::sendRequest(const QString& m,const QJsonObject& p,std::function<void(const QJsonObject&,const QJsonObject&)> cb,ProgressCallback onP){
    return sendRequest(m, p, this, cb, onP);
}

int64_t McpQtClient::sendRequest(const QString& m,const QJsonObject& p, QObject* ctx, std::function<void(const QJsonObject&,const QJsonObject&)> cb,ProgressCallback onP){
    if(!m_session)return-1;
    bool hasCtx = (ctx != nullptr);
    QPointer<QObject> pCtx(ctx);
    const bool replayable = isReplayableMethod(m);
    auto requestId = std::make_shared<int64_t>(-1);
    auto pf=onP?std::function<void(const nlohmann::json&)>([onP, pCtx, hasCtx](const nlohmann::json& pi){
        float pt=pi.value("progress",0.0f),tt=pi.value("total",0.0f);
        QString msg=QString::fromStdString(pi.value("message",""));
        if(hasCtx) {
            if(pCtx) { QMetaObject::invokeMethod(pCtx.data(), [onP,pt,tt,msg](){ onP(pt,tt,msg); }, Qt::QueuedConnection); }
        }
        else { onP(pt,tt,msg); }
    }):nullptr;
    int64_t id = m_session->sendRequest(m.toStdString(),_nl(p),[this, cb, pCtx, hasCtx, replayable, m, p, onP, requestId](const nlohmann::json& r,const nlohmann::json& e){
        if (replayable
            && !m_isUserClosed
            && m_reconnectPolicy.enabled
            && !e.empty()
            && e.value("message", std::string()) == "Connection interrupted or server crashed") {
            std::lock_guard<std::mutex> lock(m_replayMutex);
            auto it = m_inFlightReplayableRequests.find(*requestId);
            if (it != m_inFlightReplayableRequests.end()) {
                m_queuedReplayRequests.push_back(std::move(it->second));
                m_inFlightReplayableRequests.erase(it);
            }
            return;
        }

        if (replayable) {
            std::lock_guard<std::mutex> lock(m_replayMutex);
            m_inFlightReplayableRequests.erase(*requestId);
        }

        QJsonObject qr=_qj(r), qe=_qj(e);
        if(hasCtx) {
            if(pCtx) { QMetaObject::invokeMethod(pCtx.data(), [cb,qr,qe](){ cb(qr,qe); }, Qt::QueuedConnection); }
        }
        else { cb(qr,qe); }
    },pf);

    *requestId = id;
    if (replayable) {
        ReplayableRequest replay;
        replay.method = m;
        replay.params = p;
        replay.context = pCtx;
        replay.hasContext = hasCtx;
        replay.callback = cb;
        replay.progressCallback = onP;
        std::lock_guard<std::mutex> lock(m_replayMutex);
        m_inFlightReplayableRequests.emplace(id, std::move(replay));
    }

    return id;
}
void McpQtClient::cancelRequest(int64_t id){if(m_session)m_session->cancelRequest(id);}

// ========== 能力 ==========
void McpQtClient::registerCapability(const QString& n,const QJsonObject& c){if(m_session)m_session->registerCapabilities(_nl(QJsonObject{{n,c}}));}

// ========== 生命周期 ==========
bool McpQtClient::isConnected()const{return m_session&&m_session->state()==mcp::SessionState::Initialized;}
void McpQtClient::close(int to){
    m_isUserClosed = true;
    if (m_reconnectTimer) {
        m_reconnectTimer->stop();
        delete m_reconnectTimer;
        m_reconnectTimer = nullptr;
    }
    m_reconnectAttempts = 0;
    m_inRecovery = false;
    m_savedNotificationHandlers.clear();
    {
        std::lock_guard<std::mutex> lock(m_replayMutex);
        m_queuedReplayRequests.clear();
        m_inFlightReplayableRequests.clear();
    }

    if(m_session){
        m_session->shutdownSync(std::chrono::milliseconds(to));
        m_session->close();
        m_session.reset();
    }
    m_initialized=false;
    emit disconnected();
}

void McpQtClient::setReconnectPolicy(const mcp::McpReconnectPolicy& policy) {
    m_reconnectPolicy = policy;
}

mcp::McpReconnectPolicy McpQtClient::reconnectPolicy() const {
    return m_reconnectPolicy;
}

void McpQtClient::setTransportFactory(std::function<std::shared_ptr<mcp::IMcpTransport>()> factory) {
    m_transportFactory = std::move(factory);
}

void McpQtClient::handleTransportFailure() {
    if (m_isUserClosed || m_inRecovery || !m_reconnectPolicy.enabled) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_replayMutex);
        for (auto& [id, request] : m_inFlightReplayableRequests) {
            m_queuedReplayRequests.push_back(std::move(request));
        }
        m_inFlightReplayableRequests.clear();
    }

    if (m_reconnectPolicy.maxAttempts != -1 && m_reconnectAttempts >= m_reconnectPolicy.maxAttempts) {
        emit recoveryFailed(QStringLiteral("Maximum reconnect attempts reached"));
        return;
    }

    m_inRecovery = true;
    emit reconnecting();

    if (!m_reconnectTimer) {
        m_reconnectTimer = new QTimer(this);
        QObject::connect(m_reconnectTimer, &QTimer::timeout, this, &McpQtClient::executeReconnectAttempt);
    }

    int delay = m_reconnectPolicy.getDelayMs(++m_reconnectAttempts);
    m_reconnectTimer->start(delay);
}

void McpQtClient::executeReconnectAttempt() {
    m_reconnectTimer->stop();

    std::shared_ptr<mcp::IMcpTransport> newTransport;
    if (m_transportFactory) {
        newTransport = m_transportFactory();
    } else {
        if (m_transportType == 1) {
            auto t = std::make_shared<mcp_qt::QtHttpSseTransport>(m_url_or_cmd.toStdString());
            mcp_qt::QtHttpRequestConfig cfg;
            for (auto it = m_httpHeaders.constBegin(); it != m_httpHeaders.constEnd(); ++it) {
                cfg.defaultHeaders.insert(it.key().toUtf8(), it.value().toUtf8());
            }
            if (m_proxy) {
                cfg.proxy = *m_proxy;
            }
            t->setRequestConfig(cfg);
            newTransport = t;
        } else if (m_transportType == 2) {
            std::vector<std::string> a;
            for (const auto& x : m_args) {
                a.push_back(x.toStdString());
            }
            newTransport = std::make_shared<mcp_qt::QtProcessStdioTransport>(m_url_or_cmd.toStdString(), a);
        }
    }

    if (!newTransport) {
        emit recoveryFailed(QStringLiteral("Auto-reconnect failed: no transport factory or configuration set"));
        m_inRecovery = false;
        return;
    }

    QString err;
    if (connectToTransport(newTransport, m_clientName, m_clientVersion, m_timeoutMs, &err)) {
        m_reconnectAttempts = 0;
        m_inRecovery = false;

        // 延迟 100ms 异步恢复处理器与订阅状态，确保 QNAM 有充分的时间转动事件循环并接收 SSE 的 endpoint
        QTimer::singleShot(100, this, [this]() {
            refreshToolsAfterRecovery();
            restoreNotificationHandlers();
            restoreResourceSubscriptions();
            replayQueuedRequests();
            emit reconnected();
        });
    } else {
        m_inRecovery = false; // 允许下一轮触发
        handleTransportFailure();
    }
}

void McpQtClient::restoreNotificationHandlers() {
    if (!m_session) return;
    for (const auto& entry : m_savedNotificationHandlers) {
        if (!entry.hasContext || entry.context) {
            auto h = entry.handler;
            if (entry.hasContext) {
                QPointer<QObject> pCtx = entry.context;
                m_session->registerNotificationHandler(entry.method.toStdString(), [h, pCtx](const nlohmann::json& p) {
                    QJsonObject qp = _qj(p);
                    if (pCtx) {
                        QMetaObject::invokeMethod(pCtx.data(), [h, qp]() {
                            h(qp);
                        }, Qt::QueuedConnection);
                    }
                });
            } else {
                m_session->registerNotificationHandler(entry.method.toStdString(), [h](const nlohmann::json& p) {
                    QJsonObject qp = _qj(p);
                    h(qp);
                });
            }
        }
    }
}

void McpQtClient::restoreResourceSubscriptions() {
    auto uris = m_resourceRouter.subscribedUris();
    for (const auto& uri : uris) {
        if (!m_session) continue;
        m_session->subscribeResource(uri.toStdString(), [](bool, const nlohmann::json&){});
    }
}

void McpQtClient::refreshToolsAfterRecovery() {
    if (!m_session) return;

    // 异步拉取首页以更新工具缓存，避免阻塞主线程死锁
    listToolsAsync("", nullptr);


    for (auto it = m_toolsModels.begin(); it != m_toolsModels.end();) {
        if (!*it) {
            it = m_toolsModels.erase(it);
            continue;
        }
        (*it)->refresh();
        ++it;
    }
}

void McpQtClient::replayQueuedRequests() {
    std::vector<ReplayableRequest> queued;
    {
        std::lock_guard<std::mutex> lock(m_replayMutex);
        queued.swap(m_queuedReplayRequests);
    }

    for (const auto& entry : queued) {
        if (entry.hasContext && !entry.context) {
            continue;
        }
        sendRequest(entry.method,
                    entry.params,
                    entry.hasContext ? entry.context.data() : nullptr,
                    entry.callback,
                    entry.progressCallback);
    }
}

bool McpQtClient::isReplayableMethod(const QString& method) const {
    return method == QStringLiteral("tools/list")
        || method == QStringLiteral("resources/list")
        || method == QStringLiteral("resources/templates/list")
        || method == QStringLiteral("prompts/list")
        || method == QStringLiteral("resources/subscribe");
}

void McpQtClient::setTrafficLogger(TrafficLogger logger) {
    m_trafficLogger = std::move(logger);
    if (m_session) {
        if (m_trafficLogger) {
            m_session->setTrafficCallback([this](const mcp::McpTrafficEvent& event) {
                QJsonObject obj;
                QString dirStr = (event.direction == mcp::McpTrafficDirection::Outbound) ? QStringLiteral("outbound") : QStringLiteral("inbound");
                obj[QStringLiteral("direction")] = dirStr;
                obj[QStringLiteral("timestamp")] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);

                QString kindStr = QStringLiteral("unknown");
                switch (event.kind) {
                    case mcp::McpTrafficKind::Request: kindStr = QStringLiteral("request"); break;
                    case mcp::McpTrafficKind::Response: kindStr = QStringLiteral("response"); break;
                    case mcp::McpTrafficKind::Notification: kindStr = QStringLiteral("notification"); break;
                    case mcp::McpTrafficKind::Unknown: kindStr = QStringLiteral("unknown"); break;
                }
                obj[QStringLiteral("kind")] = kindStr;
                obj[QStringLiteral("payload")] = _qj(event.payload);
                obj[QStringLiteral("raw")] = QString::fromStdString(event.raw);

                if (m_trafficLogger) {
                    m_trafficLogger(obj);
                }
            });
        } else {
            m_session->setTrafficCallback(nullptr);
        }
    }
}

} // namespace mcp_qt

// ========== 同步 API ==========
// ... (以下代码保持不变)
