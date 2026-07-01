#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QJsonObject>
#include <QString>

namespace mcp_qt {

class McpQtClient;

class McpResourcesModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        UriRole         = Qt::UserRole + 1,
        NameRole        = Qt::UserRole + 2,
        DescriptionRole = Qt::UserRole + 3,
        MimeTypeRole    = Qt::UserRole + 4,
    };
    Q_ENUM(Roles)

    explicit McpResourcesModel(QObject* parent = nullptr);

    void setClient(McpQtClient* client);
    McpQtClient* client() const { return m_client; }

    Q_INVOKABLE void refresh();

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool canFetchMore(const QModelIndex& parent = QModelIndex()) const override;
    void fetchMore(const QModelIndex& parent = QModelIndex()) override;

signals:
    void countChanged();
    void refreshError(const QString& message);

private:
    struct ResourceEntry {
        QString uri;
        QString name;
        QString description;
        QString mimeType;
    };

    McpQtClient* m_client{nullptr};
    QList<ResourceEntry> m_resources;
    QString m_nextCursor;
};

} // namespace mcp_qt
