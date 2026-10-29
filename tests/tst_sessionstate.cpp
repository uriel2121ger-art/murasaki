#include "i3ipc.h"
#include "sessionstate.h"
#include "testutil.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestSessionState : public QObject
{
    Q_OBJECT

private slots:
    void kwinReport();
    void kwinIgnoresMinimizedAndOtherDesktops();
    void kwinReportsTheFocusedAndTheFullscreenApps();
    void otherCompositorsKnowNoApps();
    void swayState();
    void swayFloatingFullscreenCovers();
    void hyprlandState();
    void hyprlandBoolFullscreen();
    void selectsMonitor();
    void selectsTheGnomeShellMonitor();
    void theShellReportReadsLikeKWinsAndIsBounded();
    void i3ipcFraming();
    void i3ipcFlagsCorruption();
    void kwinEmptyCurrentDesktopIsWildcard();
    void swayFullscreenTiledContainer();
    void swayFloatingContainerWithTiledChild();
    void swayVisibleWorkspaceMissingFromTree();
    void swayNamedWorkspaceGetsListPosition();
    void hyprlandFullscreenModes();
    void hyprlandUnmappedTiledClient();
    void hyprlandSpecialWorkspaceClient();
    void hyprlandClientWithoutWorkspace();
    void hyprlandMonitorWithoutActiveWorkspace();
    void kwinFullscreenIsPerOutputAndNotMaximized();
    void theShellReportGivesTheSameFacts();
    void swayFullscreenAndOpenApps();
    void hyprlandFullscreenPerMonitorAndOpenApps();
    void openAppsAreBoundedAndPlain();
    void theFactsAsJson();
    void hyprlandMaximizedIsNotFullscreen();
    void applicationClassesAreBounded();

private:
    static QJsonArray array(const char *text) { return QJsonDocument::fromJson(text).array(); }
};

void TestSessionState::kwinReport()
{
    const SessionState state = parseKWinReport(testutil::json(
        "{\"currentDesktop\": \"d1\","
        " \"desktops\": [{\"id\": \"d1\", \"name\": \"One\"}, {\"id\": \"d2\", \"name\": \"Two\"}],"
        " \"windows\": ["
        "  {\"output\": \"DP-1\", \"desktops\": [\"d1\"], \"onAllDesktops\": false, \"fullScreen\": false,"
        "   \"minimized\": false, \"coversOutput\": true},"
        "  {\"output\": \"HDMI-A-1\", \"desktops\": [\"d2\"], \"onAllDesktops\": false, \"fullScreen\": true,"
        "   \"minimized\": false, \"coversOutput\": true},"
        "  {\"output\": \"HDMI-A-1\", \"desktops\": [\"d1\"], \"onAllDesktops\": false, \"fullScreen\": false,"
        "   \"minimized\": false, \"coversOutput\": false}]}"));

    QCOMPARE(state.defaultDesktop, u"d1"_s);
    QCOMPARE(state.desktops.size(), 2);
    QCOMPARE(state.desktops[1].id, u"d2"_s);
    QCOMPARE(state.desktops[1].name, u"Two"_s);
    QCOMPARE(state.desktops[1].number, 2);
    QCOMPARE(state.forOutput(u"DP-1"_s).desktop, u"d1"_s);
    QVERIFY(state.forOutput(u"DP-1"_s).occluded);
    QVERIFY(!state.forOutput(u"HDMI-A-1"_s).occluded); // its fullscreen window is on another desktop
    QCOMPARE(state.forOutput(u"DP-2"_s).desktop, u"d1"_s); // unreported outputs follow the global desktop
    QVERIFY(!state.forOutput(u"DP-2"_s).occluded);
}

void TestSessionState::kwinIgnoresMinimizedAndOtherDesktops()
{
    const SessionState state = parseKWinReport(testutil::json(
        "{\"currentDesktop\": \"d1\", \"desktops\": [{\"id\": \"d1\", \"name\": \"One\"}],"
        " \"windows\": ["
        "  {\"output\": \"DP-1\", \"desktops\": [\"d1\"], \"onAllDesktops\": false, \"fullScreen\": true,"
        "   \"minimized\": true, \"coversOutput\": true},"
        "  {\"output\": \"DP-2\", \"desktops\": [], \"onAllDesktops\": true, \"fullScreen\": true,"
        "   \"minimized\": false, \"coversOutput\": true}]}"));
    QVERIFY(!state.forOutput(u"DP-1"_s).occluded);
    QVERIFY(state.forOutput(u"DP-2"_s).occluded); // on all desktops
}

