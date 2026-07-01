#include "mcp_qt_client/McpResourceTemplatesModel.h"
#include "mcp_qt_client/McpQtClient.h"

namespace mcp_qt {

McpResourceTemplatesModel::McpResourceTemplatesModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

void McpResourceTemplatesModel::setClient(McpQtClient* client) {
    if (m_client == client) return;
    
    if (m_client) {
        m_client->disconnect(this);
    }
    
    m_client = client;
    
    if (m_client) {
        connect(m_client, &McpQtClient::connected, this, &McpResourceTemplatesModel::refresh);
        connect(m_client, &McpQtClient::disconnected, this, [this]() {
            beginResetModel();
            m_templates.clear();
            m_nextCursor.clear();
            endResetModel();
            emit countChanged();
        });
        connect(m_client, &McpQtClient::notificationReceived, this, [this](const QString& method, const QJsonObject&) {
            if (method == QStringLiteral("notifications/resources/list_changed")) {
                refresh();
            }
        });
    }
    
    refresh();
}

void McpResourceTemplatesModel::refresh() {
    if (!m_client) return;
    
    m_client->listResourceTemplatesAsync("", [this](const std::vector<mcp::McpResourceTemplate>& result, const QString& nextCursor, const QString& error) {
        if (!error.isEmpty()) {
            emit refreshError(error);
            return;
        }
        
        beginResetModel();
        m_templates.clear();
        for (const auto& item : result) {
            TemplateEntry e;
            e.uriTemplate = QString::fromStdString(item.uriTemplate);
            e.name = QString::fromStdString(item.name);
            e.description = QString::fromStdString(item.description);
            e.mimeType = QString::fromStdString(item.mimeType);
            m_templates.append(e);
        }
        m_nextCursor = nextCursor;
        endResetModel();
        emit countChanged();
    });
}

int McpResourceTemplatesModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return m_templates.count();
}

QVariant McpResourceTemplatesModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_templates.count())
        return QVariant();

    const auto& e = m_templates.at(index.row());
    switch (role) {
    case UriTemplateRole: return e.uriTemplate;
    case NameRole:        return e.name;
    case DescriptionRole: return e.description;
    case MimeTypeRole:    return e.mimeType;
    }
    return QVariant();
}

QHash<int, QByteArray> McpResourceTemplatesModel::roleNames() const {
    QHash<int, QByteArray> roles;
    roles[UriTemplateRole] = "uriTemplate";
    roles[NameRole]        = "name";
    roles[DescriptionRole] = "description";
    roles[MimeTypeRole]    = "mimeType";
    return roles;
}

bool McpResourceTemplatesModel::canFetchMore(const QModelIndex& parent) const {
    if (parent.isValid()) return false;
    return !m_nextCursor.isEmpty();
}

void McpResourceTemplatesModel::fetchMore(const QModelIndex& parent) {
    if (parent.isValid() || m_nextCursor.isEmpty() || !m_client)
        return;

    QString cursorToFetch = m_nextCursor;
    m_nextCursor.clear();

    m_client->listResourceTemplatesAsync(cursorToFetch, [this](const std::vector<mcp::McpResourceTemplate>& result, const QString& nextCursor, const QString& error) {
        if (!error.isEmpty()) {
            emit refreshError(error);
            return;
        }
        
        if (result.empty()) {
            m_nextCursor = nextCursor;
            return;
        }

        int start = m_templates.count();
        beginInsertRows(QModelIndex(), start, start + result.size() - 1);
        for (const auto& item : result) {
            TemplateEntry e;
            e.uriTemplate = QString::fromStdString(item.uriTemplate);
            e.name = QString::fromStdString(item.name);
            e.description = QString::fromStdString(item.description);
            e.mimeType = QString::fromStdString(item.mimeType);
            m_templates.append(e);
        }
        endInsertRows();

        m_nextCursor = nextCursor;
        emit countChanged();
    });
}

} // namespace mcp_qt
