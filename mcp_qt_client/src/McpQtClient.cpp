#include "mcp_qt_client/McpQtClient.h"
#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <sstream>

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
// McpQtClient
// ============================================================================

McpQtClient::McpQtClient(QObject* p):QObject(p),m_oauth(std::make_shared<mcp::McpOAuthClient>()){}
McpQtClient::~McpQtClient(){close();}

McpQtClient::Ptr McpQtClient::connectHttp(const QString& url,const QString& name,const QString& ver,int to){
    auto c=Ptr(new McpQtClient());auto t=std::make_shared<mcp_qt::QtHttpSseTransport>(url.toStdString());
    if(!c->connectToTransport(t,name,ver,to))return nullptr;return c;
}
McpQtClient::Ptr McpQtClient::connectStdio(const QString& cmd,const QStringList& args,const QString& name,const QString& ver,int to){
    auto c=Ptr(new McpQtClient());std::vector<std::string> a;for(const auto& x:args)a.push_back(x.toStdString());
    auto t=std::make_shared<mcp::SubprocessStdioTransport>(cmd.toStdString(),a);
    if(!c->connectToTransport(t,name,ver,to))return nullptr;return c;
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

bool McpQtClient::connectToTransport(std::shared_ptr<mcp::IMcpTransport> t,const QString& name,const QString& ver,int to){
    m_session=std::make_shared<mcp::McpClientSession>(t);m_session->init();
    if(!m_session->start())return false;return doInitialize(name,ver,to);
}
bool McpQtClient::doInitialize(const QString& name,const QString& ver,int to){
    nlohmann::json info;
    if(!m_session->initializeSync(name.toStdString(),ver.toStdString(),&info,std::chrono::milliseconds(to)))return false;
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

// ========== Tools ==========
std::vector<mcp::McpTool> McpQtClient::listTools(int to){nlohmann::json e;return m_session?m_session->listToolsSync(std::chrono::milliseconds(to),&e):std::vector<mcp::McpTool>{};}
std::vector<mcp::McpTool> McpQtClient::listTools(const QString& c,QString* n,int to){nlohmann::json e;std::string ns;auto r=m_session?m_session->listToolsSync(c.toStdString(),&ns,std::chrono::milliseconds(to),&e):std::vector<mcp::McpTool>{};if(n)*n=QString::fromStdString(ns);return r;}
QJsonObject McpQtClient::callTool(const QString& nm,const QJsonObject& a,int to){nlohmann::json e;return m_session?_qj(m_session->callToolSync(nm.toStdString(),_nl(a),&e,std::chrono::milliseconds(to))):QJsonObject{};}
QJsonObject McpQtClient::callTool(const QString& nm,const QJsonObject& a,ProgressCallback onP,int to){nlohmann::json e;auto pf=[onP](const nlohmann::json& pi){if(onP){float p=pi.value("progress",0.0f),t=pi.value("total",0.0f);onP(p,t,QString::fromStdString(pi.value("message","")));}};return m_session?_qj(m_session->callToolSync(nm.toStdString(),_nl(a),&e,std::chrono::milliseconds(to),pf)):QJsonObject{};}

// ========== Resources ==========
QJsonObject McpQtClient::listResources(int to){nlohmann::json e;return m_session?_qj(m_session->listResourcesSync(std::chrono::milliseconds(to),&e)):QJsonObject{};}
QJsonObject McpQtClient::listResources(const QString& c,QString* n,int to){nlohmann::json e;std::string ns;auto r=m_session?_qj(m_session->listResourcesSync(c.toStdString(),&ns,std::chrono::milliseconds(to),&e)):QJsonObject{};if(n)*n=QString::fromStdString(ns);return r;}
QJsonObject McpQtClient::readResource(const QString& u,int to){nlohmann::json e;return m_session?_qj(m_session->readResourceSync(u.toStdString(),&e,std::chrono::milliseconds(to))):QJsonObject{};}
bool McpQtClient::subscribeResource(const QString& u,int to){nlohmann::json e;return m_session?m_session->subscribeResourceSync(u.toStdString(),&e,std::chrono::milliseconds(to)):false;}
bool McpQtClient::unsubscribeResource(const QString& u,int to){nlohmann::json e;return m_session?m_session->unsubscribeResourceSync(u.toStdString(),&e,std::chrono::milliseconds(to)):false;}

// ========== Resource Templates ==========
std::vector<mcp::McpResourceTemplate> McpQtClient::listResourceTemplates(int to){nlohmann::json e;return m_session?m_session->listResourceTemplatesSync(std::chrono::milliseconds(to),&e):std::vector<mcp::McpResourceTemplate>{};}
std::vector<mcp::McpResourceTemplate> McpQtClient::listResourceTemplates(const QString& c,QString* n,int to){nlohmann::json e;std::string ns;auto r=m_session?m_session->listResourceTemplatesSync(c.toStdString(),&ns,std::chrono::milliseconds(to),&e):std::vector<mcp::McpResourceTemplate>{};if(n)*n=QString::fromStdString(ns);return r;}

// ========== Prompts ==========
QJsonObject McpQtClient::listPrompts(int to){nlohmann::json e;return m_session?_qj(m_session->listPromptsSync(std::chrono::milliseconds(to),&e)):QJsonObject{};}
QJsonObject McpQtClient::listPrompts(const QString& c,QString* n,int to){nlohmann::json e;std::string ns;auto r=m_session?_qj(m_session->listPromptsSync(c.toStdString(),&ns,std::chrono::milliseconds(to),&e)):QJsonObject{};if(n)*n=QString::fromStdString(ns);return r;}
QJsonObject McpQtClient::getPrompt(const QString& nm,const QJsonObject& a,int to){nlohmann::json e;return m_session?_qj(m_session->getPromptSync(nm.toStdString(),_nl(a),&e,std::chrono::milliseconds(to))):QJsonObject{};}

// ========== Etc ==========
bool McpQtClient::ping(int to){return m_session?m_session->pingSync(std::chrono::milliseconds(to)):false;}
QJsonObject McpQtClient::complete(const QJsonObject& rf,const QJsonObject& ag,int to){nlohmann::json e;return m_session?_qj(m_session->completeSync(_nl(rf),_nl(ag),&e,std::chrono::milliseconds(to))):QJsonObject{};}
bool McpQtClient::setLoggingLevel(const QString& lv,int to){if(!m_session)return false;nlohmann::json e;m_session->callToolSync("logging/setLevel",_nl(QJsonObject{{"level",lv}}),&e,std::chrono::milliseconds(to));return e.empty();}

// ========== 双向能力 ==========
void McpQtClient::setElicitationHandler(ElicitationHandler h){if(m_session)m_session->setElicitationHandler([h](const nlohmann::json&p)->nlohmann::json{return _nl(h(_qj(p)));});}
void McpQtClient::setSamplingHandler(SamplingHandler h){if(m_session)m_session->setSamplingHandler([h](const nlohmann::json&p)->nlohmann::json{return _nl(h(_qj(p)));});}
void McpQtClient::setRootsProvider(RootsProvider p){if(m_session)m_session->setRootsProvider([p]()->nlohmann::json{QJsonArray a=p();return _nl(QJsonObject{{"roots",a}});});}
void McpQtClient::notifyRootsListChanged(){if(m_session)m_session->notifyRootsListChanged();}

// ========== 通知 ==========
void McpQtClient::registerNotificationHandler(const QString& m,std::function<void(const QJsonObject&)> h){if(m_session)m_session->registerNotificationHandler(m.toStdString(),[h](const nlohmann::json&p){h(_qj(p));});}
void McpQtClient::enableNotificationDebounce(const QString& m,int ms){if(m_session)m_session->enableNotificationDebounce(m.toStdString(),std::chrono::milliseconds(ms));}
void McpQtClient::sendNotification(const QString& m,const QJsonObject& p){if(m_session)m_session->sendNotification(m.toStdString(),_nl(p));}

// ========== 异步 ==========
int64_t McpQtClient::sendRequest(const QString& m,const QJsonObject& p,std::function<void(const QJsonObject&,const QJsonObject&)> cb,ProgressCallback onP){
    if(!m_session)return-1;auto pf=onP?std::function<void(const nlohmann::json&)>([onP](const nlohmann::json& pi){float p=pi.value("progress",0.0f),t=pi.value("total",0.0f);onP(p,t,QString::fromStdString(pi.value("message","")));}):nullptr;
    return m_session->sendRequest(m.toStdString(),_nl(p),[cb](const nlohmann::json& r,const nlohmann::json& e){cb(_qj(r),_qj(e));},pf);
}
void McpQtClient::cancelRequest(int64_t id){if(m_session)m_session->cancelRequest(id);}

// ========== 能力 ==========
void McpQtClient::registerCapability(const QString& n,const QJsonObject& c){if(m_session)m_session->registerCapabilities(_nl(QJsonObject{{n,c}}));}

// ========== 生命周期 ==========
bool McpQtClient::isConnected()const{return m_session&&m_session->state()==mcp::SessionState::Initialized;}
void McpQtClient::close(int to){if(m_session){m_session->shutdownSync(std::chrono::milliseconds(to));m_session->close();m_session.reset();}m_initialized=false;emit disconnected();}

} // namespace mcp_qt

// ========== 同步 API ==========
// ... (以下代码保持不变)
