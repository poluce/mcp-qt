#include "mcp_core/McpOAuthClient.h"
#include <curl/curl.h>
#include <random>
#include <sstream>
#include <iomanip>
#include <thread>
#include <future>
#include <mutex>
#include <iostream>

#ifdef _WIN32
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <openssl/sha.h>
#endif

namespace mcp {

static std::string buildUrlEncodedBody(const json& j) {
    std::string result;
    CURL* curl = curl_easy_init();
    if (!curl) return "";
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!result.empty()) result += "&";
        char* escKey = curl_easy_escape(curl, it.key().c_str(), static_cast<int>(it.key().length()));
        std::string valStr;
        if (it.value().is_string()) {
            valStr = it.value().get<std::string>();
        } else {
            valStr = it.value().dump();
        }
        char* escVal = curl_easy_escape(curl, valStr.c_str(), static_cast<int>(valStr.length()));
        
        if (escKey && escVal) {
            result += std::string(escKey) + "=" + std::string(escVal);
        }
        if (escKey) curl_free(escKey);
        if (escVal) curl_free(escVal);
    }
    curl_easy_cleanup(curl);
    return result;
}

// libcurl 全局初始化
static std::once_flag g_oauthCurlInit;
static void ensureOAuthCurlInit() {
    std::call_once(g_oauthCurlInit, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

// 回调：累积响应数据
static size_t oauthWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* resp = static_cast<std::string*>(userdata);
    resp->append(ptr, size * nmemb);
    return size * nmemb;
}

// PKCE code_verifier: 43-128 chars from [A-Z][a-z][0-9]-._~
std::string McpOAuthClient::generateCodeVerifier() {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, sizeof(chars) - 2);
    std::string verifier;
    verifier.reserve(64);
    for (int i = 0; i < 64; ++i) {
        verifier += chars[dist(gen)];
    }
    return verifier;
}

// PKCE code_challenge = BASE64URL(SHA256(verifier))
std::string McpOAuthClient::computeCodeChallenge(const std::string& verifier) {
    unsigned char hash[32];

#ifdef _WIN32
    // Windows BCrypt SHA256
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    NTSTATUS status;

    status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) return verifier;

    status = BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(status)) { BCryptCloseAlgorithmProvider(hAlg, 0); return verifier; }

    status = BCryptHashData(hHash, const_cast<PUCHAR>(reinterpret_cast<const unsigned char*>(verifier.data())),
                            static_cast<ULONG>(verifier.size()), 0);
    if (!BCRYPT_SUCCESS(status)) { BCryptDestroyHash(hHash); BCryptCloseAlgorithmProvider(hAlg, 0); return verifier; }

    status = BCryptFinishHash(hHash, hash, 32, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (!BCRYPT_SUCCESS(status)) return verifier;
#else
    // OpenSSL SHA256
    if (SHA256(reinterpret_cast<const unsigned char*>(verifier.data()), verifier.size(), hash) == nullptr) {
        return verifier;
    }
#endif

    // BASE64URL 编码（无 padding）
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string encoded;
    encoded.reserve(43);
    for (size_t i = 0; i < 32; i += 3) {
        unsigned int n = static_cast<unsigned int>(hash[i]) << 16;
        if (i + 1 < 32) n |= static_cast<unsigned int>(hash[i + 1]) << 8;
        if (i + 2 < 32) n |= static_cast<unsigned int>(hash[i + 2]);
        encoded += b64[(n >> 18) & 0x3F];
        encoded += b64[(n >> 12) & 0x3F];
        if (i + 1 < 32) encoded += b64[(n >> 6) & 0x3F];
        if (i + 2 < 32) encoded += b64[n & 0x3F];
    }
    // 去掉尾部 padding（BASE64URL 不需要）
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    return encoded;
}

std::string McpOAuthClient::generateState() {
    static const char hex[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, 15);
    std::string state;
    state.reserve(32);
    for (int i = 0; i < 32; ++i) {
        state += hex[dist(gen)];
    }
    return state;
}

