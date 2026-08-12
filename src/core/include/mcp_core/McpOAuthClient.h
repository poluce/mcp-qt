#pragma once
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <thread>
#include <chrono>
#include <map>
#include <nlohmann/json.hpp>

namespace mcp {

using json = nlohmann::json;

// 辅助函数：将 JSON 数组字段解析为 vector<string>
inline void parseJsonStringArray(const json& j, const char* key, std::vector<std::string>& out) {
    if (j.contains(key) && j[key].is_array())
        for (const auto& s : j[key]) out.push_back(s.get<std::string>());
}

/**
 * @brief OAuth 2.0 token response.
 */
struct OAuthToken {
    std::string accessToken;
    std::string refreshToken;
    std::string tokenType = "Bearer";
    int expiresIn = 0;
    std::string scope;
    /// 签发该 token 的授权服务器 issuer（SEP-2352：凭据绑定到签发它的授权服务器）。
    std::string issuer;
    std::chrono::steady_clock::time_point obtainedAt;

    bool isExpired() const {
        if (expiresIn <= 0) return false;
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - obtainedAt);
        return elapsed.count() >= expiresIn;
    }

    bool isExpiringSoon(int bufferSeconds = 30) const {
        if (expiresIn <= 0) return false;
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - obtainedAt);
        return elapsed.count() >= (expiresIn - bufferSeconds);
    }
};

/**
 * @brief OAuth 2.0 authorization server metadata (RFC 8414).
 */
struct OAuthServerMetadata {
    std::string issuer;
    std::string authorizationEndpoint;
    std::string tokenEndpoint;
    std::string registrationEndpoint;
    std::vector<std::string> scopesSupported;
    std::vector<std::string> responseTypesSupported;
    std::vector<std::string> grantTypesSupported;
    std::vector<std::string> codeChallengeMethodsSupported;
    std::vector<std::string> tokenEndpointAuthMethodsSupported;
    /// RFC 9207 / OIDC Discovery: 授权服务器是否支持 authorization response 携带 iss 参数
    bool authorizationResponseIssParameterSupported{false};

    static OAuthServerMetadata fromJson(const json& j) {
        OAuthServerMetadata m;
        m.issuer = j.value("issuer", "");
        m.authorizationEndpoint = j.value("authorization_endpoint", "");
        m.tokenEndpoint = j.value("token_endpoint", "");
        m.registrationEndpoint = j.value("registration_endpoint", "");
        parseJsonStringArray(j, "scopes_supported", m.scopesSupported);
        parseJsonStringArray(j, "response_types_supported", m.responseTypesSupported);
        parseJsonStringArray(j, "grant_types_supported", m.grantTypesSupported);
        parseJsonStringArray(j, "code_challenge_methods_supported", m.codeChallengeMethodsSupported);
        parseJsonStringArray(j, "token_endpoint_auth_methods_supported", m.tokenEndpointAuthMethodsSupported);
        m.authorizationResponseIssParameterSupported =
            j.value("authorization_response_iss_parameter_supported", false);
        return m;
    }
};

/**
 * @brief OAuth 2.0 client registration response (RFC 7591).
 */
struct OAuthClientRegistration {
    std::string clientId;
    std::string clientSecret;
    std::vector<std::string> redirectUris;
    std::string clientName;
    int clientIdIssuedAt = 0;
    int clientSecretExpiresAt = 0;

    static OAuthClientRegistration fromJson(const json& j) {
        OAuthClientRegistration r;
        r.clientId = j.value("client_id", "");
        r.clientSecret = j.value("client_secret", "");
        r.clientName = j.value("client_name", "");
        r.clientIdIssuedAt = j.value("client_id_issued_at", 0);
        r.clientSecretExpiresAt = j.value("client_secret_expires_at", 0);
        parseJsonStringArray(j, "redirect_uris", r.redirectUris);
        return r;
    }
};

