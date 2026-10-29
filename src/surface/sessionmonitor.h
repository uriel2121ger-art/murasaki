#pragma once

#include "sessionstate.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QObject>

#include <memory>

namespace murasaki {

// Feeds SessionState from the compositor. Until the first report, every output is on desktop "*" and visible.
class SessionMonitor : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    virtual void start() {}
    // Whether the monitor should report the global cursor position (SurfaceService::ReportKWinCursor). Only KWin
    // can; the others ignore it.
    virtual void setCursorReporting(bool) {}
    // Whether `occluded` is reported per output (which outputs a fullscreen or maximized window covers): what lets a game pause
    // only the screen it is on.
    virtual bool knowsOcclusion() const { return false; }
    SessionState state() const { return m_state; }

signals:
    void stateChanged();

protected:
    void setState(const SessionState &state)
    {
        if (state == m_state)
            return;
        m_state = state;
        emit stateChanged();
    }

private:
    SessionState m_state;
};

// Two sources for one state: `primary` knows occlusion and desktops (Sway, Hyprland), `apps` the application classes the
// pause rules match (the foreign-toplevel protocol). The state is the primary's with the apps' activeApp and
// fullscreenApps.
class MergedMonitor : public SessionMonitor
{
    Q_OBJECT

public:
    MergedMonitor(std::unique_ptr<SessionMonitor> primary, std::unique_ptr<SessionMonitor> apps, QObject *parent = nullptr)
        : SessionMonitor(parent)
        , m_primary(std::move(primary))
        , m_apps(std::move(apps))
    {
        connect(m_primary.get(), &SessionMonitor::stateChanged, this, &MergedMonitor::merge);
        connect(m_apps.get(), &SessionMonitor::stateChanged, this, &MergedMonitor::merge);
    }

    bool knowsOcclusion() const override { return m_primary->knowsOcclusion(); }
    void start() override
    {
        m_primary->start();
        m_apps->start();
        merge();
    }
    void setCursorReporting(bool wanted) override { m_primary->setCursorReporting(wanted); }

private:
    void merge()
    {
        SessionState state = m_primary->state();
        const SessionState apps = m_apps->state();
        state.activeApp = apps.activeApp;
        state.fullscreenApps = apps.fullscreenApps;
        // The primary (the compositor's own tree) knows workspaces, so it is the one that says which output has a fullscreen window; the toplevel
        // list knows every window's application, including the ones on workspaces that are not shown.
        state.openApps = apps.openApps.isEmpty() ? state.openApps : apps.openApps;
        setState(state);
    }

    std::unique_ptr<SessionMonitor> m_primary;
    std::unique_ptr<SessionMonitor> m_apps;
};

// GNOME: the Murasaki extension inside the shell reports the windows (SurfaceService::ReportShellState, the same JSON as
// the KWin script), so this only parses what arrives. Until the first report every output is visible.
class ShellMonitor : public SessionMonitor
{
    Q_OBJECT

public:
    using SessionMonitor::SessionMonitor;

    bool knowsOcclusion() const override { return true; }

public slots:
    void report(const QString &json)
    {
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &error);
        if (error.error == QJsonParseError::NoError && doc.isObject())
            setState(parseKWinReport(doc.object()));
    }
};

// For compositors without a supported protocol: wallpapers show, but never pause and ignore desktops.
class NullMonitor : public SessionMonitor
{
    Q_OBJECT

public:
    using SessionMonitor::SessionMonitor;
};

} // namespace murasaki
