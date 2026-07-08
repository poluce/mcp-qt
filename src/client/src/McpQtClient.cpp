#include "mcp_qt_client/McpQtClient.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include "mcp_core/McpClientSession.h"
#include "mcp_core/McpOAuthClient.h"
#include "mcp_core/IMcpTransport.h"
#include "mcp_core/McpReconnectPolicy.h"

inline void assertNotMainGuiThread() {
#if !defined(QT_NO_DEBUG)
    QCoreApplication* app = QCoreApplication::instance();
    // Allow synchronous blocks during application teardown to cleanly close resources.
    // If the application is quitting, it might not be safe to assert.
    if (app && app->inherits("QGuiApplication")) {
        // We warn instead of crashing, because crashing on teardown or nested event loop 
        // in some specific UI designs might be too aggressive for a general SDK.
        if (QThread::currentThread() == app->thread()) {
            qWarning() << "[McpQtClient] WARNING: Blocking synchronous MCP call executed on the main GUI thread! "
                       << "This may cause UI freezes or nested event loop issues.";
        }
    }
#endif
}
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

// ========== Rich Text Parsing (McpResult Content) ==========
static QList<McpContent> parseMcpContents(const QJsonObject& raw, bool& decodingError) {
    QList<McpContent> contents;
    const QJsonArray contentArray = raw.value("content").toArray();
    for (const QJsonValue& val : contentArray) {
        const QJsonObject item = val.toObject();
        McpContent content;
        content.rawData = item;

        const QString type = item.value("type").toString();
        if (type == QStringLiteral("text")) {
            content.kind = McpContentKind::Text;
            content.text = item.value("text").toString();
        } else if (type == QStringLiteral("image")) {
            content.kind = McpContentKind::Image;
            content.mimeType = item.value("mimeType").toString();
            const QString b64 = item.value("data").toString();
            if (!b64.isEmpty()) {
                auto decodeResult = QByteArray::fromBase64Encoding(
                    b64.toUtf8(), QByteArray::AbortOnBase64DecodingErrors);
                if (decodeResult.decodingStatus == QByteArray::Base64DecodingStatus::Ok) {
                    content.binary = std::move(decodeResult.decoded);
                } else {
                    decodingError = true;
                }
            }
        } else if (type == QStringLiteral("resource")) {
            content.kind = McpContentKind::EmbeddedResource;
            QJsonObject resObj = item.value("resource").toObject();
            if (!resObj.isEmpty()) {
                content.mimeType = resObj.value("mimeType").toString();
                if (resObj.contains("text")) {
                    content.text = resObj.value("text").toString();
                } else if (resObj.contains("blob")) {
                    const QString blobB64 = resObj.value("blob").toString();
                    auto decodeResult = QByteArray::fromBase64Encoding(
                        blobB64.toUtf8(), QByteArray::AbortOnBase64DecodingErrors);
                    if (decodeResult.decodingStatus == QByteArray::Base64DecodingStatus::Ok) {
                        content.binary = std::move(decodeResult.decoded);
                    } else {
                        decodingError = true;
                    }
                }
            } else {
                content.mimeType = item.value("mimeType").toString();
                content.text = item.value("text").toString();
            }
        }
        contents.append(content);
    }
    return contents;
}

struct PromiseGuard {
    std::shared_ptr<QPromise<McpResult>> promise;
    bool finished{false};
    ~PromiseGuard() {
        if (promise && !finished) {
            McpResult cancelResult;
            cancelResult.isError = true;
            cancelResult.errorString = QStringLiteral("Call canceled due to client destruction");
            promise->addResult(cancelResult);
            promise->finish();
        }
    }
};

// ============================================================================
// QNAM 同步 HTTP（整个 Qt 客户端零 libcurl 依赖）
// ============================================================================
static QByteArray _executeSync(QNetworkReply* p) {
    QEventLoop l;
    QObject::connect(p, &QNetworkReply::finished, &l, &QEventLoop::quit);
    l.exec();
    QByteArray rb = p->readAll();
    p->deleteLater();
    return rb;
}

static QByteArray _get(const QString& u){
    QNetworkAccessManager n; QNetworkRequest r{QUrl{u}}; r.setRawHeader("Accept","application/json");
    return _executeSync(n.get(r));
}
static QByteArray _post(const QString& u, const QByteArray& b, const char* ct){
    QNetworkAccessManager n; QNetworkRequest r{QUrl{u}}; r.setHeader(QNetworkRequest::ContentTypeHeader,ct); r.setRawHeader("Accept","application/json");
    return _executeSync(n.post(r,b));
}
// POST with custom headers (for token exchange with Basic Auth)
static QByteArray _postH(const QString& u, const QByteArray& b, const QMap<QByteArray,QByteArray>& extraHeaders){
    QNetworkAccessManager n; QNetworkRequest r{QUrl{u}};
    r.setHeader(QNetworkRequest::ContentTypeHeader,"application/x-www-form-urlencoded");
    r.setRawHeader("Accept","application/json");
    for(auto it=extraHeaders.begin();it!=extraHeaders.end();++it) r.setRawHeader(it.key(),it.value());
    return _executeSync(n.post(r,b));
}

// ============================================================================
// OAuth（全 QNAM，零 libcurl）
// ============================================================================
static std::string _eRm(const std::string& w){
    for(auto* px:{"resource_metadata=\"","resource_metadata="}){
        size_t p=w.find(px);
        if(p==std::string::npos) continue;
        size_t s=p+strlen(px);
        size_t e;
        if(px[strlen(px)-1]=='"'){
            // 值被引号包围，查找结束引号
            e=w.find('"',s);
        }else{
            // 值没有引号，查找逗号或空格
            e=w.find_first_of(", ",s);
        }
        if(e==std::string::npos) e=w.length();
        std::string result=w.substr(s,e-s);
        // 移除末尾可能的引号
        if(!result.empty() && result.back()=='"') result.pop_back();
        return result;
    }
    return "";
}
static std::string _bUrl(const std::string& u){size_t p=u.find("://");if(p==std::string::npos)return u;size_t q=u.find('/',p+3);return q!=std::string::npos?u.substr(0,q):u;}
static bool _dmQt(const QString& is,mcp::OAuthServerMetadata* o, bool* cidMetaSupp = nullptr){
    QString b=is;if(!b.endsWith('/'))b+='/';QStringList u;u<<b+".well-known/oauth-authorization-server"<<b+".well-known/openid-configuration";
    int se=is.indexOf("://");if(se>=0){int ps=is.indexOf('/',se+3);if(ps>=0){QString oBase=is.left(ps),pp=is.mid(ps);if(!pp.isEmpty()&&pp!="/"){if(pp.endsWith('/'))pp.chop(1);u<<oBase+"/.well-known/oauth-authorization-server"+pp<<oBase+"/.well-known/openid-configuration"+pp;}u<<oBase+"/.well-known/oauth-authorization-server"<<oBase+"/.well-known/openid-configuration";}}
    for(const auto& x:u){QByteArray d=_get(x);if(!d.isEmpty()){auto j=nlohmann::json::parse(d.toStdString(),nullptr,false);if(!j.is_discarded()&&j.contains("token_endpoint")){try{*o=mcp::OAuthServerMetadata::fromJson(j);if(cidMetaSupp && j.contains("client_id_metadata_document_supported") && j["client_id_metadata_document_supported"].is_boolean()) *cidMetaSupp = j["client_id_metadata_document_supported"].get<bool>(); return true;}catch(...){}}}}return false;
}

