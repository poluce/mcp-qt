#pragma once

#include <functional>
#include <string>

namespace mcp_qt {

struct QtSseEvent {
    std::string eventName{"message"};
    std::string data;
    std::string lastEventId;
};

class QtSseParser {
public:
    using EventCallback = std::function<void(const QtSseEvent&)>;
    using RetryCallback = std::function<void(int)>;
    using IdCallback = std::function<void(const std::string&)>;

    void setEventCallback(EventCallback callback);
    void setRetryCallback(RetryCallback callback);
    void setIdCallback(IdCallback callback);
    void pushChunk(const std::string& chunk);

private:
    void flushEventBlock(const std::string& block);

    std::string m_buffer;
    EventCallback m_eventCallback;
    RetryCallback m_retryCallback;
    IdCallback m_idCallback;
};

} // namespace mcp_qt
