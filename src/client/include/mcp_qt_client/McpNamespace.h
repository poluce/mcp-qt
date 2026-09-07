#pragma once

#include <QPair>
#include <QString>
#include <QStringList>

namespace mcp_qt {

/**
 * @brief 命名空间前缀解析（终态架构 §4：前缀解析收敛）。
 *
 * serverName_ / mcp-{serverName}- 前缀格式的唯一实现。view 与三个 router
 * 共用，禁止各自实现，避免格式漂移。
 */
namespace McpNamespace {

/// 解析 serverName_ 前缀（工具/提示词）。返回 {serverName, 原名}；未匹配返回空 QPair。
/// 遍历全部服务器名做前缀匹配，兼容 serverName 含 "_" 的情况。
QPair<QString, QString> parseNamespacedName(const QStringList& serverNames, const QString& namespaced);

/// 解析 mcp-{serverName}- 前缀（资源 URI）。返回 {serverName, 原 URI}；未匹配返回空 QPair。
QPair<QString, QString> parseNamespacedUri(const QStringList& serverNames, const QString& namespacedUri);

} // namespace McpNamespace

} // namespace mcp_qt
