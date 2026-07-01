#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>

namespace mcp_qt {

class McpQtClient;

class McpResourceTemplatesModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        UriTemplateRole = Qt::UserRole + 1,
        NameRole        = Qt::UserRole + 2,
        DescriptionRole = Qt::UserRole + 3,
        MimeTypeRole    = Qt::UserRole + 4,
    };
    Q_ENUM(Roles)

    explicit McpResourceTemplatesModel(QObject* parent = nullptr);

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
    struct TemplateEntry {
        QString uriTemplate;
        QString name;
        QString description;
        QString mimeType;
    };

    McpQtClient* m_client{nullptr};
    QList<TemplateEntry> m_templates;
    QString m_nextCursor;
};

} // namespace mcp_qt
