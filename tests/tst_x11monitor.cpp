// X11Monitor against a fake window manager: a second X client that sets the EWMH properties of the root and of windows it
// owns, as a manager would. Runs on a private Xvfb (tests/with-xvfb.sh); skips anywhere else.

#include "fakewm.h"
#include "x11monitor.h"
#include "x11util.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTest>

#include <xcb/xcb.h>

#include <memory>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestX11Monitor : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        if (!QGuiApplication::platformName().startsWith(u"xcb"))
            QSKIP("needs the xcb platform (run through ctest: tests/with-xvfb.sh)");
        QVERIFY(x11::connection());
    }

    void readsDesktopsAndWindows();
    void aFullscreenWindowCoversTheOutputAndLeavingUncoversIt();
    void followsTheFocusAndTheCurrentDesktop();
    void aClientsOwnStateChangeIsSeen();
    void ourOwnWindowsDoNotCover();
    void aSnapshotIsConsistent();

private:
    static std::unique_ptr<X11Monitor> monitor()
    {
        auto m = std::make_unique<X11Monitor>();
        m->setOutputProvider([] { return QList<X11Output>{{u"TEST-0"_s, QRect(0, 0, 1280, 1024)}}; });
        m->setPollInterval(400); // the fallback for a missed notification also keeps the test steady
        return m;
    }
};

void TestX11Monitor::readsDesktopsAndWindows()
{
    FakeWm wm;
    QVERIFY(wm.ok());
    wm.desktops(3, 1, {u"Work"_s, u"Play"_s, u"Misc"_s});
    auto m = monitor();
    QSignalSpy changed(m.get(), &SessionMonitor::stateChanged);
    m->start();
    QTRY_COMPARE(m->state().desktops.size(), 3);
    const SessionState state = m->state();
    QCOMPARE(state.desktops.at(1).name, u"Play"_s);
    QCOMPARE(state.defaultDesktop, u"1"_s);
    QCOMPARE(state.outputs.value(u"TEST-0"_s).desktop, u"1"_s);
    QVERIFY(!state.outputs.value(u"TEST-0"_s).occluded);
    QVERIFY(changed.count() >= 1);
}

void TestX11Monitor::aFullscreenWindowCoversTheOutputAndLeavingUncoversIt()
{
    FakeWm wm;
    wm.desktops(1, 0, {});
    auto m = monitor();
    m->start();
    QTRY_COMPARE(m->state().desktops.size(), 1);
    QVERIFY(!m->state().outputs.value(u"TEST-0"_s).occluded);

    const xcb_window_t game = wm.addWindow(u"steam_app_620"_s, QRect(0, 0, 1280, 1024), {u"_NET_WM_STATE_FULLSCREEN"_s});
    QTRY_VERIFY(m->state().outputs.value(u"TEST-0"_s).occluded);
    QCOMPARE(m->state().fullscreenApps, QStringList{u"steam_app_620"_s});
    wm.removeWindow(game);
    QTRY_VERIFY(!m->state().outputs.value(u"TEST-0"_s).occluded);
    QVERIFY(m->state().fullscreenApps.isEmpty());

    // a window that is maximised both ways covers; one that is merely small does not
    const xcb_window_t small = wm.addWindow(u"calc"_s, QRect(10, 10, 300, 200));
    QTest::qWait(300);
    QVERIFY(!m->state().outputs.value(u"TEST-0"_s).occluded);
    const xcb_window_t big = wm.addWindow(u"konsole"_s, QRect(0, 30, 1280, 994),
                                          {u"_NET_WM_STATE_MAXIMIZED_VERT"_s, u"_NET_WM_STATE_MAXIMIZED_HORZ"_s});
    QTRY_VERIFY(m->state().outputs.value(u"TEST-0"_s).occluded);
    wm.removeWindow(big);
    wm.removeWindow(small);
}

