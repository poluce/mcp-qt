#include "mcp_core/HttpSseTransport.h"
#include <curl/curl.h>
#include <httplib.h>
#include <sstream>
#include <chrono>
#include <mutex>
#include <cctype>
#include <iostream>
#include "mcp_core/McpOAuthClient.h"
#include <nlohmann/json.hpp>

namespace mcp {

// 从相对路径解析为完整 URL
static std::string resolveUrl(const std::string& base, const std::string& relative) {
    if (relative.empty()) return base;
    if (relative.find("://") != std::string::npos) return relative;

    auto schemeEnd = base.find("://");
    size_t hostStart = (schemeEnd != std::string::npos) ? schemeEnd + 3 : 0;

    if (relative[0] == '/') {
        auto pathStart = base.find('/', hostStart);
        if (pathStart != std::string::npos) {
            return base.substr(0, pathStart) + relative;
        }
        return base + relative;
    }

    auto pathStart = base.find('/', hostStart);
    if (pathStart == std::string::npos) {
        return base + "/" + relative;
    }

    auto lastSlash = base.rfind('/');
    if (lastSlash != std::string::npos && lastSlash >= hostStart) {
        std::string lastPart = base.substr(lastSlash + 1);
        if (!lastPart.empty() && lastPart.find('.') == std::string::npos) {
            return base + "/" + relative;
        }
        return base.substr(0, lastSlash + 1) + relative;
    }
    return base + "/" + relative;
}

// 全局初始化：确保 libcurl 只初始化一次
static std::once_flag g_curlInit;
static void ensureCurlInit() {
    std::call_once(g_curlInit, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

// POST 响应回调
static size_t postWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* resp = static_cast<std::string*>(userdata);
    resp->append(ptr, size * nmemb);
    return size * nmemb;
}

struct HeaderUserData {
    std::string sessionId;
    std::string wwwAuthenticate;
    HttpSseTransport* transport{nullptr};
};

// POST 响应头回调（提取 MCP-Session-Id 和 WWW-Authenticate）
static size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* data = static_cast<HeaderUserData*>(userdata);
    std::string header(buffer, size * nitems);

    std::string lowerHeader = header;
    for (char& c : lowerHeader) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (lowerHeader.compare(0, 15, "mcp-session-id:") == 0) {
        data->sessionId = header.substr(15);
        size_t s = data->sessionId.find_first_not_of(" \t\r\n");
        if (s != std::string::npos) data->sessionId.erase(0, s);
        size_t e = data->sessionId.find_last_not_of("\r\n");
        if (e != std::string::npos) data->sessionId.erase(e + 1);
    } else if (lowerHeader.compare(0, 17, "www-authenticate:") == 0) {
        data->wwwAuthenticate = header.substr(17);
        size_t s = data->wwwAuthenticate.find_first_not_of(" \t\r\n");
        if (s != std::string::npos) data->wwwAuthenticate.erase(0, s);
        size_t e = data->wwwAuthenticate.find_last_not_of("\r\n");
        if (e != std::string::npos) data->wwwAuthenticate.erase(e + 1);
    }
    return size * nitems;
}

// SSE 流式接收回调（libcurl POST 响应处理时用，SSE GET 不再使用）
static size_t sseWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* transport = static_cast<HttpSseTransport*>(userdata);
    transport->onSseData(ptr, size * nmemb);
    return size * nmemb;
}

HttpSseTransport::HttpSseTransport(const std::string& sseUrl)
    : m_sseUrl(sseUrl)
{
    ensureCurlInit();

    m_postUrl = sseUrl;

    m_oauthClient = std::make_shared<McpOAuthClient>();

    m_tokenProvider = [this]() -> std::string {
        return m_oauthClient->getCurrentToken().accessToken;
    };

    m_authRetryHandler = [this](const std::string& wwwAuthHeader) -> bool {
        return handleDefaultAuthRetry(wwwAuthHeader);
    };
}

HttpSseTransport::~HttpSseTransport() {
    close();
}

bool HttpSseTransport::start() {
    if (m_running) return false;

    m_running = true;
    m_closed = false;

    m_sseThread = std::thread(&HttpSseTransport::sseReadLoop, this);

    return true;
}

bool HttpSseTransport::send(const std::string& message) {
    if (m_closed || m_postUrl.empty()) return false;
    return doPost(message);
}

void HttpSseTransport::setOnMessage(std::function<void(const std::string&)> callback) {
    m_onMessage = std::move(callback);
}

void HttpSseTransport::setOnClose(std::function<void()> callback) {
    m_onClose = std::move(callback);
}

void HttpSseTransport::setOnError(std::function<void(const std::string&)> callback) {
    m_onError = std::move(callback);
}

void HttpSseTransport::close() {
    if (m_closed) return;
    m_closed = true;
    m_running = false;

    if (m_sseThread.joinable()) {
        m_sseThread.detach();
    }

    if (m_onClose) m_onClose();
}

void HttpSseTransport::setProtocolVersion(const std::string& version) {
    m_protocolVersion = version;
}

