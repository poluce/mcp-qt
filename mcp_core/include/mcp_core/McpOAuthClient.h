#pragma once
#include <string>
#include <functional>
#include <mutex>
#include <chrono>
#include <nlohmann/json.hpp>

namespace mcp {

using json = nlohmann::json;

/**
 * @brief OAuth 2.0 token response.
 */
struct OAuthToken {
    std::string accessToken;
    std::string refreshToken;
    std::string tokenType = "Bearer";
    int expiresIn = 0;
    std::string scope;
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

    static OAuthServerMetadata fromJson(const json& j) {
        OAuthServerMetadata m;
        m.issuer = j.value("issuer", "");
        m.authorizationEndpoint = j.value("authorization_endpoint", "");
        m.tokenEndpoint = j.value("token_endpoint", "");
        m.registrationEndpoint = j.value("registration_endpoint", "");
        if (j.contains("scopes_supported") && j["scopes_supported"].is_array())
            for (auto& s : j["scopes_supported"]) m.scopesSupported.push_back(s.get<std::string>());
        if (j.contains("response_types_supported") && j["response_types_supported"].is_array())
            for (auto& s : j["response_types_supported"]) m.responseTypesSupported.push_back(s.get<std::string>());
        if (j.contains("grant_types_supported") && j["grant_types_supported"].is_array())
            for (auto& s : j["grant_types_supported"]) m.grantTypesSupported.push_back(s.get<std::string>());
        if (j.contains("code_challenge_methods_supported") && j["code_challenge_methods_supported"].is_array())
            for (auto& s : j["code_challenge_methods_supported"]) m.codeChallengeMethodsSupported.push_back(s.get<std::string>());
        if (j.contains("token_endpoint_auth_methods_supported") && j["token_endpoint_auth_methods_supported"].is_array())
            for (auto& s : j["token_endpoint_auth_methods_supported"]) m.tokenEndpointAuthMethodsSupported.push_back(s.get<std::string>());
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
        if (j.contains("redirect_uris") && j["redirect_uris"].is_array())
            for (auto& u : j["redirect_uris"]) r.redirectUris.push_back(u.get<std::string>());
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
                        const std::vector<std::string>& redirectUris, RegistrationCallback callback);
    bool registerClientSync(const std::string& registrationEndpoint, const std::string& clientName,
                            const std::vector<std::string>& redirectUris, OAuthClientRegistration* out, std::string* errorOut = nullptr,
                            std::chrono::milliseconds timeout = std::chrono::milliseconds(10000));

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
                      bool useClientSecretBasic = false);
    bool exchangeCodeSync(const std::string& tokenEndpoint, const std::string& clientId,
                          const std::string& clientSecret, const std::string& code,
                          const std::string& redirectUri, const std::string& codeVerifier,
                          OAuthToken* out, std::string* errorOut = nullptr,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds(10000),
                          const std::string& resource = "",
                          bool useClientSecretBasic = false);

    // Step 5: Refresh tokens
    void refreshToken(const std::string& tokenEndpoint, const std::string& clientId,
                      const std::string& clientSecret, const std::string& refreshToken,
                      TokenCallback callback);
    bool refreshTokenSync(const std::string& tokenEndpoint, const std::string& clientId,
                          const std::string& clientSecret, const std::string& refreshTokenValue,
                          OAuthToken* out, std::string* errorOut = nullptr,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds(10000));

    // Utility: Get a valid token, refreshing if needed
    OAuthToken getCurrentToken() const;
    void setCurrentToken(const OAuthToken& token);
    bool hasValidToken() const;

    // Inject token from outside (e.g., stored credentials)
    void setStoredToken(const OAuthToken& token);

private:
    static std::string generateCodeVerifier();
    static std::string computeCodeChallenge(const std::string& verifier);
    static std::string generateState();
    static std::string httpGet(const std::string& url);
    static std::string httpPost(const std::string& url, const std::string& body, const std::string& contentType = "application/json");

    mutable std::mutex m_mutex;
    OAuthToken m_currentToken;
    OAuthClientRegistration m_registration;
    OAuthServerMetadata m_metadata;
};

} // namespace mcp
