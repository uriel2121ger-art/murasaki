#include "shortcut/shortcutcatalog.h"

#include "appclass.h"

#include <QDir>
#include <QPointer>
#include <QThreadPool>

#include <algorithm>

namespace murasaki::shortcut {

ShortcutCatalog::ShortcutCatalog(QObject *parent) : ShortcutCatalog(applicationRoots(), systemDesktopEnvironment(), parent) {}

ShortcutCatalog::ShortcutCatalog(QStringList roots, DesktopEnvironment env, QObject *parent)
    : QAbstractListModel(parent), m_roots(std::move(roots)), m_env(std::move(env))
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(1000);
    connect(&m_debounce, &QTimer::timeout, this, &ShortcutCatalog::refresh);
    for (const QString &root : std::as_const(m_roots)) {
        if (QDir(root).exists()) {
            m_watcher.addPath(root);
        }
    }
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
}

ShortcutCatalog::~ShortcutCatalog() = default;

int ShortcutCatalog::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_visible.size());
}

QVariant ShortcutCatalog::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_visible.size())) {
        return {};
    }
    const DesktopEntry &entry = m_all[static_cast<size_t>(m_visible[static_cast<size_t>(index.row())])];
    switch (role) {
    case IdRole: return entry.id;
    case Qt::DisplayRole:
    case NameRole: return entry.name;
    case IconRole: return entry.icon;
    case ClassRole: return entry.windowClass;
    }
    return {};
}

QHash<int, QByteArray> ShortcutCatalog::roleNames() const
{
    return {{IdRole, "appId"}, {NameRole, "appName"}, {IconRole, "appIcon"}, {ClassRole, "appClass"}};
}

void ShortcutCatalog::setFilter(const QString &text)
{
    const QString clean = text.left(128);
    if (clean == m_filter) {
        return;
    }
    m_filter = clean;
    rebuildVisible();
    emit filterChanged();
}

namespace {

// What the user can type to find an application: its name, its id or any class its windows carry.
bool matchesFilter(const DesktopEntry &entry, const QString &filter)
{
    const auto has = [&](const QString &text) { return text.contains(filter, Qt::CaseInsensitive); };
    return has(entry.name) || has(entry.id) || has(entry.windowClass) || std::any_of(entry.classAliases.begin(), entry.classAliases.end(), has);
}

} // namespace

void ShortcutCatalog::rebuildVisible()
{
    beginResetModel();
    m_visible.clear();
    for (size_t i = 0; i < m_all.size(); ++i) {
        if (m_filter.isEmpty() || matchesFilter(m_all[i], m_filter)) {
            m_visible.push_back(static_cast<int>(i));
        }
    }
    endResetModel();
}

void ShortcutCatalog::apply(std::vector<DesktopEntry> entries)
{
    const int before = total();
    m_all = std::move(entries);
    rebuildVisible();
    if (before != total()) {
        emit totalChanged();
    }
}

void ShortcutCatalog::refreshNow()
{
    ++m_generation;
    apply(scanApplications(m_roots, m_env));
}

void ShortcutCatalog::refresh()
{
    if (m_scanning) {
        m_again = true; // one more pass once this one is back
        return;
    }
    m_scanning = true;
    const int generation = ++m_generation;
    QPointer<ShortcutCatalog> self(this);
    const QStringList roots = m_roots;
    const DesktopEnvironment env = m_env;
    QThreadPool::globalInstance()->start([self, roots, env, generation] {
        std::vector<DesktopEntry> entries = scanApplications(roots, env);
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self.data(), [self, generation, entries = std::move(entries)]() mutable {
            if (!self) {
                return;
            }
            self->m_scanning = false;
            if (generation == self->m_generation) {
                self->apply(std::move(entries));
            }
            if (self->m_again) {
                self->m_again = false;
                self->refresh();
            }
        }, Qt::QueuedConnection);
    });
}

std::optional<DesktopEntry> ShortcutCatalog::entryFor(const QString &id) const
{
    const auto it = std::find_if(m_all.begin(), m_all.end(), [&](const DesktopEntry &e) { return e.id == id; });
    if (it == m_all.end()) {
        return std::nullopt;
    }
    return *it;
}

QVariantMap ShortcutCatalog::entryForClass(const QString &windowClass) const
{
    if (windowClass.isEmpty()) {
        return {};
    }
    // Exact (case-insensitive) first, so a StartupWMClass wins over another entry's alias; then the other spellings.
    const auto find = [&](auto &&same) { return std::find_if(m_all.begin(), m_all.end(), same); };
    auto it = find([&](const DesktopEntry &e) { return e.windowClass.compare(windowClass, Qt::CaseInsensitive) == 0; });
    if (it == m_all.end()) {
        it = find([&](const DesktopEntry &e) {
            return std::any_of(e.classAliases.begin(), e.classAliases.end(), [&](const QString &a) { return a.compare(windowClass, Qt::CaseInsensitive) == 0; });
        });
    }
    if (it == m_all.end()) {
        it = find([&](const DesktopEntry &e) { return murasaki::sameAppClass(e.windowClass, windowClass); });
    }
    if (it == m_all.end()) {
        return {};
    }
    return {{QStringLiteral("appId"), it->id}, {QStringLiteral("name"), it->name}, {QStringLiteral("icon"), it->icon},
            {QStringLiteral("appClass"), it->windowClass}};
}

QString ShortcutCatalog::nameFor(const QString &id) const
{
    const auto entry = entryFor(id);
    return entry ? entry->name : QString();
}

QString ShortcutCatalog::iconFor(const QString &id) const
{
    const auto entry = entryFor(id);
    return entry ? entry->icon : QString();
}

} // namespace murasaki::shortcut
