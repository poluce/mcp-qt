#include "QtSseParser.h"

#include <sstream>

namespace mcp_qt {

void QtSseParser::setEventCallback(EventCallback callback) { m_eventCallback = std::move(callback); }
void QtSseParser::setRetryCallback(RetryCallback callback) { m_retryCallback = std::move(callback); }
void QtSseParser::setIdCallback(IdCallback callback) { m_idCallback = std::move(callback); }

void QtSseParser::pushChunk(const std::string& chunk) {
    m_buffer += chunk;
    while (true) {
        std::size_t posCrlf = m_buffer.find("\r\n\r\n");
        std::size_t posLf = m_buffer.find("\n\n");
        
        std::size_t pos = std::min(posCrlf, posLf);
        if (pos == std::string::npos) break;

        std::size_t len = (pos == posCrlf) ? 4 : 2;
        const std::string block = m_buffer.substr(0, pos);
        m_buffer.erase(0, pos + len);
        flushEventBlock(block);
    }
}

// 去除字符串前导空格（SSE 规范允许字段值前有一个可选空格）
static void trimLeadingSpace(std::string& s) {
    if (!s.empty() && s.front() == ' ') {
        s.erase(0, 1);
    }
}

void QtSseParser::flushEventBlock(const std::string& block) {
    QtSseEvent event;
    std::istringstream stream(block);
    std::string line;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("event:", 0) == 0) {
            event.eventName = line.substr(6);
            trimLeadingSpace(event.eventName);
        } else if (line.rfind("data:", 0) == 0) {
            std::string part = line.substr(5);
            trimLeadingSpace(part);
            if (!event.data.empty()) {
                event.data += "\n";
            }
            event.data += part;
        } else if (line.rfind("id:", 0) == 0) {
            event.lastEventId = line.substr(3);
            trimLeadingSpace(event.lastEventId);
            if (m_idCallback) m_idCallback(event.lastEventId);
        } else if (line.rfind("retry:", 0) == 0 && m_retryCallback) {
            std::string value = line.substr(6);
            trimLeadingSpace(value);
            try {
                m_retryCallback(std::stoi(value));
            } catch (...) {
            }
        }
    }

    if (!event.data.empty() && m_eventCallback) {
        m_eventCallback(event);
    }
}

} // namespace mcp_qt
