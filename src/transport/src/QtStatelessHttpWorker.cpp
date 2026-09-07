#include "QtStatelessHttpWorker.h"

#include "mcp_core/McpHeaderEncoding.h"
#include <nlohmann/json.hpp>
#include <QNetworkProxy>
#include <QDebug>

namespace mcp_qt {

QtStatelessHttpWorker::QtStatelessHttpWorker(const QUrl& endpointUrl)
    : m_endpointUrl(endpointUrl) {
    m_headers.insert("Content-Type", "application/json");
}

QtStatelessHttpWorker::~QtStatelessHttpWorker() {
    stop();
}

void QtStatelessHttpWorker::start() {
    if (m_isRunning) return;
    m_isRunning = true;
    // QNAM 必须在 I/O 线程内创建（本方法经 McpIoContext::post 投递执行）。
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
}

void QtStatelessHttpWorker::stop() {
    if (!m_isRunning) return;
    m_isRunning = false;
    m_sseListenerActive = false;

    if (m_sseReply) {
        disconnect(m_sseReply, nullptr, this, nullptr);
        m_sseReply->abort();
        m_sseReply->deleteLater();
        m_sseReply = nullptr;
    }

    emit transportClosed();
}

QString QtStatelessHttpWorker::currentBearerToken() const {
    return m_tokenProvider ? QString::fromStdString(m_tokenProvider()) : QString{};
}

void QtStatelessHttpWorker::applyCommonHeaders(QNetworkRequest& request, bool isGet) {
    Q_UNUSED(isGet);
    for (auto it = m_headers.constBegin(); it != m_headers.constEnd(); ++it) {
        request.setRawHeader(it.key(), it.value());
    }

    // Accept: 声明支持 JSON 和 SSE
    request.setRawHeader("Accept", "application/json, text/event-stream");

    // MCP 协议版本
    request.setRawHeader("MCP-Protocol-Version", QByteArray::fromStdString(m_protocolVersion));

    // Session ID（2026-07-28 已移除协议级会话，不再发送 MCP-Session-Id header）
    if (m_protocolVersion != "2026-07-28" && !m_sessionId.isEmpty()) {
        request.setRawHeader("MCP-Session-Id", m_sessionId.toUtf8());
    }

    // Authorization
    QString token = currentBearerToken();
    if (!token.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    }
}

void QtStatelessHttpWorker::postMessage(const QString& message) {
    if (!m_isRunning || !m_nam) return;

    // 检测 method 与 name（tools/call -> params.name，resources/read -> params.uri，prompts/get -> params.name），
    // 用于 2026-07-28 Mcp-Method / Mcp-Name HTTP Header 路由（SEP-2243）
    bool isInitializedNotification = false;
    std::string methodStr;
    std::string nameHeader;
    std::string bodyProtocolVersion;
    auto json = nlohmann::json::parse(message.toStdString(), nullptr, false);
    if (!json.is_discarded() && json.contains("method") && json["method"].is_string()) {
        methodStr = json["method"].get<std::string>();
        isInitializedNotification = (methodStr == "notifications/initialized");
        if (json.contains("params") && json["params"].is_object()) {
            const auto& params = json["params"];
            // 2026-07-28 Server Validation：header 版本必须与 body _meta 一致。
            // 以 body _meta 为准覆盖 header，避免 transport 默认版本(2025-11-25)
            // 与 stateless 请求 body 版本(2026-07-28)不一致导致 -32020 HeaderMismatch。
            if (params.contains("_meta") && params["_meta"].is_object()) {
                const auto& meta = params["_meta"];
                if (meta.contains("io.modelcontextprotocol/protocolVersion")
                    && meta["io.modelcontextprotocol/protocolVersion"].is_string()) {
                    bodyProtocolVersion = meta["io.modelcontextprotocol/protocolVersion"].get<std::string>();
                } else if (meta.contains("protocolVersion") && meta["protocolVersion"].is_string()) {
                    bodyProtocolVersion = meta["protocolVersion"].get<std::string>();
                }
            }
            if (methodStr == "tools/call") {
                if (params.contains("name") && params["name"].is_string()) {
                    nameHeader = params["name"].get<std::string>();
                }
            } else if (methodStr == "resources/read") {
                if (params.contains("uri") && params["uri"].is_string()) {
                    nameHeader = params["uri"].get<std::string>();
                }
            } else if (methodStr == "prompts/get") {
                if (params.contains("name") && params["name"].is_string()) {
                    nameHeader = params["name"].get<std::string>();
                }
            }
        }
    }

    // 缓存请求数据用于重试（非重试状态下）
    if (!m_isRetrying) {
        m_lastRequestData = message.toUtf8();
    }

    QNetworkRequest request(m_endpointUrl);
    applyCommonHeaders(request);

    // MCP 2026-07-28 Header 路由扩展
    if (!methodStr.empty()) {
        request.setRawHeader("Mcp-Method", QByteArray::fromStdString(methodStr));
    }
    if (!nameHeader.empty()) {
        // SEP-2243：非安全 ASCII 值须编码为 =?base64?<b64>?= sentinel
        request.setRawHeader("Mcp-Name", encodeMcpHeaderValue(nameHeader));
    }

    // x-mcp-header 扩展：附加 Mcp-Param-{Name} headers（2026-07-28, SEP-2243）
    for (const auto& [headerName, headerValue] : m_extraRequestHeaders) {
        // 防御 header 注入：名称或值含 CR/LF 的 header 一律跳过并记日志
        const bool hasCrLf = headerName.find('\r') != std::string::npos
                          || headerName.find('\n') != std::string::npos
                          || headerValue.find('\r') != std::string::npos
                          || headerValue.find('\n') != std::string::npos;
        if (hasCrLf) {
            qWarning() << "QtStatelessHttpTransport: skipping extra header"
                       << QString::fromStdString(headerName)
                       << "due to CR/LF in name or value (header injection guard)";
            continue;
        }
        request.setRawHeader(QByteArray::fromStdString(headerName),
                             QByteArray::fromStdString(headerValue));
    }

    // 若 body _meta 携带协议版本，header 以其为准（Server Validation 要求二者一致）
    if (!bodyProtocolVersion.empty()) {
        request.setRawHeader("MCP-Protocol-Version", QByteArray::fromStdString(bodyProtocolVersion));
    }

    QByteArray data = message.toUtf8();
    QNetworkReply* reply = m_nam->post(request, data);

    connect(reply, &QNetworkReply::finished, this, [this, reply, isInitializedNotification]() {
        onReplyFinished(reply, isInitializedNotification);

        // initialized 通知后启动 GET SSE 监听
        if (isInitializedNotification && m_isRunning) {
            startSseListener();
        }
    });
}

void QtStatelessHttpWorker::onReplyFinished(QNetworkReply* reply, bool isInitializedNotification) {
    Q_UNUSED(isInitializedNotification);
    reply->deleteLater();
    if (!m_isRunning) return;

    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // 提取 Session ID
    QByteArray sessionId = reply->rawHeader("MCP-Session-Id");
    if (!sessionId.isEmpty()) {
        m_sessionId = QString::fromUtf8(sessionId);
    }

    // 401 处理 - OAuth 重试
    if (statusCode == 401 || statusCode == 403) {
        QString wwwAuth = QString::fromUtf8(reply->rawHeader("WWW-Authenticate"));
        m_authRetryCount++;

        if (m_authRetryCount <= kMaxAuthRetries && m_authRetryHandler) {
            if (m_authRetryHandler(wwwAuth.toStdString())) {
                // OAuth 成功，重试原始请求
                if (!m_lastRequestData.isEmpty()) {
                    m_isRetrying = true;
                    postMessage(QString::fromUtf8(m_lastRequestData));
                    m_isRetrying = false;
                }
                return;
            }
        }

        emit transportError(QString("Auth failed: %1").arg(reply->errorString()));
        return;
    }

    // 2026-07-28: 服务器以 4xx + JSON-RPC error body 表达协议错误
    // （-32020 HeaderMismatch / -32022 UnsupportedProtocolVersion / 404 -32601 未知方法）。
    // 只要响应体非空，就交给 session 解析（JSON 或 SSE），而不是当作传输层网络错误丢弃。
    QByteArray responseData = reply->readAll();
    if (!responseData.isEmpty()) {
        QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        if (contentType.contains("text/event-stream")) {
            // SSE 格式：流中可能携带多个事件（请求相关通知 + 最终响应），逐个提取 data: 行处理。
            // 2026-07-28 subscriptions/listen 即通过该流下发 acknowledged / list_changed 等通知。
            QString response = QString::fromUtf8(responseData);
            QStringList lines = response.split('\n');
            bool deliveredAny = false;
            for (const QString& line : lines) {
                if (line.startsWith("data: ")) {
                    QString jsonStr = line.mid(6).trimmed();
                    if (!jsonStr.isEmpty()) {
                        emit messageReceived(jsonStr);
                        deliveredAny = true;
                    }
                }
            }
            // 没有找到 data: 行，发送原始响应
            if (!deliveredAny) {
                emit messageReceived(QString::fromUtf8(responseData));
            }
        } else {
            // JSON 格式
            emit messageReceived(QString::fromUtf8(responseData));
        }
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        emit transportError(QString("HTTP POST failed: %1").arg(reply->errorString()));
        return;
    }

    // 成功且无 body（如 202 Accepted 通知）：静默
    m_authRetryCount = 0;
}

void QtStatelessHttpWorker::startSseListener() {
    if (!m_isRunning || m_sseListenerActive) return;
    m_sseListenerActive = true;

    QNetworkRequest request(m_endpointUrl);
    applyCommonHeaders(request, true);

    m_sseReply = m_nam->get(request);

    connect(m_sseReply, &QNetworkReply::readyRead, this, [this]() {
        if (!m_sseReply || !m_isRunning) return;
        handleSseResponse(m_sseReply);
    });

    connect(m_sseReply, &QNetworkReply::finished, this, [this]() {
        if (!m_sseReply || !m_isRunning) return;
        // SSE 流关闭
        m_sseListenerActive = false;
        m_sseReply->deleteLater();
        m_sseReply = nullptr;
    });

    connect(m_sseReply, &QNetworkReply::errorOccurred, this, [this](QNetworkReply::NetworkError error) {
        if (!m_sseReply || !m_isRunning) return;
        // 405 表示服务端不支持 GET SSE，静默忽略
        if (error == QNetworkReply::ContentAccessDenied ||
            error == QNetworkReply::ContentOperationNotPermittedError) {
            m_sseListenerActive = false;
            return;
        }
        emit transportError(QString("SSE listener error: %1").arg(m_sseReply->errorString()));
    });
}

void QtStatelessHttpWorker::handleSseResponse(QNetworkReply* reply) {
    m_sseBuffer.append(reply->readAll());

    // 解析 SSE 事件（以双换行分隔）
    int pos = 0;
    while ((pos = m_sseBuffer.indexOf("\n\n")) != -1) {
        processSseData(m_sseBuffer.left(pos));
        m_sseBuffer = m_sseBuffer.mid(pos + 2);
    }
}

void QtStatelessHttpWorker::processSseData(const QByteArray& data) {
    QString str = QString::fromUtf8(data);
    QStringList lines = str.split('\n');

    for (const QString& line : lines) {
        if (line.startsWith("data: ")) {
            QString jsonStr = line.mid(6).trimmed();
            if (!jsonStr.isEmpty()) {
                emit messageReceived(jsonStr);
            }
        }
    }
}

void QtStatelessHttpWorker::setProtocolVersion(const std::string& version) {
    m_protocolVersion = version;
}

void QtStatelessHttpWorker::setExtraRequestHeaders(const std::map<std::string, std::string>& headers) {
    m_extraRequestHeaders = headers;
}

void QtStatelessHttpWorker::setCustomHeaders(const QMap<QByteArray, QByteArray>& headers) {
    m_headers = headers;
    if (!m_headers.contains("Content-Type")) {
        m_headers.insert("Content-Type", "application/json");
    }
}

void QtStatelessHttpWorker::setProxy(const QNetworkProxy& proxy) {
    if (m_nam) {
        m_nam->setProxy(proxy);
    }
}

void QtStatelessHttpWorker::setTokenProvider(std::function<std::string()> provider) {
    m_tokenProvider = std::move(provider);
}

void QtStatelessHttpWorker::setAuthRetryHandler(std::function<bool(const std::string&)> handler) {
    m_authRetryHandler = std::move(handler);
}

QByteArray QtStatelessHttpWorker::encodeMcpHeaderValue(const std::string& raw) const {
    return QByteArray::fromStdString(mcp::mcpHeaderEncodeValue(raw));
}

} // namespace mcp_qt
