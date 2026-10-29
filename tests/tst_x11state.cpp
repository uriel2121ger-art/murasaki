#include "x11state.h"

#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestX11State : public QObject
{
    Q_OBJECT

private slots:
    void desktopsAreNumberedAndNamed();
    void outputsStartUncoveredOnTheCurrentDesktop();
    void aFullscreenWindowCoversItsOutput();
    void aMaximisedWindowCoversItsOutputOnlyBothWays();
    void aWindowThatContainsTheOutputCovers();
    void aSmallWindowCoversNothing();
    void theOutputWithTheLargestOverlapIsTheOne();
    void otherDesktopsAndMinimisedWindowsDoNotCover();
    void stickyWindowsCoverOnEveryDesktop();
    void ourOwnAndSpecialWindowsAreIgnored();
    void aStageWindowOfTheOverlayIsNeverAnApplicationOrAFullscreenWindow();
    void theFocusedAndTheFullscreenAppsAreReported();
    void aWindowOutsideEveryOutputCoversNothing();
    void noDesktopListMeansOneDesktop();
    void fullscreenIsPerOutputAndOpenAppsListEveryOrdinaryWindow();
};

static X11Output output(const QString &name, QRect geometry)
{
    return {name, geometry};
}

static X11Window window(quint32 id, const QString &app, QRect geometry, quint32 desktop = 0)
{
    X11Window w;
    w.id = id;
    w.resourceClass = app;
    w.geometry = geometry;
    w.desktop = desktop;
    return w;
}

static X11Snapshot twoScreens()
{
    X11Snapshot s;
    s.outputs = {output(u"DP-1"_s, QRect(0, 0, 1920, 1080)), output(u"HDMI-1"_s, QRect(1920, 0, 1280, 1024))};
    return s;
}

void TestX11State::desktopsAreNumberedAndNamed()
{
    X11Snapshot s = twoScreens();
    s.desktopCount = 3;
    s.desktopNames = {u"Work"_s, QString()};
    s.currentDesktop = 1;
    const SessionState state = buildX11State(s);
    QCOMPARE(state.desktops.size(), 3);
    QCOMPARE(state.desktops.at(0).id, u"0"_s);
    QCOMPARE(state.desktops.at(0).name, u"Work"_s);
    QCOMPARE(state.desktops.at(0).number, 1);
    QCOMPARE(state.desktops.at(1).name, u"Desktop 2"_s); // an empty name
    QCOMPARE(state.desktops.at(2).name, u"Desktop 3"_s); // a missing one
    QCOMPARE(state.defaultDesktop, u"1"_s);
    // a current desktop beyond the list reads as the first
    s.currentDesktop = 9;
    QCOMPARE(buildX11State(s).defaultDesktop, u"0"_s);
}

void TestX11State::noDesktopListMeansOneDesktop()
{
    const SessionState state = buildX11State(twoScreens());
    QCOMPARE(state.desktops.size(), 1);
    QCOMPARE(state.defaultDesktop, u"0"_s);
}

void TestX11State::outputsStartUncoveredOnTheCurrentDesktop()
{
    X11Snapshot s = twoScreens();
    s.desktopCount = 2;
    s.currentDesktop = 1;
    const SessionState state = buildX11State(s);
    QCOMPARE(state.outputs.size(), 2);
    for (const auto &session : state.outputs) {
        QCOMPARE(session.desktop, u"1"_s);
        QVERIFY(!session.occluded);
    }
}

void TestX11State::aFullscreenWindowCoversItsOutput()
{
    X11Snapshot s = twoScreens();
    X11Window w = window(1, u"mpv"_s, QRect(1920, 0, 1280, 1024));
    w.fullscreen = true;
    s.windows = {w};
    const SessionState state = buildX11State(s);
    QVERIFY(state.outputs.value(u"HDMI-1"_s).occluded);
    QVERIFY(!state.outputs.value(u"DP-1"_s).occluded);
    // fullscreen counts even when the manager reports a geometry that falls short of the output
    s.windows[0].geometry = QRect(1920, 0, 1000, 800);
    QVERIFY(buildX11State(s).outputs.value(u"HDMI-1"_s).occluded);
}

void TestX11State::aMaximisedWindowCoversItsOutputOnlyBothWays()
{
    X11Snapshot s = twoScreens();
    X11Window w = window(1, u"konsole"_s, QRect(10, 30, 1900, 1000)); // short of the screen by the panel
    w.maximizedBoth = true;
    s.windows = {w};
    QVERIFY(buildX11State(s).outputs.value(u"DP-1"_s).occluded);
    s.windows[0].maximizedBoth = false; // only vertically or horizontally: a tall window, not a cover
    QVERIFY(!buildX11State(s).outputs.value(u"DP-1"_s).occluded);
}

