#include "librarymodel.h"

#include "daemonclient.h"

using namespace Qt::StringLiterals;

namespace murasaki {

LibraryModel::LibraryModel(DaemonClient *client, QObject *parent)
    : QAbstractListModel(parent)
    , m_client(client)
{
    connect(client, &DaemonClient::itemsListed, this, &LibraryModel::applyItems);
    connect(client, &DaemonClient::itemsChanged, client, &DaemonClient::refreshItems);
    connect(client, &DaemonClient::assignmentsChanged, this, &LibraryModel::applyAssignments);
    applyAssignments();
    if (client->available())
        client->refreshItems();
}

int LibraryModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_entries.size());
}

QVariant LibraryModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size())
        return {};
    const Entry &entry = m_entries.at(index.row());
    const QVariantMap &s = entry.summary;
    switch (role) {
    case ItemIdRole:
        return entry.id();
    case Qt::DisplayRole:
    case TitleRole:
        return entry.title();
    case KindRole:
        return s.value(u"kind"_s).toString();
    case StatusRole:
        return s.value(u"status"_s).toString();
    case StatusDetailRole:
        return s.value(u"statusDetail"_s).toString();
    case PreviewRole: {
        const QString preview = s.value(u"preview"_s).toString();
        return preview.isEmpty() ? QUrl() : QUrl::fromLocalFile(preview);
    }
    case TagsRole:
        return s.value(u"tags"_s).toStringList();
    case ContentRatingRole:
        return s.value(u"contentRating"_s).toString();
    case WorkshopIdRole:
        return s.value(u"workshopId"_s).toString();
    case DependencyIdRole:
        return s.value(u"dependencyId"_s).toString();
    case HasRisksRole:
        return !s.value(u"risks"_s).toList().isEmpty();
    case NetworkAllowedRole:
        return s.value(u"permissions"_s).toMap().value(u"network"_s).toBool();
    case AssignedRole:
        return entry.assigned;
    case FormatRole:
        return s.value(u"format"_s).toString(); // "puppet" for a puppet pet, empty for every other item
    default:
        return {};
    }
}

QHash<int, QByteArray> LibraryModel::roleNames() const
{
    return {
        {ItemIdRole, "itemId"},
        {TitleRole, "title"},
        {KindRole, "kind"},
        {StatusRole, "status"},
        {StatusDetailRole, "statusDetail"},
        {PreviewRole, "preview"},
        {TagsRole, "tags"},
        {ContentRatingRole, "contentRating"},
        {WorkshopIdRole, "workshopId"},
        {DependencyIdRole, "dependencyId"},
        {HasRisksRole, "hasRisks"},
        {NetworkAllowedRole, "networkAllowed"},
        {AssignedRole, "assigned"},
        {FormatRole, "format"},
    };
}

int LibraryModel::indexOf(const QString &id) const
{
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).id() == id)
            return i;
    }
    return -1;
}

QVariantMap LibraryModel::get(int row) const
{
    QVariantMap map;
    if (row < 0 || row >= m_entries.size())
        return map;
    const QModelIndex idx = index(row);
    const QHash<int, QByteArray> names = roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it)
        map.insert(QString::fromLatin1(it.value()), data(idx, it.key()));
    return map;
}

void LibraryModel::applyItems(const QVariantList &items)
{
    QList<Entry> target;
    QSet<QString> targetIds;
    target.reserve(items.size());
    for (const QVariant &item : items) {
        Entry entry;
        entry.summary = item.toMap();
        const QString id = entry.id();
        if (id.isEmpty() || targetIds.contains(id))
            continue;
        entry.assigned = m_assignedIds.contains(id);
        targetIds.insert(id);
        target.append(entry);
    }

    if (m_entries.isEmpty()) {
        // First load (or the library was empty): one insertion, not one per row.
        if (!target.isEmpty()) {
            beginInsertRows({}, 0, int(target.size()) - 1);
            m_entries = target;
            endInsertRows();
            emit countChanged();
        }
        return;
    }

    const int before = int(m_entries.size());

    // 1. Drop rows that vanished (bottom-up so indexes stay valid).
    for (int row = int(m_entries.size()) - 1; row >= 0; --row) {
        if (!targetIds.contains(m_entries.at(row).id())) {
            beginRemoveRows({}, row, row);
            m_entries.removeAt(row);
            endRemoveRows();
        }
    }

    // 2. Walk the target order: move rows that are in the wrong place, insert new ones, refresh changed ones.
    for (int row = 0; row < target.size(); ++row) {
        const Entry &wanted = target.at(row);
        if (row < m_entries.size() && m_entries.at(row).id() == wanted.id()) {
            // already in place
        } else {
            int from = -1;
            for (int j = row + 1; j < m_entries.size(); ++j) {
                if (m_entries.at(j).id() == wanted.id()) {
                    from = j;
                    break;
                }
            }
            if (from >= 0) {
                beginMoveRows({}, from, from, {}, row);
                m_entries.move(from, row);
                endMoveRows();
            } else {
                beginInsertRows({}, row, row);
                m_entries.insert(row, wanted);
                endInsertRows();
                continue;
            }
        }
        if (m_entries.at(row).summary != wanted.summary) {
            m_entries[row].summary = wanted.summary;
            emit dataChanged(index(row), index(row));
        }
    }

    if (int(m_entries.size()) != before)
        emit countChanged();
}

void LibraryModel::applyAssignments()
{
    QSet<QString> ids;
    const QVariantList assignments = m_client->assignments();
    for (const QVariant &assignment : assignments)
        ids.insert(assignment.toMap().value(u"itemId"_s).toString());
    m_assignedIds = ids;
    for (int row = 0; row < m_entries.size(); ++row) {
        const bool assigned = ids.contains(m_entries.at(row).id());
        if (m_entries.at(row).assigned != assigned) {
            m_entries[row].assigned = assigned;
            emit dataChanged(index(row), index(row), {AssignedRole});
        }
    }
}

} // namespace murasaki
