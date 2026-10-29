#include "libraryfiltermodel.h"

#include "librarymodel.h"

#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace murasaki {

LibraryFilterModel::LibraryFilterModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
    setDynamicSortFilter(true);
    setSortRole(LibraryModel::TitleRole);
    connect(this, &QAbstractItemModel::rowsInserted, this, &LibraryFilterModel::updateCount);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &LibraryFilterModel::updateCount);
    connect(this, &QAbstractItemModel::modelReset, this, &LibraryFilterModel::updateCount);
    connect(this, &QAbstractItemModel::layoutChanged, this, &LibraryFilterModel::updateCount);
}

void LibraryFilterModel::setSourceModel(QAbstractItemModel *sourceModel)
{
    QSortFilterProxyModel::setSourceModel(sourceModel);
    sort(0);
    updateCount();
}

QVariantMap LibraryFilterModel::get(int proxyRow) const
{
    QVariantMap map;
    if (proxyRow < 0 || proxyRow >= rowCount())
        return map;
    const QModelIndex idx = index(proxyRow, 0);
    const QHash<int, QByteArray> names = roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it)
        map.insert(QString::fromLatin1(it.value()), data(idx, it.key()));
    return map;
}

QString LibraryFilterModel::itemIdAt(int proxyRow) const
{
    if (proxyRow < 0 || proxyRow >= rowCount())
        return {};
    return data(index(proxyRow, 0), LibraryModel::ItemIdRole).toString();
}

void LibraryFilterModel::refilter()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    endFilterChange(Direction::Rows);
#else
    invalidateFilter();
#endif
}

void LibraryFilterModel::updateCount()
{
    const int now = rowCount();
    if (now != m_lastCount) {
        m_lastCount = now;
        emit countChanged();
    }
}

QString LibraryFilterModel::fold(const QString &text)
{
    const QString decomposed = text.normalized(QString::NormalizationForm_KD);
    QString out;
    out.reserve(decomposed.size());
    for (const QChar ch : decomposed) {
        switch (ch.category()) {
        case QChar::Mark_NonSpacing:
        case QChar::Mark_SpacingCombining:
        case QChar::Mark_Enclosing:
            continue;
        default:
            out.append(ch);
        }
    }
    return out.toCaseFolded();
}

void LibraryFilterModel::setSearchText(const QString &text)
{
    if (text == m_searchText)
        return;
    m_searchText = text;
    m_words = fold(text).split(QRegularExpression(u"\\s+"_s), Qt::SkipEmptyParts);
    refilter();
    emit searchTextChanged();
}

void LibraryFilterModel::setKindFilter(const QString &kind)
{
    if (kind == m_kindFilter)
        return;
    m_kindFilter = kind;
    refilter();
    emit kindFilterChanged();
}

void LibraryFilterModel::setOnScreenOnly(bool only)
{
    if (only == m_onScreenOnly)
        return;
    m_onScreenOnly = only;
    refilter();
    emit onScreenOnlyChanged();
}

bool LibraryFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    const QAbstractItemModel *source = sourceModel();
    const QModelIndex idx = source->index(sourceRow, 0, sourceParent);
    const QString kind = source->data(idx, LibraryModel::KindRole).toString();
    if (!m_kindFilter.isEmpty() && kind != m_kindFilter)
        return false;
    if (kind == u"asset" && m_kindFilter != u"asset") // editor asset packs are not wallpapers: out of the library
        return false;
    if (m_onScreenOnly && !source->data(idx, LibraryModel::AssignedRole).toBool())
        return false;
    if (m_words.isEmpty())
        return true;
    QString haystack = fold(source->data(idx, LibraryModel::TitleRole).toString());
    const QStringList tags = source->data(idx, LibraryModel::TagsRole).toStringList();
    for (const QString &tag : tags)
        haystack += QChar::Space + fold(tag);
    for (const QString &word : m_words) {
        if (!haystack.contains(word))
            return false;
    }
    return true;
}

bool LibraryFilterModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    const QAbstractItemModel *source = sourceModel();
    const bool leftOnScreen = source->data(left, LibraryModel::AssignedRole).toBool();
    if (leftOnScreen != source->data(right, LibraryModel::AssignedRole).toBool())
        return leftOnScreen;
    const int cmp = m_collator.compare(source->data(left, LibraryModel::TitleRole).toString(),
                                       source->data(right, LibraryModel::TitleRole).toString());
    if (cmp != 0)
        return cmp < 0;
    return source->data(left, LibraryModel::ItemIdRole).toString()
         < source->data(right, LibraryModel::ItemIdRole).toString();
}

} // namespace murasaki
