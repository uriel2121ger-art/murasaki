#pragma once

#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>

namespace murasaki {

// Emits changed() after filesystem activity on the given dirs/files: at most once per debounce interval of
// quiet, and never later than max-wait after the first event of a burst (so a continuous stream still emits).
// Paths that are missing or get dropped (atomic replace, deletion) are retried periodically; if one becomes
// watchable again, changed() is emitted because things may have appeared while it was unwatched.
class WorkshopWatcher : public QObject
{
    Q_OBJECT

public:
    explicit WorkshopWatcher(QObject *parent = nullptr);

    // Empty paths are skipped; missing ones are retried until they exist.
    void setPaths(const QStringList &paths);
    void setDebounceInterval(int ms) { m_debounceMs = qMax(0, ms); }
    void setMaxWait(int ms) { m_maxWaitMs = qMax(0, ms); }
    void setRetryInterval(int ms) { m_retry.setInterval(qMax(1, ms)); }

signals:
    void changed();

private:
    void onEvent();
    bool rewatch(); // returns true if any path became watched
    void updateRetry();

    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QTimer m_retry;
    QElapsedTimer m_burst;
    QStringList m_paths;
    int m_debounceMs = 1000;
    int m_maxWaitMs = 5000;
};

} // namespace murasaki
