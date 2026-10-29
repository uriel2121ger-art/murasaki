#include "toplevelstate.h"

#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestToplevelState : public QObject
{
    Q_OBJECT

private slots:
    void aWindowIsNothingBeforeItsFirstDone();
    void fullscreenAndMaximizedCoverTheirOutputOnly();
    void minimizedWindowsCoverNothing();
    void tilesThatAreNeitherMaximizedNorFullscreenCoverNothing();
    void activeAppFollowsTheActivatedWindow();
    void fullscreenAppsListEveryVisibleFullscreenWindowOnce();
    void changesTakeEffectOnDone();
    void movingToAnotherOutputMovesTheCover();
    void closingAWindowUncoversItsOutput();
    void unknownIdsAreIgnored();
    void everyKnownOutputHasAnEntryAndDesktopsAreWildcards();
    void fullscreenIsPerOutputMaximizedIsNotAndOpenAppsIncludeMinimized();
};

static const QStringList kOutputs{u"DP-1"_s, u"HDMI-A-1"_s};

// One window, the way the protocol delivers it: created, described, then done.
static void show(ToplevelTracker &t, ToplevelTracker::Id id, const QString &app, int flags, const QStringList &outputs)
{
    t.added(id);
    t.setAppId(id, app);
    t.setTitle(id, u"title"_s);
    t.setFlags(id, flags);
    for (const QString &output : outputs)
        t.outputEntered(id, output);
    t.done(id);
}

void TestToplevelState::aWindowIsNothingBeforeItsFirstDone()
{
    ToplevelTracker t;
    t.added(1);
    t.setAppId(1, u"mpv"_s);
    t.setFlags(1, ToplevelFullscreen | ToplevelActivated);
    t.outputEntered(1, u"DP-1"_s);
    const SessionState half = t.state(kOutputs);
    QVERIFY(!half.outputs.value(u"DP-1"_s).occluded);
    QVERIFY(half.activeApp.isEmpty() && half.fullscreenApps.isEmpty());
    t.done(1);
    QVERIFY(t.state(kOutputs).outputs.value(u"DP-1"_s).occluded);
}

void TestToplevelState::fullscreenAndMaximizedCoverTheirOutputOnly()
{
    ToplevelTracker t;
    show(t, 1, u"mpv"_s, ToplevelFullscreen, {u"DP-1"_s});
    show(t, 2, u"editor"_s, ToplevelMaximized, {u"HDMI-A-1"_s});
    const SessionState state = t.state(kOutputs);
    QVERIFY(state.outputs.value(u"DP-1"_s).occluded);
    QVERIFY(state.outputs.value(u"HDMI-A-1"_s).occluded);
    ToplevelTracker one;
    show(one, 1, u"mpv"_s, ToplevelFullscreen, {u"DP-1"_s});
    const SessionState only = one.state(kOutputs);
    QVERIFY(only.outputs.value(u"DP-1"_s).occluded);
    QVERIFY(!only.outputs.value(u"HDMI-A-1"_s).occluded);
}

void TestToplevelState::minimizedWindowsCoverNothing()
{
    ToplevelTracker t;
    show(t, 1, u"mpv"_s, ToplevelFullscreen | ToplevelMinimized | ToplevelActivated, {u"DP-1"_s});
    const SessionState state = t.state(kOutputs);
    QVERIFY(!state.outputs.value(u"DP-1"_s).occluded);
    QVERIFY(state.activeApp.isEmpty());
    QVERIFY(state.fullscreenApps.isEmpty());
}

void TestToplevelState::tilesThatAreNeitherMaximizedNorFullscreenCoverNothing()
{
    ToplevelTracker t;
    show(t, 1, u"terminal"_s, ToplevelActivated, {u"DP-1"_s});
    QVERIFY(!t.state(kOutputs).outputs.value(u"DP-1"_s).occluded);
}

void TestToplevelState::activeAppFollowsTheActivatedWindow()
{
    ToplevelTracker t;
    show(t, 1, u"firefox"_s, 0, {u"DP-1"_s});
    show(t, 2, u"steam_app_620"_s, ToplevelActivated, {u"DP-1"_s});
    QCOMPARE(t.state(kOutputs).activeApp, u"steam_app_620"_s);
    t.setFlags(2, 0);
    t.setFlags(1, ToplevelActivated);
    t.done(2);
    t.done(1);
    QCOMPARE(t.state(kOutputs).activeApp, u"firefox"_s);
}

