#include "mcp_core/McpOAuthClient.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <map>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <openssl/sha.h>
#endif

namespace mcp {

static std::string urlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped << std::hex;
    for (char c : value) {
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << std::setfill('0') << (int)(unsigned char)c;
        }
    }
    return escaped.str();
}

static std::string buildUrlEncodedBody(const json& j) {
    std::string result;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!result.empty()) result += "&";
        std::string valStr;
        if (it.value().is_string()) {
            valStr = it.value().get<std::string>();
        } else {
            valStr = it.value().dump();
        }
        result += urlEncode(it.key()) + "=" + urlEncode(valStr);
    }
    return result;
}

// ============================================================================
// URL query 解析辅助（RFC 9207 iss 校验用，文件内静态函数）
// ============================================================================

static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static std::string percentDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size()
                   && std::isxdigit((unsigned char)s[i + 1])
                   && std::isxdigit((unsigned char)s[i + 2])) {
            out += static_cast<char>((hexValue(s[i + 1]) << 4) | hexValue(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// 解析完整 URL（取 query 部分）或裸 query string，返回解码后的 key/value 映射。
static std::map<std::string, std::string> parseQueryString(const std::string& urlOrQuery) {
    std::map<std::string, std::string> result;
    std::string query = urlOrQuery;
    const size_t qmark = query.find('?');
    if (qmark != std::string::npos) query = query.substr(qmark + 1);
    const size_t hash = query.find('#');
    if (hash != std::string::npos) query = query.substr(0, hash);
    std::istringstream iss(query);
    std::string pair;
    while (std::getline(iss, pair, '&')) {
        if (pair.empty()) continue;
        const size_t eq = pair.find('=');
        const std::string key = eq == std::string::npos ? pair : pair.substr(0, eq);
        const std::string val = eq == std::string::npos ? "" : pair.substr(eq + 1);
        result[percentDecode(key)] = percentDecode(val);
    }
    return result;
}

static std::string getQueryParam(const std::string& urlOrQuery, const std::string& key) {
    const auto params = parseQueryString(urlOrQuery);
    const auto it = params.find(key);
    return it == params.end() ? std::string() : it->second;
}

// 构建 DCR（RFC 7591）注册请求 body；SEP-837 要求携带 application_type（desktop/CLI 用 "native"）。
static json buildRegistrationBody(const std::string& clientName,
                                  const std::vector<std::string>& redirectUris,
                                  const std::string& applicationType) {
    json body;
    body["client_name"] = clientName.empty() ? "mcp-qt-client" : clientName;
    body["application_type"] = applicationType.empty() ? "native" : applicationType;
    body["grant_types"] = json::array({"authorization_code", "refresh_token"});
    body["response_types"] = json::array({"code"});
    body["token_endpoint_auth_method"] = "none";
    json uris = json::array();
    for (const auto& u : redirectUris) uris.push_back(u);
    body["redirect_uris"] = uris;
    return body;
}

// Stubs for network functions (Qt handles network operations natively in _runOAuthQt)
std::string McpOAuthClient::httpGet(const std::string&) {
    return "";
}

std::string McpOAuthClient::httpPost(const std::string&, const std::string&, const std::string&) {
    return "";
}

McpOAuthClient::McpOAuthClient() {}
McpOAuthClient::~McpOAuthClient() {}

void McpOAuthClient::discoverMetadata(const std::string&, MetadataCallback callback) {
    if (callback) {
        callback(false, OAuthServerMetadata{}, "Network discovery is disabled in pure C++ core");
    }
}

bool McpOAuthClient::discoverMetadataSync(const std::string&, OAuthServerMetadata*, std::string* errorOut, std::chrono::milliseconds) {
    if (errorOut) *errorOut = "Network discovery is disabled in pure C++ core";
    return false;
}

void McpOAuthClient::registerClient(const std::string& registrationEndpoint, const std::string& clientName,
                                    const std::vector<std::string>& redirectUris, RegistrationCallback callback,
                                    const std::string& applicationType) {
    // SEP-837: DCR body 必须携带 application_type；当前 httpPost 为 stub（网络由 Qt 层原生处理），
    // 此处仅保留 body 构建逻辑，验证 application_type 进入请求体。
    const json body = buildRegistrationBody(clientName, redirectUris, applicationType);
    (void)httpPost(registrationEndpoint, body.dump());
    if (callback) {
        callback(false, OAuthClientRegistration{}, "Dynamic registration is disabled in pure C++ core");
    }
}

bool McpOAuthClient::registerClientSync(const std::string& registrationEndpoint, const std::string& clientName,
                                        const std::vector<std::string>& redirectUris, OAuthClientRegistration*,
                                        std::string* errorOut, std::chrono::milliseconds,
                                        const std::string& applicationType) {
    const json body = buildRegistrationBody(clientName, redirectUris, applicationType);
    (void)httpPost(registrationEndpoint, body.dump());
    if (errorOut) *errorOut = "Dynamic registration is disabled in pure C++ core";
    return false;
}

void McpOAuthClient::exchangeCode(const std::string&, const std::string&, const std::string&, const std::string&, const std::string&, const std::string&, TokenCallback callback, const std::string&, bool, const std::string&) {
    if (callback) {
        callback(false, OAuthToken{}, "Token exchange is disabled in pure C++ core");
    }
}

bool McpOAuthClient::exchangeCodeSync(const std::string&, const std::string&, const std::string&, const std::string&, const std::string&, const std::string&, OAuthToken*, std::string* errorOut, std::chrono::milliseconds, const std::string&, bool, const std::string&) {
    if (errorOut) *errorOut = "Token exchange is disabled in pure C++ core";
    return false;
}

void McpOAuthClient::refreshToken(const std::string&, const std::string&, const std::string&, const std::string&, TokenCallback callback, const std::string&) {
    if (callback) {
        callback(false, OAuthToken{}, "Token refresh is disabled in pure C++ core");
    }
}

bool McpOAuthClient::refreshTokenSync(const std::string&, const std::string&, const std::string&, const std::string&, OAuthToken*, std::string* errorOut, std::chrono::milliseconds, const std::string&) {
    if (errorOut) *errorOut = "Token refresh is disabled in pure C++ core";
    return false;
}

// Local helper implementation for PKCE & state generation
std::string McpOAuthClient::generateCodeVerifier() {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    std::string verifier;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, sizeof(chars) - 2);
    
    // Verifier length between 43 and 128
    for (int i = 0; i < 64; ++i) {
        verifier += chars[dis(gen)];
    }
    return verifier;
}

std::string McpOAuthClient::computeCodeChallenge(const std::string& verifier) {
    unsigned char hash[32];
#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0) {
        BCryptHash(hAlg, nullptr, 0, (PUCHAR)verifier.data(), (ULONG)verifier.size(), hash, 32);
        BCryptCloseAlgorithmProvider(hAlg, 0);
    }
#else
    SHA256(reinterpret_cast<const unsigned char*>(verifier.data()), verifier.size(), hash);
#endif

    // Base64Url encode the SHA256 hash (no padding, replace + with -, / with _)
    static const char* b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string challenge;
    int val = 0, valb = -6;
    for (unsigned char c : hash) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            challenge.push_back(b64chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) {
        challenge.push_back(b64chars[((val << 8) >> (valb + 8)) & 0x3F]);
    }
    return challenge;
}

std::string McpOAuthClient::generateState() {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::string state;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, sizeof(chars) - 2);
    for (int i = 0; i < 16; ++i) {
        state += chars[dis(gen)];
    }
    return state;
}

