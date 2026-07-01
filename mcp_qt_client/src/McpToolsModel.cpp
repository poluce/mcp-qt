#include "mcp_qt_client/McpToolsModel.h"
#include "mcp_qt_client/McpQtClient.h"
#include <QPointer>
#include "mcp_qt_client/McpQtClient.h"

namespace mcp_qt {

McpToolsModel::McpToolsModel(QObject* parent)
    : QAbstractListModel(parent) {}

void McpToolsModel::setClient(McpQtClient* client) {
    if (m_client == client) return;
    if (m_client) {
        disconnect(m_client, &McpQtClient::toolsChanged, this, &McpToolsModel::refresh);
    }
    m_client = client;

    if (!m_client) return;

    connect(m_client, &McpQtClient::toolsChanged, this, &McpToolsModel::refresh);
}

void McpToolsModel::refresh() {
    if (!m_client) {
        emit refreshError("No client set. Call setClient() before refresh().");
        return;
    }

    QPointer<McpToolsModel> self = this;
    m_client->listToolsAsync("", [self](const std::vector<McpQtTool>& tools, const QString& newCursor, const QString& error) {
        if (!self) return;
        if (!error.isEmpty()) {
            emit self->refreshError(error);
            return;
        }

        // 数据比对防 Churn 优化 (仅针对第一页或全量数据)
        bool changed = false;
        if (self->m_tools.size() != static_cast<int>(tools.size()) || !self->m_nextCursor.isEmpty()) {
            // 如果当前已经加载了多页，或者新的 cursor 状态有变化，直接认为变了
            changed = true;
        } else {
            for (size_t i = 0; i < tools.size(); ++i) {
                const auto& t = tools[i];
                const auto& existing = self->m_tools[static_cast<int>(i)];
                if (existing.name != t.name ||
                    existing.description != t.description ||
                    existing.inputSchema != t.inputSchema) {
                    changed = true;
                    break;
                }
            }
        }

        if (!changed) {
            return; // 工具列表无变化，直接返回，避免重置 View
        }

        self->beginResetModel();
        self->m_tools.clear();
        for (const auto& t : tools) {
            self->m_tools.append({t.name, t.description, t.inputSchema});
        }
        self->m_nextCursor = newCursor;
        self->endResetModel();

        emit self->countChanged();
    });
}

bool McpToolsModel::canFetchMore(const QModelIndex& parent) const {
    if (parent.isValid()) return false;
    return !m_nextCursor.isEmpty();
}

void McpToolsModel::fetchMore(const QModelIndex& parent) {
    if (parent.isValid() || m_nextCursor.isEmpty() || !m_client) return;

    QPointer<McpToolsModel> self = this;
    m_client->listToolsAsync(m_nextCursor, [self](const std::vector<McpQtTool>& tools, const QString& newCursor, const QString& error) {
        if (!self) return;
        if (!error.isEmpty()) {
            emit self->refreshError(error);
            return;
        }

        if (tools.empty()) {
            self->m_nextCursor = newCursor;
            return;
        }

        self->beginInsertRows(QModelIndex(), self->m_tools.size(), self->m_tools.size() + tools.size() - 1);
        for (const auto& t : tools) {
            self->m_tools.append({t.name, t.description, t.inputSchema});
        }
        self->m_nextCursor = newCursor;
        self->endInsertRows();

        emit self->countChanged();
    });
}

int McpToolsModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return m_tools.size();
}

QVariant McpToolsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_tools.size()) {
        return {};
    }
    const ToolEntry& entry = m_tools.at(index.row());
    switch (role) {
    case NameRole:
        return entry.name;
    case DescriptionRole:
        return entry.description;
    case InputSchemaRole:
        return entry.inputSchema;
    case Qt::DisplayRole:
        return entry.name; // 默认显示工具名
    default:
        return {};
    }
}

QHash<int, QByteArray> McpToolsModel::roleNames() const {
    return {
        {NameRole,        "name"},
        {DescriptionRole, "description"},
        {InputSchemaRole, "inputSchema"},
    };
}

} // namespace mcp_qt
