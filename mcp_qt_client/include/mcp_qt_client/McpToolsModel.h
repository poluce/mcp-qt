#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QJsonObject>
#include <QString>

namespace mcp_qt {

class McpQtClient;

/**
 * @brief MCP 工具列表的 Qt Model/View 适配器
 *
 * 将 McpQtClient::fetchAllTools() 的结果暴露为 QAbstractListModel，
 * 可直接绑定到 QListView 或 QML ListView，无需编写额外的 glue 代码。
 *
 * 生命周期：可通过 setClient() 替换 client，不绑定固定 client 实例。
 * 刷新：调用 refresh()（显式）或设置 autoRefresh = true（连接 notifications/tools/list-changed）。
 *
 * 用法：
 * @code
 *   auto model = new McpToolsModel(this);
 *   model->setClient(client.get());
 *   model->refresh();
 *   listView->setModel(model);
 * @endcode
 */
class McpToolsModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        NameRole        = Qt::UserRole + 1,  ///< 工具名称（QString）
        DescriptionRole = Qt::UserRole + 2,  ///< 工具描述（QString）
        InputSchemaRole = Qt::UserRole + 3,  ///< 参数 JSON Schema（QJsonObject）
    };
    Q_ENUM(Roles)

    explicit McpToolsModel(QObject* parent = nullptr);

    /**
     * @brief 设置关联的 MCP 客户端
     *
     * 设置后会自动监听 notifications/tools/list-changed 通知（如果 client 已初始化），
     * 以实现自动刷新。
     */
    void setClient(McpQtClient* client);
    McpQtClient* client() const { return m_client; }

    /**
     * @brief 手动触发工具列表刷新（同步调用 fetchAllTools）
     *
     * 如果服务器不发送 notifications/tools/list-changed 通知，
     * 调用方负责在适当时机手动调用 refresh()。
     */
    Q_INVOKABLE void refresh();

    // QAbstractListModel 接口
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    
    bool canFetchMore(const QModelIndex& parent = QModelIndex()) const override;
    void fetchMore(const QModelIndex& parent = QModelIndex()) override;


signals:
    void countChanged();
    void refreshError(const QString& message);

private:
    struct ToolEntry {
        QString name;
        QString description;
        QJsonObject inputSchema;
    };

    McpQtClient* m_client{nullptr};
    QList<ToolEntry> m_tools;
    QString m_nextCursor;
};

} // namespace mcp_qt
