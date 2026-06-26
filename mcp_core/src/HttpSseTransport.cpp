#include "mcp_core/HttpSseTransport.h"
#include <curl/curl.h>
#include <sstream>
#include <chrono>
#include <mutex>

namespace mcp {

// 从相对路径解析为完整 URL
static std::string resolveUrl(const std::string& base, const std::string& relative) {
    if (relative.empty()) return base;
    if (relative.find("://") != std::string::npos) return relative;
    if (relative[0] == '/') {
        // 绝对路径：替换 base 的 path 部分
        auto schemeEnd = base.find("://");
        if (schemeEnd != std::string::npos) {
            auto hostStart = schemeEnd + 3;
            auto pathStart = base.find('/', hostStart);
            if (pathStart != std::string::npos) {
                return base.substr(0, pathStart) + relative;
            }
        }
        return base + relative;
    }
    // 相对路径：基于 base 的目录
    auto lastSlash = base.rfind('/');
    if (lastSlash != std::string::npos) {
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

// POST 响应头回调（提取 MCP-Session-Id）
static size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* sessionId = static_cast<std::string*>(userdata);
    std::string header(buffer, size * nitems);

    // 查找 MCP-Session-Id 响应头
    if (header.compare(0, 16, "MCP-Session-Id:") == 0) {
        *sessionId = header.substr(15);
        // 去掉 \r\n 和前导空格
        size_t s = sessionId->find_first_not_of(" \t\r\n");
        if (s != std::string::npos) sessionId->erase(0, s);
        size_t e = sessionId->find_last_not_of("\r\n");
        if (e != std::string::npos) sessionId->erase(e + 1);
    }
    return size * nitems;
}

// SSE 流式接收回调
static size_t sseWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* transport = static_cast<HttpSseTransport*>(userdata);
    transport->onSseData(ptr, size * nmemb);
    return size * nmemb;
}

HttpSseTransport::HttpSseTransport(const std::string& sseUrl)
    : m_sseUrl(sseUrl)
{
    ensureCurlInit();

    // 默认 POST endpoint（SSE endpoint 事件可能覆盖）
    m_postUrl = resolveUrl(sseUrl, "message");
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

    std::lock_guard<std::mutex> lock(m_sendMutex);
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
        m_sseThread.join();
    }

    if (m_onClose) m_onClose();
}

// SSE 流式数据处理：累积缓冲区，按 "\n\n" 分割 SSE 事件块
void HttpSseTransport::onSseData(const char* data, size_t len) {
    m_sseBuffer.append(data, len);

    size_t pos;
    while ((pos = m_sseBuffer.find("\n\n")) != std::string::npos) {
        std::string block = m_sseBuffer.substr(0, pos);
        m_sseBuffer.erase(0, pos + 2);

        std::string eventType = "message";
        std::string dataContent;

        // 解析 SSE 字段
        std::istringstream stream(block);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();

            if (line.compare(0, 6, "event:") == 0) {
                eventType = line.substr(6);
                size_t s = eventType.find_first_not_of(" \t");
                if (s != std::string::npos) eventType = eventType.substr(s);
            } else if (line.compare(0, 5, "data:") == 0) {
                dataContent = line.substr(5);
                size_t s = dataContent.find_first_not_of(" \t");
                if (s != std::string::npos) dataContent = dataContent.substr(s);
            }
        }

        if (!dataContent.empty()) {
            if (eventType == "endpoint") {
                // MCP 规范：服务端通过 SSE endpoint 事件告知 POST 地址
                m_postUrl = resolveUrl(m_sseUrl, dataContent);
            } else if (m_onMessage) {
                m_onMessage(dataContent);
            }
        }
    }
}

// SSE 读取主循环（后台线程），支持断线重连
void HttpSseTransport::sseReadLoop() {
    while (m_running) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            if (m_onError) m_onError("curl_easy_init failed");
            break;
        }

        // 清空 SSE 缓冲区
        m_sseBuffer.clear();

        // 配置 SSE GET 请求
        curl_easy_setopt(curl, CURLOPT_URL, m_sseUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sseWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L); // SSE 长连接不设超时

        // 设置请求头
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Accept: text/event-stream");
        headers = curl_slist_append(headers, "MCP-Protocol-Version: 2025-11-25");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        if (m_onError) m_onError("SSE connecting to: " + m_sseUrl);

        // 发起 SSE GET 请求（阻塞直到连接关闭）
        CURLcode res = curl_easy_perform(curl);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (!m_running) break;

        // 连接已断开
        if (m_onError) {
            if (res == CURLE_OK) {
                m_onError("SSE disconnected. Reconnecting in 2s...");
            } else {
                m_onError(std::string("SSE error: ") + curl_easy_strerror(res) + ". Reconnecting in 2s...");
            }
        }

        // 等待 2 秒后重连
        for (int i = 0; i < 20 && m_running; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

// 发送 HTTP POST 请求
bool HttpSseTransport::doPost(const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string responseBody;

    curl_easy_setopt(curl, CURLOPT_URL, m_postUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, postWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    // 设置请求头
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json, text/event-stream");
    headers = curl_slist_append(headers, "MCP-Protocol-Version: 2025-11-25");

    if (!m_sessionId.empty()) {
        std::string sid = "MCP-Session-Id: " + m_sessionId;
        headers = curl_slist_append(headers, sid.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // 设置响应头回调以提取 MCP-Session-Id
    std::string responseSessionId;
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &responseSessionId);

    CURLcode res = curl_easy_perform(curl);

    // 更新 session ID
    if (res == CURLE_OK && !responseSessionId.empty()) {
        m_sessionId = responseSessionId;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        if (m_onError) {
            m_onError(std::string("HTTP POST failed: ") + curl_easy_strerror(res));
        }
        return false;
    }

    // 解析响应中的 SSE 数据行（服务端可能在 POST 响应中直接返回 SSE 流）
    if (!responseBody.empty()) {
        // 检查是否是 SSE 格式
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
        }
    }

    return true;
}

} // namespace mcp