void TestSessionState::swayState()
{
    const QJsonArray workspaces = array(
        "[{\"name\": \"1\", \"num\": 1, \"visible\": true, \"output\": \"DP-1\"},"
        " {\"name\": \"2\", \"num\": 2, \"visible\": true, \"output\": \"HDMI-A-1\"},"
        " {\"name\": \"web\", \"num\": -1, \"visible\": false, \"output\": \"DP-1\"}]");
    const QJsonObject tree = testutil::json(
        "{\"type\": \"root\", \"nodes\": ["
        " {\"type\": \"output\", \"name\": \"__i3\", \"nodes\": [{\"type\": \"workspace\", \"name\": \"__i3_scratch\","
        "   \"nodes\": [{\"type\": \"con\", \"nodes\": []}]}]},"
        " {\"type\": \"output\", \"name\": \"DP-1\", \"nodes\": ["
        "   {\"type\": \"workspace\", \"name\": \"1\", \"nodes\": [],"
        "    \"floating_nodes\": [{\"type\": \"floating_con\", \"nodes\": [], \"fullscreen_mode\": 0}]},"
        "   {\"type\": \"workspace\", \"name\": \"web\", \"nodes\": [{\"type\": \"con\", \"nodes\": []}]}]},"
        " {\"type\": \"output\", \"name\": \"HDMI-A-1\", \"nodes\": ["
        "   {\"type\": \"workspace\", \"name\": \"2\", \"nodes\": [{\"type\": \"con\", \"nodes\": ["
        "     {\"type\": \"con\", \"nodes\": []}]}]}]}]}");

    const SessionState state = parseSwayState(workspaces, tree);
    QCOMPARE(state.forOutput(u"DP-1"_s).desktop, u"1"_s);
    QVERIFY(!state.forOutput(u"DP-1"_s).occluded);     // only a floating window
    QCOMPARE(state.forOutput(u"HDMI-A-1"_s).desktop, u"2"_s);
    QVERIFY(state.forOutput(u"HDMI-A-1"_s).occluded);  // a tiled window
    QCOMPARE(state.desktops.size(), 3);
    QCOMPARE(state.desktops[2].id, u"web"_s);
}

void TestSessionState::swayFloatingFullscreenCovers()
{
    const SessionState state = parseSwayState(
        array("[{\"name\": \"1\", \"num\": 1, \"visible\": true, \"output\": \"DP-1\"}]"),
        testutil::json("{\"type\": \"root\", \"nodes\": [{\"type\": \"output\", \"name\": \"DP-1\", \"nodes\": ["
                       " {\"type\": \"workspace\", \"name\": \"1\", \"nodes\": [], \"floating_nodes\": ["
                       "  {\"type\": \"floating_con\", \"nodes\": [], \"fullscreen_mode\": 1}]}]}]}"));
    QVERIFY(state.forOutput(u"DP-1"_s).occluded);
}

void TestSessionState::hyprlandState()
{
    const SessionState state = parseHyprlandState(
        array("[{\"id\": 0, \"name\": \"DP-1\", \"activeWorkspace\": {\"id\": 1, \"name\": \"1\"}},"
              " {\"id\": 1, \"name\": \"HDMI-A-1\", \"activeWorkspace\": {\"id\": 2, \"name\": \"2\"}}]"),
        array("[{\"mapped\": true, \"hidden\": false, \"floating\": true, \"fullscreen\": 0, \"workspace\": {\"id\": 1}},"
              " {\"mapped\": true, \"hidden\": false, \"floating\": false, \"fullscreen\": 0, \"workspace\": {\"id\": 2}},"
              " {\"mapped\": true, \"hidden\": false, \"floating\": false, \"fullscreen\": 0, \"workspace\": {\"id\": 3}}]"),
        array("[{\"id\": 2, \"name\": \"2\"}, {\"id\": 1, \"name\": \"1\"}, {\"id\": 3, \"name\": \"3\"},"
              " {\"id\": -99, \"name\": \"special:scratch\"}]"));
    QCOMPARE(state.forOutput(u"DP-1"_s).desktop, u"1"_s);
    QVERIFY(!state.forOutput(u"DP-1"_s).occluded);
    QCOMPARE(state.forOutput(u"HDMI-A-1"_s).desktop, u"2"_s);
    QVERIFY(state.forOutput(u"HDMI-A-1"_s).occluded);
    QCOMPARE(state.desktops.size(), 3); // special workspaces are not desktops
    QCOMPARE(state.desktops[0].id, u"1"_s);
    QCOMPARE(state.desktops[0].number, 1);
}