std::string McpOAuthClient::httpGet(const std::string& url) {
    ensureOAuthCurlInit();
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, oauthWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

std::string McpOAuthClient::httpPost(const std::string& url, const std::string& body, const std::string& contentType) {
    ensureOAuthCurlInit();
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, oauthWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    struct curl_slist* headers = nullptr;
    std::string ct = "Content-Type: " + contentType;
    headers = curl_slist_append(headers, ct.c_str());
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

McpOAuthClient::McpOAuthClient() {
    ensureOAuthCurlInit();
}

McpOAuthClient::~McpOAuthClient() = default;

// ===== Step 1: Metadata Discovery =====

void McpOAuthClient::discoverMetadata(const std::string& serverUrl, MetadataCallback callback) {
    std::thread([this, serverUrl, callback]() {
        std::vector<std::string> urlsToTry;

        // 如果 URL 本身已经包含 .well-known（显式元数据路径），直接使用
        if (serverUrl.find(".well-known") != std::string::npos) {
            urlsToTry.push_back(serverUrl);
        } else {
            // 否则从 issuer URL 派生 well-known 路径
            std::string baseUrl = serverUrl;
            if (baseUrl.back() != '/') baseUrl += '/';
            urlsToTry.push_back(baseUrl + ".well-known/oauth-authorization-server");
            urlsToTry.push_back(baseUrl + ".well-known/openid-configuration");

            // 如果 issuer URL 有路径前缀（如 /tenant1），也尝试 origin + well-known + pathPrefix
            size_t pathStart = serverUrl.find("://");
            if (pathStart != std::string::npos) {
                pathStart = serverUrl.find('/', pathStart + 3);
                if (pathStart != std::string::npos) {
                    std::string origin = serverUrl.substr(0, pathStart);
                    std::string pathPrefix = serverUrl.substr(pathStart);
                    if (!pathPrefix.empty() && pathPrefix != "/") {
                        if (pathPrefix.back() != '/') pathPrefix += '/';
                        urlsToTry.push_back(origin + "/.well-known/oauth-authorization-server" + pathPrefix);
                        urlsToTry.push_back(origin + "/.well-known/openid-configuration" + pathPrefix);
                    }
                }
            }
        }

        for (const auto& metadataUrl : urlsToTry) {
            std::cerr << "[SDK OAuth Debug] discoverMetadata: fetching from " << metadataUrl << std::endl;
            std::string body = httpGet(metadataUrl);
            if (!body.empty()) {
                try {
                    json j = json::parse(body);
                    auto metadata = OAuthServerMetadata::fromJson(j);
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_metadata = metadata;
                    }
                    std::cerr << "[SDK OAuth Debug] discoverMetadata: success from " << metadataUrl << std::endl;
                    callback(true, metadata, "");
                    return;
                } catch (const std::exception& e) {
                    std::cerr << "[SDK OAuth Debug] discoverMetadata: parse failed for " << metadataUrl << ": " << e.what() << std::endl;
                }
            }
        }
        callback(false, OAuthServerMetadata{}, "Failed to fetch metadata from all well-known paths");
    }).detach();
}

bool McpOAuthClient::discoverMetadataSync(const std::string& serverUrl, OAuthServerMetadata* out,
                                           std::string* errorOut, std::chrono::milliseconds timeout) {
    struct SyncData {
        std::promise<std::pair<bool, OAuthServerMetadata>> pr;
        std::string err;
    };
    auto sd = std::make_shared<SyncData>();
    auto fut = sd->pr.get_future();
    discoverMetadata(serverUrl, [sd](bool ok, const OAuthServerMetadata& m, const std::string& e) {
        sd->err = e;
        sd->pr.set_value({ok, m});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (out) *out = res.second;
        if (errorOut) *errorOut = sd->err;
        return res.first;
    }
    if (errorOut) *errorOut = "Metadata discovery timed out";
    return false;
}

// ===== Step 2: Dynamic Client Registration =====

void McpOAuthClient::registerClient(const std::string& registrationEndpoint, const std::string& clientName,
                                     const std::vector<std::string>& redirectUris, RegistrationCallback callback) {
    json req = {
        {"client_name", clientName},
        {"redirect_uris", redirectUris},
        {"grant_types", {"authorization_code", "refresh_token"}},
        {"response_types", {"code"}},
        {"token_endpoint_auth_method", "none"}
    };

    std::thread([this, registrationEndpoint, reqStr = req.dump(), callback]() {
        std::cerr << "[SDK OAuth Debug] registerClient: endpoint=" << registrationEndpoint << ", request=" << reqStr << std::endl;
        std::string body = httpPost(registrationEndpoint, reqStr);
        std::cerr << "[SDK OAuth Debug] registerClient response=" << body << std::endl;
        if (body.empty()) {
            callback(false, OAuthClientRegistration{}, "Registration request failed");
            return;
        }
        try {
            json j = json::parse(body);
            auto reg = OAuthClientRegistration::fromJson(j);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_registration = reg;
            }
            callback(true, reg, "");
        } catch (const std::exception& e) {
            callback(false, OAuthClientRegistration{}, std::string("Failed to parse registration: ") + e.what());
        }
    }).detach();
}

bool McpOAuthClient::registerClientSync(const std::string& registrationEndpoint, const std::string& clientName,
                                         const std::vector<std::string>& redirectUris, OAuthClientRegistration* out,
                                         std::string* errorOut, std::chrono::milliseconds timeout) {
    struct SyncData {
        std::promise<std::pair<bool, OAuthClientRegistration>> pr;
        std::string err;
    };
    auto sd = std::make_shared<SyncData>();
    auto fut = sd->pr.get_future();
    registerClient(registrationEndpoint, clientName, redirectUris, [sd](bool ok, const OAuthClientRegistration& r, const std::string& e) {
        sd->err = e;
        sd->pr.set_value({ok, r});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (out) *out = res.second;
        if (errorOut) *errorOut = sd->err;
        return res.first;
    }
    if (errorOut) *errorOut = "Client registration timed out";
    return false;
}

// ===== Step 3: Build Authorization URL =====

McpOAuthClient::AuthRequest McpOAuthClient::buildAuthorizationUrl(
    const OAuthServerMetadata& metadata, const std::string& clientId, const std::string& redirectUri,
    const std::vector<std::string>& scopes, const std::string& resource) {
    AuthRequest req;
    req.codeVerifier = generateCodeVerifier();
    req.codeChallenge = computeCodeChallenge(req.codeVerifier);
    req.state = generateState();

    std::string scopeStr;
    for (size_t i = 0; i < scopes.size(); ++i) {
        if (i > 0) scopeStr += " ";
        scopeStr += scopes[i];
    }

    // URL 编码 scope 中的空格为 +
    std::string encodedScope;
    for (char c : scopeStr) {
        encodedScope += (c == ' ') ? '+' : c;
    }

    req.authorizationUrl = metadata.authorizationEndpoint;
    req.authorizationUrl += "?response_type=code";
    req.authorizationUrl += "&client_id=" + clientId;
    req.authorizationUrl += "&state=" + req.state;
    req.authorizationUrl += "&code_challenge=" + req.codeChallenge;
    req.authorizationUrl += "&code_challenge_method=S256";
    if (!encodedScope.empty()) {
        req.authorizationUrl += "&scope=" + encodedScope;
    }
    if (!redirectUri.empty()) {
        CURL* curl = curl_easy_init();
        if (curl) {
            char* escaped = curl_easy_escape(curl, redirectUri.c_str(), static_cast<int>(redirectUri.length()));
            if (escaped) {
                req.authorizationUrl += "&redirect_uri=" + std::string(escaped);
                curl_free(escaped);
            }
            curl_easy_cleanup(curl);
        }
    }

    // RFC 8707: resource parameter
    if (!resource.empty()) {
        CURL* curl = curl_easy_init();
        if (curl) {
            char* escaped = curl_easy_escape(curl, resource.c_str(), static_cast<int>(resource.length()));
            if (escaped) {
                req.authorizationUrl += "&resource=" + std::string(escaped);
                curl_free(escaped);
            }
            curl_easy_cleanup(curl);
        }
    }

    return req;
}

// ===== Step 4: Exchange Code for Token =====

void McpOAuthClient::exchangeCode(const std::string& tokenEndpoint, const std::string& clientId,
                                   const std::string& clientSecret, const std::string& code,
                                   const std::string& redirectUri, const std::string& codeVerifier,
                                   TokenCallback callback, const std::string& resource,
                                   bool useClientSecretBasic) {
    json body = {
        {"grant_type", "authorization_code"},
        {"code", code},
        {"redirect_uri", redirectUri},
        {"code_verifier", codeVerifier}
    };
    // client_secret_basic: 通过 HTTP Basic Auth 发送凭据，不在 body 中包含
    if (!useClientSecretBasic) {
        body["client_id"] = clientId;
        if (!clientSecret.empty()) {
            body["client_secret"] = clientSecret;
        }
    }
    // RFC 8707: resource parameter
    if (!resource.empty()) {
        body["resource"] = resource;
    }

    std::thread([this, tokenEndpoint, bodyStr = buildUrlEncodedBody(body), callback, useClientSecretBasic, clientId, clientSecret]() {
        std::cerr << "[SDK OAuth Debug] exchangeCode: tokenEndpoint=" << tokenEndpoint << ", body=" << bodyStr << " basic=" << useClientSecretBasic << std::endl;
        std::string resp;
        if (useClientSecretBasic) {
            // client_secret_basic: 使用 HTTP Basic Auth 发送凭据
            ensureOAuthCurlInit();
            CURL* curl = curl_easy_init();
            if (curl) {
                curl_easy_setopt(curl, CURLOPT_URL, tokenEndpoint.c_str());
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(bodyStr.size()));
                curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
                std::string userpwd = clientId + ":" + clientSecret;
                curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
                struct curl_slist* hdrs = nullptr;
                hdrs = curl_slist_append(hdrs, "Content-Type: application/x-www-form-urlencoded");
                hdrs = curl_slist_append(hdrs, "Accept: application/json");
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, oauthWriteCallback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
                curl_easy_perform(curl);
                curl_slist_free_all(hdrs);
                curl_easy_cleanup(curl);
            }
        } else {
            resp = httpPost(tokenEndpoint, bodyStr, "application/x-www-form-urlencoded");
        }
        std::cerr << "[SDK OAuth Debug] exchangeCode response=" << resp << std::endl;
        if (resp.empty()) {
            callback(false, OAuthToken{}, "Token exchange request failed");
            return;
        }
        try {
            json j = json::parse(resp);
            if (j.contains("error")) {
                std::string err = j.value("error", "unknown");
                if (j.contains("error_description")) err += ": " + j["error_description"].get<std::string>();
                callback(false, OAuthToken{}, err);
                return;
            }
            OAuthToken token;
            token.accessToken = j.value("access_token", "");
            token.refreshToken = j.value("refresh_token", "");
            token.tokenType = j.value("token_type", "Bearer");
            token.expiresIn = j.value("expires_in", 0);
            token.scope = j.value("scope", "");
            token.obtainedAt = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_currentToken = token;
            }
            callback(true, token, "");
        } catch (const std::exception& e) {
            callback(false, OAuthToken{}, std::string("Failed to parse token response: ") + e.what());
        }
    }).detach();
}

