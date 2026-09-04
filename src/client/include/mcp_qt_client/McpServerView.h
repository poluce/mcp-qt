#pragma once

#include "mcp_qt_client/McpQtClient.h"
#include <QObject>
#include <QStringList>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>

namespace mcp_qt {

class McpHost;

/**
 * @brief 按会话/Agent 过滤可见 MCP 服务器与能力的轻量视图（issue #9）。
 *
 * 产品需求：每个会话一套独立 MCP 环境——不同会话启用不同的 MCP 服务器，
 * 切换会话 = 切换环境（工具/提示词/资源完全独立）。
 *
 * 设计原则：
 * - 底层连接仍全局共享（McpHost/McpServerManager 统一注册并连接所有服务器，
 *   同一服务器只连一次），视图**不复制连接**，只持有可见服务器列表；
 * - 过滤粒度是服务器级：整个服务器的工具/提示词/资源一起可见或不可见；
 * - 信任应用层过滤：视图只做导出裁剪，调用层不做拒绝（宿主代码本就有全部访问权）；
 * - 视图不缓存：每次导出实时从可见服务器的 client 取数据，天然支持动态切换。
 *
 * 用法：
 * @code
 *   McpServerView view(host);
 *   view.setVisibleServers(registry->serversFor("agentA"));  // 空 = 全部可见
 *   QJsonArray tools = view.exportAllToolsToLlmFormat();     // 只含可见服务器工具
 * @endcode
 */
class McpServerView : public QObject {
    Q_OBJECT
public:
    explicit McpServerView(McpHost* host, QObject* parent = nullptr);
    ~McpServerView() override = default;

    // ========== 可见服务器 ==========

    /// 设置可见服务器列表；空列表 = 全部可见。支持运行时更新（切换会话时调用）。
    void setVisibleServers(const QStringList& servers);
    QStringList visibleServers() const;

    /// 可见服务器名（与 visibleServers 相同，语义别名）
    QStringList serverNames() const;

    // ========== 工具 ==========

    /// 只导出可见服务器的工具（LLM 格式，工具名带 serverName_ 前缀）
    QJsonArray exportAllToolsToLlmFormat(McpQtClient::LlmFormat format = McpQtClient::LlmFormat::OpenAI) const;

    /// 只导出可见服务器的工具（标准 MCP Schema 格式，带 serverName_ 前缀）
    QJsonArray exportAllToolsAsMcpSchema() const;

    /// 查询某可见服务器的工具（不带前缀的原始工具名）
    QList<McpQtTool> toolsForServer(const QString& serverName) const;

    // ========== 提示词 ==========

    /// 只导出可见服务器的提示词（name 带 serverName_ 前缀）。
    /// 注意：会向可见服务器发起 listPrompts 请求（同步）。
    QJsonArray exportAllPrompts() const;

    /// 获取提示词（name 为带前缀的完整名，如 "serverName_promptName"）
    QJsonObject getPrompt(const QString& nameSpacedPromptName, const QJsonObject& arguments, int timeoutMs = 10000);

    // ========== 资源 ==========

    /// 只导出可见服务器的资源（uri 重写为 mcp-{serverName}-{uri}）。
    /// 注意：会向可见服务器发起 listResources 请求（同步）。
    QJsonArray exportAllResources() const;

    /// 读取资源（uri 为带前缀的完整名，如 "mcp-serverName-uri"）
    QJsonObject readResource(const QString& nameSpacedUri, int timeoutMs = 10000);

private:
    bool isServerVisible(const QString& serverName) const;

    McpHost* m_host{nullptr};
    QStringList m_visibleServers;
};

} // namespace mcp_qt
