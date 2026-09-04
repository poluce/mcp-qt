#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace mcp_qt {

/**
 * @brief 工具结果内容的类型枚举
 */
enum class McpQtContentKind {
    Text,     ///< 纯文本内容 (type == "text")
    Image,    ///< 图片内容 (type == "image"，data 字段为 Base64)
    Resource, ///< 嵌入资源内容 (type == "resource")
    Unknown   ///< 未知内容类型，保留为原始 JSON
};

/**
 * @brief 单个工具结果内容块
 *
 * 遵循 MCP 规范的 content 数组元素。无论类型如何，原始 JSON 始终在 raw 字段中保留。
 */
struct McpQtContent {
    McpQtContentKind kind{McpQtContentKind::Unknown};
    QString mimeType;     ///< 对 Image/Resource 有效
    QString text;         ///< 对 Text 有效
    QByteArray binary;    ///< 对 Image 有效（Base64 解码后的原始字节）
    QString decodeError;  ///< Base64 解码失败时的错误描述（非空表示失败）
    QJsonObject raw;      ///< 原始 JSON，始终保留
};

/**
 * @brief 类型化的工具调用返回值
 *
 * 对 callTool() 返回的 `McpResult::data` 做高层解析。
 * 原始 JSON 始终通过 raw 字段可访问，以满足"永不丢弃原始结果"的要求。
 */
struct McpQtToolResult {
    QList<McpQtContent> content;      ///< 解析后的内容列表
    QJsonObject structuredContent;    ///< MCP 2025-11-25 的结构化输出字段（如有）
    QJsonObject raw;                  ///< 工具调用返回的完整原始 JSON
    bool isError{false};              ///< 是否为工具级错误
    QString errorString;              ///< 错误描述（isError 为 true 时有效）
    bool isInputRequired{false};      ///< MCP 2026-07-28: 是否处于等待用户补充输入状态
    QJsonObject inputSchema;          ///< MCP 2026-07-28: 等待补充输入的 Schema
};

} // namespace mcp_qt