bool McpOAuthClient::exchangeCodeSync(const std::string& tokenEndpoint, const std::string& clientId,
                                       const std::string& clientSecret, const std::string& code,
                                       const std::string& redirectUri, const std::string& codeVerifier,
                                       OAuthToken* out, std::string* errorOut, std::chrono::milliseconds timeout,
                                       const std::string& resource, bool useClientSecretBasic) {
    struct SyncData {
        std::promise<std::pair<bool, OAuthToken>> pr;
        std::string err;
    };
    auto sd = std::make_shared<SyncData>();
    auto fut = sd->pr.get_future();
    exchangeCode(tokenEndpoint, clientId, clientSecret, code, redirectUri, codeVerifier,
                 [sd](bool ok, const OAuthToken& t, const std::string& e) {
        sd->err = e;
        sd->pr.set_value({ok, t});
    }, resource, useClientSecretBasic);
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (out) *out = res.second;
        if (errorOut) *errorOut = sd->err;
        return res.first;
    }
    if (errorOut) *errorOut = "Token exchange timed out";
    return false;
}

// ===== Step 5: Refresh Token =====

void McpOAuthClient::refreshToken(const std::string& tokenEndpoint, const std::string& clientId,
                                   const std::string& clientSecret, const std::string& refreshTokenValue,
                                   TokenCallback callback) {
    json body = {
        {"grant_type", "refresh_token"},
        {"refresh_token", refreshTokenValue},
        {"client_id", clientId}
    };
    if (!clientSecret.empty()) {
        body["client_secret"] = clientSecret;
    }

    std::string rtv = refreshTokenValue;
    std::thread([this, tokenEndpoint, bodyStr = buildUrlEncodedBody(body), callback, rtv]() {
        std::cerr << "[SDK OAuth Debug] refreshToken: tokenEndpoint=" << tokenEndpoint << ", body=" << bodyStr << std::endl;
        std::string resp = httpPost(tokenEndpoint, bodyStr, "application/x-www-form-urlencoded");
        std::cerr << "[SDK OAuth Debug] refreshToken response=" << resp << std::endl;
        if (resp.empty()) {
            callback(false, OAuthToken{}, "Token refresh request failed");
            return;
        }
        try {
            json j = json::parse(resp);
            if (j.contains("error")) {
                std::string err = j.value("error", "unknown");
                if (j.contains("error_description")) err += ": " + j["error_description"].get<std::string>();
                callback(false, OAuthToken{}, err);
                return;
            }
            OAuthToken token;
            token.accessToken = j.value("access_token", "");
            token.refreshToken = j.value("refresh_token", rtv);
            token.tokenType = j.value("token_type", "Bearer");
            token.expiresIn = j.value("expires_in", 0);
            token.scope = j.value("scope", "");
            token.obtainedAt = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_currentToken = token;
            }
            callback(true, token, "");
        } catch (const std::exception& e) {
            callback(false, OAuthToken{}, std::string("Failed to parse refresh response: ") + e.what());
        }
    }).detach();
}

