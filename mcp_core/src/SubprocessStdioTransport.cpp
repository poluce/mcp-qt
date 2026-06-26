#include "mcp_core/SubprocessStdioTransport.h"
#include <cstring>

#ifdef _WIN32
// Windows 实现
#include <windows.h>

namespace mcp {

SubprocessStdioTransport::SubprocessStdioTransport(
    const std::string& program, const std::vector<std::string>& args)
    : m_program(program), m_args(args) {}

SubprocessStdioTransport::~SubprocessStdioTransport() {
    close();
}

bool SubprocessStdioTransport::start() {
    if (m_running) return false;

    // 创建三组管道：stdin(父写→子读)、stdout(子写→父读)、stderr(子写→父读)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hChildStdinRd = nullptr, hChildStdinWr = nullptr;
    HANDLE hChildStdoutRd = nullptr, hChildStdoutWr = nullptr;
    HANDLE hChildStderrRd = nullptr, hChildStderrWr = nullptr;

    if (!CreatePipe(&hChildStdinRd, &hChildStdinWr, &sa, 0)) return false;
    if (!CreatePipe(&hChildStdoutRd, &hChildStdoutWr, &sa, 0)) {
        CloseHandle(hChildStdinRd); CloseHandle(hChildStdinWr);
        return false;
    }
    if (!CreatePipe(&hChildStderrRd, &hChildStderrWr, &sa, 0)) {
        CloseHandle(hChildStdinRd); CloseHandle(hChildStdinWr);
        CloseHandle(hChildStdoutRd); CloseHandle(hChildStdoutWr);
        return false;
    }

    // 父进程端不继承，子进程端需要继承
    SetHandleInformation(hChildStdinWr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hChildStdoutRd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hChildStderrRd, HANDLE_FLAG_INHERIT, 0);

    // 拼接命令行
    std::string cmdLine = "\"" + m_program + "\"";
    for (const auto& arg : m_args) {
        cmdLine += " \"" + arg + "\"";
    }

    // 配置子进程启动信息
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hChildStdinRd;
    si.hStdOutput = hChildStdoutWr;
    si.hStdError = hChildStderrWr;

    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessA(
        nullptr,
        const_cast<char*>(cmdLine.c_str()),
        nullptr, nullptr,
        TRUE,           // 继承句柄
        0,              // 无特殊创建标志
        nullptr, nullptr,
        &si, &pi
    );

    // 关闭子进程端的句柄（父进程不需要）
    CloseHandle(hChildStdinRd);
    CloseHandle(hChildStdoutWr);
    CloseHandle(hChildStderrWr);

    if (!ok) {
        CloseHandle(hChildStdinWr);
        CloseHandle(hChildStdoutRd);
        CloseHandle(hChildStderrRd);
        if (m_onError) {
            m_onError("CreateProcess failed for: " + m_program + ", error=" + std::to_string(GetLastError()));
        }
        return false;
    }

    // 保存父进程端句柄
    m_hChildStdinWr = hChildStdinWr;
    m_hChildStdoutRd = hChildStdoutRd;
    m_hChildStderrRd = hChildStderrRd;
    m_hProcess = pi.hProcess;
    CloseHandle(pi.hThread);

    m_running = true;

    // 启动 stdout/stderr 读取线程
    m_stdoutThread = std::thread(&SubprocessStdioTransport::readStdoutLoop, this);
    m_stderrThread = std::thread(&SubprocessStdioTransport::readStderrLoop, this);

    return true;
}

bool SubprocessStdioTransport::send(const std::string& message) {
    if (!m_running || !m_hChildStdinWr) return false;

    std::string line = message + "\n";
    DWORD written = 0;
    BOOL ok = WriteFile(
        m_hChildStdinWr,
        line.c_str(),
        static_cast<DWORD>(line.size()),
        &written,
        nullptr
    );
    return ok && written == line.size();
}

void SubprocessStdioTransport::setOnMessage(std::function<void(const std::string&)> cb) {
    m_onMessage = std::move(cb);
}

void SubprocessStdioTransport::setOnClose(std::function<void()> cb) {
    m_onClose = std::move(cb);
}

void SubprocessStdioTransport::setOnError(std::function<void(const std::string&)> cb) {
    m_onError = std::move(cb);
}

void SubprocessStdioTransport::readStdoutLoop() {
    char buf[4096];
    std::string lineBuf;

    while (m_running) {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(m_hChildStdoutRd, buf, sizeof(buf) - 1, &bytesRead, nullptr);
        if (!ok || bytesRead == 0) break;

        buf[bytesRead] = '\0';
        lineBuf.append(buf, bytesRead);

        // 逐行提取
        size_t pos;
        while ((pos = lineBuf.find('\n')) != std::string::npos) {
            std::string line = lineBuf.substr(0, pos);
            lineBuf.erase(0, pos + 1);

            // 去除 \r
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            if (m_onMessage) m_onMessage(line);
        }
    }
}

void SubprocessStdioTransport::readStderrLoop() {
    char buf[4096];
    std::string lineBuf;

    while (m_running) {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(m_hChildStderrRd, buf, sizeof(buf) - 1, &bytesRead, nullptr);
        if (!ok || bytesRead == 0) break;

        buf[bytesRead] = '\0';
        lineBuf.append(buf, bytesRead);

        size_t pos;
        while ((pos = lineBuf.find('\n')) != std::string::npos) {
            std::string line = lineBuf.substr(0, pos);
            lineBuf.erase(0, pos + 1);

            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            if (m_onError) m_onError("[Server Stderr] " + line);
        }
    }
}

void SubprocessStdioTransport::close() {
    if (!m_running) return;
    m_running = false;

    // 关闭 stdin 管道，通知子进程输入结束
    if (m_hChildStdinWr) {
        CloseHandle(m_hChildStdinWr);
        m_hChildStdinWr = nullptr;
    }

    // 终止子进程
    if (m_hProcess) {
        TerminateProcess(m_hProcess, 1);
        WaitForSingleObject(m_hProcess, 3000);
        CloseHandle(m_hProcess);
        m_hProcess = nullptr;
    }

    // 关闭读管道（这将解除 ReadFile 阻塞，使读线程退出）
    if (m_hChildStdoutRd) {
        CloseHandle(m_hChildStdoutRd);
        m_hChildStdoutRd = nullptr;
    }
    if (m_hChildStderrRd) {
        CloseHandle(m_hChildStderrRd);
        m_hChildStderrRd = nullptr;
    }

    if (m_stdoutThread.joinable()) m_stdoutThread.join();
    if (m_stderrThread.joinable()) m_stderrThread.join();

    if (m_onClose) m_onClose();
}

} // namespace mcp