// SSE 流式数据处理：累积缓冲区，按 "\n\n" 分割 SSE 事件块
void HttpSseTransport::onSseData(const char* data, size_t len) {
    std::cerr << "[SDK SSE Raw] Received data chunk (len=" << len << "): " << std::string(data, len) << std::endl;
    m_sseBuffer.append(data, len);

    size_t pos;
    while ((pos = m_sseBuffer.find("\n\n")) != std::string::npos) {
        std::string block = m_sseBuffer.substr(0, pos);
        m_sseBuffer.erase(0, pos + 2);

        std::string eventType = "message";
        std::string dataContent;

        std::istringstream stream(block);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();

            if (line.compare(0, 6, "event:") == 0) {
                eventType = line.substr(6);
                size_t s = eventType.find_first_not_of(" \t");
                if (s != std::string::npos) eventType = eventType.substr(s);
            } else if (line.compare(0, 5, "data:") == 0) {
                std::string currentData = line.substr(5);
                size_t s = currentData.find_first_not_of(" \t");
                if (s != std::string::npos) currentData = currentData.substr(s);

                if (!dataContent.empty()) {
                    dataContent += "\n";
                }
                dataContent += currentData;
            } else if (line.compare(0, 3, "id:") == 0) {
                std::string idVal = line.substr(3);
                size_t s = idVal.find_first_not_of(" \t");
                if (s != std::string::npos) idVal = idVal.substr(s);
                if (!idVal.empty()) {
                    m_lastEventId = idVal;
                }
            } else if (line.compare(0, 6, "retry:") == 0) {
                std::string retryVal = line.substr(6);
                size_t s = retryVal.find_first_not_of(" \t");
                if (s != std::string::npos) retryVal = retryVal.substr(s);
                if (!retryVal.empty()) {
                    try {
                        int ms = std::stoi(retryVal);
                        if (ms > 0) {
                            m_sseRetryMs = ms;
                        }
                    } catch (...) {
                    }
                }
            }
        }

        if (!dataContent.empty()) {
            if (eventType == "endpoint") {
                m_postUrl = resolveUrl(m_sseUrl, dataContent);
            } else if (m_onMessage) {
                m_onMessage(dataContent);
            }
        }
    }
}