void TestX11State::aWindowThatContainsTheOutputCovers()
{
    X11Snapshot s = twoScreens();
    s.windows = {window(1, u"game"_s, QRect(-5, -5, 1930, 1090))}; // borderless, as big as the output (or more)
    QVERIFY(buildX11State(s).outputs.value(u"DP-1"_s).occluded);
}

void TestX11State::aSmallWindowCoversNothing()
{
    X11Snapshot s = twoScreens();
    s.windows = {window(1, u"calc"_s, QRect(100, 100, 400, 300))};
    const SessionState state = buildX11State(s);
    QVERIFY(!state.outputs.value(u"DP-1"_s).occluded);
    QVERIFY(!state.outputs.value(u"HDMI-1"_s).occluded);
}

void TestX11State::theOutputWithTheLargestOverlapIsTheOne()
{
    X11Snapshot s = twoScreens();
    X11Window w = window(1, u"mpv"_s, QRect(1500, 0, 1700, 1024)); // 420 px on DP-1, 1280 on HDMI-1
    w.fullscreen = true;
    s.windows = {w};
    const SessionState state = buildX11State(s);
    QVERIFY(state.outputs.value(u"HDMI-1"_s).occluded);
    QVERIFY(!state.outputs.value(u"DP-1"_s).occluded);
}

void TestX11State::aWindowOutsideEveryOutputCoversNothing()
{
    X11Snapshot s = twoScreens();
    X11Window w = window(1, u"mpv"_s, QRect(9000, 9000, 800, 600));
    w.fullscreen = true;
    s.windows = {w};
    const SessionState state = buildX11State(s);
    QVERIFY(!state.outputs.value(u"DP-1"_s).occluded);
    QVERIFY(!state.outputs.value(u"HDMI-1"_s).occluded);
    QCOMPARE(state.fullscreenApps, QStringList{u"mpv"_s}); // still a fullscreen application for the rules
}

void TestX11State::otherDesktopsAndMinimisedWindowsDoNotCover()
{
    X11Snapshot s = twoScreens();
    s.desktopCount = 2;
    s.currentDesktop = 0;
    X11Window elsewhere = window(1, u"mpv"_s, QRect(0, 0, 1920, 1080), 1);
    elsewhere.fullscreen = true;
    X11Window hidden = window(2, u"vlc"_s, QRect(0, 0, 1920, 1080), 0);
    hidden.fullscreen = true;
    hidden.hidden = true;
    s.windows = {elsewhere, hidden};
    const SessionState state = buildX11State(s);
    QVERIFY(!state.outputs.value(u"DP-1"_s).occluded);
    QVERIFY(state.fullscreenApps.isEmpty());
    // switching to its desktop makes it count
    s.currentDesktop = 1;
    QVERIFY(buildX11State(s).outputs.value(u"DP-1"_s).occluded);
}

void TestX11State::stickyWindowsCoverOnEveryDesktop()
{
    X11Snapshot s = twoScreens();
    s.desktopCount = 3;
    X11Window w = window(1, u"mpv"_s, QRect(0, 0, 1920, 1080), kX11AllDesktops);
    w.fullscreen = true;
    s.windows = {w};
    for (quint32 desktop : {0u, 1u, 2u}) {
        s.currentDesktop = desktop;
        QVERIFY(buildX11State(s).outputs.value(u"DP-1"_s).occluded);
    }
}

void TestX11State::ourOwnAndSpecialWindowsAreIgnored()
{
    X11Snapshot s = twoScreens();
    s.ownPid = 4242;
    X11Window ours = window(1, u"murasaki-surface"_s, QRect(0, 0, 1920, 1080));
    ours.pid = 4242;
    ours.fullscreen = true;
    X11Window dock = window(2, u"plasmashell"_s, QRect(0, 0, 1920, 1080));
    dock.normal = false; // a dock, desktop or splash window
    X11Window desktop = window(3, u"nemo-desktop"_s, QRect(0, 0, 1920, 1080));
    desktop.normal = false;
    desktop.fullscreen = true;
    s.windows = {ours, dock, desktop};
    s.activeWindow = 3;
    const SessionState state = buildX11State(s);
    QVERIFY(!state.outputs.value(u"DP-1"_s).occluded);
    QVERIFY(state.fullscreenApps.isEmpty());
    QVERIFY(state.activeApp.isEmpty()); // not an ordinary window
    s.activeWindow = 1; // our own window focused: not an application either
    QVERIFY(buildX11State(s).activeApp.isEmpty());
}

