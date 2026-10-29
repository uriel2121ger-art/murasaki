#pragma once

#include <QAbstractListModel>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

namespace murasaki {

class DaemonClient;

// The wallpaper library as a list model, fed by DaemonClient::itemsListed(). Rows keep the daemon's order
// (sorted by title). Every update after the first is applied as inserts, moves, removals and dataChanged, so
// views keep their scroll position and selection; the model is never reset.
class LibraryModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Role {
        ItemIdRole = Qt::UserRole + 1,
        TitleRole,
        KindRole,
        StatusRole,
        StatusDetailRole,
        PreviewRole,
        TagsRole,
        ContentRatingRole,
        WorkshopIdRole,
        DependencyIdRole,
        HasRisksRole,
        NetworkAllowedRole,
        AssignedRole,
        FormatRole,
    };
    Q_ENUM(Role)

    explicit LibraryModel(DaemonClient *client, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE int indexOf(const QString &id) const;
    Q_INVOKABLE QVariantMap get(int row) const;

signals:
    void countChanged();

private:
    struct Entry
    {
        QVariantMap summary;
        bool assigned = false;

        QString id() const { return summary.value(QStringLiteral("id")).toString(); }
        QString title() const { return summary.value(QStringLiteral("title")).toString(); }
    };

    void applyItems(const QVariantList &items);
    void applyAssignments();

    DaemonClient *m_client;
    QList<Entry> m_entries;
    QSet<QString> m_assignedIds;
};

} // namespace murasaki