// SSE 读取主循环（后台线程），使用 cpp-httplib 实现可靠的连接关闭检测
void HttpSseTransport::sseReadLoop() {
    while (m_running) {
        m_sseBuffer.clear();

        // 解析 URL：提取 scheme、host、port、path
        std::string scheme, host, path;
        int port = 80;
        {
            std::string url = m_sseUrl;
            size_t schemeEnd = url.find("://");
            if (schemeEnd != std::string::npos) {
                scheme = url.substr(0, schemeEnd);
                std::string rest = url.substr(schemeEnd + 3);
                size_t pathStart = rest.find('/');
                if (pathStart != std::string::npos) {
                    std::string hostPart = rest.substr(0, pathStart);
                    path = rest.substr(pathStart);
                    size_t colon = hostPart.find(':');
                    if (colon != std::string::npos) {
                        host = hostPart.substr(0, colon);
                        port = std::stoi(hostPart.substr(colon + 1));
                    } else {
                        host = hostPart;
                        port = (scheme == "https") ? 443 : 80;
                    }
                } else {
                    host = rest;
                    path = "/";
                    size_t colon = host.find(':');
                    if (colon != std::string::npos) {
                        port = std::stoi(host.substr(colon + 1));
                        host = host.substr(0, colon);
                    } else {
                        port = (scheme == "https") ? 443 : 80;
                    }
                }
            } else {
                host = url;
                path = "/";
            }
        }

        // 获取 session id 和 token
        std::string sid;
        std::string token;
        {
            std::lock_guard<std::recursive_mutex> lock(m_sendMutex);
            sid = m_sessionId;
            if (m_tokenProvider) {
                token = m_tokenProvider();
            }
        }

        m_sseConnected = false;
        std::string responseSessionId;
        std::string wwwAuthenticate;
        int responseCode = 0;

        try {
            httplib::Client client(host, port);
            client.set_connection_timeout(10, 0);
            // 设 1 秒读超时：服务端关闭 SSE 流后能及时检测到（原生 recv 超时）
            client.set_read_timeout(1, 0);

            httplib::Headers headers;
            headers.emplace("Accept", "text/event-stream");
            headers.emplace("Cache-Control", "no-cache");
            if (!sid.empty()) {
                headers.emplace("Mcp-Session-Id", sid);
            }
            if (!m_lastEventId.empty()) {
                headers.emplace("Last-Event-ID", m_lastEventId);
            }
            if (!token.empty()) {
                headers.emplace("Authorization", "Bearer " + token);
            }

            if (m_onError) m_onError("SSE connecting to: " + m_sseUrl);

            // 使用流式接收：httplib 在 recv() 返回 0（连接关闭）时自然结束
            httplib::ResponseHandler respHandler =
                [&](const httplib::Response& resp) {
                    responseCode = resp.status;
                    auto it = resp.headers.find("mcp-session-id");
                    if (it != resp.headers.end()) {
                        responseSessionId = it->second;
                    }
                    auto authIt = resp.headers.find("www-authenticate");
                    if (authIt != resp.headers.end()) {
                        wwwAuthenticate = authIt->second;
                    }
                    if (resp.status == 200) {
                        m_sseConnected = true;
                    }
                    return true;
                };
            httplib::ContentReceiver contentReceiver =
                [this](const char* data, size_t data_length) {
                    onSseData(data, data_length);
                    return m_running.load();
                };
            auto res = client.Get(path.c_str(), headers, respHandler, contentReceiver);

            if (!res) {
                auto err = res.error();
                std::cerr << "[SDK httplib] GET error: " << httplib::to_string(err) << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[SDK httplib] Exception: " << e.what() << std::endl;
        }

        // 更新 session ID
        if (!responseSessionId.empty()) {
            std::lock_guard<std::recursive_mutex> lock(m_sendMutex);
            m_sessionId = responseSessionId;
        }

        if (!m_running) break;

        // 如果是 401/403 尝试 auth retry
        if ((responseCode == 401 || responseCode == 403) && m_authRetryHandler) {
            if (m_onError) {
                m_onError("sseReadLoop: Got " + std::to_string(responseCode) + ", attempting auth retry...");
            }
            if (m_authRetryHandler(wwwAuthenticate)) {
                continue;
            }
        }

        // 连接已断开
        if (m_onError) {
            if (responseCode == 200 || responseCode == 0) {
                m_onError("SSE disconnected (code=" + std::to_string(responseCode) + "). Reconnecting in " + std::to_string(m_sseRetryMs) + "ms...");
            } else if (responseCode == 401 || responseCode == 403) {
                m_onError("SSE Auth failed (" + std::to_string(responseCode) + "). Reconnecting in " + std::to_string(m_sseRetryMs) + "ms...");
            } else {
                m_onError("SSE error (code=" + std::to_string(responseCode) + "). Reconnecting in " + std::to_string(m_sseRetryMs) + "ms...");
            }
        }

        // 使用 SSE retry 延迟后重连
        {
            int delayMs = m_sseRetryMs;
            int steps = (delayMs + 99) / 100;
            std::cerr << "[SDK SSE] Reconnect delay=" << delayMs << "ms steps=" << steps << " lastEventId=" << m_lastEventId << std::endl;
            for (int i = 0; i < steps && m_running; ++i) {
                {
                    std::lock_guard<std::recursive_mutex> lock(m_sendMutex);
                    if (!m_sessionId.empty() && responseCode == 400) {
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
}

// 发送 HTTP POST 请求
bool HttpSseTransport::doPost(const std::string& body) {
    int maxRetries = 3;
    for (int retry = 0; retry < maxRetries; ++retry) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        std::string responseBody;
        HeaderUserData headerData;

        std::string postUrl;
        std::string protocolVersion;
        std::string sessionId;
        std::string token;
        {
            std::lock_guard<std::recursive_mutex> lock(m_sendMutex);
            postUrl = m_postUrl;
            protocolVersion = m_protocolVersion;
            sessionId = m_sessionId;
            if (m_tokenProvider) {
                token = m_tokenProvider();
            }
        }

        curl_easy_setopt(curl, CURLOPT_URL, postUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, postWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: application/json, text/event-stream");
        std::string versionHeader = "MCP-Protocol-Version: " + protocolVersion;
        headers = curl_slist_append(headers, versionHeader.c_str());

        if (!sessionId.empty()) {
            std::string sid = "MCP-Session-Id: " + sessionId;
            headers = curl_slist_append(headers, sid.c_str());
        }

        if (!token.empty()) {
            std::string authHeader = "Authorization: Bearer " + token;
            headers = curl_slist_append(headers, authHeader.c_str());
        }

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headerData);

        CURLcode res = curl_easy_perform(curl);

        long responseCode = 0;
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
        }

        // 更新 session ID
        bool isInitialize = false;
        if (res == CURLE_OK && !headerData.sessionId.empty()) {
            std::lock_guard<std::recursive_mutex> lock(m_sendMutex);
            if (m_sessionId.empty()) {
                isInitialize = true;
            }
            m_sessionId = headerData.sessionId;
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            if (m_onError) {
                m_onError(std::string("HTTP POST failed: ") + curl_easy_strerror(res));
            }
            return false;
        }

        // 如果刚刚完成 initialize 握手，等待 SSE 连接建立（如果尚未连接）
        if (isInitialize && !m_sseConnected) {
            std::cerr << "[SDK] Waiting for SSE connection to be established..." << std::endl;
            for (int i = 0; i < 50 && m_running && !m_sseConnected; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (m_sseConnected) {
                std::cerr << "[SDK] SSE connection established successfully." << std::endl;
            } else {
                std::cerr << "[SDK] Warning: SSE connection establishment timed out." << std::endl;
            }
        }

        // 处理响应体（SSE 等待必须在 m_onMessage 之前完成）
        if (!responseBody.empty()) {
            if (responseBody.find("data:") != std::string::npos) {
                std::istringstream stream(responseBody);
                std::string line;
                while (std::getline(stream, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.compare(0, 5, "data:") == 0) {
                        std::string data = line.substr(5);
                        size_t s = data.find_first_not_of(" \t");
                        if (s != std::string::npos) data = data.substr(s);
                        if (!data.empty() && m_onMessage) {
                            m_onMessage(data);
                        }
                    }
                }
            } else if (m_onMessage) {
                m_onMessage(responseBody);
            }
        }

        // 如果是 401/403 并且有 auth retry handler，尝试重试
        if ((responseCode == 401 || responseCode == 403) && m_authRetryHandler) {
            if (m_onError) {
                m_onError("doPost: Got " + std::to_string(responseCode) + ", attempting auth retry...");
            }
            if (m_authRetryHandler(headerData.wwwAuthenticate)) {
                continue;
            }
        }

        if (responseCode == 401 || responseCode == 403) {
            if (m_onError) {
                m_onError("HTTP POST auth failed (" + std::to_string(responseCode) + ") without retry success.");
            }
            return false;
        }

        return true;
    }
    return false;
}

void HttpSseTransport::setTokenProvider(TokenProvider provider) {
    m_tokenProvider = std::move(provider);
}

void HttpSseTransport::setAuthRetryHandler(AuthRetryHandler handler) {
    m_authRetryHandler = std::move(handler);
}

static std::string getEnvVar(const std::string& key, const std::string& defaultVal = "") {
    const char* val = std::getenv(key.c_str());
    return val ? std::string(val) : defaultVal;
}

static std::string extractResourceMetadataUrl(const std::string& wwwAuth) {
    std::string target = "resource_metadata=\"";
    size_t pos = wwwAuth.find(target);
    if (pos == std::string::npos) {
        target = "resource_metadata=";
        pos = wwwAuth.find(target);
        if (pos == std::string::npos) return "";
        size_t start = pos + target.length();
        size_t end = wwwAuth.find_first_of(", ", start);
        if (end == std::string::npos) end = wwwAuth.length();
        return wwwAuth.substr(start, end - start);
    }
    size_t start = pos + target.length();
    size_t end = wwwAuth.find("\"", start);
    if (end == std::string::npos) return "";
    return wwwAuth.substr(start, end - start);
}

static std::string extractScope(const std::string& wwwAuth) {
    std::string target = "scope=\"";
    size_t pos = wwwAuth.find(target);
    if (pos == std::string::npos) {
        target = "scope=";
        pos = wwwAuth.find(target);
        if (pos == std::string::npos) return "";
        size_t start = pos + target.length();
        size_t end = wwwAuth.find_first_of(", ", start);
        if (end == std::string::npos) end = wwwAuth.length();
        return wwwAuth.substr(start, end - start);
    }
    size_t start = pos + target.length();
    size_t end = wwwAuth.find("\"", start);
    if (end == std::string::npos) return "";
    return wwwAuth.substr(start, end - start);
}

static size_t curlGetCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* resp = static_cast<std::string*>(userdata);
    resp->append(ptr, size * nmemb);
    return size * nmemb;
}

static std::string curlHttpGet(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlGetCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return response;
}

struct LocationUserData {
    std::string location;
};

static std::string getAuthCodeFromRedirect(const std::string& authUrl) {
    std::cerr << "[SDK Auth Debug] getAuthCodeFromRedirect: requesting authUrl=" << authUrl << std::endl;
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    LocationUserData data;
    curl_easy_setopt(curl, CURLOPT_URL, authUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    auto locHeaderCallback = [](char* buffer, size_t size, size_t nitems, void* userdata) -> size_t {
        auto* d = static_cast<LocationUserData*>(userdata);
        std::string header(buffer, size * nitems);
        std::string lowerHeader = header;
        for (char& c : lowerHeader) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (lowerHeader.length() >= 9 && lowerHeader.compare(0, 9, "location:") == 0) {
            d->location = header.substr(9);
            size_t s = d->location.find_first_not_of(" \t\r\n");
            if (s != std::string::npos) d->location.erase(0, s);
            size_t e = d->location.find_last_not_of("\r\n");
            if (e != std::string::npos) d->location.erase(e + 1);
        }
        return size * nitems;
    };

    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, +locHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &data);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        return size * nmemb;
    });

    CURLcode res = curl_easy_perform(curl);
    std::cerr << "[SDK Auth Debug] getAuthCodeFromRedirect: curl_easy_perform result=" << res << ", location=" << data.location << std::endl;
    curl_easy_cleanup(curl);

    if (res == CURLE_OK && !data.location.empty()) {
        size_t codePos = data.location.find("code=");
        if (codePos != std::string::npos) {
            size_t start = codePos + 5;
            size_t end = data.location.find("&", start);
            if (end == std::string::npos) end = data.location.length();
            std::string codeVal = data.location.substr(start, end - start);
            std::cerr << "[SDK Auth Debug] getAuthCodeFromRedirect: extracted code=" << codeVal << std::endl;
            return codeVal;
        }
    }
    std::cerr << "[SDK Auth Debug] getAuthCodeFromRedirect: failed to extract code from redirect Location" << std::endl;
    return "";
}

bool HttpSseTransport::handleDefaultAuthRetry(const std::string& wwwAuthenticateHeader) {
    std::cerr << "[SDK Auth Debug] Received WWW-Authenticate: " << wwwAuthenticateHeader << std::endl;
    if (m_onError) m_onError("[SDK Auth] Received WWW-Authenticate: " + wwwAuthenticateHeader);

    std::string resourceMetadataUrl = extractResourceMetadataUrl(wwwAuthenticateHeader);
    std::cerr << "[SDK Auth Debug] Extracted resourceMetadataUrl: " << resourceMetadataUrl << std::endl;
    std::string prmJsonStr;
    nlohmann::json prmJson;
    if (resourceMetadataUrl.empty()) {
        if (m_onError) m_onError("[SDK Auth] No resource_metadata in WWW-Authenticate, trying path-based discovery...");

        std::string baseUrl = m_sseUrl;
        size_t pathStart = baseUrl.find("://");
        if (pathStart != std::string::npos) {
            pathStart = baseUrl.find('/', pathStart + 3);
            if (pathStart != std::string::npos) {
                baseUrl = baseUrl.substr(0, pathStart);
            }
        }

        // 按 MCP 规范优先级尝试多个 well-known 路径
        std::vector<std::string> prmPaths = {
            "/.well-known/oauth-protected-resource/mcp",
            "/.well-known/oauth-protected-resource",
            "/.well-known/mcp-protected-resource-metadata"
        };
        for (const auto& p : prmPaths) {
            std::string candidateUrl = baseUrl + p;
            std::cerr << "[SDK Auth Debug] Trying path-based PRM URL: " << candidateUrl << std::endl;
            std::string candidateJson = curlHttpGet(candidateUrl);
            auto candidatePrm = nlohmann::json::parse(candidateJson, nullptr, false);
            if (!candidatePrm.is_discarded() && candidatePrm.contains("authorization_servers") &&
                candidatePrm["authorization_servers"].is_array() && !candidatePrm["authorization_servers"].empty()) {
                resourceMetadataUrl = candidateUrl;
                prmJsonStr = candidateJson;
                prmJson = candidatePrm;
                std::cerr << "[SDK Auth Debug] Found PRM at: " << candidateUrl << std::endl;
                break;
            }
        }

        if (resourceMetadataUrl.empty()) {
            std::cerr << "[SDK Auth Debug] All path-based PRM attempts failed" << std::endl;
            if (m_onError) m_onError("[SDK Auth] All path-based PRM discovery attempts failed");
            return false;
        }
    }

    // 如果 fallback 已经取到并解析了 PRM，跳过重复获取
    if (prmJsonStr.empty()) {
        std::cerr << "[SDK Auth Debug] Fetching Protected Resource Metadata from: " << resourceMetadataUrl << std::endl;
        if (m_onError) m_onError("[SDK Auth] Fetching Protected Resource Metadata from " + resourceMetadataUrl);
        prmJsonStr = curlHttpGet(resourceMetadataUrl);
        std::cerr << "[SDK Auth Debug] PRM Response: " << prmJsonStr << std::endl;
        prmJson = nlohmann::json::parse(prmJsonStr, nullptr, false);
    }
    if (prmJson.is_discarded() || !prmJson.contains("authorization_servers") || !prmJson["authorization_servers"].is_array() || prmJson["authorization_servers"].empty()) {
        std::cerr << "[SDK Auth Debug] Invalid Protected Resource Metadata JSON" << std::endl;
        if (m_onError) m_onError("[SDK Auth] Invalid Protected Resource Metadata JSON");
        return false;
    }

    std::string issuerUrl = prmJson["authorization_servers"][0].get<std::string>();
    std::cerr << "[SDK Auth Debug] Discovered issuer: " << issuerUrl << std::endl;
    if (m_onError) m_onError("[SDK Auth] Discovered issuer: " + issuerUrl);

    // 如果 PRM 提供了 oauthMetadataLocation，优先使用它（用于 var2/var3 等变体）
    std::string prmMetadataLoc;
    if (prmJson.contains("oauthMetadataLocation") && prmJson["oauthMetadataLocation"].is_string()) {
        prmMetadataLoc = prmJson["oauthMetadataLocation"].get<std::string>();
        std::cerr << "[SDK Auth Debug] PRM provides oauthMetadataLocation: " << prmMetadataLoc << std::endl;
    }

    OAuthServerMetadata serverMetadata;
    std::string err;
    std::string metadataUrl = issuerUrl;

    // 如果有 PRM 指定的 metadata 路径（绝对路径），基于 MCP 服务 URL 构建
    if (!prmMetadataLoc.empty()) {
        std::string baseUrl = m_sseUrl;
        size_t pathStart = baseUrl.find("://");
        if (pathStart != std::string::npos) {
            pathStart = baseUrl.find('/', pathStart + 3);
            if (pathStart != std::string::npos) {
                baseUrl = baseUrl.substr(0, pathStart);
            }
        }
        metadataUrl = baseUrl + prmMetadataLoc;
        std::cerr << "[SDK Auth Debug] Using PRM-specified metadata URL: " << metadataUrl << std::endl;
    }

    std::cerr << "[SDK Auth Debug] Starting Metadata Discovery from: " << metadataUrl << std::endl;
    if (!m_oauthClient->discoverMetadataSync(metadataUrl, &serverMetadata, &err)) {
        std::cerr << "[SDK Auth Debug] Metadata discovery failed: " << err << std::endl;
        if (m_onError) m_onError("[SDK Auth] Metadata discovery failed: " + err);
        return false;
    }
    std::cerr << "[SDK Auth Debug] Metadata Discovery Success. TokenEndpoint=" << serverMetadata.tokenEndpoint << ", RegistrationEndpoint=" << serverMetadata.registrationEndpoint << std::endl;

    std::string scenario = getEnvVar("MCP_CONFORMANCE_SCENARIO", "");
    std::string contextStr = getEnvVar("MCP_CONFORMANCE_CONTEXT", "{}");
    std::cerr << "[SDK Auth Debug] Current Conformance Scenario=" << scenario << ", Context=" << contextStr << std::endl;

    nlohmann::json ctx = nlohmann::json::parse(contextStr, nullptr, false);
    if (ctx.is_discarded()) {
        ctx = nlohmann::json::object();
    }
    std::string ctxClientId = ctx.value("client_id", "");
    std::string ctxClientSecret = ctx.value("client_secret", "");

    bool hasPreRegisteredCredentials = !ctxClientId.empty();

    std::string clientId;
    std::string clientSecret;

    if (!ctxClientId.empty()) {
        clientId = ctxClientId;
        clientSecret = ctxClientSecret;
        std::cerr << "[SDK Auth Debug] Using Client credentials from context: clientId=" << clientId << std::endl;
    } else if (scenario == "auth/basic-cimd") {
        clientId = "https://conformance-test.local/client-metadata.json";
        clientSecret = "";
        std::cerr << "[SDK Auth Debug] Using hardcoded clientId for auth/basic-cimd: " << clientId << std::endl;
    } else if (!serverMetadata.registrationEndpoint.empty()) {
        std::cerr << "[SDK Auth Debug] Performing Dynamic Client Registration..." << std::endl;
        if (m_onError) m_onError("[SDK Auth] Performing Dynamic Client Registration...");
        OAuthClientRegistration reg;
        std::string regErr;
        if (m_oauthClient->registerClientSync(serverMetadata.registrationEndpoint, "conformance-client-cpp", {"http://localhost:3000/callback"}, &reg, &regErr)) {
            clientId = reg.clientId;
            clientSecret = reg.clientSecret;
            std::cerr << "[SDK Auth Debug] DCR Success. clientId=" << clientId << ", clientSecret=" << clientSecret << std::endl;
            if (m_onError) m_onError("[SDK Auth] DCR Success. clientId: " + clientId);
        } else {
            std::cerr << "[SDK Auth Debug] DCR failed: " << regErr << std::endl;
            if (m_onError) m_onError("[SDK Auth] DCR failed: " + regErr);
            return false;
        }
    }

    bool isJwtBearer = false;
    for (const auto& gt : serverMetadata.grantTypesSupported) {
        if (gt == "urn:ietf:params:oauth:grant-type:jwt-bearer") {
            isJwtBearer = true;
            break;
        }
    }

    if (isJwtBearer && ctx.contains("idp_id_token") && !ctx["idp_id_token"].get<std::string>().empty()) {
        std::string assertion;

        if (ctx.contains("idp_token_endpoint") && !ctx["idp_token_endpoint"].get<std::string>().empty()) {
            std::string idpTokenEndpoint = ctx["idp_token_endpoint"].get<std::string>();
            std::string idpClientId = ctx.value("idp_client_id", "");
            std::string idpClientSecret = ctx.value("idp_client_secret", "");
            std::string idToken = ctx["idp_id_token"].get<std::string>();

            std::cerr << "[SDK Auth Debug] Cross-App Flow: Exchanging ID Token for ID-JAG at IdP: " << idpTokenEndpoint << std::endl;

            CURL* curlIdp = curl_easy_init();
            if (!curlIdp) {
                std::cerr << "[SDK Auth Debug] Failed to init curl for IdP token exchange" << std::endl;
                return false;
            }

            auto urlEncodeIdp = [curlIdp](const std::string& value) -> std::string {
                char* escaped = curl_easy_escape(curlIdp, value.c_str(), static_cast<int>(value.length()));
                std::string res;
                if (escaped) {
                    res = escaped;
                    curl_free(escaped);
                } else {
                    res = value;
                }
                return res;
            };

            std::string idpResponse;
            curl_easy_setopt(curlIdp, CURLOPT_URL, idpTokenEndpoint.c_str());
            curl_easy_setopt(curlIdp, CURLOPT_POST, 1L);

            std::vector<std::pair<std::string, std::string>> idpParams = {
                {"grant_type", "urn:ietf:params:oauth:grant-type:token-exchange"},
                {"requested_token_type", "urn:ietf:params:oauth:token-type:id-jag"},
                {"audience", issuerUrl},
                {"resource", m_sseUrl},
                {"subject_token", idToken},
                {"subject_token_type", "urn:ietf:params:oauth:token-type:id_token"},
                {"client_id", idpClientId}
            };
            if (!idpClientSecret.empty()) {
                idpParams.push_back({"client_secret", idpClientSecret});
            }

            std::string idpPostFields;
            for (const auto& param : idpParams) {
                if (!idpPostFields.empty()) {
                    idpPostFields += "&";
                }
                idpPostFields += urlEncodeIdp(param.first) + "=" + urlEncodeIdp(param.second);
            }

            curl_easy_setopt(curlIdp, CURLOPT_POSTFIELDS, idpPostFields.c_str());

            struct curl_slist* idpHeaders = nullptr;
            idpHeaders = curl_slist_append(idpHeaders, "Content-Type: application/x-www-form-urlencoded");
            idpHeaders = curl_slist_append(idpHeaders, "Accept: application/json");
            curl_easy_setopt(curlIdp, CURLOPT_HTTPHEADER, idpHeaders);

            curl_easy_setopt(curlIdp, CURLOPT_WRITEFUNCTION, curlGetCallback);
            curl_easy_setopt(curlIdp, CURLOPT_WRITEDATA, &idpResponse);

            CURLcode idpRes = curl_easy_perform(curlIdp);
            std::cerr << "[SDK Auth Debug] IdP Token Exchange perform result=" << idpRes << ", response=" << idpResponse << std::endl;

            curl_slist_free_all(idpHeaders);
            curl_easy_cleanup(curlIdp);

            if (idpRes != CURLE_OK) {
                std::cerr << "[SDK Auth Debug] IdP Token Exchange curl failed" << std::endl;
                return false;
            }

            auto idpTokenJson = nlohmann::json::parse(idpResponse, nullptr, false);
            if (idpTokenJson.is_discarded() || !idpTokenJson.contains("access_token")) {
                std::cerr << "[SDK Auth Debug] IdP Token Exchange response invalid or missing access_token" << std::endl;
                return false;
            }

            assertion = idpTokenJson["access_token"].get<std::string>();
            std::cerr << "[SDK Auth Debug] Successfully obtained ID-JAG token" << std::endl;
        } else {
            assertion = ctx["idp_id_token"].get<std::string>();
        }

        std::cerr << "[SDK Auth Debug] Performing JWT-Bearer Exchange. URL=" << serverMetadata.tokenEndpoint << ", clientId=" << clientId << std::endl;

        CURL* curl = curl_easy_init();
        if (!curl) return false;

        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, serverMetadata.tokenEndpoint.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);

        char* escapedAssertion = curl_easy_escape(curl, assertion.c_str(), static_cast<int>(assertion.length()));
        std::string postFields = "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion=" + std::string(escapedAssertion ? escapedAssertion : assertion.c_str());
        if (escapedAssertion) curl_free(escapedAssertion);

        std::cerr << "[SDK Auth Debug] JWT-Bearer POST fields: " << postFields << std::endl;
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postFields.c_str());

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
        headers = curl_slist_append(headers, "Accept: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        if (!clientSecret.empty()) {
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            std::string userpwd = clientId + ":" + clientSecret;
            curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
        }

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlGetCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        CURLcode res = curl_easy_perform(curl);
        std::cerr << "[SDK Auth Debug] JWT-Bearer perform result=" << res << ", response=" << response << std::endl;
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res == CURLE_OK) {
            auto tokenJson = nlohmann::json::parse(response, nullptr, false);
            if (!tokenJson.is_discarded() && tokenJson.contains("access_token")) {
                OAuthToken token;
                token.accessToken = tokenJson["access_token"].get<std::string>();
                if (tokenJson.contains("refresh_token")) token.refreshToken = tokenJson["refresh_token"].get<std::string>();
                token.expiresIn = tokenJson.value("expires_in", 0);
                token.obtainedAt = std::chrono::steady_clock::now();
                m_oauthClient->setCurrentToken(token);
                std::cerr << "[SDK Auth Debug] JWT-Bearer Token Exchange Success!" << std::endl;
                return true;
            }
        }
        std::cerr << "[SDK Auth Debug] JWT-Bearer Token Exchange Failed" << std::endl;
        return false;
    }

    if (scenario == "auth/client-credentials-basic") {
        std::cerr << "[SDK Auth Debug] Exchange Client Credentials..." << std::endl;
        if (m_onError) m_onError("[SDK Auth] Exchange Client Credentials...");
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, serverMetadata.tokenEndpoint.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);

        std::string postFields = "grant_type=client_credentials&client_id=" + clientId + "&client_secret=" + clientSecret;
        std::cerr << "[SDK Auth Debug] Client Credentials POST fields: " << postFields << std::endl;
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postFields.c_str());

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
        headers = curl_slist_append(headers, "Accept: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        std::string userpwd = clientId + ":" + clientSecret;
        curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlGetCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        CURLcode res = curl_easy_perform(curl);
        std::cerr << "[SDK Auth Debug] Client Credentials perform result=" << res << ", response=" << response << std::endl;
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res == CURLE_OK) {
            auto tokenJson = nlohmann::json::parse(response, nullptr, false);
            if (!tokenJson.is_discarded() && tokenJson.contains("access_token")) {
                OAuthToken token;
                token.accessToken = tokenJson["access_token"].get<std::string>();
                if (tokenJson.contains("refresh_token")) token.refreshToken = tokenJson["refresh_token"].get<std::string>();
                token.expiresIn = tokenJson.value("expires_in", 0);
                token.obtainedAt = std::chrono::steady_clock::now();
                m_oauthClient->setCurrentToken(token);
                std::cerr << "[SDK Auth Debug] Client Credentials Token Exchange Success!" << std::endl;
                if (m_onError) m_onError("[SDK Auth] Client Credentials Token Exchange Success!");
                return true;
            }
        }
        std::cerr << "[SDK Auth Debug] Client Credentials Token Exchange Failed" << std::endl;
        if (m_onError) m_onError("[SDK Auth] Client Credentials Token Exchange Failed");
        return false;
    }

    // 确定 scope：优先 WWW-Authenticate header，其次 PRM scopes_supported，
    // 再其次 metadata scopes_supported，最后省略
    std::vector<std::string> scopes;
    std::string reqScope = extractScope(wwwAuthenticateHeader);
    if (!reqScope.empty()) {
        std::istringstream iss(reqScope);
        std::string s;
        while (iss >> s) scopes.push_back(s);
    } else if (prmJson.contains("scopes_supported") && prmJson["scopes_supported"].is_array()) {
        for (auto& s : prmJson["scopes_supported"]) {
            scopes.push_back(s.get<std::string>());
        }
        std::cerr << "[SDK Auth Debug] Using scopes from PRM scopes_supported" << std::endl;
    } else if (!serverMetadata.scopesSupported.empty()) {
        scopes = serverMetadata.scopesSupported;
        std::cerr << "[SDK Auth Debug] Using scopes from metadata scopes_supported" << std::endl;
    } else {
        std::cerr << "[SDK Auth Debug] No scope available, omitting scope parameter" << std::endl;
    }

    // RFC 8707: resource = MCP 服务器 URL
    std::string resourceUrl = m_sseUrl;

    // 检测是否需要 client_secret_basic
    bool useBasicAuth = hasPreRegisteredCredentials;
    for (const auto& am : serverMetadata.tokenEndpointAuthMethodsSupported) {
        if (am == "client_secret_basic") {
            useBasicAuth = true;
            break;
        }
    }

    std::cerr << "[SDK Auth Debug] Building Authorization URL..." << std::endl;
    if (m_onError) m_onError("[SDK Auth] Building Authorization URL...");
    auto authReq = m_oauthClient->buildAuthorizationUrl(serverMetadata, clientId, "http://localhost:3000/callback", scopes, resourceUrl);
    std::cerr << "[SDK Auth Debug] Auth URL: " << authReq.authorizationUrl << std::endl;
    if (m_onError) m_onError("[SDK Auth] Auth URL: " + authReq.authorizationUrl);

    std::cerr << "[SDK Auth Debug] Requesting Auth URL to get code..." << std::endl;
    if (m_onError) m_onError("[SDK Auth] Requesting Auth URL to get code...");
    std::string code = getAuthCodeFromRedirect(authReq.authorizationUrl);
    if (code.empty()) {
        std::cerr << "[SDK Auth Debug] Failed to get auth code" << std::endl;
        if (m_onError) m_onError("[SDK Auth] Failed to get auth code");
        return false;
    }
    std::cerr << "[SDK Auth Debug] Got Auth Code: " << code << std::endl;
    if (m_onError) m_onError("[SDK Auth] Got Auth Code: " + code);

    OAuthToken token;
    std::string exErr;
    std::cerr << "[SDK Auth Debug] Starting exchangeCodeSync... TokenEndpoint=" << serverMetadata.tokenEndpoint << ", clientId=" << clientId << " basic=" << useBasicAuth << std::endl;
    if (!m_oauthClient->exchangeCodeSync(serverMetadata.tokenEndpoint, clientId, clientSecret, code, "http://localhost:3000/callback", authReq.codeVerifier, &token, &exErr, std::chrono::milliseconds(10000), resourceUrl, useBasicAuth)) {
        std::cerr << "[SDK Auth Debug] Token exchange failed: " << exErr << std::endl;
        if (m_onError) m_onError("[SDK Auth] Token exchange failed: " + exErr);
        return false;
    }

    m_oauthClient->setCurrentToken(token);
    if (m_onError) m_onError("[SDK Auth] Token Exchange Success!");
    return true;
}

} // namespace mcp
