// The EWMH hints of the wallpaper and pet windows, read back from the X server. Runs on a private Xvfb
// (tests/with-xvfb.sh); skips anywhere else.

#include "petsurface.h"
#include "x11hints.h"
#include "x11util.h"

#include <QGuiApplication>
#include <QPair>
#include <QSet>
#include <algorithm>
#include <QTest>
#include <QWindow>

#include <X11/Xlib.h>
#include <X11/extensions/shape.h>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestX11Hints : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        if (!QGuiApplication::platformName().startsWith(u"xcb"))
            QSKIP("needs the xcb platform (run through ctest: tests/with-xvfb.sh)");
        QVERIFY(x11::connection());
    }

    void desktopHintsMarkAWallpaperWindow();
    void aMappedWindowKeepsItsHintsAndAnotherGetsTheStateByMessage();
    void petHintsMarkAnOverlayWindow();
    void aPetWindowIsOverrideRedirectAndAWallpaperWindowIsNot();
    void keepAboveOffTakesTheAboveStateAwayAndOnGivesItBack();
    void lowerPetPutsAnOverrideRedirectPetBelowTheOrdinaryWindows();
    void theSiblingToStackAboveIsTheTopmostDesktopWindow();
    void clickThroughWindowsHaveAnEmptyInputShape();
    void aStageWindowCoversItsOutputAsAPetWindowNeverFullscreenAndWithoutInput();
    void anInputShapeLeavesWhatIsDrawnAlone();
    void rectanglesThatAreNotBandedMakeTheRightShape();
    void hintsNeedANativeWindow();

private:
    static QList<QString> names(const QList<xcb_atom_t> &list)
    {
        QList<QString> out;
        for (const xcb_atom_t a : list)
            out.append(x11::atomName(a));
        return out;
    }
    static bool overrideRedirect(QWindow *window)
    {
        xcb_connection_t *c = x11::connection();
        xcb_get_window_attributes_reply_t *reply =
            xcb_get_window_attributes_reply(c, xcb_get_window_attributes(c, xcb_window_t(window->winId())), nullptr);
        const bool value = reply && reply->override_redirect;
        free(reply);
        return value;
    }
};