void TestSessionState::hyprlandBoolFullscreen()
{
    const SessionState state = parseHyprlandState(
        array("[{\"id\": 0, \"name\": \"DP-1\", \"activeWorkspace\": {\"id\": 1, \"name\": \"1\"}}]"),
        array("[{\"mapped\": true, \"hidden\": false, \"floating\": true, \"fullscreen\": true, \"workspace\": {\"id\": 1}},"
              " {\"mapped\": true, \"hidden\": true, \"floating\": false, \"fullscreen\": 0, \"workspace\": {\"id\": 1}}]"),
        array("[{\"id\": 1, \"name\": \"1\"}]"));
    QVERIFY(state.forOutput(u"DP-1"_s).occluded);
}

void TestSessionState::selectsMonitor()
{
    QProcessEnvironment kde;
    kde.insert(u"XDG_CURRENT_DESKTOP"_s, u"KDE"_s);
    QCOMPARE(selectMonitorKind(kde, true), MonitorKind::KWin);
    QCOMPARE(selectMonitorKind(kde, false), MonitorKind::None); // KWin not on the bus

    QProcessEnvironment sway;
    sway.insert(u"SWAYSOCK"_s, u"/run/user/1000/sway-ipc.sock"_s);
    QCOMPARE(selectMonitorKind(sway, false), MonitorKind::Sway);

    QProcessEnvironment hypr;
    hypr.insert(u"HYPRLAND_INSTANCE_SIGNATURE"_s, u"abc"_s);
    QCOMPARE(selectMonitorKind(hypr, false), MonitorKind::Hyprland);

    QCOMPARE(selectMonitorKind(QProcessEnvironment(), false), MonitorKind::None);
}

void TestSessionState::selectsTheGnomeShellMonitor()
{
    QProcessEnvironment gnome;
    gnome.insert(u"XDG_CURRENT_DESKTOP"_s, u"ubuntu:GNOME"_s);
    QCOMPARE(selectMonitorKind(gnome, false, true), MonitorKind::GnomeShell);
    QCOMPARE(selectMonitorKind(gnome, false, false), MonitorKind::None); // the shell is not on the bus (X11 without it)
    // Plasma first, then the others, as before; GNOME's name alone with KWin on the bus is still Plasma.
    QProcessEnvironment kde;
    kde.insert(u"XDG_CURRENT_DESKTOP"_s, u"KDE"_s);
    QCOMPARE(selectMonitorKind(kde, true, true), MonitorKind::KWin);
    QCOMPARE(selectMonitorKind(gnome, true, true), MonitorKind::GnomeShell);
}

void TestSessionState::theShellReportReadsLikeKWinsAndIsBounded()
{
    // What lib/report.js of the extension produces.
    const SessionState state = parseKWinReport(testutil::json(
        "{\"currentDesktop\": \"1\", \"desktops\": [{\"id\": \"0\", \"name\": \"Workspace 1\"}, {\"id\": \"1\", \"name\": \"Workspace 2\"}],"
        " \"activeWindow\": {\"resourceClass\": \"org.gnome.Totem\", \"fullScreen\": true},"
        " \"windows\": [{\"output\": \"DP-1\", \"desktops\": [\"1\"], \"onAllDesktops\": false, \"fullScreen\": true,"
        "   \"minimized\": false, \"coversOutput\": true, \"resourceClass\": \"org.gnome.Totem\"},"
        "  {\"output\": \"HDMI-A-1\", \"desktops\": [\"0\"], \"onAllDesktops\": false, \"fullScreen\": false,"
        "   \"minimized\": false, \"coversOutput\": true, \"resourceClass\": \"firefox\"}]}"));
    QCOMPARE(state.defaultDesktop, u"1"_s);
    QVERIFY(state.outputs.value(u"DP-1"_s).occluded);
    QVERIFY(!state.outputs.value(u"HDMI-A-1"_s).occluded); // covered, but on another workspace
    QCOMPARE(state.activeApp, u"org.gnome.Totem"_s);
    QCOMPARE(state.fullscreenApps, QStringList{u"org.gnome.Totem"_s});
}