// 完整 OAuth 认证（全 QNAM，无 libcurl）
static bool _runOAuthQt(const std::string& sseUrl, const nlohmann::json& ctx,
                        const std::string& wwwAuth, std::shared_ptr<mcp::McpOAuthClient> oc,
                        const QString& redirectUri="http://localhost:3000/callback"){
    std::cerr << "[OAuth] Starting OAuth flow for " << sseUrl << std::endl;
    std::string pu=_eRm(wwwAuth); nlohmann::json pj;
    // 1) PRM 发现（可选，失败则回退到直接 Metadata 发现）
    if(pu.empty()){std::string b=_bUrl(sseUrl);bool fd=false;
        for(const char* px:{"/.well-known/oauth-protected-resource/mcp","/.well-known/oauth-protected-resource","/.well-known/mcp-protected-resource-metadata"}){
            QByteArray d=_get(QString::fromStdString(b+px));auto j=nlohmann::json::parse(d.toStdString(),nullptr,false);
            if(!j.is_discarded()&&j.contains("authorization_servers")&&j["authorization_servers"].is_array()&&!j["authorization_servers"].empty()){pu=b+px;pj=std::move(j);fd=true;break;}
        }
        if(!fd){std::cerr << "[OAuth] PRM discovery failed, trying direct metadata discovery" << std::endl;}
    }else{QByteArray ps=_get(QString::fromStdString(pu));pj=nlohmann::json::parse(ps.toStdString(),nullptr,false);}

    // 2) Metadata 发现
    mcp::OAuthServerMetadata mm;
    bool cidMetaSupp = false;
    if(!pj.is_discarded()&&pj.contains("authorization_servers")){
        // 从 PRM 获取 authorization_servers
        std::cerr << "[OAuth] PRM discovered, authorization_servers: " << pj["authorization_servers"][0].get<std::string>() << std::endl;
        QString iu=QString::fromStdString(pj["authorization_servers"][0].get<std::string>());
        if(pj.contains("oauthMetadataLocation")&&pj["oauthMetadataLocation"].is_string())iu=QString::fromStdString(_bUrl(sseUrl))+QString::fromStdString(pj["oauthMetadataLocation"].get<std::string>());
        if(!_dmQt(iu,&mm,&cidMetaSupp)){std::cerr << "[OAuth] Metadata discovery failed" << std::endl;return false;}
    }else{
        // 直接尝试 OAuth Authorization Server Metadata（2025-03-26 兼容）
        std::cerr << "[OAuth] Trying direct OAuth metadata discovery" << std::endl;
        QString b=QString::fromStdString(sseUrl);
        if(!_dmQt(b,&mm,&cidMetaSupp)){std::cerr << "[OAuth] Direct metadata discovery failed" << std::endl;return false;}
    }
    std::cerr << "[OAuth] Metadata discovered, registration_endpoint: " << mm.registrationEndpoint << std::endl;

    // 3) 获取 client 凭据
    std::cerr << "[OAuth] Step 3: Getting client credentials" << std::endl;
    std::string cid, csc;
    bool pr = false;
    if (!ctx.is_null() && ctx.is_object()) {
        cid = ctx.value("client_id","");
        csc = ctx.value("client_secret","");
        pr = !cid.empty();
    }
    std::cerr << "[OAuth] Client credentials from context: cid='" << cid << "', pr=" << pr << std::endl;
    std::cerr << "[OAuth] Registration endpoint: " << mm.registrationEndpoint << std::endl;
    if(pr){
        std::cerr << "[OAuth] Skipping registration: cid.empty()=" << cid.empty() << ", registrationEndpoint.empty()=" << mm.registrationEndpoint.empty() << std::endl;
    }else if(!mm.registrationEndpoint.empty()){
        std::cerr << "[OAuth] Registering client at " << mm.registrationEndpoint << std::endl;
        nlohmann::json rr={{"client_name","mcp-qt-client"},{"grant_types",{"authorization_code","refresh_token"}},{"redirect_uris",{redirectUri.toStdString()}},{"response_types",{"code"}},{"token_endpoint_auth_method","none"}};
        QByteArray rb=_post(QString::fromStdString(mm.registrationEndpoint),QByteArray::fromStdString(rr.dump()),"application/json");
        std::cerr << "[OAuth] Registration response: " << rb.toStdString().substr(0, 200) << std::endl;
        auto rj=nlohmann::json::parse(rb.toStdString(),nullptr,false);if(rj.is_discarded()||!rj.contains("client_id")){std::cerr << "[OAuth] Registration failed" << std::endl;return false;}
        cid=rj["client_id"].get<std::string>();csc=rj.value("client_secret","");
        std::cerr << "[OAuth] Registered client_id: " << cid << std::endl;
    }else{
        std::cerr << "[OAuth] No registration endpoint, using default client_id" << std::endl;
        cid = cidMetaSupp ? redirectUri.toStdString() : "mcp-qt-client";
    }

    // 4) JWT-Bearer
    for(const auto& gt:mm.grantTypesSupported)if(gt=="urn:ietf:params:oauth:grant-type:jwt-bearer"&&ctx.contains("idp_id_token")&&!ctx["idp_id_token"].get<std::string>().empty()){
        std::string a=ctx["idp_id_token"].get<std::string>();
        QByteArray pf=QByteArray("grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion=")+QByteArray::fromStdString(a)+"&resource="+QUrl::toPercentEncoding(QString::fromStdString(sseUrl));
        QMap<QByteArray,QByteArray> hdrs;
        if(!csc.empty()){hdrs["Authorization"]="Basic "+QByteArray::fromStdString(cid+":"+csc).toBase64();}
        QByteArray resp=_postH(QString::fromStdString(mm.tokenEndpoint),pf,hdrs);
        auto tj=nlohmann::json::parse(resp.toStdString(),nullptr,false);if(!tj.is_discarded()&&tj.contains("access_token")){mcp::OAuthToken t;t.accessToken=tj["access_token"].get<std::string>();oc->setCurrentToken(t);return true;}return false;
    }

    // 5) Client Credentials（当 context 有 client_id 且支持 client_credentials grant 时）
    bool hasPrivateKey = false;
    if(!ctx.is_null() && ctx.is_object()){
        hasPrivateKey = ctx.contains("private_key_pem") && ctx["private_key_pem"].is_string() && !ctx["private_key_pem"].get<std::string>().empty();
    }
    bool supportsCC = mm.grantTypesSupported.end() != std::find(mm.grantTypesSupported.begin(), mm.grantTypesSupported.end(), "client_credentials");
    bool isCC = !cid.empty() && (hasPrivateKey || !csc.empty()) && supportsCC;
    std::cerr << "[OAuth] Client Credentials check: ctx.is_null()=" << ctx.is_null() << ", ctx.is_object()=" << (ctx.is_null() ? false : ctx.is_object()) << ", hasPrivateKey=" << hasPrivateKey << ", csc.empty()=" << csc.empty() << ", supportsCC=" << supportsCC << ", isCC=" << isCC << std::endl;
    if(!ctx.is_null() && ctx.is_object()){
        std::cerr << "[OAuth] Context keys: ";
        for(auto it = ctx.begin(); it != ctx.end(); ++it) std::cerr << it.key() << " ";
        std::cerr << std::endl;
    }
    if(isCC){
        std::cerr << "[OAuth] Using Client Credentials grant" << std::endl;
        if(hasPrivateKey){
            // JWT-Bearer grant type
            std::string signingAlg = ctx.value("signing_algorithm","ES256");
            std::cerr << "[OAuth] Using JWT-Bearer with " << signingAlg << std::endl;
            // TODO: 生成 JWT assertion
            // 目前先尝试 client_credentials
        }
        QByteArray pf=QByteArray::fromStdString("grant_type=client_credentials&client_id="+cid+"&client_secret="+csc)+"&resource="+QUrl::toPercentEncoding(QString::fromStdString(sseUrl));
        QMap<QByteArray,QByteArray> hdrs;
        if(!csc.empty()){hdrs["Authorization"]="Basic "+QByteArray::fromStdString(cid+":"+csc).toBase64();}
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
McpQtClientBuilder& McpQtClientBuilder::setEnvironment(const QMap<QString, QString>& env) { m_env = env; return *this; }
McpQtClientBuilder& McpQtClientBuilder::setHttpHeaders(const QMap<QString, QString>& headers) { m_httpHeaders = headers; return *this; }
McpQtClientBuilder& McpQtClientBuilder::setHttpProxy(const QNetworkProxy& proxy) { m_proxy = proxy; return *this; }
McpQtClientBuilder& McpQtClientBuilder::setReconnectPolicy(const mcp::McpReconnectPolicy& policy) { m_reconnectPolicy = policy; return *this; }
McpQtClientBuilder& McpQtClientBuilder::setNamespace(const QString& ns) {
    m_namespace = ns;
    return *this;
}

McpQtClientBuilder& McpQtClientBuilder::setClientInfo(const QString& name, const QString& version) { m_clientName = name; m_clientVersion = version; return *this; }
McpQtClientBuilder& McpQtClientBuilder::setTimeout(int ms) { m_timeoutMs = ms; return *this; }

std::shared_ptr<McpQtClient> McpQtClientBuilder::buildAndConnectAndWait(QString* errorString) {
    auto c = std::shared_ptr<McpQtClient>(new McpQtClient());
    c->m_namespace = m_namespace;
    c->m_transportType = m_transportType;
    c->m_url_or_cmd = m_url_or_cmd;
    c->m_args = m_args;
    c->m_env = m_env;
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

        if (!c->connectToTransportAndWait(t, m_clientName, m_clientVersion, m_timeoutMs, errorString)) {
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

        if (!c->connectToTransportAndWait(t, m_clientName, m_clientVersion, m_timeoutMs, errorString)) {
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
        if (!m_env.isEmpty()) {
            std::unordered_map<std::string, std::string> envMap;
            for (auto it = m_env.constBegin(); it != m_env.constEnd(); ++it) {
                envMap[it.key().toStdString()] = it.value().toStdString();
            }
            t->setEnvironment(envMap);
        }
        if (!c->connectToTransportAndWait(t, m_clientName, m_clientVersion, m_timeoutMs, errorString)) {
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

McpQtClient::McpQtClient(QObject* p)
    : QObject(p)
    , m_oauth(std::make_shared<mcp::McpOAuthClient>())
{
    connect(this, &McpQtClient::toolsChanged, this, [this](const std::vector<McpQtTool>& tools) {
        m_toolCache.clear();
        for (const auto& t : tools) {
            m_toolCache[t.name] = t;
        }
    });
}
McpQtClient::~McpQtClient() {
    // 先触发所有挂起的 fetchAllToolsAsync 回调，防止调用方永久等待
    fireAllPendingFetchCallbacks();
    close();
}

void McpQtClient::fireAllPendingFetchCallbacks() {
    std::unordered_map<uint64_t, std::function<void()>> pending;
    {
        std::lock_guard<std::mutex> lock(m_pendingFetchMutex);
        pending.swap(m_pendingFetchCallbacks);
    }
    for (auto& [id, cb] : pending) {
        cb();
    }
}

McpQtClient::Ptr McpQtClient::connectHttpAndWait(const QString& url,const QString& name,const QString& ver,int to, QString* err){
    auto c=Ptr(new McpQtClient());
    c->m_transportType = 1;
    c->m_url_or_cmd = url;
    c->m_clientName = name;
    c->m_clientVersion = ver;
    c->m_timeoutMs = to;
    // 使用无状态 HTTP 传输（兼容 SSE 和无状态服务器）
    auto t=std::make_shared<mcp_qt::QtStatelessHttpTransport>(url);
    const char* pVer = std::getenv("MCP_CONFORMANCE_PROTOCOL_VERSION");
    if (pVer) t->setProtocolVersion(pVer);
    if(!c->connectToTransportAndWait(t,name,ver,to,err))return nullptr;return c;
}
McpQtClient::Ptr McpQtClient::connectStdioAndWait(const QString& cmd,const QStringList& args,const QString& name,const QString& ver,int to, QString* err){
    auto c=Ptr(new McpQtClient());
    c->m_transportType = 2;
    c->m_url_or_cmd = cmd;
    c->m_args = args;
    c->m_clientName = name;
    c->m_clientVersion = ver;
    c->m_timeoutMs = to;
    std::vector<std::string> a;for(const auto& x:args)a.push_back(x.toStdString());
    auto t=std::make_shared<mcp_qt::QtProcessStdioTransport>(cmd.toStdString(),a);
    if(!c->connectToTransportAndWait(t,name,ver,to,err))return nullptr;return c;
}
McpQtClient::Ptr McpQtClient::connectWithOAuthAndWait(const OAuthConfig& oa,const QString& name,const QString& ver,int to){
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
    if(!c->connectToTransportAndWait(t,name,ver,to))return nullptr;return c;
}

bool McpQtClient::connectToTransportAndWait(std::shared_ptr<mcp::IMcpTransport> t,const QString& name,const QString& ver,int to, QString* err){
    setupTransportCommon(t);
    if(!m_session->start()){ if(err)*err="Failed to start transport"; emit errorOccurred(mcp_qt::McpError{-1, *err, QJsonObject{}}); return false; }
    return doInitializeAndWait(name,ver,to,err);
}

void McpQtClient::setClientCapabilities(const QJsonObject& caps) {
    m_clientCapabilities = _nl(caps);
}

template <typename Initiator>
bool McpQtClient::runSyncWithTimeout(Initiator&& initiator, int timeoutMs) {
    assertNotMainGuiThread();
    QEventLoop loop;
    struct SyncContext {
        QEventLoop* loopPtr{nullptr};
        bool completed{false};
        bool exited{false};
    };
    auto ctx = std::make_shared<SyncContext>();
    ctx->loopPtr = &loop;
    
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    
    auto safeQuit = [ctx]() {
        if (ctx->exited || !ctx->loopPtr) return;
        ctx->completed = true;
        ctx->loopPtr->quit();
    };
    
    int64_t idBefore = m_session ? m_session->getLastRequestId() : 0;
    
    initiator(safeQuit);
    
    if (timeoutMs > 0) {
        timer.start(timeoutMs);
    }
    loop.exec();
    ctx->exited = true;
    ctx->loopPtr = nullptr;
    
    if (!ctx->completed) {
        if (m_session) {
            int64_t idAfter = m_session->getLastRequestId();
            if (idAfter > idBefore) {
                m_session->cancelRequest(idAfter);
            }
        }
    }
    
    return ctx->completed;
}

void McpQtClient::setupTransportCommon(std::shared_ptr<mcp::IMcpTransport> t) {
    m_session = std::make_shared<mcp::McpClientSession>(t);
    m_session->init();
    
    nlohmann::json caps = m_clientCapabilities;
    if (caps.is_null() || caps.empty()) {
        caps = {
            {"tools", {{"listChanged", true}}},
            {"resources", {{"subscribe", true}, {"listChanged", true}}},
            {"prompts", {{"listChanged", true}}}
        };
    }
    m_session->registerCapabilities(caps);

    if (m_trafficLogger) {
        setTrafficLogger(m_trafficLogger);
    }
    m_session->setOnError([this](const std::string& err) {
        QString errStr = QString::fromStdString(err);
        QMetaObject::invokeMethod(this, [this, errStr]() {
            emit errorOccurred(mcp_qt::McpError{-1, errStr, QJsonObject{}});
            // stderr 已由 QtProcessStdioTransport::serverLog 信号单独处理，
            // 此处 onError 仅在真正的传输层故障时触发，可安全恢复重连
            handleTransportFailure();
        }, Qt::QueuedConnection);
    });

    // 如果底层是 Stdio 子进程传输，连接 serverLog 信号以接收服务端日志
    auto* processTransport = dynamic_cast<QtProcessStdioTransport*>(t.get());
    if (processTransport) {
        QObject::connect(processTransport, &QtProcessStdioTransport::serverLog, this, [](const QString& msg) {
            qDebug().noquote() << "[MCP Server Log]" << msg.trimmed();
        }, Qt::QueuedConnection);
    }
    m_session->setNotificationCallback([this](const std::string& method, const nlohmann::json& params) {
        QString methodStr = QString::fromStdString(method);
        emit notificationReceived(methodStr, _qj(params));
        
        if (methodStr == "notifications/tools/list_changed") {
            QMetaObject::invokeMethod(this, [this]() {
                refreshToolsCacheAsync();
            }, Qt::QueuedConnection);
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

    // Apply any capabilities that were registered before the session existed
    for (const auto& pc : m_pendingCapabilities) {
        m_session->registerCapabilities(_nl(QJsonObject{{pc.name, pc.config}}));
    }
    m_pendingCapabilities.clear();
}
bool McpQtClient::doInitializeAndWait(const QString& name,const QString& ver,int to, QString* err){
    auto initOkPtr = std::make_shared<bool>(false);
    bool ok = runSyncWithTimeout([initOkPtr, name, ver, this](auto quit) {
        m_session->initialize(name.toStdString(), ver.toStdString(), [initOkPtr, quit](bool success, const nlohmann::json&) {
            *initOkPtr = success;
            quit();
        });
    }, to);

    if(!ok || !(*initOkPtr)){ if(err)*err="Initialization timeout or failure"; emit errorOccurred(mcp_qt::McpError{-1, *err, QJsonObject{}}); return false; }
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

bool McpQtClient::runOAuthFlow(const std::string& serverUrl,
                               const nlohmann::json& context,
                               const std::string& wwwAuth,
                               std::shared_ptr<mcp::McpOAuthClient> oauthClient) {
    if (!oauthClient) return false;
    return _runOAuthQt(serverUrl, context, wwwAuth, oauthClient);
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
QString McpQtClient::stripNamespace(const QString& name) const {
    if (!m_namespace.isEmpty() && name.startsWith(m_namespace + QStringLiteral("__"))) {
        return name.mid(m_namespace.length() + 2);
    }
    return name;
}

std::vector<McpQtTool> McpQtClient::listTools(int to){
    if (!m_session) return {};
    auto result = std::make_shared<std::vector<mcp::McpTool>>();
    runSyncWithTimeout([&](const std::function<void()>& quit) {
        m_session->listTools("", [result, quit](const std::vector<mcp::McpTool>& tools, const std::string& nextCursor, const nlohmann::json& error) {
            *result = tools;
            quit();
        });
    }, to);
    auto cvtRes = _cvt(*result);
    for(const auto& t : cvtRes) m_toolCache[t.name] = t;
    return cvtRes;
}

std::vector<McpQtTool> McpQtClient::listTools(const QString& c,QString* n,int to){
    if (!m_session) return {};
    auto result = std::make_shared<std::vector<mcp::McpTool>>();
    auto ns = std::make_shared<std::string>();
    runSyncWithTimeout([&](const std::function<void()>& quit) {
        m_session->listTools(c.toStdString(), [result, ns, quit](const std::vector<mcp::McpTool>& tools, const std::string& nextCursor, const nlohmann::json& error) {
            *result = tools;
            *ns = nextCursor;
            quit();
        });
    }, to);
    if (n) *n = QString::fromStdString(*ns);
    auto res = _cvt(*result);
    for(const auto& t : res) m_toolCache[t.name] = t;
    return res;
}
std::vector<McpQtTool> McpQtClient::fetchAllTools(int to) {
    std::vector<McpQtTool> all; QString c;
    do { QString nc; auto r=listTools(c,&nc,to); all.insert(all.end(),r.begin(),r.end()); c=nc; } while(!c.isEmpty());
    return all;
}

std::vector<McpQtTool> McpQtClient::cachedTools() const {
    std::vector<McpQtTool> tools;
    tools.reserve(m_toolCache.size());
    for (auto it = m_toolCache.begin(); it != m_toolCache.end(); ++it) {
        tools.push_back(it->second);
    }
    return tools;
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
            for(const auto& t : res) m_toolCache[t.name] = t;
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

bool McpQtClient::validateToolArguments(const QString& name, const QJsonObject& arguments, QString* errorString) const {
    QString actualName = stripNamespace(name);
    auto it = m_toolCache.find(actualName);
    if (it == m_toolCache.end()) return true;
    QJsonObject schema = it->second.inputSchema;
    
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
    QString actualName = stripNamespace(nm);
    emit toolCalled(nm, a);
    QString errStr;
    if(!validateToolArguments(nm, a, &errStr)) {
        McpResult r{true, {}, errStr, {}};
        emit toolFinished(nm, r);
        return r;
    }
    if(!m_session) {
        McpResult r{true, {}, QStringLiteral("No session"), {}};
        emit toolFinished(nm, r);
        return r;
    }
    auto resultData = std::make_shared<nlohmann::json>();
    auto errorData = std::make_shared<nlohmann::json>();
    bool ok = runSyncWithTimeout([&](const std::function<void()>& quit) {
        m_session->callTool(actualName.toStdString(), _nl(a), [resultData, errorData, quit](const nlohmann::json& result, const nlohmann::json& error) {
            *resultData = result;
            *errorData = error;
            quit();
        });
    }, to);
    if (!ok) {
        McpResult r{true, {}, QStringLiteral("Timeout"), {}};
        emit toolFinished(nm, r);
        return r;
    }
    bool isErr = !errorData->empty();
    QJsonObject data = _qj(*resultData);
    QString errorMsg = isErr ? _qj(*errorData).value("message").toString() : QString();
    QList<McpContent> contents;
    if (!isErr) {
        bool decodingError = false;
        contents = parseMcpContents(data, decodingError);
        if (decodingError) {
            isErr = true;
            errorMsg = QStringLiteral("Base64 decoding failed for multimedia contents");
        }
    }
    McpResult r{isErr, data, errorMsg, contents};
    emit toolFinished(nm, r);
    return r;
}
McpResult McpQtClient::callTool(const QString& nm,const QJsonObject& a,ProgressCallback onP,int to){
    QString actualName = stripNamespace(nm);
    emit toolCalled(nm, a);
    QString errStr;
    if(!validateToolArguments(nm, a, &errStr)) {
        McpResult r{true, {}, errStr, {}};
        emit toolFinished(nm, r);
        return r;
    }
    if(!m_session) {
        McpResult r{true, {}, QStringLiteral("No session"), {}};
        emit toolFinished(nm, r);
        return r;
    }
    auto resultData = std::make_shared<nlohmann::json>();
    auto errorData = std::make_shared<nlohmann::json>();
    auto pf = [this, nm, onP](const nlohmann::json& pi) {
        float p = pi.value("progress", 0.0f), t = pi.value("total", 0.0f);
        QString msg = QString::fromStdString(pi.value("message", ""));
        QMetaObject::invokeMethod(this, [this, nm, p, t, msg]() {
            emit progressReported(nm, p, t, msg);
        }, Qt::QueuedConnection);
        if (onP) {
            onP(p, t, msg);
        }
    };
    bool ok = runSyncWithTimeout([&](const std::function<void()>& quit) {
        m_session->callTool(actualName.toStdString(), _nl(a), [resultData, errorData, quit](const nlohmann::json& result, const nlohmann::json& error) {
            *resultData = result;
            *errorData = error;
            quit();
        }, pf);
    }, to);
    if (!ok) {
        McpResult r{true, {}, QStringLiteral("Timeout"), {}};
        emit toolFinished(nm, r);
        return r;
    }
    bool isErr = !errorData->empty();
    QJsonObject data = _qj(*resultData);
    QString errorMsg = isErr ? _qj(*errorData).value("message").toString() : QString();
    QList<McpContent> contents;
    if (!isErr) {
        bool decodingError = false;
        contents = parseMcpContents(data, decodingError);
        if (decodingError) {
            isErr = true;
            errorMsg = QStringLiteral("Base64 decoding failed for multimedia contents");
        }
    }
    McpResult r{isErr, data, errorMsg, contents};
    emit toolFinished(nm, r);
    return r;
}
void McpQtClient::callToolAsync(const QString& nm, const QJsonObject& a, std::function<void(McpResult)> cb, ProgressCallback onP) {
    QString actualName = stripNamespace(nm);
    callToolAsync(nm, a, this, cb, onP);
}
void McpQtClient::callToolAsync(const QString& nm, const QJsonObject& a, QObject* ctx, std::function<void(McpResult)> cb, ProgressCallback onP) {
    QString actualName = stripNamespace(nm);
    emit toolCalled(nm, a);
    QString errStr;
    if(!validateToolArguments(nm, a, &errStr)) {
        McpResult res{true, {}, errStr, {}};
        emit toolFinished(nm, res);
        if(ctx) {
            QPointer<QObject> pCtx(ctx);
            if (pCtx) {
                QMetaObject::invokeMethod(pCtx.data(), [cb, res](){ cb(res); }, Qt::QueuedConnection);
            }
        }
        else { cb(res); }
        return;
    }

    auto internalOnP = [this, nm, onP](float p, float t, const QString& msg) {
        QMetaObject::invokeMethod(this, [this, nm, p, t, msg]() {
            emit progressReported(nm, p, t, msg);
        }, Qt::QueuedConnection);
        if (onP) {
            onP(p, t, msg);
        }
    };

    sendRequest("tools/call", QJsonObject{{"name",nm},{"arguments",a}}, ctx, [this, nm, cb](const QJsonObject& r, const QJsonObject& e) {
        bool isErr = !e.isEmpty();
        QString errorMsg = isErr ? e.value("message").toString() : QString();
        QList<McpContent> contents;
        if (!isErr) {
            bool decodingError = false;
            contents = parseMcpContents(r, decodingError);
            if (decodingError) {
                isErr = true;
                errorMsg = QStringLiteral("Base64 decoding failed for multimedia contents");
            }
        }
        McpResult res{isErr, r, errorMsg, contents};
        emit toolFinished(nm, res);
        cb(res);
    }, internalOnP);
}
QFuture<McpResult> McpQtClient::callToolFuture(const QString& name, const QJsonObject& arguments) {
    auto promise = std::make_shared<QPromise<McpResult>>();
    promise->start();
    auto guard = std::make_shared<PromiseGuard>();
    guard->promise = promise;
    
    callToolAsync(name, arguments, this, [promise, guard](McpResult res) {
        promise->addResult(res);
        promise->finish();
        guard->finished = true;
    });
    return promise->future();
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
    QString actualName = stripNamespace(nm);
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

// ========== 工具定义导出为 LLM 格式 ==========

static QJsonObject ensureValidSchema(const QJsonObject& schema) {
    if (schema.isEmpty() || !schema.contains(QStringLiteral("type"))) {
        QJsonObject fallbackSchema;
        fallbackSchema[QStringLiteral("type")] = QStringLiteral("object");
        fallbackSchema[QStringLiteral("properties")] = QJsonObject{};
        return fallbackSchema;
    }
    return schema;
}

QJsonObject McpQtClient::exportToolToLlmFormat(const McpQtTool& tool, LlmFormat format, const QString& prefix) {
    QJsonObject result;
    QJsonObject validSchema = ensureValidSchema(tool.inputSchema);
    QString exportName = tool.name;
    if (!prefix.isEmpty()) {
        exportName = prefix + QStringLiteral("__") + tool.name;
    }

    if (format == LlmFormat::OpenAI) {
        QJsonObject functionObj;
        functionObj[QStringLiteral("name")] = exportName;
        if (!tool.description.isEmpty()) {
            functionObj[QStringLiteral("description")] = tool.description;
        }
        functionObj[QStringLiteral("parameters")] = validSchema;
        result[QStringLiteral("type")] = QStringLiteral("function");
        result[QStringLiteral("function")] = functionObj;
    } else if (format == LlmFormat::Anthropic) {
        result[QStringLiteral("name")] = exportName;
        if (!tool.description.isEmpty()) {
            result[QStringLiteral("description")] = tool.description;
        }
        result[QStringLiteral("input_schema")] = validSchema;
    } else if (format == LlmFormat::Gemini) {
        result[QStringLiteral("name")] = exportName;
        if (!tool.description.isEmpty()) {
            result[QStringLiteral("description")] = tool.description;
        }
        result[QStringLiteral("parameters")] = validSchema;
    }
    return result;
}

QJsonObject McpQtClient::exportToolToLlmFormat(const QString& name, LlmFormat format) const {
    auto it = m_toolCache.find(stripNamespace(name));
    if (it == m_toolCache.end()) {
        return QJsonObject{};
    }
    return exportToolToLlmFormat(it->second, format, m_namespace);
}

QJsonArray McpQtClient::exportAllToolsToLlmFormat(LlmFormat format) const {
    QJsonArray arr;
    for (auto it = m_toolCache.begin(); it != m_toolCache.end(); ++it) {
        arr.append(exportToolToLlmFormat(it->second, format, m_namespace));
    }
    return arr;
}

QJsonArray McpQtClient::exportAllToolsAsMcpSchema() const {
    QJsonArray arr;
    for (auto it = m_toolCache.begin(); it != m_toolCache.end(); ++it) {
        QJsonObject obj;
        QString exportName = it->second.name;
        if (!m_namespace.isEmpty()) {
            exportName = m_namespace + QStringLiteral("__") + exportName;
        }
        obj[QStringLiteral("name")] = exportName;
        if (!it->second.description.isEmpty()) {
            obj[QStringLiteral("description")] = it->second.description;
        }
        obj[QStringLiteral("inputSchema")] = ensureValidSchema(it->second.inputSchema);
        arr.append(obj);
    }
    return arr;
}

// ========== 并发多工具调用 ==========

void McpQtClient::callToolsConcurrentAsync(const std::vector<McpBatchCallRequest>& requests,
                                           std::function<void(const std::vector<McpBatchCallResult>&)> callback,
                                           int timeoutMs) {
    if (requests.empty()) {
        if (callback) {
            QMetaObject::invokeMethod(this, [callback]() { callback({}); }, Qt::QueuedConnection);
        }
        return;
    }

    struct BatchContext {
        std::mutex mutex;
        size_t pending;
        std::vector<McpBatchCallResult> results;
        std::function<void(const std::vector<McpBatchCallResult>&)> callback;
        bool finished = false;
        QTimer* timer = nullptr;
        std::vector<int64_t> requestIds;
    };

    auto ctx = std::make_shared<BatchContext>();
    ctx->pending = requests.size();
    ctx->results.resize(requests.size());
    ctx->callback = callback;

    for (size_t i = 0; i < requests.size(); ++i) {
        ctx->results[i].name = requests[i].name;
        ctx->results[i].arguments = requests[i].arguments;
    }

    // 设置整体超时机制
    if (timeoutMs > 0) {
        ctx->timer = new QTimer(this);
        ctx->timer->setSingleShot(true);
        QObject::connect(ctx->timer, &QTimer::timeout, this, [this, ctx]() {
            std::function<void(const std::vector<McpBatchCallResult>&)> cb;
            std::vector<McpBatchCallResult> res;
            std::vector<int64_t> idsToCancel;
            {
                std::lock_guard<std::mutex> lock(ctx->mutex);
                if (ctx->finished) return;
                ctx->finished = true;
                
                // 将所有未完成的任务置为超时状态
                for (auto& item : ctx->results) {
                    if (item.result.errorString.isEmpty() && item.result.data.isEmpty() && !item.result.isError) {
                        item.result.isError = true;
                        item.result.errorString = QStringLiteral("Timeout");
                        emit toolFinished(item.name, item.result);
                    }
                }
                res = ctx->results;
                cb = ctx->callback;
                idsToCancel = ctx->requestIds;
            }

            // 取消底层的请求以释放连接与并发资源
            for (int64_t id : idsToCancel) {
                if (id != -1) {
                    cancelRequest(id);
                }
            }

            if (ctx->timer) {
                ctx->timer->deleteLater();
                ctx->timer = nullptr;
            }

            if (cb) cb(res);
        });
        ctx->timer->start(timeoutMs);
    }

    // 并发启动每一个子任务
    for (size_t i = 0; i < requests.size(); ++i) {
        const auto& req = requests[i];
        
        emit toolCalled(req.name, req.arguments);

        // 1. 发起请求前加锁检查是否已经结束
        {
            std::lock_guard<std::mutex> lock(ctx->mutex);
            if (ctx->finished) {
                break;
            }
        }

        // 本地参数校验
        QString errStr;
        if (!validateToolArguments(req.name, req.arguments, &errStr)) {
            std::lock_guard<std::mutex> lock(ctx->mutex);
            McpResult localRes{true, {}, errStr, {}};
            emit toolFinished(req.name, localRes);
            ctx->results[i].result = localRes;
            ctx->pending--;
            if (ctx->pending == 0 && !ctx->finished) {
                ctx->finished = true;
                if (ctx->timer) {
                    ctx->timer->stop();
                    ctx->timer->deleteLater();
                    ctx->timer = nullptr;
                }
                auto cb = ctx->callback;
                auto res = ctx->results;
                QMetaObject::invokeMethod(this, [cb, res]() { if (cb) cb(res); }, Qt::QueuedConnection);
            }
            continue;
        }

        // 定义回调处理器
        auto cbWrapper = [this, ctx, i, req](McpResult res) {
            emit toolFinished(req.name, res);
            std::function<void(const std::vector<McpBatchCallResult>&)> cb;
            std::vector<McpBatchCallResult> finalResults;
            {
                std::lock_guard<std::mutex> lock(ctx->mutex);
                if (ctx->finished) return;
                
                ctx->results[i].result = res;
                ctx->pending--;
                
                if (ctx->pending == 0) {
                    ctx->finished = true;
                    if (ctx->timer) {
                        ctx->timer->stop();
                        ctx->timer->deleteLater();
                        ctx->timer = nullptr;
                    }
                    cb = ctx->callback;
                    finalResults = ctx->results;
                }
            }
            if (cb) {
                cb(finalResults);
            }
        };

        // 异步发起请求
        QString actualName = stripNamespace(req.name);
        int64_t reqId = sendRequest(
            QStringLiteral("tools/call"), 
            QJsonObject{{QStringLiteral("name"), actualName}, {QStringLiteral("arguments"), req.arguments}}, 
            this,
            [cbWrapper](const QJsonObject& r, const QJsonObject& e) {
                bool isErr = !e.isEmpty();
                QString errorMsg = isErr ? e.value(QStringLiteral("message")).toString() : QString();
                QList<McpContent> contents;
                if (!isErr) {
                    bool decodingError = false;
                    contents = parseMcpContents(r, decodingError);
                    if (decodingError) {
                        isErr = true;
                        errorMsg = QStringLiteral("Base64 decoding failed for multimedia contents");
                    }
                }
                cbWrapper({isErr, r, errorMsg, contents});
            }
        );

        // 2. 判定超时是否在此期间已发生，防范竞态条件和资源泄漏
        bool needCancel = false;
        {
            std::lock_guard<std::mutex> lock(ctx->mutex);
            if (!ctx->finished) {
                ctx->requestIds.push_back(reqId);
            } else {
                needCancel = true;
            }
        }
        if (needCancel && reqId != -1) {
            cancelRequest(reqId);
        }
    }
}

std::vector<McpBatchCallResult> McpQtClient::callToolsConcurrent(const std::vector<McpBatchCallRequest>& requests,
                                                                 int timeoutMs) {
    std::vector<McpBatchCallResult> output;
    QEventLoop loop;
    callToolsConcurrentAsync(requests, [&output, &loop](const std::vector<McpBatchCallResult>& res) {
        output = res;
        // 跨线程安全地唤醒运行在工作线程栈中的 QEventLoop
        QMetaObject::invokeMethod(&loop, "quit", Qt::QueuedConnection);
    }, timeoutMs);
    loop.exec();
    return output;
}

// ========== Resources ==========

QJsonObject McpQtClient::listResources(int to){
    return listResources(QString(), nullptr, to);
}
QJsonObject McpQtClient::listResources(const QString& c,QString* n,int to){
    if (!m_session) return {};
    auto resultData = std::make_shared<nlohmann::json>();
    auto ns = std::make_shared<std::string>();
    runSyncWithTimeout([&](const std::function<void()>& quit) {
        m_session->listResources(c.toStdString(), [resultData, ns, quit](const nlohmann::json& result, const std::string& nextCursor, const nlohmann::json& error) {
            *resultData = result;
            *ns = nextCursor;
            quit();
        });
    }, to);
    if (n) *n = QString::fromStdString(*ns);
    return _qj(*resultData);
}
QJsonObject McpQtClient::fetchAllResources(int to) {
    QJsonObject all;
    QJsonArray arr;
    QString c;
    do {
        QString nc;
        auto r = listResources(c, &nc, to);
        QJsonArray ta = r.value("resources").toArray();
        for (const auto& x : ta) {
            arr.append(x);
        }
        c = nc;
    } while (!c.isEmpty());
    all["resources"] = arr;
    return all;
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

QJsonObject McpQtClient::readResource(const QString& u,int to){
    if (!m_session) return {};
    auto resultData = std::make_shared<nlohmann::json>();
    runSyncWithTimeout([&](const std::function<void()>& quit) {
        m_session->readResource(u.toStdString(), [resultData, quit](const nlohmann::json& result, const nlohmann::json& error) {
            *resultData = result;
            quit();
        });
    }, to);
    return _qj(*resultData);
}

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
    bool ok = false;
    runSyncWithTimeout([&](const std::function<void()>& quit) {
        m_session->subscribeResource(u.toStdString(), [&](bool success, const nlohmann::json& error) {
            ok = success;
            quit();
        });
    }, to);
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
    if(!m_session) return false;
    bool ok = false;
    runSyncWithTimeout([&](const std::function<void()>& quit) {
        m_session->unsubscribeResource(u.toStdString(), [&](bool success, const nlohmann::json& error) {
            ok = success;
            quit();
        });
    }, to);
    return ok;
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
    bool ok = false;
    runSyncWithTimeout([&](const std::function<void()>& quit) {
        m_session->subscribeResource(uri.toStdString(), [&](bool success, const nlohmann::json& error) {
            ok = success;
            quit();
        });
    }, to);
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
    assertNotMainGuiThread();
    m_resourceRouter.unsubscribe(uri, routerToken);
    if (!m_resourceRouter.hasSubscribers(uri)) {
        if(!m_session) return false;
        auto okPtr = std::make_shared<bool>(false);
        runSyncWithTimeout([okPtr, uri, this](auto quit) {
            m_session->unsubscribeResource(uri.toStdString(), [okPtr, quit](bool success, const nlohmann::json&) {
                *okPtr = success;
                quit();
            });
        }, to);
        return *okPtr;
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
std::vector<mcp::McpResourceTemplate> McpQtClient::listResourceTemplates(int to){
    return listResourceTemplates(QString(), nullptr, to);
}
std::vector<mcp::McpResourceTemplate> McpQtClient::listResourceTemplates(const QString& c,QString* n,int to){
    if (!m_session) return {};
    auto resultData = std::make_shared<std::vector<mcp::McpResourceTemplate>>();
    auto ns = std::make_shared<std::string>();
    runSyncWithTimeout([&](const std::function<void()>& quit) {
        m_session->listResourceTemplates(c.toStdString(), [resultData, ns, quit](const std::vector<mcp::McpResourceTemplate>& templates, const std::string& nextCursor, const nlohmann::json& error) {
            *resultData = templates;
            *ns = nextCursor;
            quit();
        });
    }, to);
    if (n) *n = QString::fromStdString(*ns);
    return *resultData;
}
std::vector<mcp::McpResourceTemplate> McpQtClient::fetchAllResourceTemplates(int to) {
    std::vector<mcp::McpResourceTemplate> all;
    QString c;
    do {
        QString nc;
        auto r = listResourceTemplates(c, &nc, to);
        all.insert(all.end(), r.begin(), r.end());
        c = nc;
    } while (!c.isEmpty());
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
QJsonObject McpQtClient::listPrompts(int to){
    return listPrompts(QString(), nullptr, to);
}
QJsonObject McpQtClient::listPrompts(const QString& c,QString* n,int to){
    if (!m_session) return {};
    auto resultData = std::make_shared<nlohmann::json>();
    auto ns = std::make_shared<std::string>();
    runSyncWithTimeout([&](const std::function<void()>& quit) {
        m_session->listPrompts(c.toStdString(), [resultData, ns, quit](const nlohmann::json& result, const std::string& nextCursor, const nlohmann::json& error) {
            *resultData = result;
            *ns = nextCursor;
            quit();
        });
    }, to);
    if (n) *n = QString::fromStdString(*ns);
    return _qj(*resultData);
}
QJsonObject McpQtClient::fetchAllPrompts(int to) {
    QJsonObject all;
    QJsonArray arr;
    QString c;
    do {
        QString nc;
        auto r = listPrompts(c, &nc, to);
        QJsonArray ta = r.value("prompts").toArray();
        for (const auto& x : ta) {
            arr.append(x);
        }
        c = nc;
    } while (!c.isEmpty());
    all["prompts"] = arr;
    return all;
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

QJsonObject McpQtClient::getPrompt(const QString& nm,const QJsonObject& a,int to){
    QString actualName = stripNamespace(nm);
    if (!m_session) return {};
    auto resultData = std::make_shared<nlohmann::json>();
    runSyncWithTimeout([&](const std::function<void()>& quit) {
        m_session->getPrompt(actualName.toStdString(), _nl(a), [resultData, quit](const nlohmann::json& result, const nlohmann::json& error) {
            *resultData = result;
            quit();
        });
    }, to);
    return _qj(*resultData);
}

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
bool McpQtClient::ping(int to){
    if (!m_session) return false;
    auto ok = std::make_shared<bool>(false);
    runSyncWithTimeout([&](const std::function<void()>& quit) {
        m_session->ping([ok, quit](bool success, const nlohmann::json& error) {
            *ok = success;
            quit();
        });
    }, to);
    return *ok;
}

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

QJsonObject McpQtClient::complete(const QJsonObject& rf,const QJsonObject& ag,int to){
    if (!m_session) return {};
    auto resultData = std::make_shared<nlohmann::json>();
    runSyncWithTimeout([&](const std::function<void()>& quit) {
        m_session->complete(_nl(rf), _nl(ag), [resultData, quit](const nlohmann::json& result, const nlohmann::json& error) {
            *resultData = result;
            quit();
        });
    }, to);
    return _qj(*resultData);
}

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
bool McpQtClient::setLoggingLevel(const QString& lv,int to){
    if(!m_session) return false;
    auto errorData = std::make_shared<nlohmann::json>();
    runSyncWithTimeout([&](const std::function<void()>& quit) {
        m_session->callTool("logging/setLevel", _nl(QJsonObject{{"level",lv}}), [errorData, quit](const nlohmann::json& result, const nlohmann::json& error) {
            *errorData = error;
            quit();
        });
    }, to);
    return errorData->empty();
}

// ========== 双向能力 ==========
void McpQtClient::setElicitationHandler(ElicitationHandler h){
    setElicitationHandler(this, h);
}
void McpQtClient::setElicitationHandler(QObject* ctx, ElicitationHandler h){
    m_savedElicitationHandler = h;
    m_savedElicitationContext = QPointer<QObject>(ctx);
    m_hasSavedElicitationContext = (ctx != nullptr);
    if(!m_session) return;

    registerCapability("elicitation", QJsonObject{{"form", QJsonObject{{"applyDefaults", true}}}});
    
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
void McpQtClient::registerCapability(const QString& n,const QJsonObject& c){
    if(m_session){
        m_session->registerCapabilities(_nl(QJsonObject{{n,c}}));
    }else{
        m_pendingCapabilities.push_back({n, c});
    }
}

// ========== 生命周期 ==========
bool McpQtClient::isConnected()const{return m_session&&m_session->state()==mcp::SessionState::Initialized;}
void McpQtClient::close(int to){
    assertNotMainGuiThread();
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
        runSyncWithTimeout([this](auto quit) {
            m_session->shutdown([quit](bool) {
                quit();
            });
        }, to);

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
            auto t = std::make_shared<mcp_qt::QtProcessStdioTransport>(m_url_or_cmd.toStdString(), a);
            if (!m_env.isEmpty()) {
                std::unordered_map<std::string, std::string> envMap;
                for (auto it = m_env.constBegin(); it != m_env.constEnd(); ++it) {
                    envMap[it.key().toStdString()] = it.value().toStdString();
                }
                t->setEnvironment(envMap);
            }
            newTransport = t;
        }
    }

    if (!newTransport) {
        emit recoveryFailed(QStringLiteral("Auto-reconnect failed: no transport factory or configuration set"));
        m_inRecovery = false;
        return;
    }

    QString err;
    if (connectToTransportAndWait(newTransport, m_clientName, m_clientVersion, m_timeoutMs, &err)) {
        qInfo() << "[McpQtClient] Reconnect successful!";
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

    // 异步刷新工具缓存并发射变动信号
    refreshToolsCacheAsync();

    for (auto it = m_toolsModels.begin(); it != m_toolsModels.end();) {
        if (!*it) {
            it = m_toolsModels.erase(it);
            continue;
        }
        (*it)->refresh();
        ++it;
    }
}

void McpQtClient::refreshToolsCacheAsync() {
    constexpr int kMaxPages = 50;
    struct Helper {
        static void fetchPage(QPointer<McpQtClient> safeClient, QString cursor,
                              std::shared_ptr<std::vector<McpQtTool>> accumulated, int depth) {
            if (!safeClient) return;
            if (depth >= kMaxPages) {
                qWarning() << "[McpQtClient] refreshToolsCacheAsync: exceeded max pages (" << kMaxPages << "), aborting pagination";
                safeClient->m_toolCache.clear();
                for (const auto& t : *accumulated) {
                    safeClient->m_toolCache[t.name] = t;
                }
                emit safeClient->toolsChanged(*accumulated);
                return;
            }
            safeClient->listToolsAsync(cursor, [safeClient, accumulated, depth](const std::vector<McpQtTool>& tools, const QString& nextCursor, const QString& error) {
                if (!safeClient) return;
                if (!error.isEmpty()) {
                    return;
                }
                accumulated->insert(accumulated->end(), tools.begin(), tools.end());
                if (!nextCursor.isEmpty()) {
                    fetchPage(safeClient, nextCursor, accumulated, depth + 1);
                } else {
                    safeClient->m_toolCache.clear();
                    for (const auto& t : *accumulated) {
                        safeClient->m_toolCache[t.name] = t;
                    }
                    emit safeClient->toolsChanged(*accumulated);
                }
            });
        }
    };

    auto accumulated = std::make_shared<std::vector<McpQtTool>>();
    Helper::fetchPage(QPointer<McpQtClient>(this), QStringLiteral(""), accumulated, 0);
}

void McpQtClient::fetchAllToolsAsync() {
    constexpr int kMaxPages = 50;
    struct Helper {
        static void fetchPage(QPointer<McpQtClient> safeClient, QString cursor,
                              std::shared_ptr<std::vector<McpQtTool>> accumulated,
                              std::shared_ptr<std::once_flag> doneFlag,
                              uint64_t fetchId, int depth) {
            if (!safeClient) return;
            if (depth >= kMaxPages) {
                qWarning() << "[McpQtClient] fetchAllToolsAsync: exceeded max pages (" << kMaxPages << "), aborting pagination";
                fireDone(safeClient, accumulated, doneFlag, fetchId);
                return;
            }
            safeClient->listToolsAsync(cursor, [safeClient, accumulated, doneFlag, fetchId, depth](const std::vector<McpQtTool>& tools, const QString& nextCursor, const QString& error) {
                if (!safeClient) return;
                if (!error.isEmpty()) {
                    qWarning() << "[McpQtClient] fetchAllToolsAsync page error:" << error;
                    return;
                }
                accumulated->insert(accumulated->end(), tools.begin(), tools.end());
                if (!nextCursor.isEmpty()) {
                    fetchPage(safeClient, nextCursor, accumulated, doneFlag, fetchId, depth + 1);
                } else {
                    fireDone(safeClient, accumulated, doneFlag, fetchId);
                }
            });
        }

        static void fireDone(QPointer<McpQtClient> safeClient,
                             std::shared_ptr<std::vector<McpQtTool>> accumulated,
                             std::shared_ptr<std::once_flag> doneFlag,
                             uint64_t fetchId) {
            std::call_once(*doneFlag, [safeClient, accumulated, fetchId]() {
                if (safeClient) {
                    // 任务已完成，注销兜底回调，释放捕获的 accumulated 内存
                    {
                        std::lock_guard<std::mutex> lock(safeClient->m_pendingFetchMutex);
                        safeClient->m_pendingFetchCallbacks.erase(fetchId);
                    }
                    for (const auto& t : *accumulated) {
                        safeClient->m_toolCache[t.name] = t;
                    }
                    emit safeClient->toolsReady(*accumulated);
                    emit safeClient->toolsChanged(*accumulated);
                }
            });
        }
    };

    auto accumulated = std::make_shared<std::vector<McpQtTool>>();
    auto doneFlag = std::make_shared<std::once_flag>();
    uint64_t fetchId = m_nextFetchId.fetch_add(1);

    // 注册析构时兜底回调：客户端被销毁时强制触发
    auto safeThis = QPointer<McpQtClient>(this);
    auto rescueCb = [safeThis, accumulated, doneFlag, fetchId]() {
        Helper::fireDone(safeThis, accumulated, doneFlag, fetchId);
    };
    {
        std::lock_guard<std::mutex> lock(m_pendingFetchMutex);
        m_pendingFetchCallbacks[fetchId] = std::move(rescueCb);
    }

    Helper::fetchPage(QPointer<McpQtClient>(this), QStringLiteral(""), accumulated, doneFlag, fetchId, 0);
}

void McpQtClient::fetchAllToolsAsync(std::function<void(const std::vector<McpQtTool>&)> callback) {
    constexpr int kMaxPages = 50;
    struct Helper {
        static void fetchPage(QPointer<McpQtClient> safeClient, QString cursor,
                              std::shared_ptr<std::vector<McpQtTool>> accumulated,
                              std::shared_ptr<std::function<void(const std::vector<McpQtTool>&)>> cb,
                              std::shared_ptr<std::once_flag> doneFlag,
                              uint64_t fetchId, int depth) {
            if (!safeClient) return;
            if (depth >= kMaxPages) {
                qWarning() << "[McpQtClient] fetchAllToolsAsync: exceeded max pages (" << kMaxPages << "), aborting pagination";
                fireDone(safeClient, accumulated, cb, doneFlag, fetchId);
                return;
            }
            safeClient->listToolsAsync(cursor, [safeClient, accumulated, cb, doneFlag, fetchId, depth](const std::vector<McpQtTool>& tools, const QString& nextCursor, const QString& error) {
                if (!safeClient) return;
                if (!error.isEmpty()) {
                    qWarning() << "[McpQtClient] fetchAllToolsAsync page error:" << error;
                    fireDone(safeClient, accumulated, cb, doneFlag, fetchId);
                    return;
                }
                accumulated->insert(accumulated->end(), tools.begin(), tools.end());
                if (!nextCursor.isEmpty()) {
                    fetchPage(safeClient, nextCursor, accumulated, cb, doneFlag, fetchId, depth + 1);
                } else {
                    fireDone(safeClient, accumulated, cb, doneFlag, fetchId);
                }
            });
        }

        static void fireDone(QPointer<McpQtClient> safeClient,
                             std::shared_ptr<std::vector<McpQtTool>> accumulated,
                             std::shared_ptr<std::function<void(const std::vector<McpQtTool>&)>> cb,
                             std::shared_ptr<std::once_flag> doneFlag,
                             uint64_t fetchId) {
            std::call_once(*doneFlag, [safeClient, accumulated, cb, fetchId]() {
                if (safeClient) {
                    // 任务已完成，注销兜底回调，释放捕获的 accumulated 内存
                    {
                        std::lock_guard<std::mutex> lock(safeClient->m_pendingFetchMutex);
                        safeClient->m_pendingFetchCallbacks.erase(fetchId);
                    }
                    for (const auto& t : *accumulated) {
                        safeClient->m_toolCache[t.name] = t;
                    }
                    emit safeClient->toolsReady(*accumulated);
                    emit safeClient->toolsChanged(*accumulated);
                }
                if (*cb) (*cb)(*accumulated);
            });
        }
    };

    auto accumulated = std::make_shared<std::vector<McpQtTool>>();
    auto cb = std::make_shared<std::function<void(const std::vector<McpQtTool>&)>>(std::move(callback));
    auto doneFlag = std::make_shared<std::once_flag>();
    uint64_t fetchId = m_nextFetchId.fetch_add(1);

    // 注册析构时兜底回调：客户端被销毁时强制触发
    auto safeThis = QPointer<McpQtClient>(this);
    auto rescueCb = [safeThis, accumulated, cb, doneFlag, fetchId]() {
        Helper::fireDone(safeThis, accumulated, cb, doneFlag, fetchId);
    };
    {
        std::lock_guard<std::mutex> lock(m_pendingFetchMutex);
        m_pendingFetchCallbacks[fetchId] = std::move(rescueCb);
    }

    Helper::fetchPage(QPointer<McpQtClient>(this), QStringLiteral(""), accumulated, cb, doneFlag, fetchId, 0);
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


std::shared_ptr<McpQtClient> McpQtClientBuilder::buildAndConnectAsync() {
    auto c = std::shared_ptr<McpQtClient>(new McpQtClient());
    c->m_namespace = m_namespace;
    c->m_transportType = m_transportType;
    c->m_url_or_cmd = m_url_or_cmd;
    c->m_args = m_args;
    c->m_env = m_env;
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
        if (!m_env.isEmpty()) {
            std::unordered_map<std::string, std::string> envMap;
            for (auto it = m_env.constBegin(); it != m_env.constEnd(); ++it) {
                envMap[it.key().toStdString()] = it.value().toStdString();
            }
            t->setEnvironment(envMap);
        }
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
            emit errorOccurred(mcp_qt::McpError{-1, QStringLiteral("Failed to start transport"), QJsonObject{}});
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
                emit errorOccurred(mcp_qt::McpError{-1, errStr, QJsonObject{}});
            }, Qt::QueuedConnection);
        } else {
            QMetaObject::invokeMethod(this, [this]() {
                m_initialized = true;
                emit connected();
            }, Qt::QueuedConnection);
        }
    });
}

} // namespace mcp_qt

// ========== 同步 API ==========
// ... (以下代码保持不变)