void TestX11Hints::desktopHintsMarkAWallpaperWindow()
{
    QWindow window;
    window.setFlags(Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    window.setGeometry(0, 0, 640, 480);
    QVERIFY(x11::applyDesktopHints(&window));
    const auto id = xcb_window_t(window.winId());
    QCOMPARE(names(x11::atoms(id, x11::atom("_NET_WM_WINDOW_TYPE"))), (QList<QString>{u"_NET_WM_WINDOW_TYPE_DESKTOP"_s}));
    const QList<QString> state = names(x11::atoms(id, x11::atom("_NET_WM_STATE")));
    for (const char *wanted : {"_NET_WM_STATE_BELOW", "_NET_WM_STATE_STICKY", "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_STATE_SKIP_PAGER"})
        QVERIFY2(state.contains(QLatin1String(wanted)), wanted);
    QVERIFY(!state.contains(u"_NET_WM_STATE_ABOVE"_s));
    QCOMPARE(x11::cardinals(id, x11::atom("_NET_WM_DESKTOP")), (QList<quint32>{0xFFFFFFFFu}));
}

void TestX11Hints::aMappedWindowKeepsItsHintsAndAnotherGetsTheStateByMessage()
{
    QWindow window;
    window.setFlags(Qt::FramelessWindowHint);
    window.setGeometry(0, 0, 200, 200);
    QVERIFY(x11::applyDesktopHints(&window));
    window.show();
    QTRY_VERIFY(window.isExposed());
    const auto id = xcb_window_t(window.winId());
    // set before mapping, so they are still there, and the type too
    QCOMPARE(names(x11::atoms(id, x11::atom("_NET_WM_WINDOW_TYPE"))), (QList<QString>{u"_NET_WM_WINDOW_TYPE_DESKTOP"_s}));
    const QList<QString> state = names(x11::atoms(id, x11::atom("_NET_WM_STATE")));
    for (const char *wanted : {"_NET_WM_STATE_BELOW", "_NET_WM_STATE_STICKY", "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_STATE_SKIP_PAGER"})
        QVERIFY2(state.contains(QLatin1String(wanted)), wanted); // Qt's own mapping did not replace them
    QCOMPARE(x11::cardinals(id, x11::atom("_NET_WM_DESKTOP")), (QList<quint32>{0xFFFFFFFFu}));
    // on a mapped window the call sends the state to the (here absent) manager and still succeeds
    QVERIFY(x11::applyDesktopHints(&window));
    QVERIFY(x11::applyPetHints(&window));
    QCOMPARE(names(x11::atoms(id, x11::atom("_NET_WM_WINDOW_TYPE"))), (QList<QString>{u"_NET_WM_WINDOW_TYPE_NOTIFICATION"_s}));
}

void TestX11Hints::keepAboveOffTakesTheAboveStateAwayAndOnGivesItBack()
{
    QWindow window;
    window.setFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    window.setGeometry(50, 60, 128, 128);
    QVERIFY(x11::applyPetHints(&window));
    const auto id = xcb_window_t(window.winId());
    QVERIFY(names(x11::atoms(id, x11::atom("_NET_WM_STATE"))).contains(u"_NET_WM_STATE_ABOVE"_s));
    // not mapped yet: the property the manager reads when it maps the window; the other states stay
    x11::setPetKeepAbove(&window, false);
    QList<QString> state = names(x11::atoms(id, x11::atom("_NET_WM_STATE")));
    QVERIFY(!state.contains(u"_NET_WM_STATE_ABOVE"_s));
    for (const char *kept : {"_NET_WM_STATE_STICKY", "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_STATE_SKIP_PAGER"})
        QVERIFY2(state.contains(QLatin1String(kept)), kept);
    x11::setPetKeepAbove(&window, true);
    state = names(x11::atoms(id, x11::atom("_NET_WM_STATE")));
    QVERIFY(state.contains(u"_NET_WM_STATE_ABOVE"_s));
    QVERIFY(state.contains(u"_NET_WM_STATE_STICKY"_s));
    // a mapped window gets the change by message to the (here absent) manager and the call still succeeds; a null window is harmless
    window.show();
    QTRY_VERIFY(window.isExposed());
    x11::setPetKeepAbove(&window, false);
    x11::setPetKeepAbove(&window, true);
    x11::setPetKeepAbove(nullptr, false);
}

namespace {

// The children of the root window, bottom to top.
QList<xcb_window_t> rootStack()
{
    xcb_connection_t *c = x11::connection();
    xcb_query_tree_reply_t *reply = xcb_query_tree_reply(c, xcb_query_tree(c, x11::rootWindow()), nullptr);
    QList<xcb_window_t> out;
    if (reply) {
        const xcb_window_t *children = xcb_query_tree_children(reply);
        for (int i = 0; i < xcb_query_tree_children_length(reply); ++i)
            out.append(children[i]);
        free(reply);
    }
    return out;
}

void setManagerStack(const QList<xcb_window_t> &bottomToTop)
{
    xcb_connection_t *c = x11::connection();
    if (bottomToTop.isEmpty())
        xcb_delete_property(c, x11::rootWindow(), x11::atom("_NET_CLIENT_LIST_STACKING"));
    else
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, x11::rootWindow(), x11::atom("_NET_CLIENT_LIST_STACKING"), XCB_ATOM_WINDOW, 32,
                            uint32_t(bottomToTop.size()), bottomToTop.constData());
    xcb_flush(c);
}

} // namespace

void TestX11Hints::theSiblingToStackAboveIsTheTopmostDesktopWindow()
{
    QCOMPARE(x11::stackAboveIndex({}), -1);                       // nothing: the bottom
    QCOMPARE(x11::stackAboveIndex({false, false}), -1);
    QCOMPARE(x11::stackAboveIndex({true, false, false}), 0);      // the wallpaper is the lowest window
    QCOMPARE(x11::stackAboveIndex({false, true, false, true}), 3); // the topmost of two desktop windows
}