bool McpOAuthClient::refreshTokenSync(const std::string& tokenEndpoint, const std::string& clientId,
                                       const std::string& clientSecret, const std::string& refreshTokenValue,
                                       OAuthToken* out, std::string* errorOut, std::chrono::milliseconds timeout) {
    struct SyncData {
        std::promise<std::pair<bool, OAuthToken>> pr;
        std::string err;
    };
    auto sd = std::make_shared<SyncData>();
    auto fut = sd->pr.get_future();
    refreshToken(tokenEndpoint, clientId, clientSecret, refreshTokenValue,
                 [sd](bool ok, const OAuthToken& t, const std::string& e) {
        sd->err = e;
        sd->pr.set_value({ok, t});
    });
    if (fut.wait_for(timeout) == std::future_status::ready) {
        auto res = fut.get();
        if (out) *out = res.second;
        if (errorOut) *errorOut = sd->err;
        return res.first;
    }
    if (errorOut) *errorOut = "Token refresh timed out";
    return false;
}

// ===== Token Management =====

OAuthToken McpOAuthClient::getCurrentToken() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_currentToken;
}

void McpOAuthClient::setCurrentToken(const OAuthToken& token) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentToken = token;
}

bool McpOAuthClient::hasValidToken() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_currentToken.accessToken.empty() && !m_currentToken.isExpired();
}

void McpOAuthClient::setStoredToken(const OAuthToken& token) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentToken = token;
}

} // namespace mcp
