#include "workshopwatcher.h"

#include <QFileInfo>

namespace murasaki {

WorkshopWatcher::WorkshopWatcher(QObject *parent)
    : QObject(parent)
{
    // Steam touches many files per download; coalesce them into one rescan.
    m_debounce.setSingleShot(true);
    m_retry.setInterval(5000);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { onEvent(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { onEvent(); });
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        m_burst.invalidate();
        rewatch();
        emit changed();
    });
    connect(&m_retry, &QTimer::timeout, this, [this] {
        if (rewatch())
            emit changed();
    });
}

void WorkshopWatcher::onEvent()
{
    if (!m_burst.isValid())
        m_burst.start();
    const int remaining = qMax<qint64>(0, m_maxWaitMs - m_burst.elapsed());
    m_debounce.start(qMin(m_debounceMs, remaining));
}

void WorkshopWatcher::setPaths(const QStringList &paths)
{
    m_debounce.stop();
    m_burst.invalidate();
    const QStringList current = m_watcher.files() + m_watcher.directories();
    if (!current.isEmpty())
        m_watcher.removePaths(current);
    m_paths = paths;
    rewatch();
}

bool WorkshopWatcher::rewatch()
{
    // QFileSystemWatcher drops a path when its file is replaced (Steam rewrites .acf files atomically).
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    bool added = false;
    for (const QString &path : std::as_const(m_paths)) {
        if (!path.isEmpty() && !watched.contains(path) && QFileInfo::exists(path) && m_watcher.addPath(path))
            added = true;
    }
    updateRetry();
    return added;
}

void WorkshopWatcher::updateRetry()
{
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    bool missing = false;
    for (const QString &path : std::as_const(m_paths)) {
        if (!path.isEmpty() && !watched.contains(path))
            missing = true;
    }
    if (missing && !m_retry.isActive())
        m_retry.start();
    else if (!missing)
        m_retry.stop();
}

} // namespace murasaki