void TestX11Hints::lowerPetPutsAnOverrideRedirectPetBelowTheOrdinaryWindows()
{
    // Xvfb has no window manager, so the stacking list of the manager is written by the test, as a manager would keep it.
    QWindow desktop, ordinary, pet, late;
    desktop.setFlags(Qt::FramelessWindowHint);
    desktop.setGeometry(0, 0, 100, 100);
    QVERIFY(x11::applyDesktopHints(&desktop));
    ordinary.setGeometry(10, 10, 100, 100);
    pet.setFlags(Qt::FramelessWindowHint | Qt::X11BypassWindowManagerHint | Qt::WindowStaysOnTopHint);
    pet.setGeometry(20, 20, 100, 100);
    QVERIFY(x11::applyPetHints(&pet));
    for (QWindow *window : {&desktop, &ordinary, &pet}) {
        window->show();
        QTRY_VERIFY(window->isExposed());
    }
    const auto at = [&](QWindow &window) { return rootStack().indexOf(xcb_window_t(window.winId())); };
    // the pet on top of the stack, as an always-on-top pet is (the order the windows were created in is not the point)
    pet.raise();
    QCOMPARE(at(pet), rootStack().size() - 1);
    QVERIFY(at(desktop) < at(ordinary));

    // with a manager that lists a desktop window: just above it, below the ordinary window
    setManagerStack({xcb_window_t(desktop.winId()), xcb_window_t(ordinary.winId())});
    x11::lowerPet(&pet);
    QVERIFY(at(desktop) < at(pet));
    QVERIFY2(at(pet) < at(ordinary), "the pet is still above an ordinary window");
    QCOMPARE(at(pet), at(desktop) + 1);

    // a window mapped later is above the pet, and so is one raised later; the pet is not raised back by anything
    late.setGeometry(30, 30, 100, 100);
    late.show();
    QTRY_VERIFY(late.isExposed());
    QVERIFY(at(pet) < at(late));
    ordinary.raise();
    QVERIFY(at(pet) < at(ordinary));

    // without a list (no manager): the very bottom
    setManagerStack({});
    x11::lowerPet(&pet);
    QCOMPARE(at(pet), 0);
    x11::lowerPet(nullptr); // harmless
}

void TestX11Hints::petHintsMarkAnOverlayWindow()
{
    QWindow window;
    window.setFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    window.setGeometry(50, 60, 128, 128);
    QVERIFY(x11::applyPetHints(&window));
    const auto id = xcb_window_t(window.winId());
    QCOMPARE(names(x11::atoms(id, x11::atom("_NET_WM_WINDOW_TYPE"))), (QList<QString>{u"_NET_WM_WINDOW_TYPE_NOTIFICATION"_s}));
    const QList<QString> state = names(x11::atoms(id, x11::atom("_NET_WM_STATE")));
    for (const char *wanted : {"_NET_WM_STATE_ABOVE", "_NET_WM_STATE_STICKY", "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_STATE_SKIP_PAGER"})
        QVERIFY2(state.contains(QLatin1String(wanted)), wanted);
    QVERIFY(!state.contains(u"_NET_WM_STATE_BELOW"_s));
    QCOMPARE(x11::cardinals(id, x11::atom("_NET_WM_DESKTOP")), (QList<quint32>{0xFFFFFFFFu}));
    x11::raiseWindow(&window); // harmless on an unmapped window
}

void TestX11Hints::aPetWindowIsOverrideRedirectAndAWallpaperWindowIsNot()
{
    QWindow pet;
    pet.setFlags(Qt::FramelessWindowHint | Qt::X11BypassWindowManagerHint | Qt::WindowStaysOnTopHint);
    pet.setGeometry(0, 0, 64, 64);
    QVERIFY(x11::applyPetHints(&pet));
    QVERIFY(overrideRedirect(&pet));
    QWindow wallpaper;
    wallpaper.setFlags(Qt::FramelessWindowHint);
    wallpaper.setGeometry(0, 0, 64, 64);
    QVERIFY(x11::applyDesktopHints(&wallpaper));
    QVERIFY(!overrideRedirect(&wallpaper));
}