/**
 * @brief Pure C++ OAuth 2.0 client with PKCE and Dynamic Client Registration.
 *
 * Implements the full MCP authorization flow:
 * 1. Discover OAuth server metadata (RFC 8414)
 * 2. Dynamic Client Registration (RFC 7591, if supported)
 * 3. Authorization code flow with PKCE (RFC 7636)
 * 4. Token exchange and refresh
 *
 * Authorization hardening (MCP 2026-07-28):
 * - SEP-2468 (RFC 9207): 上层必须在收到 authorization response（浏览器重定向回
 *   redirect_uri，query 含 code 与 iss）后、调用 exchangeCode 之前调用
 *   validateAuthorizationIss()，校验 response query 中的 iss 与记录的 issuer 一致。
 * - SEP-837: DCR 注册请求必须携带 application_type（desktop/CLI 使用默认值 "native"），
 *   避免 OpenID Connect redirect_uri 冲突。
 * - SEP-2352: 凭据按签发它们的授权服务器 issuer 键控持久化（m_tokensByIssuer），
 *   不得跨授权服务器复用；授权服务器变更必须重新注册。
 *
 * Thread-safe. Uses libcurl for HTTP.
 */
class McpOAuthClient {
public:
    McpOAuthClient();
    ~McpOAuthClient();

    // Step 1: Discover authorization server metadata
    using MetadataCallback = std::function<void(bool success, const OAuthServerMetadata& metadata, const std::string& error)>;
    void discoverMetadata(const std::string& serverUrl, MetadataCallback callback);
    bool discoverMetadataSync(const std::string& serverUrl, OAuthServerMetadata* out, std::string* errorOut = nullptr,
                              std::chrono::milliseconds timeout = std::chrono::milliseconds(10000));

    // Step 2: Dynamic Client Registration
    using RegistrationCallback = std::function<void(bool success, const OAuthClientRegistration& registration, const std::string& error)>;
    void registerClient(const std::string& registrationEndpoint, const std::string& clientName,
                        const std::vector<std::string>& redirectUris, RegistrationCallback callback,
                        const std::string& applicationType = "native");
    bool registerClientSync(const std::string& registrationEndpoint, const std::string& clientName,
                            const std::vector<std::string>& redirectUris, OAuthClientRegistration* out, std::string* errorOut = nullptr,
                            std::chrono::milliseconds timeout = std::chrono::milliseconds(10000),
                            const std::string& applicationType = "native");

    // Step 3: Build authorization URL with PKCE
    struct AuthRequest {
        std::string authorizationUrl;
        std::string codeVerifier;
        std::string codeChallenge;
        std::string state;
    };
    AuthRequest buildAuthorizationUrl(const OAuthServerMetadata& metadata,
                                      const std::string& clientId,
                                      const std::string& redirectUri = "",
                                      const std::vector<std::string>& scopes = {"openid"},
                                      const std::string& resource = "");

    // Step 4: Exchange authorization code for tokens
    using TokenCallback = std::function<void(bool success, const OAuthToken& token, const std::string& error)>;
    void exchangeCode(const std::string& tokenEndpoint, const std::string& clientId,
                      const std::string& clientSecret, const std::string& code,
                      const std::string& redirectUri, const std::string& codeVerifier,
                      TokenCallback callback, const std::string& resource = "",
                      bool useClientSecretBasic = false,
                      const std::string& expectedIssuer = "");
    bool exchangeCodeSync(const std::string& tokenEndpoint, const std::string& clientId,
                          const std::string& clientSecret, const std::string& code,
                          const std::string& redirectUri, const std::string& codeVerifier,
                          OAuthToken* out, std::string* errorOut = nullptr,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds(10000),
                          const std::string& resource = "",
                          bool useClientSecretBasic = false,
                          const std::string& expectedIssuer = "");

