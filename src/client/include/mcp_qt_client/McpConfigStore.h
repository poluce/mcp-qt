#pragma once

#include "mcp_qt_client/McpServerConfig.h"
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <functional>

namespace mcp_qt {

/**
 * @brief MCP 配置文件持久化（终态架构 §4：McpHost 瘦身）。
 *
 * 从 McpHost 抽出的配置读写职责：配置文件路径、读-改-写（QSaveFile 原子写）、
 * 服务器条目增删改。McpHost 只做编排，不再直接碰文件。
 */
class McpConfigStore {
public:
    void setConfigPath(const QString& path) { m_configPath = path; }
    QString configPath() const { return m_configPath; }
    bool hasConfigPath() const { return !m_configPath.isEmpty(); }

    /// 读-改-写 mcpServers 配置：allowMissing 时文件缺失则从空对象开始；
    /// mutate 返回 false 不写回。
    bool readWrite(bool allowMissing, const std::function<bool(QJsonObject&)>& mutate);

    /// 修改某服务器条目的单个属性（服务器不存在返回 false）。
    bool setServerProperty(const QString& serverName, const QString& key, const QJsonValue& value);
    /// 覆盖某服务器条目（不存在则新增）。
    bool setServerObject(const QString& serverName, const QJsonObject& obj);
    /// 删除某服务器条目。
    bool removeServer(const QString& serverName);

    /// McpServerConfig → 配置文件 JSON 对象。
    static QJsonObject serializeServerConfig(const McpServerConfig& cfg);

private:
    QString m_configPath;
};

} // namespace mcp_qt