McpOAuthClient::AuthRequest McpOAuthClient::buildAuthorizationUrl(const OAuthServerMetadata& metadata,
                                                                 const std::string& clientId,
                                                                 const std::string& redirectUri,
                                                                 const std::vector<std::string>& scopes,
                                                                 const std::string& resource) {
    AuthRequest req;
    req.codeVerifier = generateCodeVerifier();
    req.codeChallenge = computeCodeChallenge(req.codeVerifier);
    req.state = generateState();

    std::string url = metadata.authorizationEndpoint;
    url += (url.find('?') == std::string::npos) ? "?" : "&";
    url += "response_type=code";
    url += "&client_id=" + urlEncode(clientId);
    if (!redirectUri.empty()) {
        url += "&redirect_uri=" + urlEncode(redirectUri);
    }
    url += "&state=" + urlEncode(req.state);
    url += "&code_challenge=" + urlEncode(req.codeChallenge);
    url += "&code_challenge_method=S256";

    if (!scopes.empty()) {
        std::string scopeStr;
        for (const auto& s : scopes) {
            if (!scopeStr.empty()) scopeStr += " ";
            scopeStr += s;
        }
        url += "&scope=" + urlEncode(scopeStr);
    }

    if (!resource.empty()) {
        url += "&resource=" + urlEncode(resource);
    }

    req.authorizationUrl = url;
    return req;
}