void TestX11Hints::clickThroughWindowsHaveAnEmptyInputShape()
{
    Display *display = XOpenDisplay(nullptr);
    if (!display)
        QSKIP("cannot open the display for the shape extension");
    int eventBase = 0, errorBase = 0;
    if (!XShapeQueryExtension(display, &eventBase, &errorBase)) {
        XCloseDisplay(display);
        QSKIP("no SHAPE extension");
    }
    const auto inputRectangles = [&](QWindow *window) {
        int count = 0, ordering = 0;
        XRectangle *rects = XShapeGetRectangles(display, Window(window->winId()), ShapeInput, &count, &ordering);
        if (rects)
            XFree(rects);
        return count;
    };
    QWindow clickThrough;
    clickThrough.setFlags(Qt::FramelessWindowHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
    clickThrough.setGeometry(0, 0, 200, 200);
    clickThrough.show();
    QTRY_VERIFY(clickThrough.isExposed());
    XSync(display, False);
    QCOMPARE(inputRectangles(&clickThrough), 0); // nothing takes the pointer

    QWindow normal;
    normal.setFlags(Qt::FramelessWindowHint);
    normal.setGeometry(300, 0, 200, 200);
    normal.show();
    QTRY_VERIFY(normal.isExposed());
    XSync(display, False);
    QVERIFY(inputRectangles(&normal) >= 1); // the whole window

    // and it can be turned on and off, which the arrange mode of the pets does
    clickThrough.setFlag(Qt::WindowTransparentForInput, false);
    QTest::qWait(100);
    XSync(display, False);
    QVERIFY(inputRectangles(&clickThrough) >= 1);
    XCloseDisplay(display);
}

void TestX11Hints::aStageWindowCoversItsOutputAsAPetWindowNeverFullscreenAndWithoutInput()
{
    // What PetWindow makes on X11 for a stage (docs/pets/stage.md): the pet's window flags and hints, the size of the output, and nothing that says fullscreen.
    Display *display = XOpenDisplay(nullptr);
    if (!display)
        QSKIP("cannot open the display for the shape extension");
    const QRect screen = QGuiApplication::primaryScreen()->geometry();
    QWindow stage, ordinary;
    stage.setFlags(petX11WindowFlags(false)); // (the flags PetWindow really uses)
    stage.setGeometry(screen);
    QVERIFY(x11::applyPetHints(&stage));
    ordinary.setGeometry(10, 10, 100, 100);
    for (QWindow *window : {&stage, &ordinary}) {
        window->show();
        QTRY_VERIFY(window->isExposed());
    }
    XSync(display, False);
    const auto id = xcb_window_t(stage.winId());
    QVERIFY(overrideRedirect(&stage)); // no window manager sees it: it is in no client list, so no monitor takes it for an application
    const QList<QString> state = names(x11::atoms(id, x11::atom("_NET_WM_STATE")));
    QVERIFY(state.contains(u"_NET_WM_STATE_ABOVE"_s));
    QVERIFY2(!state.contains(u"_NET_WM_STATE_FULLSCREEN"_s), "a stage must never be a fullscreen window");
    QVERIFY(!state.contains(u"_NET_WM_STATE_MAXIMIZED_VERT"_s) && !state.contains(u"_NET_WM_STATE_MAXIMIZED_HORZ"_s));
    QCOMPARE(names(x11::atoms(id, x11::atom("_NET_WM_WINDOW_TYPE"))), (QList<QString>{u"_NET_WM_WINDOW_TYPE_NOTIFICATION"_s}));
    QCOMPARE(stage.geometry(), screen);
    // no input: the input shape is empty
    int count = 0, ordering = 0;
    XRectangle *rects = XShapeGetRectangles(display, Window(stage.winId()), ShapeInput, &count, &ordering);
    if (rects)
        XFree(rects);
    QCOMPARE(count, 0);
    // keep above off: it loses the above state and goes under the ordinary windows (the desktop's own are the manager's business)
    setManagerStack({}); // (no manager list: the very bottom)
    x11::setPetKeepAbove(&stage, false);
    x11::lowerPet(&stage);
    const auto stack = rootStack();
    QVERIFY2(stack.indexOf(id) < stack.indexOf(xcb_window_t(ordinary.winId())), "a stage with keep above off is above an ordinary window");
    // (the above state of a mapped window is the window manager's to change, and there is none here; the unmapped case is the test above)
    QVERIFY(!names(x11::atoms(id, x11::atom("_NET_WM_STATE"))).contains(u"_NET_WM_STATE_FULLSCREEN"_s));
    // keep above on again: back on top
    x11::setPetKeepAbove(&stage, true);
    x11::raiseWindow(&stage);
    const auto again = rootStack();
    QCOMPARE(again.indexOf(id), again.size() - 1);
    XCloseDisplay(display);
}

void TestX11Hints::anInputShapeLeavesWhatIsDrawnAlone()
{
#ifndef MURASAKI_HAVE_X11_SHAPE
    QSKIP("built without the SHAPE calls");
#else
    Display *display = XOpenDisplay(nullptr);
    if (!display)
        QSKIP("cannot open the display for the shape extension");
    const auto rectangles = [&](QWindow *window, int kind) {
        int count = 0, ordering = 0;
        QList<QRect> out;
        XRectangle *rects = XShapeGetRectangles(display, Window(window->winId()), kind, &count, &ordering);
        for (int i = 0; i < count; ++i)
            out.append(QRect(rects[i].x, rects[i].y, rects[i].width, rects[i].height));
        if (rects)
            XFree(rects);
        std::sort(out.begin(), out.end(), [](const QRect &a, const QRect &b) { return a.y() != b.y() ? a.y() < b.y() : a.x() < b.x(); });
        return out;
    };
    QWindow window;
    window.setFlags(Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    window.setGeometry(0, 0, 200, 100);
    window.show();
    QTRY_VERIFY(window.isExposed());
    const QList<QRect> shape{QRect(10, 10, 30, 20), QRect(100, 50, 40, 40)};
    QVERIFY(x11::setInputShape(&window, shape));
    XSync(display, False);
    QCOMPARE(rectangles(&window, ShapeInput), shape);
    // Only the input: what is drawn (the bounding shape) is the whole window still.
    QCOMPARE(rectangles(&window, ShapeBounding), (QList<QRect>{QRect(0, 0, 200, 100)}));
    // Another shape replaces it; an empty one lets everything through.
    QVERIFY(x11::setInputShape(&window, {QRect(0, 0, 50, 50)}));
    XSync(display, False);
    {
        // (the server may split a region into bands as it likes: what counts is the area it covers)
        qint64 area = 0;
        for (const QRect &r : rectangles(&window, ShapeInput))
            area += qint64(r.width()) * r.height();
        QCOMPARE(area, qint64(50 * 50));
        QVERIFY(QRect(0, 0, 50, 50).contains(rectangles(&window, ShapeInput).first()));
    }
    QVERIFY(x11::setInputShape(&window, {}));
    XSync(display, False);
    QVERIFY(rectangles(&window, ShapeInput).isEmpty());
    QWindow notNative;
    QVERIFY(!x11::setInputShape(nullptr, shape));
    XCloseDisplay(display);
#endif
}

void TestX11Hints::rectanglesThatAreNotBandedMakeTheRightShape()
{
#ifndef MURASAKI_HAVE_X11_SHAPE
    QSKIP("built without the SHAPE calls");
#else
    Display *display = XOpenDisplay(nullptr);
    if (!display)
        QSKIP("cannot open the display for the shape extension");
    QWindow window;
    window.setFlags(Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    window.setGeometry(0, 0, 100, 120);
    window.show();
    QTRY_VERIFY(window.isExposed());
    // The rectangles of a mask merged vertically: a tall limb next to a short one, overlapping in y without sharing a band (what YXBanded forbids).
    const QList<QRect> shape{QRect(0, 0, 10, 100), QRect(20, 10, 10, 20), QRect(40, 50, 10, 60)};
    QVERIFY(x11::setInputShape(&window, shape));
    XSync(display, False);
    int count = 0, ordering = 0;
    XRectangle *rects = XShapeGetRectangles(display, Window(window.winId()), ShapeInput, &count, &ordering);
    QVERIFY(rects);
    QSet<QPair<int, int>> got, want;
    for (int i = 0; i < count; ++i) {
        for (int y = rects[i].y; y < rects[i].y + rects[i].height; ++y) {
            for (int x = rects[i].x; x < rects[i].x + rects[i].width; ++x)
                got.insert({x, y});
        }
    }
    XFree(rects);
    for (const QRect &r : shape) {
        for (int y = r.top(); y <= r.bottom(); ++y) {
            for (int x = r.left(); x <= r.right(); ++x)
                want.insert({x, y});
        }
    }
    QCOMPARE(got.size(), want.size());
    QVERIFY(got == want); // exactly the union of the rectangles: no malformed region
    XCloseDisplay(display);
#endif
}

void TestX11Hints::hintsNeedANativeWindow()
{
    QVERIFY(!x11::applyDesktopHints(nullptr));
    QVERIFY(!x11::applyPetHints(nullptr));
    x11::raiseWindow(nullptr); // no crash
}

QTEST_MAIN(TestX11Hints)
#include "tst_x11hints.moc"
