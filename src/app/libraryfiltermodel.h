#pragma once

#include <QCollator>
#include <QSortFilterProxyModel>
#include <QVariantMap>

namespace murasaki {

// Search, kind filter and title ordering over a LibraryModel.
//
// searchText: every whitespace-separated word must occur in the title or in one of the tags, ignoring case
// and accents ("pokemon" finds "Pokémon"). kindFilter: "", "video", "web", "scene" or "pet". onScreenOnly: only the
// items an assignment names (the library's "On screen" badge).
// Rows on a screen come first; then by title with the locale's collation.
class LibraryFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(QString kindFilter READ kindFilter WRITE setKindFilter NOTIFY kindFilterChanged)
    Q_PROPERTY(bool onScreenOnly READ onScreenOnly WRITE setOnScreenOnly NOTIFY onScreenOnlyChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit LibraryFilterModel(QObject *parent = nullptr);

    void setSourceModel(QAbstractItemModel *sourceModel) override;

    QString searchText() const { return m_searchText; }
    void setSearchText(const QString &text);
    QString kindFilter() const { return m_kindFilter; }
    void setKindFilter(const QString &kind);
    bool onScreenOnly() const { return m_onScreenOnly; }
    void setOnScreenOnly(bool only);
    int count() const { return rowCount(); }

    // Row numbers here are rows of this (filtered, sorted) model.
    Q_INVOKABLE QVariantMap get(int proxyRow) const;
    Q_INVOKABLE QString itemIdAt(int proxyRow) const;

    // Lowercased, NFKD-decomposed text without combining marks.
    static QString fold(const QString &text);

signals:
    void searchTextChanged();
    void kindFilterChanged();
    void onScreenOnlyChanged();
    void countChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    void refilter();
    void updateCount();

    QString m_searchText;
    QStringList m_words; // folded
    QString m_kindFilter;
    bool m_onScreenOnly = false;
    QCollator m_collator;
    int m_lastCount = 0;
};

} // namespace murasaki
