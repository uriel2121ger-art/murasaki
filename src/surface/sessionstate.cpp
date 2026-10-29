#include "sessionstate.h"

#include <algorithm>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

bool truthy(const QJsonValue &value)
{
    return value.isBool() ? value.toBool() : value.toInt() > 0;
}

// Hyprland reports the fullscreen state of a client as a mode: 0 none, 1 maximized, 2 fullscreen, 3 both (older versions: a boolean). Only a mode with bit 2
// is a fullscreen window; a maximized one still covers the output (truthy).
bool hyprlandFullscreen(const QJsonValue &value)
{
    return value.isBool() ? value.toBool() : (value.toInt() & 2) != 0;
}

// True when a tiled leaf window (or any fullscreen container) is inside node. The recursion depth is bounded by
// QJsonDocument's own nesting limit.
bool swayCovers(const QJsonObject &node, bool floating)
{
    const QString type = node.value(u"type"_s).toString();
    if (type != u"workspace" && node.value(u"fullscreen_mode"_s).toInt() > 0)
        return true;
    const QJsonArray children = node.value(u"nodes"_s).toArray();
    if (!floating && type == u"con" && children.isEmpty())
        return true;
    for (const QJsonValue &child : children) {
        if (swayCovers(child.toObject(), floating))
            return true;
    }
    for (const QJsonValue &child : node.value(u"floating_nodes"_s).toArray()) {
        if (swayCovers(child.toObject(), true))
            return true;
    }
    return false;
}

// The application class of a sway container: Wayland clients have app_id, Xwayland ones a window class.
QString swayApp(const QJsonObject &node)
{
    const QString appId = node.value(u"app_id"_s).toString();
    return !appId.isEmpty() ? appId : node.value(u"window_properties"_s).toObject().value(u"class"_s).toString();
}

// The application of the first fullscreen container inside `node` (a workspace), found; the depth is bounded by QJsonDocument's nesting limit.
bool swayFullscreenApp(const QJsonObject &node, QString *app)
{
    if (node.value(u"type"_s).toString() != u"workspace" && node.value(u"fullscreen_mode"_s).toInt() > 0) {
        *app = swayApp(node);
        return true;
    }
    for (const char *key : {"nodes", "floating_nodes"}) {
        for (const QJsonValue &child : node.value(QLatin1String(key)).toArray()) {
            if (swayFullscreenApp(child.toObject(), app))
                return true;
        }
    }
    return false;
}

// Every leaf window of the tree: a container without children that has an application.
void swayOpenApps(const QJsonObject &node, QStringList &apps)
{
    const QJsonArray children = node.value(u"nodes"_s).toArray();
    const QJsonArray floating = node.value(u"floating_nodes"_s).toArray();
    if (children.isEmpty() && floating.isEmpty() && node.value(u"type"_s).toString().endsWith(u"con"))
        addOpenApp(apps, swayApp(node));
    for (const QJsonValue &child : children)
        swayOpenApps(child.toObject(), apps);
    for (const QJsonValue &child : floating)
        swayOpenApps(child.toObject(), apps);
}

} // namespace

void addOpenApp(QStringList &apps, const QString &appClass)
{
    const QString name = appClass.trimmed().left(kMaxAppClassLength);
    if (name.isEmpty() || apps.size() >= kMaxOpenApps || apps.contains(name))
        return;
    apps.append(name);
}

QJsonObject outputFullscreenJson(const SessionState &state, const QStringList &outputs)
{
    QJsonObject json;
    for (const QString &output : outputs) {
        const OutputSession session = state.forOutput(output);
        json.insert(output, QJsonObject{{u"fullscreen"_s, session.fullscreen}, {u"app"_s, session.fullscreenApp}});
    }
    return json;
}

QJsonObject DesktopInfo::toJson() const
{
    return QJsonObject{{u"id"_s, id}, {u"name"_s, name}, {u"number"_s, number}};
}

OutputSession SessionState::forOutput(const QString &output) const
{
    const auto it = outputs.constFind(output);
    if (it != outputs.cend())
        return *it;
    OutputSession session;
    session.desktop = defaultDesktop;
    return session;
}

SessionState parseKWinReport(const QJsonObject &report)
{
    SessionState state;
    const QString current = report.value(u"currentDesktop"_s).toString();
    if (!current.isEmpty())
        state.defaultDesktop = current;
    int number = 0;
    for (const QJsonValue &value : report.value(u"desktops"_s).toArray()) {
        const QJsonObject desktop = value.toObject();
        state.desktops.append({desktop.value(u"id"_s).toString(), desktop.value(u"name"_s).toString(), ++number});
    }
    state.activeApp = report.value(u"activeWindow"_s).toObject().value(u"resourceClass"_s).toString().left(kMaxAppClassLength);
    for (const QJsonValue &value : report.value(u"windows"_s).toArray()) {
        const QJsonObject window = value.toObject();
        addOpenApp(state.openApps, window.value(u"resourceClass"_s).toString());
        const QString output = window.value(u"output"_s).toString();
        if (output.isEmpty())
            continue;
        OutputSession &session = state.outputs[output];
        session.desktop = state.defaultDesktop;
        const bool onCurrent = window.value(u"onAllDesktops"_s).toBool()
            || window.value(u"desktops"_s).toArray().contains(QJsonValue(current));
        const bool visible = onCurrent && !window.value(u"minimized"_s).toBool();
        if (visible && (window.value(u"fullScreen"_s).toBool() || window.value(u"coversOutput"_s).toBool()))
            session.occluded = true;
        const QString app = window.value(u"resourceClass"_s).toString();
        if (visible && window.value(u"fullScreen"_s).toBool()) {
            if (!session.fullscreen) {
                session.fullscreen = true;
                session.fullscreenApp = app.left(kMaxAppClassLength);
            }
            addOpenApp(state.fullscreenApps, app); // (the same bounds as the open list: a class is at most 128 characters, a list at most 64)
        }
    }
    return state;
}