void TestSessionState::i3ipcFraming()
{
    const QByteArray frame = i3ipcFrame(4, "{}");
    QCOMPARE(frame.size(), 6 + 4 + 4 + 2);
    QVERIFY(frame.startsWith("i3-ipc"));

    QByteArray buffer = i3ipcFrame(1, "[1]") + i3ipcFrame(kI3IpcEventBit | 0, "{\"change\":\"focus\"}");
    const QByteArray partial = i3ipcFrame(4, "{\"nodes\":[]}");
    buffer += partial.left(9);
    const QList<I3IpcMessage> messages = i3ipcTake(buffer);
    QCOMPARE(messages.size(), 2);
    QCOMPARE(messages[0].type, 1u);
    QCOMPARE(messages[0].payload, QByteArray("[1]"));
    QVERIFY(messages[1].type & kI3IpcEventBit);
    QCOMPARE(buffer, partial.left(9)); // the incomplete message stays buffered
    buffer += partial.mid(9);
    QCOMPARE(i3ipcTake(buffer).size(), 1);
    QVERIFY(buffer.isEmpty());

    QByteArray garbage("not-i3-ipc-data-at-all");
    QVERIFY(i3ipcTake(garbage).isEmpty());
    QVERIFY(garbage.isEmpty()); // malformed data is dropped
}

void TestSessionState::i3ipcFlagsCorruption()
{
    bool corrupt = true;
    QByteArray ok = i3ipcFrame(1, "[1]");
    QCOMPARE(i3ipcTake(ok, &corrupt).size(), 1);
    QVERIFY(!corrupt); // reset on every call

    QByteArray badMagic = i3ipcFrame(1, "[1]") + QByteArray("not-i3-ipc-data-at-all");
    const QList<I3IpcMessage> messages = i3ipcTake(badMagic, &corrupt);
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages[0].payload, QByteArray("[1]"));
    QVERIFY(corrupt);
    QVERIFY(badMagic.isEmpty());

    QByteArray oversized("i3-ipc");
    const quint32 header[2] = {16u * 1024 * 1024 + 1, 4};
    oversized.append(reinterpret_cast<const char *>(header), sizeof(header));
    corrupt = false;
    QVERIFY(i3ipcTake(oversized, &corrupt).isEmpty());
    QVERIFY(corrupt);
    QVERIFY(oversized.isEmpty());
}

void TestSessionState::kwinEmptyCurrentDesktopIsWildcard()
{
    const SessionState state = parseKWinReport(testutil::json(
        "{\"currentDesktop\": \"\", \"desktops\": [], \"windows\": []}"));
    QCOMPARE(state.defaultDesktop, u"*"_s);
    QCOMPARE(state.forOutput(u"DP-1"_s).desktop, u"*"_s);
}

namespace {
const char kSwayOneWorkspace[] = "[{\"name\": \"1\", \"num\": 1, \"visible\": true, \"output\": \"DP-1\"}]";
QJsonObject swayTree(const char *workspaceBody)
{
    const QByteArray text = QByteArray("{\"type\": \"root\", \"nodes\": [{\"type\": \"output\", \"name\": \"DP-1\","
                                       " \"nodes\": [{\"type\": \"workspace\", \"name\": \"1\", ")
        + workspaceBody + "}]}]}";
    return testutil::json(text.constData());
}
} // namespace

void TestSessionState::swayFullscreenTiledContainer()
{
    const SessionState state = parseSwayState(
        array(kSwayOneWorkspace),
        swayTree("\"nodes\": [{\"type\": \"con\", \"fullscreen_mode\": 1, \"nodes\": ["
                 "{\"type\": \"con\", \"nodes\": []}, {\"type\": \"con\", \"nodes\": []}]}]"));
    QVERIFY(state.forOutput(u"DP-1"_s).occluded);
}

void TestSessionState::swayFloatingContainerWithTiledChild()
{
    const SessionState state = parseSwayState(
        array(kSwayOneWorkspace),
        swayTree("\"nodes\": [], \"floating_nodes\": [{\"type\": \"floating_con\", \"fullscreen_mode\": 0,"
                 " \"nodes\": [{\"type\": \"con\", \"nodes\": []}]}]"));
    QVERIFY(!state.forOutput(u"DP-1"_s).occluded);
}

void TestSessionState::swayVisibleWorkspaceMissingFromTree()
{
    const SessionState state = parseSwayState(
        array("[{\"name\": \"9\", \"num\": 9, \"visible\": true, \"output\": \"DP-1\"}]"),
        swayTree("\"nodes\": [{\"type\": \"con\", \"nodes\": []}]")); // the tree only has workspace "1"
    QCOMPARE(state.forOutput(u"DP-1"_s).desktop, u"9"_s);
    QVERIFY(!state.forOutput(u"DP-1"_s).occluded);
}

