#include "mcp_qt_client/McpPromptsModel.h"
#include "mcp_qt_client/McpQtClient.h"
#include <QPointer>
#include "mcp_qt_client/McpQtClient.h"

namespace mcp_qt {

McpPromptsModel::McpPromptsModel(QObject* parent)
    : QAbstractListModel(parent) {}

void McpPromptsModel::setClient(McpQtClient* client) {
    if (m_client == client) return;
    if (m_client) {
        disconnect(m_client, &McpQtClient::promptsChanged, this, &McpPromptsModel::refresh);
    }
    m_client = client;

    if (!m_client) return;

    connect(m_client, &McpQtClient::promptsChanged, this, &McpPromptsModel::refresh);
}

void McpPromptsModel::refresh() {
    if (!m_client) {
        emit refreshError("No client set. Call setClient() before refresh().");
        return;
    }

    QPointer<McpPromptsModel> self = this;
    m_client->listPromptsAsync("", [self](const QJsonObject& response, const QString& newCursor, const QString& error) {
        if (!self) return;
        if (!error.isEmpty()) {
            emit self->refreshError(error);
            return;
        }

        const QJsonArray promptsArr = response.value("prompts").toArray();

        self->beginResetModel();
        self->m_prompts.clear();
        for (const QJsonValue& val : promptsArr) {
            QJsonObject obj = val.toObject();
            self->m_prompts.append({
                obj.value("name").toString(),
                obj.value("description").toString(),
                obj.value("arguments").toArray()
            });
        }
        self->m_nextCursor = newCursor;
        self->endResetModel();

        emit self->countChanged();
    });
}

bool McpPromptsModel::canFetchMore(const QModelIndex& parent) const {
    if (parent.isValid()) return false;
    return !m_nextCursor.isEmpty();
}

void McpPromptsModel::fetchMore(const QModelIndex& parent) {
    if (parent.isValid() || m_nextCursor.isEmpty() || !m_client) return;

    QPointer<McpPromptsModel> self = this;
    m_client->listPromptsAsync(m_nextCursor, [self](const QJsonObject& response, const QString& newCursor, const QString& error) {
        if (!self) return;
        if (!error.isEmpty()) {
            emit self->refreshError(error);
            return;
        }

        const QJsonArray promptsArr = response.value("prompts").toArray();

        if (promptsArr.isEmpty()) {
            self->m_nextCursor = newCursor;
            return;
        }

        self->beginInsertRows(QModelIndex(), self->m_prompts.size(), self->m_prompts.size() + promptsArr.size() - 1);
        for (const QJsonValue& val : promptsArr) {
            QJsonObject obj = val.toObject();
            self->m_prompts.append({
                obj.value("name").toString(),
                obj.value("description").toString(),
                obj.value("arguments").toArray()
            });
        }
        self->m_nextCursor = newCursor;
        self->endInsertRows();

        emit self->countChanged();
    });
}

int McpPromptsModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return m_prompts.size();
}

QVariant McpPromptsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_prompts.size()) {
        return {};
    }
    const PromptEntry& entry = m_prompts.at(index.row());
    switch (role) {
    case NameRole:
        return entry.name;
    case DescriptionRole:
        return entry.description;
    case ArgumentsRole:
        return entry.arguments;
    case Qt::DisplayRole:
        return entry.name;
    default:
        return {};
    }
}

QHash<int, QByteArray> McpPromptsModel::roleNames() const {
    return {
        {NameRole,        "name"},
        {DescriptionRole, "description"},
        {ArgumentsRole,   "arguments"},
    };
}

} // namespace mcp_qt
