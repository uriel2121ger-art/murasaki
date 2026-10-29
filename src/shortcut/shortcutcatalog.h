#pragma once

// The applications the GUI offers for a `usershortcut` property: a list model over the scanned desktop entries, filtered by what
// the user types. The scan runs on a worker thread and is repeated when an applications directory changes. It lives in the GUI
// process; QML shows the icon with the theme name or path of `icon` (the host rasterises it for the engine separately).

#include "shortcut/desktopentry.h"

#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QVariantMap>

#include <memory>
#include <vector>

namespace murasaki::shortcut {

class ShortcutCatalog : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(int total READ total NOTIFY totalChanged)

public:
    enum Role { IdRole = Qt::UserRole + 1, NameRole, IconRole, ClassRole };

    // `roots` and `env` are injectable for tests; the defaults are the system's.
    explicit ShortcutCatalog(QObject *parent = nullptr);
    ShortcutCatalog(QStringList roots, DesktopEnvironment env, QObject *parent = nullptr);
    ~ShortcutCatalog() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString filter() const { return m_filter; }
    void setFilter(const QString &text);
    int total() const { return static_cast<int>(m_all.size()); }

    // Scans now, synchronously (tests), or on a worker thread and applies the result when it is back.
    Q_INVOKABLE void refresh();
    void refreshNow();

    // The display name and icon of an installed application, or empty strings (a stored id that no longer resolves).
    Q_INVOKABLE QString nameFor(const QString &id) const;
    Q_INVOKABLE QString iconFor(const QString &id) const;
    std::optional<DesktopEntry> entryFor(const QString &id) const;
    // The application whose windows carry this class (case-insensitive): {appId, name, icon, appClass}, or empty.
    Q_INVOKABLE QVariantMap entryForClass(const QString &windowClass) const;

signals:
    void filterChanged();
    void totalChanged();

private:
    void apply(std::vector<DesktopEntry> entries);
    void rebuildVisible();

    QStringList m_roots;
    DesktopEnvironment m_env;
    std::vector<DesktopEntry> m_all;
    std::vector<int> m_visible; // indices into m_all
    QString m_filter;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    int m_generation = 0; // a scan whose generation is not the latest is dropped
    bool m_scanning = false;
    bool m_again = false;
};

} // namespace murasaki::shortcut
