#pragma once

#include "sessionmonitor.h"

#include <QTimer>

class QDBusServiceWatcher;

namespace murasaki {

// Loads kwin-monitor.js into KWin over D-Bus. The script calls back SurfaceService::ReportKWinState, which is
// connected to report().
class KWinMonitor : public SessionMonitor
{
    Q_OBJECT

public:
    bool knowsOcclusion() const override { return true; }
    explicit KWinMonitor(QObject *parent = nullptr);
    ~KWinMonitor() override;

    void start() override;
    // Loads kwin-cursor.js while wanted and unloads it again: it costs a D-Bus call per cursor movement, so it
    // only runs while a Plasma host needs the cursor.
    void setCursorReporting(bool enabled) override;

public slots:
    void report(const QString &json);

private:
    void applyPending();
    void loadScript();
    void loadCursorScript();
    void unloadCursorScript();

    QTimer m_coalesce;
    QString m_pending;
    QString m_scriptPath;
    QString m_cursorScriptPath;
    bool m_cursorWanted = false;
    bool m_cursorLoaded = false;
    QDBusServiceWatcher *m_watcher = nullptr;
    int m_retries = 0;
    bool m_loaded = false;
};

} // namespace murasaki