void TestSessionState::swayNamedWorkspaceGetsListPosition()
{
    const SessionState state = parseSwayState(
        array("[{\"name\": \"1\", \"num\": 1, \"visible\": true, \"output\": \"DP-1\"},"
              " {\"name\": \"web\", \"num\": -1, \"visible\": false, \"output\": \"DP-1\"}]"),
        QJsonObject{});
    QCOMPARE(state.desktops.size(), 2);
    QCOMPARE(state.desktops[0].number, 1);
    QCOMPARE(state.desktops[1].id, u"web"_s);
    QCOMPARE(state.desktops[1].number, 2);
}

namespace {
SessionState hyprWithClient(const char *client)
{
    return parseHyprlandState(
        QJsonDocument::fromJson("[{\"name\": \"DP-1\", \"activeWorkspace\": {\"id\": 1, \"name\": \"1\"}}]").array(),
        QJsonDocument::fromJson(QByteArray("[") + client + "]").array(),
        QJsonDocument::fromJson("[{\"id\": 1, \"name\": \"1\"}]").array());
}
} // namespace

void TestSessionState::hyprlandFullscreenModes()
{
    for (const char *mode : {"1", "2", "3"}) {
        const QByteArray client = QByteArray("{\"mapped\": true, \"hidden\": false, \"floating\": true,"
                                             " \"fullscreen\": ") + mode + ", \"workspace\": {\"id\": 1}}";
        QVERIFY2(hyprWithClient(client.constData()).forOutput(u"DP-1"_s).occluded, mode);
    }
}

void TestSessionState::hyprlandUnmappedTiledClient()
{
    const SessionState state = hyprWithClient(
        "{\"mapped\": false, \"hidden\": false, \"floating\": false, \"fullscreen\": 0, \"workspace\": {\"id\": 1}}");
    QVERIFY(!state.forOutput(u"DP-1"_s).occluded);
}

void TestSessionState::hyprlandSpecialWorkspaceClient()
{
    const SessionState state = hyprWithClient(
        "{\"mapped\": true, \"hidden\": false, \"floating\": false, \"fullscreen\": 2, \"workspace\": {\"id\": -99}}");
    QVERIFY(!state.forOutput(u"DP-1"_s).occluded);
    QCOMPARE(state.forOutput(u"DP-1"_s).desktop, u"1"_s);
}

void TestSessionState::hyprlandClientWithoutWorkspace()
{
    // A monitor without an active workspace sits on desktop "0"; a client with no workspace object must not match it.
    const SessionState state = parseHyprlandState(
        array("[{\"name\": \"DP-2\"}]"),
        array("[{\"mapped\": true, \"hidden\": false, \"floating\": false, \"fullscreen\": 2}]"), QJsonArray{});
    QVERIFY(!state.forOutput(u"DP-2"_s).occluded);
    QVERIFY(!hyprWithClient("{\"mapped\": true, \"hidden\": false, \"floating\": false, \"fullscreen\": 2}")
                 .forOutput(u"DP-1"_s).occluded);
}

void TestSessionState::hyprlandMonitorWithoutActiveWorkspace()
{
    const SessionState state = parseHyprlandState(array("[{\"name\": \"DP-2\"}]"), QJsonArray{}, QJsonArray{});
    QCOMPARE(state.forOutput(u"DP-2"_s).desktop, u"0"_s);
    QVERIFY(!state.forOutput(u"DP-2"_s).occluded);
}

