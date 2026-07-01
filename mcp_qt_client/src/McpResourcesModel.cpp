#include "mcp_qt_client/McpResourcesModel.h"
#include "mcp_qt_client/McpQtClient.h"
#include <QPointer>
#include "mcp_qt_client/McpQtClient.h"
#include <QJsonArray>

namespace mcp_qt {

McpResourcesModel::McpResourcesModel(QObject* parent)
    : QAbstractListModel(parent) {}

void McpResourcesModel::setClient(McpQtClient* client) {
    if (m_client == client) return;
    if (m_client) {
        disconnect(m_client, &McpQtClient::resourcesChanged, this, &McpResourcesModel::refresh);
    }
    m_client = client;

    if (!m_client) return;

    connect(m_client, &McpQtClient::resourcesChanged, this, &McpResourcesModel::refresh);
}

void McpResourcesModel::refresh() {
    if (!m_client) {
        emit refreshError("No client set. Call setClient() before refresh().");
        return;
    }

    QPointer<McpResourcesModel> self = this;
    m_client->listResourcesAsync("", [self](const QJsonObject& response, const QString& newCursor, const QString& error) {
        if (!self) return;
        if (!error.isEmpty()) {
            emit self->refreshError(error);
            return;
        }

        const QJsonArray resourcesArr = response.value("resources").toArray();

        self->beginResetModel();
        self->m_resources.clear();
        for (const QJsonValue& val : resourcesArr) {
            QJsonObject obj = val.toObject();
            self->m_resources.append({
                obj.value("uri").toString(),
                obj.value("name").toString(),
                obj.value("description").toString(),
                obj.value("mimeType").toString()
            });
        }
        self->m_nextCursor = newCursor;
        self->endResetModel();

        emit self->countChanged();
    });
}

bool McpResourcesModel::canFetchMore(const QModelIndex& parent) const {
    if (parent.isValid()) return false;
    return !m_nextCursor.isEmpty();
}

void McpResourcesModel::fetchMore(const QModelIndex& parent) {
    if (parent.isValid() || m_nextCursor.isEmpty() || !m_client) return;

    QPointer<McpResourcesModel> self = this;
    m_client->listResourcesAsync(m_nextCursor, [self](const QJsonObject& response, const QString& newCursor, const QString& error) {
        if (!self) return;
        if (!error.isEmpty()) {
            emit self->refreshError(error);
            return;
        }

        const QJsonArray resourcesArr = response.value("resources").toArray();

        if (resourcesArr.isEmpty()) {
            self->m_nextCursor = newCursor;
            return;
        }

        self->beginInsertRows(QModelIndex(), self->m_resources.size(), self->m_resources.size() + resourcesArr.size() - 1);
        for (const QJsonValue& val : resourcesArr) {
            QJsonObject obj = val.toObject();
            self->m_resources.append({
                obj.value("uri").toString(),
                obj.value("name").toString(),
                obj.value("description").toString(),
                obj.value("mimeType").toString()
            });
        }
        self->m_nextCursor = newCursor;
        self->endInsertRows();

        emit self->countChanged();
    });
}

int McpResourcesModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return m_resources.size();
}

QVariant McpResourcesModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_resources.size()) {
        return {};
    }
    const ResourceEntry& entry = m_resources.at(index.row());
    switch (role) {
    case UriRole:
        return entry.uri;
    case NameRole:
        return entry.name;
    case DescriptionRole:
        return entry.description;
    case MimeTypeRole:
        return entry.mimeType;
    case Qt::DisplayRole:
        return entry.name;
    default:
        return {};
    }
}

QHash<int, QByteArray> McpResourcesModel::roleNames() const {
    return {
        {UriRole,         "uri"},
        {NameRole,        "name"},
        {DescriptionRole, "description"},
        {MimeTypeRole,    "mimeType"},
    };
}

} // namespace mcp_qt
