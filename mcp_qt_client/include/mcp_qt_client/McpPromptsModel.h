#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>

namespace mcp_qt {

class McpQtClient;

class McpPromptsModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        NameRole        = Qt::UserRole + 1,
        DescriptionRole = Qt::UserRole + 2,
        ArgumentsRole   = Qt::UserRole + 3,
    };
    Q_ENUM(Roles)

    explicit McpPromptsModel(QObject* parent = nullptr);

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
    struct PromptEntry {
        QString name;
        QString description;
        QJsonArray arguments;
    };

    McpQtClient* m_client{nullptr};
    QList<PromptEntry> m_prompts;
    QString m_nextCursor;
};

} // namespace mcp_qt