void TestSessionState::kwinReportsTheFocusedAndTheFullscreenApps()
{
    const SessionState state = parseKWinReport(testutil::json(
        "{\"currentDesktop\": \"d1\","
        " \"desktops\": [{\"id\": \"d1\", \"name\": \"One\"}, {\"id\": \"d2\", \"name\": \"Two\"}],"
        " \"activeWindow\": {\"resourceClass\": \"obs\", \"fullScreen\": false},"
        " \"windows\": ["
        "  {\"output\": \"DP-1\", \"desktops\": [\"d1\"], \"onAllDesktops\": false, \"fullScreen\": true,"
        "   \"minimized\": false, \"coversOutput\": true, \"resourceClass\": \"steam_app_620\"},"
        "  {\"output\": \"DP-1\", \"desktops\": [\"d2\"], \"onAllDesktops\": false, \"fullScreen\": true,"
        "   \"minimized\": false, \"coversOutput\": true, \"resourceClass\": \"elsewhere\"},"
        "  {\"output\": \"DP-1\", \"desktops\": [\"d1\"], \"onAllDesktops\": false, \"fullScreen\": true,"
        "   \"minimized\": true, \"coversOutput\": true, \"resourceClass\": \"hidden\"},"
        "  {\"output\": \"HDMI-A-1\", \"desktops\": [], \"onAllDesktops\": true, \"fullScreen\": true,"
        "   \"minimized\": false, \"coversOutput\": true, \"resourceClass\": \"mpv\"},"
        "  {\"output\": \"HDMI-A-1\", \"desktops\": [\"d1\"], \"onAllDesktops\": false, \"fullScreen\": false,"
        "   \"minimized\": false, \"coversOutput\": false, \"resourceClass\": \"konsole\"}]}"));
    QCOMPARE(state.activeApp, u"obs"_s);
    // fullscreen windows on the current desktop that are not minimized, whichever the output, each class once
    QCOMPARE(state.fullscreenApps, (QStringList{u"steam_app_620"_s, u"mpv"_s}));

    // no active window (the desktop has the focus) and no classes: nothing, not an error
    const SessionState bare = parseKWinReport(testutil::json(
        "{\"currentDesktop\": \"d1\", \"activeWindow\": null,"
        " \"windows\": [{\"output\": \"DP-1\", \"desktops\": [\"d1\"], \"fullScreen\": true}]}"));
    QVERIFY(bare.activeApp.isEmpty());
    QVERIFY(bare.fullscreenApps.isEmpty());
    // a report from before the fields existed parses as before
    QVERIFY(parseKWinReport(testutil::json("{\"currentDesktop\": \"d1\", \"windows\": []}")).activeApp.isEmpty());

    // The apps are part of the state: a change of focus is a change.
    SessionState other = state;
    other.activeApp = u"firefox"_s;
    QVERIFY(!(other == state));
}

void TestSessionState::otherCompositorsKnowNoApps()
{
    const SessionState sway = parseSwayState(array("[]"), testutil::json("{\"nodes\": []}"));
    QVERIFY(sway.activeApp.isEmpty());
    QVERIFY(sway.fullscreenApps.isEmpty());
}

void TestSessionState::kwinFullscreenIsPerOutputAndNotMaximized()
{
    const SessionState state = parseKWinReport(testutil::json(
        "{\"currentDesktop\": \"d1\", \"windows\": ["
        "  {\"output\": \"DP-1\", \"desktops\": [\"d1\"], \"fullScreen\": true, \"minimized\": false, \"coversOutput\": true, \"resourceClass\": \"mpv\"},"
        "  {\"output\": \"HDMI-A-1\", \"desktops\": [\"d1\"], \"fullScreen\": false, \"minimized\": false, \"coversOutput\": true, \"resourceClass\": \"maximizedapp\"},"
        "  {\"output\": \"HDMI-A-1\", \"desktops\": [\"d2\"], \"fullScreen\": true, \"minimized\": false, \"coversOutput\": true, \"resourceClass\": \"otherdesktop\"},"
        "  {\"output\": \"HDMI-A-1\", \"desktops\": [\"d1\"], \"fullScreen\": true, \"minimized\": true, \"coversOutput\": true, \"resourceClass\": \"minimized\"}]}"));
    QVERIFY(state.outputs.value(u"DP-1"_s).fullscreen);
    QCOMPARE(state.outputs.value(u"DP-1"_s).fullscreenApp, u"mpv"_s);
    // a maximized window covers the output but is not fullscreen; a fullscreen window of another desktop or a minimized one is not either
    QVERIFY(state.outputs.value(u"HDMI-A-1"_s).occluded);
    QVERIFY(!state.outputs.value(u"HDMI-A-1"_s).fullscreen);
    // all four are open applications (minimized and other desktops too)
    QCOMPARE(state.openApps, (QStringList{u"mpv"_s, u"maximizedapp"_s, u"otherdesktop"_s, u"minimized"_s}));
    // an output the state does not know has no fullscreen window
    QVERIFY(!state.forOutput(u"DP-9"_s).fullscreen);
}

void TestSessionState::theShellReportGivesTheSameFacts()
{
    // the shape lib/report.js produces for GNOME: output, fullScreen, resourceClass
    const SessionState state = parseKWinReport(testutil::json(
        "{\"currentDesktop\": \"0\", \"desktops\": [{\"id\": \"0\", \"name\": \"1\"}], \"activeWindow\": {\"resourceClass\": \"org.gnome.Totem\", \"fullScreen\": true},"
        " \"windows\": [{\"output\": \"eDP-1\", \"desktops\": [\"0\"], \"onAllDesktops\": false, \"fullScreen\": true, \"minimized\": false,"
        "  \"coversOutput\": true, \"resourceClass\": \"org.gnome.Totem\"}]}"));
    QVERIFY(state.outputs.value(u"eDP-1"_s).fullscreen);
    QCOMPARE(state.outputs.value(u"eDP-1"_s).fullscreenApp, u"org.gnome.Totem"_s);
    QCOMPARE(state.openApps, QStringList{u"org.gnome.Totem"_s});
}

