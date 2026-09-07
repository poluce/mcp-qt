#include "QtProcessStdioWorker.h"

#include <QCoreApplication>
#include <QDebug>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QThread>
#include <QUrl>

#include <string_view>

namespace mcp_qt {

namespace {

// systemProxyForQuery 在部分 Windows/PAC 环境下可能阻塞很久；进程级缓存一次即可。
QString cachedSystemProxyUrl()
{
    static const QString kProxy = []() -> QString {
        const QList<QNetworkProxy> proxies = QNetworkProxyFactory::systemProxyForQuery(
            QNetworkProxyQuery(QUrl(QStringLiteral("https://example.com"))));
        if (proxies.isEmpty()) {
            return {};
        }

        const QNetworkProxy &proxy = proxies.first();
        if (proxy.type() != QNetworkProxy::HttpProxy && proxy.type() != QNetworkProxy::Socks5Proxy) {
            return {};
        }
        if (proxy.hostName().isEmpty() || proxy.port() <= 0) {
            return {};
        }

        const QString scheme = (proxy.type() == QNetworkProxy::HttpProxy)
            ? QStringLiteral("http")
            : QStringLiteral("socks5");
        return QStringLiteral("%1://%2:%3").arg(scheme, proxy.hostName()).arg(proxy.port());
    }();
    return kProxy;
}

bool envHasProxy(const std::unordered_map<std::string, std::string> &env)
{
    return env.count("HTTP_PROXY") || env.count("http_proxy")
        || env.count("HTTPS_PROXY") || env.count("https_proxy");
}

void insertProxyEnv(QProcessEnvironment &env, const QString &proxyUrl)
{
    env.insert(QStringLiteral("HTTP_PROXY"), proxyUrl);
    env.insert(QStringLiteral("HTTPS_PROXY"), proxyUrl);
    env.insert(QStringLiteral("http_proxy"), proxyUrl);
    env.insert(QStringLiteral("https_proxy"), proxyUrl);
}

} // namespace

QtProcessStdioWorker::QtProcessStdioWorker(const std::string& command, const std::vector<std::string>& args)
    : m_command(command), m_args(args) {}

QtProcessStdioWorker::~QtProcessStdioWorker() {
    stop();
}

void QtProcessStdioWorker::setEnvironment(const std::unordered_map<std::string, std::string>& env) {
    m_env = env;
}

void QtProcessStdioWorker::start() {
    if (m_started) return;

    // QProcess 必须在 I/O 线程内创建（本方法经 McpIoContext::post 投递执行）。
    m_process = new QProcess(this);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &QtProcessStdioWorker::handleReadyReadStandardOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, &QtProcessStdioWorker::handleReadyReadStandardError);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &QtProcessStdioWorker::handleProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &QtProcessStdioWorker::handleProcessError);

#ifdef _WIN32
    connect(m_process, &QProcess::started, this, [this]() {
        if (m_jobObject) {
            qint64 pid = m_process->processId();
            if (pid > 0) {
                HANDLE hProcess = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
                if (hProcess) {
                    AssignProcessToJobObject(m_jobObject, hProcess);
                    CloseHandle(hProcess);
                }
            }
        }
    });
#endif

    // 代理探测可能很慢：在 I/O 线程执行，不阻塞调用线程。
    bool userHasProxy = envHasProxy(m_env);
    QString proxyUrl;
    if (!userHasProxy) {
        proxyUrl = cachedSystemProxyUrl();
        if (!proxyUrl.isEmpty()) {
            qDebug() << "[QtProcessStdioTransport] Auto-detected system proxy:" << proxyUrl;
        }
    }

#ifdef _WIN32
    if (!m_jobObject) {
        m_jobObject = CreateJobObjectW(nullptr, nullptr);
        if (m_jobObject) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
            ZeroMemory(&info, sizeof(info));
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(m_jobObject, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
                CloseHandle(m_jobObject);
                m_jobObject = nullptr;
            }
        }
    }
#endif

    // 先注入系统代理，再叠加用户自定义环境变量（后者覆盖同名键）
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!proxyUrl.isEmpty()) {
        insertProxyEnv(env, proxyUrl);
    }
    for (const auto &kv : m_env) {
        env.insert(QString::fromStdString(kv.first), QString::fromStdString(kv.second));
    }
    m_process->setProcessEnvironment(env);

    QStringList qargs;
    for (const auto &a : m_args) {
        qargs.push_back(QString::fromStdString(a));
    }

    m_process->start(QString::fromStdString(m_command), qargs);
    m_started = true;
}

void QtProcessStdioWorker::stop() {
#ifdef _WIN32
    if (m_jobObject) {
        CloseHandle(m_jobObject);
        m_jobObject = nullptr;
    }
#endif
    if (!m_started) return;
    m_started = false;

    if (m_process && m_process->state() != QProcess::NotRunning) {
        // I/O 线程可以安全等待子进程退出（原实现主线程禁止 waitForFinished）。
        m_process->terminate();
        if (!m_process->waitForFinished(500)) {
            m_process->kill();
            m_process->waitForFinished(500);
        }
    }

    emit transportClosed();
}

void QtProcessStdioWorker::postMessage(const QString& message) {
    if (!m_started || !m_process
        || (m_process->state() != QProcess::Running && m_process->state() != QProcess::Starting)) {
        return;
    }

    QByteArray data = message.toUtf8();
    m_process->write(data.constData(), data.size());
    m_process->write("\n", 1);
    m_process->waitForBytesWritten(100);
}

void QtProcessStdioWorker::handleReadyReadStandardOutput() {
    QByteArray data = m_process->readAllStandardOutput();

    std::vector<std::string> messages;
    m_buffer.append(data.constData(), data.size());

    size_t pos = 0;
    while ((pos = m_buffer.find('\n')) != std::string::npos) {
        std::string line = m_buffer.substr(0, pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        messages.push_back(std::move(line));
        m_buffer.erase(0, pos + 1);
    }

    for (const auto& msg : messages) {
        if (msg.empty()) continue;

        auto start = msg.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;

        auto end = msg.find_last_not_of(" \t\r\n");
        std::string_view trimmed(msg.data() + start, end - start + 1);

        if (trimmed.front() == '{' && trimmed.back() == '}') {
            emit messageReceived(QString::fromStdString(msg));
        } else {
            qWarning() << "[QtProcessStdioTransport] Filtered out dirty stdout message:"
                       << QString::fromStdString(msg);
        }
    }
}

void QtProcessStdioWorker::handleReadyReadStandardError() {
    QByteArray data = m_process->readAllStandardError();
    // stderr 是服务端日志通道，不是传输层故障 —— 通过 serverLog 信号向上报告
    // 使用 fromLocal8Bit 兼容 Windows GBK 等本地编码（MCP 规范建议 UTF-8，但 Windows 子进程未必遵守）
    emit serverLog(QString::fromLocal8Bit(data));
}

void QtProcessStdioWorker::handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    Q_UNUSED(exitCode);
    Q_UNUSED(exitStatus);
    if (!m_started) return;
    m_started = false;
    emit transportClosed();
}

void QtProcessStdioWorker::handleProcessError(QProcess::ProcessError error) {
    if (!m_started) return;
    m_started = false;
    emit transportError(QStringLiteral("QProcess error occurred: %1").arg(static_cast<int>(error)));
    emit transportClosed();
}

} // namespace mcp_qt