OAuthToken McpOAuthClient::getCurrentToken() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_currentToken;
}

void McpOAuthClient::setCurrentToken(const OAuthToken& token) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentToken = token;
    // 2026-09: token 带 issuer 时同步更新按 issuer 键控的持久化存储，
    // 避免 setCurrentToken / setStoredTokenForIssuer 双写不一致（旧 token 残留）。
    if (!token.issuer.empty()) {
        m_tokensByIssuer[token.issuer] = token;
    }
}

bool McpOAuthClient::hasValidToken() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_currentToken.accessToken.empty() && !m_currentToken.isExpired();
}

std::vector<std::string> McpOAuthClient::requestedScopes() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_requestedScopes;
}

void McpOAuthClient::recordRequestedScopes(const std::vector<std::string>& scopes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_requestedScopes = scopes;
}

void McpOAuthClient::clearRequestedScopes() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_requestedScopes.clear();
}

void McpOAuthClient::setStoredToken(const OAuthToken& token) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentToken = token;
    // SEP-2352: token 带 issuer 时同时按 issuer 键控持久化，供不同授权服务器隔离使用。
    if (!token.issuer.empty()) {
        m_tokensByIssuer[token.issuer] = token;
    }
}

bool McpOAuthClient::hasTokenForIssuer(const std::string& issuer) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_tokensByIssuer.find(issuer) != m_tokensByIssuer.end();
}

OAuthToken McpOAuthClient::getTokenForIssuer(const std::string& issuer) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_tokensByIssuer.find(issuer);
    return it == m_tokensByIssuer.end() ? OAuthToken{} : it->second;
}

void McpOAuthClient::setStoredTokenForIssuer(const std::string& issuer, const OAuthToken& token) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_tokensByIssuer[issuer] = token;
}

// ============================================================================
// 认证/刷新流程互斥（2026-09: 防止并发 401 触发多个 OAuth 流程互相覆盖 token）
// ============================================================================

bool McpOAuthClient::tryAcquireAuthFlow() {
    std::lock_guard<std::mutex> lock(m_authFlowMutex);
    if (m_authFlowActive) return false;
    m_authFlowActive = true;
    m_authFlowOwner = std::this_thread::get_id();
    return true;
}

void McpOAuthClient::releaseAuthFlow() {
    std::lock_guard<std::mutex> lock(m_authFlowMutex);
    m_authFlowActive = false;
    m_authFlowOwner = std::thread::id{};
}

bool McpOAuthClient::hasAuthFlowInProgress() const {
    std::lock_guard<std::mutex> lock(m_authFlowMutex);
    return m_authFlowActive;
}

bool McpOAuthClient::authFlowOwnerIsCurrentThread() const {
    std::lock_guard<std::mutex> lock(m_authFlowMutex);
    return m_authFlowActive && m_authFlowOwner == std::this_thread::get_id();
}

void McpOAuthClient::setLastTokenEndpoint(const std::string& ep) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lastTokenEndpoint = ep;
}

std::string McpOAuthClient::lastTokenEndpoint() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastTokenEndpoint;
}

bool McpOAuthClient::validateAuthorizationIss(const std::string& authorizationResponseUrlOrQuery,
                                              const std::string& expectedIssuer,
                                              std::string* error) const {
    // RFC 9207 (SEP-2468): authorization response 的 query 携带 iss 时必须与记录中的
    // 授权服务器 issuer 完全匹配；缺失 iss 视为不安全（此处按严格模式返回 false）。
    // 不依赖任何可变成员，线程安全。
    const auto params = parseQueryString(authorizationResponseUrlOrQuery);
    const auto it = params.find("iss");
    if (it == params.end()) {
        if (error) {
            *error = "authorization response does not contain 'iss' parameter (RFC 9207 requires it)";
        }
        return false;
    }
    if (it->second != expectedIssuer) {
        if (error) {
            *error = "authorization response 'iss' (" + it->second + ") does not match expected issuer (" + expectedIssuer + ")";
        }
        return false;
    }
    return true;
}

} // namespace mcp
