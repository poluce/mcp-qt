#pragma once
#include <QObject>
#include <QJsonObject>
#include <QJsonDocument>
#include <nlohmann/json.hpp>
#include "mcp_core/McpTool.h"

namespace mcp {

/**
 * @brief Bridge helper utility to integrate Qt UI structures with core C++ MCP structures.
 * 
 * Provides type conversion functions between nlohmann::json and Qt's JSON objects (QJsonObject),
 * and maps custom invokable Qt objects to MCP tool schemas.
 */
class QtMcpToolBridge : public QObject {
    Q_OBJECT
public:
    explicit QtMcpToolBridge(QObject* parent = nullptr);
    ~QtMcpToolBridge() override = default;

    /**
     * @brief Convert nlohmann::json to QJsonObject.
     */
    static QJsonObject toJson(const nlohmann::json& j);

    /**
     * @brief Convert QJsonObject to nlohmann::json.
     */
    static nlohmann::json fromJson(const QJsonObject& qj);
    
    /**
     * @brief Utility to construct an McpTool definition from Qt inputs.
     */
    static McpTool toMcpTool(const QString& name, const QString& description, const QJsonObject& schema);
};

} // namespace mcp