void TestToplevelState::fullscreenAppsListEveryVisibleFullscreenWindowOnce()
{
    ToplevelTracker t;
    show(t, 1, u"mpv"_s, ToplevelFullscreen, {u"DP-1"_s});
    show(t, 2, u"mpv"_s, ToplevelFullscreen, {u"HDMI-A-1"_s});
    show(t, 3, u"obs"_s, ToplevelFullscreen, {}); // on no known output yet: still a fullscreen app
    show(t, 4, u"hidden"_s, ToplevelFullscreen | ToplevelMinimized, {u"DP-1"_s});
    show(t, 5, u""_s, ToplevelFullscreen, {u"DP-1"_s}); // no app id: covers, but is no rule input
    QCOMPARE(t.state(kOutputs).fullscreenApps, (QStringList{u"mpv"_s, u"obs"_s}));
}

void TestToplevelState::changesTakeEffectOnDone()
{
    ToplevelTracker t;
    show(t, 1, u"mpv"_s, 0, {u"DP-1"_s});
    t.setFlags(1, ToplevelFullscreen);
    QVERIFY(!t.state(kOutputs).outputs.value(u"DP-1"_s).occluded); // not yet
    t.done(1);
    QVERIFY(t.state(kOutputs).outputs.value(u"DP-1"_s).occluded);
    t.setAppId(1, u"vlc"_s);
    QCOMPARE(t.appIdOf(1), u"mpv"_s);
    t.done(1);
    QCOMPARE(t.appIdOf(1), u"vlc"_s);
}

void TestToplevelState::movingToAnotherOutputMovesTheCover()
{
    ToplevelTracker t;
    show(t, 1, u"mpv"_s, ToplevelFullscreen, {u"DP-1"_s});
    t.outputLeft(1, u"DP-1"_s);
    t.outputEntered(1, u"HDMI-A-1"_s);
    t.done(1);
    const SessionState state = t.state(kOutputs);
    QVERIFY(!state.outputs.value(u"DP-1"_s).occluded);
    QVERIFY(state.outputs.value(u"HDMI-A-1"_s).occluded);
}

void TestToplevelState::closingAWindowUncoversItsOutput()
{
    ToplevelTracker t;
    show(t, 1, u"mpv"_s, ToplevelFullscreen | ToplevelActivated, {u"DP-1"_s});
    QCOMPARE(t.count(), 1);
    t.closed(1);
    QCOMPARE(t.count(), 0);
    const SessionState state = t.state(kOutputs);
    QVERIFY(!state.outputs.value(u"DP-1"_s).occluded);
    QVERIFY(state.activeApp.isEmpty() && state.fullscreenApps.isEmpty());
}

void TestToplevelState::unknownIdsAreIgnored()
{
    ToplevelTracker t;
    t.setAppId(99, u"ghost"_s);
    t.setFlags(99, ToplevelFullscreen);
    t.outputEntered(99, u"DP-1"_s);
    t.done(99);
    t.closed(99);
    QCOMPARE(t.count(), 0);
    QVERIFY(!t.state(kOutputs).outputs.value(u"DP-1"_s).occluded);
}

void TestToplevelState::everyKnownOutputHasAnEntryAndDesktopsAreWildcards()
{
    ToplevelTracker t;
    const SessionState state = t.state(kOutputs);
    QCOMPARE(state.outputs.size(), 2);
    for (const QString &output : kOutputs)
        QCOMPARE(state.forOutput(output).desktop, u"*"_s);
    QVERIFY(state.desktops.isEmpty());
}

void TestToplevelState::fullscreenIsPerOutputMaximizedIsNotAndOpenAppsIncludeMinimized()
{
    ToplevelTracker t;
    show(t, 1, u"mpv"_s, ToplevelFullscreen, {u"DP-1"_s});
    show(t, 2, u"editor"_s, ToplevelMaximized, {u"HDMI-A-1"_s});
    show(t, 3, u"hidden"_s, ToplevelFullscreen | ToplevelMinimized, {u"HDMI-A-1"_s});
    show(t, 4, u"mpv"_s, 0, {u"HDMI-A-1"_s}); // a second window of the same application
    const SessionState state = t.state(kOutputs);
    QVERIFY(state.outputs.value(u"DP-1"_s).fullscreen);
    QCOMPARE(state.outputs.value(u"DP-1"_s).fullscreenApp, u"mpv"_s);
    QVERIFY(state.outputs.value(u"HDMI-A-1"_s).occluded);
    QVERIFY(!state.outputs.value(u"HDMI-A-1"_s).fullscreen); // maximized and minimized-fullscreen are not fullscreen windows
    QCOMPARE(state.openApps, (QStringList{u"mpv"_s, u"editor"_s, u"hidden"_s}));
    t.closed(1);
    QVERIFY(!t.state(kOutputs).outputs.value(u"DP-1"_s).fullscreen);
}

QTEST_GUILESS_MAIN(TestToplevelState)
#include "tst_toplevelstate.moc"