void TestSessionState::swayFullscreenAndOpenApps()
{
    const QJsonArray workspaces = array("[{\"name\": \"1\", \"output\": \"DP-1\", \"visible\": true}, {\"name\": \"2\", \"output\": \"HDMI-A-1\", \"visible\": true}]");
    const QJsonObject tree = testutil::json(
        "{\"nodes\": ["
        " {\"type\": \"output\", \"name\": \"DP-1\", \"nodes\": [{\"type\": \"workspace\", \"name\": \"1\", \"nodes\": ["
        "   {\"type\": \"con\", \"fullscreen_mode\": 1, \"app_id\": \"mpv\", \"nodes\": []}], \"floating_nodes\": []}]},"
        " {\"type\": \"output\", \"name\": \"HDMI-A-1\", \"nodes\": [{\"type\": \"workspace\", \"name\": \"2\", \"nodes\": ["
        "   {\"type\": \"con\", \"fullscreen_mode\": 0, \"app_id\": \"\", \"window_properties\": {\"class\": \"Steam\"}, \"nodes\": []}], \"floating_nodes\": []}]}]}");
    const SessionState state = parseSwayState(workspaces, tree);
    QVERIFY(state.outputs.value(u"DP-1"_s).fullscreen);
    QCOMPARE(state.outputs.value(u"DP-1"_s).fullscreenApp, u"mpv"_s);
    QVERIFY(!state.outputs.value(u"HDMI-A-1"_s).fullscreen);
    QVERIFY(state.outputs.value(u"HDMI-A-1"_s).occluded); // (a tiled window)
    // Wayland clients by app_id, Xwayland ones by class
    QCOMPARE(state.openApps, (QStringList{u"mpv"_s, u"Steam"_s}));
}

void TestSessionState::hyprlandFullscreenPerMonitorAndOpenApps()
{
    const QJsonArray monitors = array("[{\"id\": 0, \"name\": \"DP-1\", \"activeWorkspace\": {\"id\": 1}}, {\"id\": 1, \"name\": \"HDMI-A-1\", \"activeWorkspace\": {\"id\": 2}}]");
    const QJsonArray clients = array(
        "[{\"mapped\": true, \"hidden\": false, \"floating\": false, \"fullscreen\": 2, \"monitor\": 1, \"class\": \"mpv\", \"workspace\": {\"id\": 2}},"
        " {\"mapped\": true, \"hidden\": false, \"floating\": false, \"fullscreen\": 0, \"monitor\": 0, \"class\": \"kitty\", \"workspace\": {\"id\": 1}},"
        " {\"mapped\": true, \"hidden\": false, \"floating\": false, \"fullscreen\": 1, \"monitor\": 0, \"class\": \"game\", \"workspace\": {\"id\": 5}},"
        " {\"mapped\": false, \"hidden\": false, \"class\": \"unmapped\", \"workspace\": {\"id\": 1}}]");
    const QJsonArray workspaces = array("[{\"id\": 1, \"name\": \"1\"}, {\"id\": 2, \"name\": \"2\"}, {\"id\": 5, \"name\": \"5\"}]");
    const SessionState state = parseHyprlandState(monitors, clients, workspaces);
    QVERIFY(state.outputs.value(u"HDMI-A-1"_s).fullscreen);
    QCOMPARE(state.outputs.value(u"HDMI-A-1"_s).fullscreenApp, u"mpv"_s);
    // a fullscreen client on a workspace that no monitor shows is not a fullscreen window anyone sees
    QVERIFY(!state.outputs.value(u"DP-1"_s).fullscreen);
    QCOMPARE(state.openApps, (QStringList{u"mpv"_s, u"kitty"_s, u"game"_s}));
}

