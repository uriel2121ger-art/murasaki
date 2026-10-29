#pragma once

#include "pointersource.h"

#include <QDBusConnection>
#include <QDBusContext>
#include <QElapsedTimer>
#include <QPointF>
#include <QString>
#include <QTimer>

#include <optional>

class QDBusServiceWatcher;

namespace murasaki {

// KWin (Plasma Wayland): the global cursor from a script that runs inside the compositor only while some pet wants it.
//   wanted(true)  writes kwin-pet-cursor.js (a resource) to the runtime directory, loads and starts it through org.kde.kwin.Scripting;
//                 the script calls ReportPointer(x, y) on this object at most every 34 ms.
//   wanted(false) unloads the script: nothing runs in KWin, no D-Bus call per cursor movement.
// ReportPointer is accepted only from the current owner of org.kde.KWin (a peer that merely knows the method gets nothing), and reports
// that arrive faster than kMinIntervalMs are coalesced into a trailing one. Never used on X11 (pointercapability.h).
// The object is exported at kObjectPath on `bus` by the constructor; the bus name that the script targets is the process's own (the
// overlay's), so the script and `serviceName` must agree.
// The script must never outlive the overlay: the constructor unloads one left by a crashed instance (before anything is wanted), the
// destructor unloads whenever a load may be under way or done, and the script itself stops reporting when the overlay's name has no owner
// (kwin-pet-cursor.js). The script has one fixed name, so two overlays of one user would unload each other's: the overlay is single-instance
// (it owns a well-known bus name), and a second one is not started.
class KWinPointerSource : public PointerSource, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.uriel2121ger_art.Murasaki.Overlay.Pointer1")

public:
    static constexpr char kObjectPath[] = "/io/github/uriel2121ger_art/Murasaki/Overlay/Pointer";
    static constexpr char kDefaultServiceName[] = "io.github.uriel2121ger_art.Murasaki.Overlay";
    static constexpr int kMinIntervalMs = 16;
    static constexpr int kMaxCoordinate = 1000000; // a report beyond it is ignored, like Hyprland's

    struct Options {
        QDBusConnection bus = QDBusConnection::sessionBus();
        QString runtimeDir;   // where the script file is written; default $XDG_RUNTIME_DIR/murasaki
        QString kwinService = QStringLiteral("org.kde.KWin");
        QString serviceName = QString::fromLatin1(kDefaultServiceName); // what the script calls (replaced in the written copy)
        int callTimeoutMs = 1500;
    };

    explicit KWinPointerSource(const Options &options, QObject *parent = nullptr);
    explicit KWinPointerSource(QObject *parent = nullptr);
    ~KWinPointerSource() override;

    Level level() const override { return Level::Global; }
    void wanted(bool on) override;

    bool isWanted() const { return m_wanted; }
    bool scriptLoaded() const { return m_loaded; }
    QString scriptPath() const { return m_scriptPath; }

public slots:
    // Called by the script. `x`, `y`: the cursor in global logical pixels.
    Q_SCRIPTABLE void ReportPointer(int x, int y);

private:
    bool calledByKWin() const;
    static bool validServiceName(const QString &name);
    void setKWinOwner(const QString &owner);
    void sendUnload(bool blocking);
    bool writeScript();
    void load();
    void flushPending();
    void emitMoved(QPointF p);

    Options m_options;
    bool m_registered = false;
    bool m_wanted = false;
    bool m_loaded = false;
    bool m_loading = false;
    bool m_mayExist = false;   // a loadScript was sent and no unloadScript since: the script may be in KWin
    bool m_retried = false;    // the one retry after a failed load while KWin is on the bus
    QString m_kwinOwner;       // unique name of the current owner of org.kde.KWin, kept up to date by the watcher
    int m_generation = 0; // a load that finishes after wanted(false) must not leave the script running
    QString m_scriptPath;
    QString m_scriptName;
    QDBusServiceWatcher *m_watcher = nullptr;
    QElapsedTimer m_clock;
    qint64 m_lastEmitMs = -1000;
    std::optional<QPointF> m_pending;
    QTimer m_trailing;
};

} // namespace murasaki
