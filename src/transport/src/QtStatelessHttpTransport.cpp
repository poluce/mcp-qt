#include "mcp_qt_transport/QtStatelessHttpTransport.h"
#include "mcp_core/McpHeaderEncoding.h"
#include <nlohmann/json.hpp>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QDebug>

namespace mcp_qt {

QtStatelessHttpTransport::QtStatelessHttpTransport(const QString& endpointUrl, QObject* parent)
    : QObject(parent), m_endpointUrl(endpointUrl), m_nam(new QNetworkAccessManager(this))
{
    m_headers.insert("Content-Type", "application/json");
}

QtStatelessHttpTransport::~QtStatelessHttpTransport() {
    close();
}

bool QtStatelessHttpTransport::start() {
    if (m_isRunning) return true;
    m_isRunning = true;
    return true;
}

void QtStatelessHttpTransport::close() {
    if (!m_isRunning) return;
    m_isRunning = false;
    m_sseListenerActive = false;

    if (m_sseReply) {
        disconnect(m_sseReply, nullptr, this, nullptr);
        m_sseReply->abort();
        m_sseReply->deleteLater();
        m_sseReply = nullptr;
    }

    if (m_onClose) {
        m_onClose();
    }
}

QString QtStatelessHttpTransport::currentBearerToken() const {
    return m_tokenProvider ? QString::fromStdString(m_tokenProvider()) : QString{};
}

void QtStatelessHttpTransport::applyCommonHeaders(QNetworkRequest& request, bool isGet) {
    // 自定义 headers
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

bool QtStatelessHttpTransport::send(const std::string& message) {
    if (!m_isRunning || !m_nam) return false;

    // 检测 method 与 name（tools/call -> params.name，resources/read -> params.uri，prompts/get -> params.name），
    // 用于 2026-07-28 Mcp-Method / Mcp-Name HTTP Header 路由（SEP-2243）
    bool isInitializedNotification = false;
    std::string methodStr;
    std::string nameHeader;
    std::string bodyProtocolVersion;
    auto json = nlohmann::json::parse(message, nullptr, false);
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
        m_lastRequestData = QByteArray::fromStdString(message);
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

    QByteArray data = QByteArray::fromStdString(message);
    QNetworkReply* reply = m_nam->post(request, data);

    connect(reply, &QNetworkReply::finished, this, [this, reply, isInitializedNotification]() {
        onReplyFinished(reply);

        // initialized 通知后启动 GET SSE 监听
        if (isInitializedNotification && m_isRunning) {
            startSseListener();
        }
    });

    return true;
}

void QtStatelessHttpTransport::onReplyFinished(QNetworkReply* reply) {
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
                    send(m_lastRequestData.toStdString());
                    m_isRetrying = false;
                }
                return;
            }
        }

        if (m_onError) {
            m_onError(QString("Auth failed: %1").arg(reply->errorString()).toStdString());
        }
        return;
    }

    // 2026-07-28: 服务器以 4xx + JSON-RPC error body 表达协议错误
    // （-32020 HeaderMismatch / -32022 UnsupportedProtocolVersion / 404 -32601 未知方法）。
    // 只要响应体非空，就交给 session 解析（JSON 或 SSE），而不是当作传输层网络错误丢弃。
    QByteArray responseData = reply->readAll();
    if (!responseData.isEmpty() && m_onMessage) {
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
                        m_onMessage(jsonStr.toStdString());
                        deliveredAny = true;
                    }
                }
            }
            // 没有找到 data: 行，发送原始响应
            if (!deliveredAny) {
                m_onMessage(responseData.toStdString());
            }
        } else {
            // JSON 格式
            m_onMessage(responseData.toStdString());
        }
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        QString errorMsg = QString("HTTP POST failed: %1").arg(reply->errorString());
        if (m_onError) {
            m_onError(errorMsg.toStdString());
        }
        return;
    }

    // 成功且无 body（如 202 Accepted 通知）：静默
    m_authRetryCount = 0;
}

void QtStatelessHttpTransport::startSseListener() {
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
        if (m_onError) {
            m_onError(QString("SSE listener error: %1").arg(m_sseReply->errorString()).toStdString());
        }
    });
}

void QtStatelessHttpTransport::handleSseResponse(QNetworkReply* reply) {
    m_sseBuffer.append(reply->readAll());

    // 解析 SSE 事件（以双换行分隔）
    int pos = 0;
    while ((pos = m_sseBuffer.indexOf("\n\n")) != -1) {
        processSseData(m_sseBuffer.left(pos));
        m_sseBuffer = m_sseBuffer.mid(pos + 2);
    }
}

void QtStatelessHttpTransport::processSseData(const QByteArray& data) {
    QString str = QString::fromUtf8(data);
    QStringList lines = str.split('\n');

    for (const QString& line : lines) {
        if (line.startsWith("data: ")) {
            QString jsonStr = line.mid(6).trimmed();
            if (!jsonStr.isEmpty() && m_onMessage) {
                m_onMessage(jsonStr.toStdString());
            }
        }
    }
}

void QtStatelessHttpTransport::setOnMessage(std::function<void(const std::string&)> callback) {
    m_onMessage = std::move(callback);
}

void QtStatelessHttpTransport::setOnClose(std::function<void()> callback) {
    m_onClose = std::move(callback);
}

void QtStatelessHttpTransport::setOnError(std::function<void(const std::string&)> callback) {
    m_onError = std::move(callback);
}

void QtStatelessHttpTransport::setProtocolVersion(const std::string& version) {
    m_protocolVersion = version;
}

void QtStatelessHttpTransport::setExtraRequestHeaders(const std::map<std::string, std::string>& headers) {
    m_extraRequestHeaders = headers;
}

QByteArray QtStatelessHttpTransport::encodeMcpHeaderValue(const std::string& raw) const {
    return QByteArray::fromStdString(mcp::mcpHeaderEncodeValue(raw));
}

void QtStatelessHttpTransport::setCustomHeaders(const QMap<QByteArray, QByteArray>& headers) {
    m_headers = headers;
    if (!m_headers.contains("Content-Type")) {
        m_headers.insert("Content-Type", "application/json");
    }
}

void QtStatelessHttpTransport::setProxy(const QNetworkProxy& proxy) {
    if (m_nam) {
        m_nam->setProxy(proxy);
    }
}

void QtStatelessHttpTransport::setTokenProvider(TokenProvider provider) {
    m_tokenProvider = std::move(provider);
}

void QtStatelessHttpTransport::setAuthRetryHandler(AuthRetryHandler handler) {
    m_authRetryHandler = std::move(handler);
}

} // namespace mcp_qt