void TestSessionState::openAppsAreBoundedAndPlain()
{
    QStringList apps;
    addOpenApp(apps, u"  firefox "_s);
    addOpenApp(apps, u"firefox"_s);
    addOpenApp(apps, QString());
    addOpenApp(apps, u"   "_s);
    addOpenApp(apps, QString(300, QLatin1Char('x')));
    QCOMPARE(apps.size(), 2);
    QCOMPARE(apps.at(0), u"firefox"_s);
    QCOMPARE(apps.at(1).size(), kMaxAppClassLength);
    for (int i = 0; i < 200; ++i)
        addOpenApp(apps, QStringLiteral("app%1").arg(i));
    QCOMPARE(apps.size(), kMaxOpenApps);
    // a pathological report cannot grow the list
    QJsonArray windows;
    for (int i = 0; i < 500; ++i)
        windows.append(QJsonObject{{u"output"_s, u"DP-1"_s}, {u"resourceClass"_s, QStringLiteral("w%1").arg(i)}, {u"desktops"_s, QJsonArray{u"d1"_s}}});
    const SessionState state = parseKWinReport(QJsonObject{{u"currentDesktop"_s, u"d1"_s}, {u"windows"_s, windows}});
    QCOMPARE(state.openApps.size(), kMaxOpenApps);
}

void TestSessionState::theFactsAsJson()
{
    SessionState state;
    state.outputs[u"DP-1"_s].fullscreen = true;
    state.outputs[u"DP-1"_s].fullscreenApp = u"mpv"_s;
    const QJsonObject json = outputFullscreenJson(state, {u"DP-1"_s, u"HDMI-A-1"_s});
    QCOMPARE(json.value(u"DP-1"_s).toObject().value(u"fullscreen"_s).toBool(), true);
    QCOMPARE(json.value(u"DP-1"_s).toObject().value(u"app"_s).toString(), u"mpv"_s);
    QCOMPARE(json.value(u"HDMI-A-1"_s).toObject().value(u"fullscreen"_s).toBool(), false);
    QVERIFY(!(state == SessionState())); // the new fields are part of the state: a change is a change
}

void TestSessionState::hyprlandMaximizedIsNotFullscreen()
{
    const QJsonArray monitors = array("[{\"id\": 0, \"name\": \"DP-1\", \"activeWorkspace\": {\"id\": 1}}]");
    const QJsonArray workspaces = array("[{\"id\": 1, \"name\": \"1\"}]");
    const auto client = [](int mode) {
        return array(QByteArray("[{\"mapped\": true, \"hidden\": false, \"floating\": false, \"fullscreen\": ") + QByteArray::number(mode)
                     + ", \"monitor\": 0, \"class\": \"app\", \"workspace\": {\"id\": 1}}]");
    };
    // 1 = maximized: covers the output, is no fullscreen window
    SessionState state = parseHyprlandState(monitors, client(1), workspaces);
    QVERIFY(state.outputs.value(u"DP-1"_s).occluded);
    QVERIFY(!state.outputs.value(u"DP-1"_s).fullscreen);
    for (int mode : {2, 3}) {
        state = parseHyprlandState(monitors, client(mode), workspaces);
        QVERIFY2(state.outputs.value(u"DP-1"_s).fullscreen, qPrintable(QString::number(mode)));
        QVERIFY(state.outputs.value(u"DP-1"_s).occluded);
    }
    state = parseHyprlandState(monitors, client(0), workspaces);
    QVERIFY(!state.outputs.value(u"DP-1"_s).fullscreen);
    // an older Hyprland says true
    state = parseHyprlandState(monitors, array("[{\"mapped\": true, \"hidden\": false, \"floating\": false, \"fullscreen\": true, \"monitor\": 0, \"class\": \"app\", \"workspace\": {\"id\": 1}}]"), workspaces);
    QVERIFY(state.outputs.value(u"DP-1"_s).fullscreen);
}

void TestSessionState::applicationClassesAreBounded()
{
    const QString huge(5000, QLatin1Char('x'));
    QJsonArray windows;
    for (int i = 0; i < 300; ++i)
        windows.append(QJsonObject{{u"output"_s, u"DP-1"_s}, {u"desktops"_s, QJsonArray{u"d1"_s}}, {u"fullScreen"_s, true}, {u"resourceClass"_s, QStringLiteral("%1%2").arg(huge).arg(i)}});
    const SessionState state = parseKWinReport(QJsonObject{{u"currentDesktop"_s, u"d1"_s},
                                                           {u"activeWindow"_s, QJsonObject{{u"resourceClass"_s, huge}}},
                                                           {u"windows"_s, windows}});
    QCOMPARE(state.activeApp.size(), kMaxAppClassLength);
    QCOMPARE(state.fullscreenApps.size(), 1); // (the first 128 characters are the same for all 300)
    QVERIFY(state.fullscreenApps.first().size() <= kMaxAppClassLength);
    QVERIFY(state.openApps.size() <= kMaxOpenApps);
}

QTEST_GUILESS_MAIN(TestSessionState)
#include "tst_sessionstate.moc"