    // Step 5: Refresh tokens
    void refreshToken(const std::string& tokenEndpoint, const std::string& clientId,
                      const std::string& clientSecret, const std::string& refreshToken,
                      TokenCallback callback,
                      const std::string& expectedIssuer = "");
    bool refreshTokenSync(const std::string& tokenEndpoint, const std::string& clientId,
                          const std::string& clientSecret, const std::string& refreshTokenValue,
                          OAuthToken* out, std::string* errorOut = nullptr,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds(10000),
                          const std::string& expectedIssuer = "");

    // Utility: Get a valid token, refreshing if needed
    OAuthToken getCurrentToken() const;
    void setCurrentToken(const OAuthToken& token);
    bool hasValidToken() const;

    // SEP-2350: 重新授权时合并先前已请求的 scope（授权流程应累加而非覆盖）
    std::vector<std::string> requestedScopes() const;
    void recordRequestedScopes(const std::vector<std::string>& scopes);
    void clearRequestedScopes();

    // Inject token from outside (e.g., stored credentials).
    // token 带 issuer 时同时按 issuer 键控持久化（SEP-2352）。
    void setStoredToken(const OAuthToken& token);

    // SEP-2468 (RFC 9207): 校验 authorization response 中的 iss 参数。
    // 调用时机：上层在 OAuth 回调（浏览器重定向回 redirect_uri）拿到
    // authorizationResponseUrlOrQuery 后、调用 exchangeCode 之前调用；
    // expectedIssuer 传 discoverMetadata 得到的 metadata.issuer。
    // response 缺失 iss 或 iss 与 expectedIssuer 不一致时返回 false 并填充 error。
    bool validateAuthorizationIss(const std::string& authorizationResponseUrlOrQuery,
                                  const std::string& expectedIssuer,
                                  std::string* error = nullptr) const;

    // SEP-2352: 凭据按 issuer 键控持久化访问（不同授权服务器互不复用）。
    bool hasTokenForIssuer(const std::string& issuer) const;
    OAuthToken getTokenForIssuer(const std::string& issuer) const;
    void setStoredTokenForIssuer(const std::string& issuer, const OAuthToken& token);

    // ===== 认证/刷新流程互斥（2026-09: 防止并发 401 触发多个 OAuth 流程）=====
    /// 尝试获取认证/刷新流程权。成功返回 true，流程结束后必须调用 releaseAuthFlow()。
    /// 失败（有其它流程进行中，包括当前线程重入）返回 false。
    bool tryAcquireAuthFlow();
    /// 释放认证/刷新流程权。
    void releaseAuthFlow();
    /// 是否有认证/刷新流程正在进行（跨线程轮询用）。
    bool hasAuthFlowInProgress() const;
    /// 当前线程是否是流程持有者（用于检测嵌套事件循环导致的同线程重入）。
    bool authFlowOwnerIsCurrentThread() const;

    /// 记录最近一次成功发现并使用的 token endpoint（供发送前主动刷新使用）。
    void setLastTokenEndpoint(const std::string& ep);
    /// 读取最近记录的 token endpoint。
    std::string lastTokenEndpoint() const;

private:
    static std::string generateCodeVerifier();
    static std::string computeCodeChallenge(const std::string& verifier);
    static std::string generateState();
    static std::string httpGet(const std::string& url);
    static std::string httpPost(const std::string& url, const std::string& body, const std::string& contentType = "application/json");

    mutable std::mutex m_mutex;
    OAuthToken m_currentToken;
    std::map<std::string, OAuthToken> m_tokensByIssuer;  // SEP-2352: issuer -> token
    std::vector<std::string> m_requestedScopes;  // SEP-2350: 已请求的 scope 集合

    // 认证/刷新流程互斥状态（独立于 m_mutex，避免与 token 读写互相阻塞）
    mutable std::mutex m_authFlowMutex;
    std::thread::id m_authFlowOwner;
    bool m_authFlowActive{false};

    std::string m_lastTokenEndpoint;  // 最近一次发现的 token endpoint
};

} // namespace mcp