SessionState parseSwayState(const QJsonArray &workspaces, const QJsonObject &tree)
{
    SessionState state;
    int number = 0;
    for (const QJsonValue &value : workspaces) {
        const QJsonObject workspace = value.toObject();
        const QString name = workspace.value(u"name"_s).toString();
        // Named workspaces report num -1, so the list position is the number, as for KWin.
        state.desktops.append({name, name, ++number});
        if (workspace.value(u"visible"_s).toBool())
            state.outputs.insert(workspace.value(u"output"_s).toString(), OutputSession{name, false});
    }
    for (const QJsonValue &outputValue : tree.value(u"nodes"_s).toArray()) {
        const QJsonObject output = outputValue.toObject();
        const auto session = state.outputs.find(output.value(u"name"_s).toString());
        if (output.value(u"type"_s).toString() != u"output" || session == state.outputs.end())
            continue;
        for (const QJsonValue &workspaceValue : output.value(u"nodes"_s).toArray()) {
            const QJsonObject workspace = workspaceValue.toObject();
            if (workspace.value(u"type"_s).toString() == u"workspace"
                && workspace.value(u"name"_s).toString() == session->desktop) {
                session->occluded = swayCovers(workspace, false);
                QString app;
                if (swayFullscreenApp(workspace, &app)) {
                    session->fullscreen = true;
                    session->fullscreenApp = app.left(kMaxAppClassLength);
                }
            }
        }
    }
    swayOpenApps(tree, state.openApps);
    return state;
}

SessionState parseHyprlandState(const QJsonArray &monitors, const QJsonArray &clients, const QJsonArray &workspaces)
{
    SessionState state;
    for (const QJsonValue &value : workspaces) {
        const QJsonObject workspace = value.toObject();
        const int id = workspace.value(u"id"_s).toInt();
        if (id <= 0)
            continue; // special workspaces (negative ids) are overlays, deliberately not desktops and never occluding
        state.desktops.append({QString::number(id), workspace.value(u"name"_s).toString(), id});
    }
    std::sort(state.desktops.begin(), state.desktops.end(),
              [](const DesktopInfo &a, const DesktopInfo &b) { return a.number < b.number; });

    QHash<int, QString> monitorNames;
    for (const QJsonValue &value : monitors) {
        const QJsonObject monitor = value.toObject();
        const int workspace = monitor.value(u"activeWorkspace"_s).toObject().value(u"id"_s).toInt();
        state.outputs.insert(monitor.value(u"name"_s).toString(), OutputSession{QString::number(workspace), false});
        monitorNames.insert(monitor.value(u"id"_s).toInt(-1), monitor.value(u"name"_s).toString());
    }
    for (const QJsonValue &value : clients) {
        const QJsonObject client = value.toObject();
        if (!client.value(u"mapped"_s).toBool(true))
            continue;
        addOpenApp(state.openApps, client.value(u"class"_s).toString());
        if (client.value(u"hidden"_s).toBool())
            continue;
        if (hyprlandFullscreen(client.value(u"fullscreen"_s)) && client.value(u"workspace"_s).isObject()) {
            const QString clientWorkspace = QString::number(client.value(u"workspace"_s).toObject().value(u"id"_s).toInt());
            const QString output = monitorNames.value(client.value(u"monitor"_s).toInt(-1));
            if (const auto it = state.outputs.find(output); it != state.outputs.end() && it->desktop == clientWorkspace && !it->fullscreen) {
                it->fullscreen = true;
                it->fullscreenApp = client.value(u"class"_s).toString().left(kMaxAppClassLength);
            }
        }
        if (client.value(u"floating"_s).toBool() && !truthy(client.value(u"fullscreen"_s)))
            continue;
        if (!client.value(u"workspace"_s).isObject())
            continue; // no workspace to attribute it to, so it cannot cover an output
        const QString workspace = QString::number(client.value(u"workspace"_s).toObject().value(u"id"_s).toInt());
        for (OutputSession &session : state.outputs) {
            if (session.desktop == workspace)
                session.occluded = true;
        }
    }
    return state;
}

MonitorKind selectMonitorKind(const QProcessEnvironment &env, bool kwinOnBus, bool gnomeShellOnBus)
{
    const bool kde = !env.value(u"KDE_FULL_SESSION"_s).isEmpty()
        || env.value(u"XDG_CURRENT_DESKTOP"_s).split(u':').contains(u"KDE"_s, Qt::CaseInsensitive);
    if (kde && kwinOnBus)
        return MonitorKind::KWin;
    if (gnomeShellOnBus && env.value(u"XDG_CURRENT_DESKTOP"_s).split(u':').contains(u"GNOME"_s, Qt::CaseInsensitive))
        return MonitorKind::GnomeShell;
    if (!env.value(u"SWAYSOCK"_s).isEmpty())
        return MonitorKind::Sway;
    if (!env.value(u"HYPRLAND_INSTANCE_SIGNATURE"_s).isEmpty())
        return MonitorKind::Hyprland;
    return MonitorKind::None;
}

} // namespace murasaki