#else
// POSIX 实现 (Linux / macOS)
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

namespace mcp {

SubprocessStdioTransport::SubprocessStdioTransport(
    const std::string& program, const std::vector<std::string>& args)
    : m_program(program), m_args(args) {}

SubprocessStdioTransport::~SubprocessStdioTransport() {
    close();
}

bool SubprocessStdioTransport::start() {
    if (m_running) return false;

    // 创建管道对
    int stdinPipe[2], stdoutPipe[2], stderrPipe[2];
    if (pipe(stdinPipe) != 0 || pipe(stdoutPipe) != 0 || pipe(stderrPipe) != 0) {
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        ::close(stdinPipe[0]); ::close(stdinPipe[1]);
        ::close(stdoutPipe[0]); ::close(stdoutPipe[1]);
        ::close(stderrPipe[0]); ::close(stderrPipe[1]);
        return false;
    }

    if (pid == 0) {
        // 子进程：重定向 stdin/stdout/stderr
        ::close(stdinPipe[1]);   // 关闭父进程端
        ::close(stdoutPipe[0]);
        ::close(stderrPipe[0]);

        dup2(stdinPipe[0], STDIN_FILENO);
        dup2(stdoutPipe[1], STDOUT_FILENO);
        dup2(stderrPipe[1], STDERR_FILENO);

        ::close(stdinPipe[0]);
        ::close(stdoutPipe[1]);
        ::close(stderrPipe[1]);

        // 构造 argv
        std::vector<const char*> argv;
        argv.push_back(m_program.c_str());
        for (const auto& arg : m_args) argv.push_back(arg.c_str());
        argv.push_back(nullptr);

        execvp(m_program.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127); // exec 失败
    }

    // 父进程
    ::close(stdinPipe[0]);   // 关闭子进程端
    ::close(stdoutPipe[1]);
    ::close(stderrPipe[1]);

    m_stdinFd = stdinPipe[1];
    m_stdoutFd = stdoutPipe[0];
    m_stderrFd = stderrPipe[0];
    m_childPid = pid;

    m_running = true;
    m_stdoutThread = std::thread(&SubprocessStdioTransport::readStdoutLoop, this);
    m_stderrThread = std::thread(&SubprocessStdioTransport::readStderrLoop, this);

    return true;
}

bool SubprocessStdioTransport::send(const std::string& message) {
    if (!m_running || m_stdinFd < 0) return false;

    std::string line = message + "\n";
    ssize_t written = ::write(m_stdinFd, line.c_str(), line.size());
    return written == static_cast<ssize_t>(line.size());
}

void SubprocessStdioTransport::setOnMessage(std::function<void(const std::string&)> cb) {
    m_onMessage = std::move(cb);
}

void SubprocessStdioTransport::setOnClose(std::function<void()> cb) {
    m_onClose = std::move(cb);
}

void SubprocessStdioTransport::setOnError(std::function<void(const std::string&)> cb) {
    m_onError = std::move(cb);
}

void SubprocessStdioTransport::readStdoutLoop() {
    char buf[4096];
    std::string lineBuf;

    while (m_running) {
        ssize_t n = ::read(m_stdoutFd, buf, sizeof(buf));
        if (n <= 0) break;

        lineBuf.append(buf, n);

        size_t pos;
        while ((pos = lineBuf.find('\n')) != std::string::npos) {
            std::string line = lineBuf.substr(0, pos);
            lineBuf.erase(0, pos + 1);

            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            if (m_onMessage) m_onMessage(line);
        }
    }
}

void SubprocessStdioTransport::readStderrLoop() {
    char buf[4096];
    std::string lineBuf;

    while (m_running) {
        ssize_t n = ::read(m_stderrFd, buf, sizeof(buf));
        if (n <= 0) break;

        lineBuf.append(buf, n);

        size_t pos;
        while ((pos = lineBuf.find('\n')) != std::string::npos) {
            std::string line = lineBuf.substr(0, pos);
            lineBuf.erase(0, pos + 1);

            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            if (m_onError) m_onError("[Server Stderr] " + line);
        }
    }
}

void SubprocessStdioTransport::close() {
    if (!m_running) return;
    m_running = false;

    // 关闭 stdin，通知子进程输入结束
    if (m_stdinFd >= 0) {
        ::close(m_stdinFd);
        m_stdinFd = -1;
    }

    // 终止子进程
    if (m_childPid > 0) {
        kill(m_childPid, SIGTERM);
        int status = 0;
        struct timespec ts { 3, 0 }; // 3 秒超时
        pid_t result = waitpid(m_childPid, &status, WNOHANG);
        if (result == 0) {
            // 超时后强杀
            kill(m_childPid, SIGKILL);
            waitpid(m_childPid, &status, 0);
        }
        m_childPid = -1;
    }

    // 关闭读管道（解除 read 阻塞）
    if (m_stdoutFd >= 0) {
        ::close(m_stdoutFd);
        m_stdoutFd = -1;
    }
    if (m_stderrFd >= 0) {
        ::close(m_stderrFd);
        m_stderrFd = -1;
    }

    if (m_stdoutThread.joinable()) m_stdoutThread.join();
    if (m_stderrThread.joinable()) m_stderrThread.join();

    if (m_onClose) m_onClose();
}

} // namespace mcp

#endif
