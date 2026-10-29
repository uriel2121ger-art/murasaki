#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <utility>

namespace murasaki {

inline constexpr int kMaxOpenApps = 64;
inline constexpr int kMaxAppClassLength = 128;

// Adds an application class to a list of open applications: trimmed, cut to kMaxAppClassLength, not twice, not past kMaxOpenApps. Empty names are ignored.
void addOpenApp(QStringList &apps, const QString &appClass);

struct DesktopInfo {
    QString id;
    QString name;
    int number = 0; // 1-based position for KWin, the workspace number for Sway and Hyprland

    QJsonObject toJson() const;
    bool operator==(const DesktopInfo &other) const = default;
};

struct OutputSession {
    OutputSession() = default;
    OutputSession(QString desktopId, bool isOccluded) : desktop(std::move(desktopId)), occluded(isOccluded) {}
    QString desktop = QStringLiteral("*");
    bool occluded = false; // a fullscreen or maximized window covers the output
    // A window in the real fullscreen state is on this output (visible: not minimized, on the current desktop) and `fullscreenApp` is its application class (empty
    // when the compositor does not say). Not "maximized": that is only `occluded`. What the pets' rules use (docs/pets/session-facts.md).
    bool fullscreen = false;
    QString fullscreenApp;

    bool operator==(const OutputSession &other) const = default;
};

// What the compositor reports, per output.
struct SessionState {
    QString defaultDesktop = QStringLiteral("*"); // for outputs the compositor did not mention
    QHash<QString, OutputSession> outputs;
    QList<DesktopInfo> desktops;
    // The pause rules' exclusion list: the resource class of the window that has the focus, and of every fullscreen
    // window on the current desktop. Only the KWin script reports them; the other compositors leave them empty.
    QString activeApp;
    QStringList fullscreenApps;
    // The application classes of the windows that exist (minimized and on other desktops too), without duplicates, at most kMaxOpenApps of at most
    // kMaxAppClassLength characters each; no titles, nothing else about a window. Empty where the compositor does not list its windows.
    QStringList openApps;

    OutputSession forOutput(const QString &output) const;
    bool operator==(const SessionState &other) const = default;
};

// The facts the surface publishes (docs/pets/session-facts.md): per output `{fullscreen, app}` for OutputFullscreen(), and the whole set for SessionFacts().
QJsonObject outputFullscreenJson(const SessionState &state, const QStringList &outputs);

// The JSON sent by kwin-monitor.js. KWin desktops are global, so every output shows the current desktop.
SessionState parseKWinReport(const QJsonObject &report);

// Sway GET_WORKSPACES + GET_TREE replies. In a tiling layout any tiled window covers the background.
SessionState parseSwayState(const QJsonArray &workspaces, const QJsonObject &tree);

// Hyprland j/monitors, j/clients and j/workspaces replies; same covering rule as Sway.
SessionState parseHyprlandState(const QJsonArray &monitors, const QJsonArray &clients, const QJsonArray &workspaces);

enum class MonitorKind { None, KWin, Sway, Hyprland, ForeignToplevel, GnomeShell, X11Ewmh };

// `gnomeShellOnBus`: org.gnome.Shell is on the session bus (the extension reports through it).
MonitorKind selectMonitorKind(const QProcessEnvironment &env, bool kwinOnBus, bool gnomeShellOnBus = false);

} // namespace murasaki
