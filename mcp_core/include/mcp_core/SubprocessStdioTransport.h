#pragma once
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include "IMcpTransport.h"

namespace mcp {

/**
 * @brief Pure C++17 subprocess Stdio transport for MCP.
 *
 * Spawns a child process and communicates via stdin/stdout using line-delimited JSON.
 * Zero external dependencies — uses platform-native APIs (CreateProcess on Windows, fork/pipe on POSIX).
 */
class SubprocessStdioTransport : public IMcpTransport {
public:
    SubprocessStdioTransport(const std::string& program, const std::vector<std::string>& args = {});
    ~SubprocessStdioTransport() override;

    bool send(const std::string& message) override;
    void setOnMessage(std::function<void(const std::string&)> callback) override;
    void setOnClose(std::function<void()> callback) override;
    void setOnError(std::function<void(const std::string&)> callback) override;
    bool start() override;
    void close() override;

private:
    void readStdoutLoop();
    void readStderrLoop();

    std::string m_program;
    std::vector<std::string> m_args;

    std::function<void(const std::string&)> m_onMessage;
    std::function<void()> m_onClose;
    std::function<void(const std::string&)> m_onError;

    std::thread m_stdoutThread;
    std::thread m_stderrThread;
    std::atomic<bool> m_running{false};

#ifdef _WIN32
    void* m_hChildStdinWr = nullptr;   // HANDLE
    void* m_hChildStdoutRd = nullptr;  // HANDLE
    void* m_hChildStderrRd = nullptr;  // HANDLE
    void* m_hProcess = nullptr;        // HANDLE
#else
    int m_stdinFd = -1;
    int m_stdoutFd = -1;
    int m_stderrFd = -1;
    pid_t m_childPid = -1;
#endif
};

} // namespace mcp
