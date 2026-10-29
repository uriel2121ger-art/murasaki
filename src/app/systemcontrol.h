#pragma once

#include "plasmacontrol.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>

namespace murasaki {

// What the GUI asks of the user's systemd session: whether the wallpaper starts with the session (autostart),
// and starting it now. The services are the ones installed by data/*.service.in.
class SystemControl : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool autostartEnabled READ autostartEnabled NOTIFY autostartEnabledChanged)
    Q_PROPERTY(bool autostartBusy READ autostartBusy NOTIFY autostartBusyChanged)
    // Desktop integration on Plasma: the wallpaper is shown as the Plasma desktop wallpaper (murasakictl plasma).
    Q_PROPERTY(bool plasmaSupported READ plasmaSupported CONSTANT)
    Q_PROPERTY(bool plasmaEnabled READ plasmaEnabled NOTIFY plasmaEnabledChanged)
    Q_PROPERTY(bool plasmaBusy READ plasmaBusy NOTIFY plasmaBusyChanged)
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)
    // Counts the scene engine verdicts that arrived: a label binding reads it to be evaluated again (sceneEngineLabel).
    Q_PROPERTY(int sceneEngineRevision READ sceneEngineRevision NOTIFY sceneEngineRevisionChanged)
    // The global settings' limits and defaults, so the Settings page has no literals of its own.
    Q_PROPERTY(int minWebFpsLimit READ minWebFpsLimit CONSTANT)
    Q_PROPERTY(int maxWebFpsLimit READ maxWebFpsLimit CONSTANT)
    Q_PROPERTY(int defaultWebFpsLimit READ defaultWebFpsLimit CONSTANT)
    Q_PROPERTY(bool defaultPauseWhenCovered READ defaultPauseWhenCovered CONSTANT)

public:
    // Starts a program without waiting for it; true if it could be started. Replaceable so tests never touch
    // the real systemd.
    using Launcher = std::function<bool(const QString &program, const QStringList &arguments)>;

    // Runs `systemctl` with these arguments without blocking and calls `done` once, from the event loop, with its
    // exit code (-1: it could not be started, or did not finish within timeoutMs). Replaceable so tests never
    // touch the real systemd.
    using AsyncRunner = std::function<void(const QStringList &arguments, int timeoutMs, std::function<void(int)> done)>;

    // What the Plasma toggle asks of plasmashell and the daemon: runs the operation off the GUI thread (it blocks on
    // D-Bus) and calls `done` once, from any thread, with the outcome. Replaceable so tests never touch a real
    // plasmashell; the default runs murasaki::plasma::enable / disable / status with the D-Bus backend.
    enum class PlasmaOperation { Status, Enable, Disable };
    using PlasmaRunner = std::function<void(PlasmaOperation, std::function<void(const plasma::Result &)> done)>;

    explicit SystemControl(QObject *parent = nullptr);

    bool autostartEnabled() const { return m_autostartEnabled; }
    // A systemctl query or change is running; further refresh/set calls are ignored meanwhile.
    bool autostartBusy() const { return m_busy; }
    QString appVersion() const;
    int sceneEngineRevision() const { return m_sceneEngineRevision; }
    int minWebFpsLimit() const;
    int maxWebFpsLimit() const;
    int defaultWebFpsLimit() const;
    bool defaultPauseWhenCovered() const;
    // The local path of a file URL (what a FileDialog returns), or an empty string for anything else.
    Q_INVOKABLE QString localFile(const QUrl &url) const;
    // Whether a scene engine is installed (see findVulkanSceneEngine()). Looked up on every call, so it is invokable
    // rather than a property: a page opened after installing the engine sees it.
    Q_INVOKABLE bool sceneEngineAvailable() const;
    // What the item page shows after "Engine:" for a scene: the engine the surface will pick for it under `mode` (the
    // daemon's sceneEngine setting) and, when it is not the one that was wanted, why. A verdict that is not cached yet
    // is fetched in the background and sceneEngineRevision changes when it arrives. Never blocks.
    // What confines the scene engines on this system, one line ("Landlock ABI 8, TCP refused, mount namespace yes"), or
    // why nothing does. Asks `murasaki-sandbox --report` (kept for the life of the program); empty when it is not installed.
    Q_INVOKABLE QString sandboxStatus() const;
    Q_INVOKABLE QString sceneEngineLabel(const QString &mode, const QString &itemDir, const QString &assetsDir) const;

    bool plasmaSupported() const; // XDG_CURRENT_DESKTOP names KDE
    bool plasmaEnabled() const { return m_plasmaEnabled; }
    bool plasmaBusy() const { return m_plasmaBusy; }
    // Installs a Plasma runner and returns the previous one; an empty runner restores the real one.
    static PlasmaRunner setPlasmaRunner(PlasmaRunner runner);
    // Whether the wallpaper is Murasaki's right now; read it when the page opens. No-op unless plasmaSupported().
    Q_INVOKABLE void refreshPlasma();
    // Switches the Plasma desktop wallpaper to Murasaki or back. A failure is reported through error() and
    // plasmaEnabledChanged is emitted when it finishes either way, so a switch bound to the property falls back.
    Q_INVOKABLE void setPlasma(bool enabled);

    // Installs a launcher and returns the previous one; an empty launcher restores QProcess::startDetached.
    static Launcher setLauncher(Launcher launcher);

    // Installs an async runner and returns the previous one; an empty runner restores the real systemctl.
    static AsyncRunner setAsyncRunner(AsyncRunner runner);

    // Changes how long systemctl may take (defaults: 5 s for a query, 30 s for a change). For tests.
    static void setTimeouts(int queryMs, int changeMs);

    // Reads the autostart state from systemd without blocking. Call it when a page opens.
    Q_INVOKABLE void refreshAutostart();
    // Enables or disables autostart without blocking. A failure is reported through error().
    // autostartEnabledChanged is emitted when it finishes either way, so a switch bound to the property can
    // fall back to the real state after a failure.
    Q_INVOKABLE void setAutostart(bool enabled);
    // `systemctl --user start murasaki-surface.service`, detached. False (and error()) if it cannot be run.
    Q_INVOKABLE bool startSurface();

signals:
    void autostartEnabledChanged();
    void autostartBusyChanged();
    void sceneEngineRevisionChanged();
    void plasmaEnabledChanged();
    void plasmaBusyChanged();
    void error(const QString &message);

private:
    int m_sceneEngineRevision = 0;
    void setBusy(bool busy);
    void setPlasmaBusy(bool busy);
    void runPlasma(PlasmaOperation operation);

    bool m_autostartEnabled = false;
    bool m_busy = false;
    bool m_plasmaEnabled = false;
    bool m_plasmaBusy = false;
};

} // namespace murasaki