void TestX11State::aStageWindowOfTheOverlayIsNeverAnApplicationOrAFullscreenWindow()
{
    // The overlay's stage covers the whole output as a notification-type window of another process (pid 777, not the monitor's): even if a window manager listed it, it
    // is not "normal", so it covers nothing, is not fullscreen, is not an open application and is not the focus.
    X11Snapshot s = twoScreens();
    s.ownPid = 4242;
    X11Window stage = window(1, u"murasaki-overlay"_s, QRect(0, 0, 1920, 1080));
    stage.pid = 777;
    stage.normal = false;
    stage.desktop = kX11AllDesktops;
    s.windows = {stage};
    s.activeWindow = 1;
    const SessionState state = buildX11State(s);
    QVERIFY(!state.outputs.value(u"DP-1"_s).occluded);
    QVERIFY(!state.outputs.value(u"DP-1"_s).fullscreen);
    QVERIFY(state.openApps.isEmpty() && state.fullscreenApps.isEmpty() && state.activeApp.isEmpty());
    // while the same window with the state a stage must never have would count
    stage.normal = true;
    stage.fullscreen = true;
    s.windows = {stage};
    QVERIFY(buildX11State(s).outputs.value(u"DP-1"_s).fullscreen);
}

void TestX11State::theFocusedAndTheFullscreenAppsAreReported()
{
    X11Snapshot s = twoScreens();
    X11Window a = window(1, u"obs"_s, QRect(100, 100, 800, 600));
    X11Window b = window(2, u"steam_app_620"_s, QRect(0, 0, 1920, 1080));
    b.fullscreen = true;
    X11Window c = window(3, u"mpv"_s, QRect(1920, 0, 1280, 1024));
    c.fullscreen = true;
    X11Window dup = window(4, u"mpv"_s, QRect(0, 0, 100, 100));
    dup.fullscreen = true;
    s.windows = {a, b, c, dup};
    s.activeWindow = 1;
    const SessionState state = buildX11State(s);
    QCOMPARE(state.activeApp, u"obs"_s);
    QCOMPARE(state.fullscreenApps, (QStringList{u"steam_app_620"_s, u"mpv"_s})); // each once, in window order
    s.activeWindow = 99; // not in the list
    QVERIFY(buildX11State(s).activeApp.isEmpty());
    s.activeWindow = 0; // nothing focused
    QVERIFY(buildX11State(s).activeApp.isEmpty());
}

void TestX11State::fullscreenIsPerOutputAndOpenAppsListEveryOrdinaryWindow()
{
    X11Snapshot s = twoScreens();
    s.ownPid = 77;
    X11Window video = window(1, u"mpv"_s, QRect(1920, 0, 1280, 1024));
    video.fullscreen = true;
    X11Window maximized = window(2, u"editor"_s, QRect(0, 0, 1920, 1080));
    maximized.maximizedBoth = true;
    X11Window minimized = window(3, u"chat"_s, QRect(0, 0, 400, 300));
    minimized.hidden = true;
    X11Window elsewhere = window(4, u"game"_s, QRect(0, 0, 1920, 1080), 1);
    elsewhere.fullscreen = true; // on another desktop
    X11Window own = window(5, u"murasaki"_s, QRect(0, 0, 1920, 1080));
    own.fullscreen = true;
    own.pid = 77;
    X11Window dock = window(6, u"panel"_s, QRect(0, 0, 1920, 30));
    dock.normal = false;
    s.windows = {video, maximized, minimized, elsewhere, own, dock};
    const SessionState state = buildX11State(s);
    QVERIFY(state.outputs.value(u"HDMI-1"_s).fullscreen);
    QCOMPARE(state.outputs.value(u"HDMI-1"_s).fullscreenApp, u"mpv"_s);
    QVERIFY(!state.outputs.value(u"DP-1"_s).fullscreen); // maximized, a fullscreen window of another desktop, our own window: none counts
    QVERIFY(state.outputs.value(u"DP-1"_s).occluded);
    // ordinary windows of any desktop, minimized too; not ours, not docks
    QCOMPARE(state.openApps, (QStringList{u"mpv"_s, u"editor"_s, u"chat"_s, u"game"_s}));
}

QTEST_APPLESS_MAIN(TestX11State)
#include "tst_x11state.moc"