void TestX11Monitor::followsTheFocusAndTheCurrentDesktop()
{
    FakeWm wm;
    wm.desktops(2, 0, {u"One"_s, u"Two"_s});
    const xcb_window_t obs = wm.addWindow(u"obs"_s, QRect(100, 100, 400, 300));
    const xcb_window_t video = wm.addWindow(u"mpv"_s, QRect(0, 0, 1280, 1024), {u"_NET_WM_STATE_FULLSCREEN"_s}, 1);
    auto m = monitor();
    m->start();
    QTRY_COMPARE(m->state().desktops.size(), 2);
    QVERIFY(m->state().activeApp.isEmpty()); // nothing focused yet
    QVERIFY(!m->state().outputs.value(u"TEST-0"_s).occluded); // the video is on the other desktop

    wm.activate(obs);
    QTRY_COMPARE(m->state().activeApp, u"obs"_s);
    wm.activate(0);
    QTRY_VERIFY(m->state().activeApp.isEmpty());

    wm.desktops(2, 1, {u"One"_s, u"Two"_s}); // switch to the desktop with the video
    QTRY_VERIFY(m->state().outputs.value(u"TEST-0"_s).occluded);
    QCOMPARE(m->state().defaultDesktop, u"1"_s);
    QCOMPARE(m->state().outputs.value(u"TEST-0"_s).desktop, u"1"_s);
    wm.removeWindow(video);
    wm.removeWindow(obs);
}

void TestX11Monitor::aClientsOwnStateChangeIsSeen()
{
    FakeWm wm;
    wm.desktops(1, 0, {});
    const xcb_window_t window = wm.addWindow(u"mpv"_s, QRect(10, 10, 800, 600)); // small until it goes fullscreen
    auto m = monitor();
    m->start();
    QTRY_COMPARE(m->state().desktops.size(), 1);
    QVERIFY(!m->state().outputs.value(u"TEST-0"_s).occluded);
    wm.setState(window, {u"_NET_WM_STATE_FULLSCREEN"_s});
    QTRY_VERIFY(m->state().outputs.value(u"TEST-0"_s).occluded);
    wm.setState(window, {u"_NET_WM_STATE_FULLSCREEN"_s, u"_NET_WM_STATE_HIDDEN"_s}); // minimised: it no longer covers
    QTRY_VERIFY(!m->state().outputs.value(u"TEST-0"_s).occluded);
    wm.removeWindow(window);
}

void TestX11Monitor::ourOwnWindowsDoNotCover()
{
    FakeWm wm;
    wm.desktops(1, 0, {});
    wm.addWindow(u"murasaki-surface"_s, QRect(0, 0, 1280, 1024), {u"_NET_WM_STATE_FULLSCREEN"_s}, 0,
                 quint32(QCoreApplication::applicationPid()));
    auto m = monitor();
    m->start();
    QTRY_COMPARE(m->state().desktops.size(), 1);
    QTest::qWait(500);
    QVERIFY(!m->state().outputs.value(u"TEST-0"_s).occluded);
    QVERIFY(m->state().fullscreenApps.isEmpty());
}

void TestX11Monitor::aSnapshotIsConsistent()
{
    FakeWm wm;
    wm.desktops(2, 1, {u"A"_s, u"B"_s});
    const xcb_window_t window = wm.addWindow(u"firefox"_s, QRect(20, 40, 800, 600), {}, kX11AllDesktops);
    auto m = monitor();
    const X11Snapshot snapshot = m->snapshot();
    QCOMPARE(snapshot.desktopCount, 2);
    QCOMPARE(snapshot.currentDesktop, 1u);
    QCOMPARE(snapshot.desktopNames, (QStringList{u"A"_s, u"B"_s}));
    QCOMPARE(snapshot.windows.size(), 1);
    const X11Window &w = snapshot.windows.first();
    QCOMPARE(w.id, quint32(window));
    QCOMPARE(w.resourceClass, u"firefox"_s);
    QCOMPARE(w.desktop, kX11AllDesktops);
    QCOMPARE(w.geometry.size(), QSize(800, 600));
    QVERIFY(w.normal);
    QVERIFY(!w.fullscreen && !w.hidden && !w.maximizedBoth);
    wm.removeWindow(window);
}

QTEST_MAIN(TestX11Monitor)
#include "tst_x11monitor.moc"
