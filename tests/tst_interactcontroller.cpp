// InteractController against a scripted daemon and surface and fake catcher windows: which outputs get a catcher, what the pointer
// sends to the host, and every way the interaction with the wallpaper ends.

#include "fakebackend.h"
#include "interactcontroller.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

#include <map>
#include <memory>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

class FakeCatcher : public InteractSurface
{
public:
    FakeCatcher(QString output, std::map<QString, FakeCatcher *> *registry)
        : m_output(std::move(output))
        , m_registry(registry)
    {
        (*m_registry)[m_output] = this;
    }
    ~FakeCatcher() override { m_registry->erase(m_output); }

    QString output() const override { return m_output; }
    void point(double x, double y, quint32 buttons = 0, QPoint wheel = {}) { onPointer(InteractPointer{QPointF(x, y), QSizeF(1920, 1080), buttons, wheel}); }
    void escape() { onEscape(); }

private:
    QString m_output;
    std::map<QString, FakeCatcher *> *m_registry;
};

QByteArray outputs(const QList<std::tuple<QString, bool, bool>> &list)
{
    QJsonArray array;
    for (const auto &[name, hosted, interactive] : list)
        array.append(QJsonObject{{u"name"_s, name}, {u"hosted"_s, hosted}, {u"interactive"_s, interactive}});
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

} // namespace

class TestInteractController : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        daemon = std::make_unique<FakeBackend>();
        surface = std::make_unique<FakeBackend>();
        catchers.clear();
        daemon->answer(u"SetWallpaperInteract"_s, QVariant());
        surface->answer(u"ReportHostPointer"_s, QVariant());
        surface->answerJson(u"Outputs"_s, outputs({{u"DP-1"_s, true, true}, {u"DP-2"_s, true, false}, {u"HDMI-1"_s, false, true}}));
        controller = std::make_unique<InteractController>(daemon.get(), surface.get(), [this](const QString &output) -> std::unique_ptr<InteractSurface> {
            if (output == u"GONE"_s)
                return nullptr;
            return std::make_unique<FakeCatcher>(output, &catchers);
        });
        controller->setGraceMs(0);
    }
    void cleanup() { controller.reset(); }
    void hardCapEndsTheModeWhateverThePointerDoes();
    void aTransientOutputListWithoutInteractiveWallpaperIsForgiven();

    void clampKeepsThePointerInsideTheOutput();
    void catchersExistOnlyWhileTheDaemonSaysSoAndOnlyForHostedInteractiveOutputs();
    void anOutputThatStopsBeingInteractiveLosesItsCatcher();
    void noInteractiveWallpaperEndsTheMode();
    void buttonsGoAtOnceAndMovesAreLimitedButNeverLost();
    void aWindowThatClosesWithAButtonDownReleasesIt();
    void escapeEndsTheMode();
    void idleEndsTheModeAndInputRestartsTheTimer();
    void aLockedSessionEndsTheModeAndKeepsItFromStarting();
    void arrangeEndsTheModeAtOnce();
    void theShortcutFlipsTheModeThroughTheDaemon();
    void aDaemonThatGoesAwayEndsTheMode();

private:
    void startOn()
    {
        daemon->setProperty(u"WallpaperInteract"_s, true);
        surface->setAvailable(true);
        daemon->setAvailable(true);
        QTRY_VERIFY(!catchers.empty());
    }
    QList<QVariantList> reports() const
    {
        QList<QVariantList> list;
        for (const auto &call : surface->calls) {
            if (call.first == u"ReportHostPointer"_s)
                list.append(call.second);
        }
        return list;
    }

    std::unique_ptr<FakeBackend> daemon;
    std::unique_ptr<FakeBackend> surface;
    std::map<QString, FakeCatcher *> catchers;
    std::unique_ptr<InteractController> controller;
};

void TestInteractController::clampKeepsThePointerInsideTheOutput()
{
    const InteractPointer inside = clampedInteractPointer({QPointF(10, 20), QSizeF(100, 50), kInteractLeft, {}});
    QCOMPARE(inside.position, QPointF(10, 20));
    const InteractPointer out = clampedInteractPointer({QPointF(-5, 900), QSizeF(100, 50), 0xff, QPoint(0, 120)});
    QCOMPARE(out.position, QPointF(0, 50));
    QCOMPARE(out.buttons, kInteractLeft | kInteractRight | kInteractMiddle);
    QCOMPARE(out.wheel, QPoint(0, 120));
    QCOMPARE(clampedInteractPointer({QPointF(qQNaN(), 3), QSizeF(100, 50), 0, {}}).position, QPointF(0, 3));
    QCOMPARE(clampedInteractPointer({QPointF(7, 8), QSizeF(0, 0), 0, {}}).position, QPointF(7, 8)); // no size: kept
}

void TestInteractController::catchersExistOnlyWhileTheDaemonSaysSoAndOnlyForHostedInteractiveOutputs()
{
    // The mode is off: no window, nothing asked of the surface.
    daemon->setProperty(u"WallpaperInteract"_s, false);
    surface->setAvailable(true);
    daemon->setAvailable(true);
    QTest::qWait(50);
    QVERIFY(catchers.empty());
    QCOMPARE(surface->callCount(u"Outputs"_s), 0);
    QVERIFY(!controller->active());

    daemon->emitSignal(u"WallpaperInteractChanged"_s, {true});
    QTRY_COMPARE(catchers.size(), size_t(1));
    QCOMPARE(controller->catcherOutputs(), QStringList{u"DP-1"_s}); // DP-2 shows a video, HDMI-1 has a window of the surface's own
    QVERIFY(controller->active());

    daemon->emitSignal(u"WallpaperInteractChanged"_s, {false});
    QVERIFY(catchers.empty());
    QVERIFY(!controller->active());
    QCOMPARE(daemon->callCount(u"SetWallpaperInteract"_s), 0); // (the daemon ended it: nothing to tell it)
}

void TestInteractController::anOutputThatStopsBeingInteractiveLosesItsCatcher()
{
    surface->answerJson(u"Outputs"_s, outputs({{u"DP-1"_s, true, true}, {u"DP-2"_s, true, true}}));
    startOn();
    QTRY_COMPARE(catchers.size(), size_t(2));

    surface->answerJson(u"Outputs"_s, outputs({{u"DP-1"_s, true, false}, {u"DP-2"_s, true, true}}));
    surface->emitSignal(u"OutputsChanged"_s);
    QTRY_COMPARE(controller->catcherOutputs(), QStringList{u"DP-2"_s});
    QVERIFY(controller->active());

    surface->answerJson(u"Outputs"_s, outputs({{u"DP-1"_s, true, true}, {u"DP-2"_s, true, true}, {u"DP-3"_s, true, true}}));
    surface->emitSignal(u"OutputsChanged"_s);
    QTRY_COMPARE(catchers.size(), size_t(3));
    QCOMPARE(daemon->callCount(u"SetWallpaperInteract"_s), 0);
}

void TestInteractController::noInteractiveWallpaperEndsTheMode()
{
    surface->answerJson(u"Outputs"_s, outputs({{u"DP-2"_s, true, false}}));
    daemon->setProperty(u"WallpaperInteract"_s, true);
    surface->setAvailable(true);
    daemon->setAvailable(true);
    QTRY_COMPARE(daemon->callCount(u"SetWallpaperInteract"_s), 1);
    QCOMPARE(daemon->calls.last().second, QVariantList{false});
    QVERIFY(catchers.empty());
    QVERIFY(!controller->active());

    // The last interactive wallpaper going away ends it too.
    cleanup();
    init();
    startOn();
    surface->answerJson(u"Outputs"_s, outputs({{u"DP-1"_s, true, false}}));
    surface->emitSignal(u"OutputsChanged"_s);
    QTRY_COMPARE(daemon->callCount(u"SetWallpaperInteract"_s), 1);
    QVERIFY(catchers.empty());
}

void TestInteractController::buttonsGoAtOnceAndMovesAreLimitedButNeverLost()
{
    startOn();
    FakeCatcher *catcher = catchers.at(u"DP-1"_s);

    catcher->point(100, 200); // the first move
    catcher->point(101, 200, kInteractLeft); // a press: at once, whatever came before
    QCOMPARE(reports().size(), 2);
    QCOMPARE(reports().at(1), (QVariantList{u"DP-1"_s, 101.0, 200.0, 1920.0, 1080.0, uint(kInteractLeft), 0, 0}));

    // A burst of drag moves: not one message each, and the last position is the last one sent.
    const int before = reports().size();
    for (int i = 0; i < 40; ++i)
        catcher->point(110 + i, 200, kInteractLeft);
    QTRY_VERIFY(reports().last().at(1).toDouble() == 149.0);
    QVERIFY(reports().size() - before < 40);
    QCOMPARE(reports().last().at(5).toUInt(), uint(kInteractLeft));

    // The release is in order after the held-back move.
    catcher->point(150, 200, kInteractLeft);
    catcher->point(150, 200, 0);
    QCOMPARE(reports().last().at(5).toUInt(), 0u);
    QCOMPARE(reports().at(reports().size() - 2).at(1).toDouble(), 150.0);

    // The wheel is an event of its own, with its position.
    catcher->point(150, 200, 0, QPoint(0, -120));
    QCOMPARE(reports().last().at(7).toInt(), -120);

    // Outside the output counts as its edge.
    catcher->point(-30, 5000, 0);
    QTRY_VERIFY(reports().last().at(1).toDouble() == 0.0);
    QCOMPARE(reports().last().at(2).toDouble(), 1080.0);
}

void TestInteractController::aWindowThatClosesWithAButtonDownReleasesIt()
{
    startOn();
    catchers.at(u"DP-1"_s)->point(12, 34, kInteractLeft | kInteractRight);
    const int before = reports().size();
    daemon->emitSignal(u"WallpaperInteractChanged"_s, {false});
    QCOMPARE(reports().size(), before + 1);
    QCOMPARE(reports().last(), (QVariantList{u"DP-1"_s, 12.0, 34.0, 1920.0, 1080.0, 0u, 0, 0}));
}

void TestInteractController::escapeEndsTheMode()
{
    startOn();
    catchers.at(u"DP-1"_s)->point(5, 6, kInteractMiddle);
    catchers.at(u"DP-1"_s)->escape();
    QVERIFY(catchers.empty());
    QVERIFY(!controller->active());
    QCOMPARE(daemon->callCount(u"SetWallpaperInteract"_s), 1);
    QCOMPARE(daemon->calls.last().second, QVariantList{false});
    QCOMPARE(reports().last().at(5).toUInt(), 0u); // the middle button is let go
}

void TestInteractController::idleEndsTheModeAndInputRestartsTheTimer()
{
    controller->setIdleTimeoutMs(150);
    startOn(); // (the timer starts with the mode)
    for (int i = 0; i < 4; ++i) {
        QTest::qWait(60);
        QVERIFY(controller->active());
        catchers.at(u"DP-1"_s)->point(i, i); // every input starts the time again: 240 ms in all
    }
    QCOMPARE(daemon->callCount(u"SetWallpaperInteract"_s), 0);
    QTRY_COMPARE_WITH_TIMEOUT(daemon->callCount(u"SetWallpaperInteract"_s), 1, 2000);
    QVERIFY(!controller->active());
    QVERIFY(catchers.empty());
    QCOMPARE(daemon->calls.last().second, QVariantList{false});
}

void TestInteractController::aLockedSessionEndsTheModeAndKeepsItFromStarting()
{
    startOn();
    controller->setSessionPaused(true);
    QVERIFY(catchers.empty());
    QCOMPARE(daemon->callCount(u"SetWallpaperInteract"_s), 1);
    QCOMPARE(daemon->calls.last().second, QVariantList{false});

    // The shortcut does nothing while locked, and a daemon that says "on" is told "off".
    controller->onShortcut(u"toggle-interact"_s);
    QCOMPARE(daemon->callCount(u"SetWallpaperInteract"_s), 1);
    daemon->emitSignal(u"WallpaperInteractChanged"_s, {true});
    QTRY_COMPARE(daemon->callCount(u"SetWallpaperInteract"_s), 2); // (told to turn it off again)
    QCOMPARE(daemon->calls.last().second, QVariantList{false});
    QVERIFY(catchers.empty());
    controller->setSessionPaused(false);
    controller->onShortcut(u"toggle-interact"_s);
    QCOMPARE(daemon->callCount(u"SetWallpaperInteract"_s), 3);
    QCOMPARE(daemon->calls.last().second, QVariantList{true});
}

void TestInteractController::arrangeEndsTheModeAtOnce()
{
    startOn();
    daemon->emitSignal(u"PetsArrangeChanged"_s, {true});
    QVERIFY(catchers.empty());
    QVERIFY(!controller->active());
    QCOMPARE(daemon->callCount(u"SetWallpaperInteract"_s), 0); // (the daemon turns it off itself)
}

void TestInteractController::theShortcutFlipsTheModeThroughTheDaemon()
{
    daemon->setProperty(u"WallpaperInteract"_s, false);
    surface->setAvailable(true);
    daemon->setAvailable(true);
    QTest::qWait(30);
    controller->onShortcut(u"rescue"_s); // not ours
    controller->onShortcut(u"toggle-arrange"_s);
    QCOMPARE(daemon->callCount(u"SetWallpaperInteract"_s), 0);
    controller->onShortcut(u"toggle-interact"_s);
    QCOMPARE(daemon->callCount(u"SetWallpaperInteract"_s), 1);
    QCOMPARE(daemon->calls.last().second, QVariantList{true});
    daemon->emitSignal(u"WallpaperInteractChanged"_s, {true});
    QTRY_VERIFY(!catchers.empty());
    controller->onShortcut(u"toggle-interact"_s);
    QCOMPARE(daemon->calls.last().second, QVariantList{false});
}

void TestInteractController::aDaemonThatGoesAwayEndsTheMode()
{
    startOn();
    daemon->setAvailable(false);
    QVERIFY(catchers.empty());
    QVERIFY(!controller->active());
    // And a daemon that comes back off (or too old to have the property) shows nothing.
    daemon->properties.clear();
    daemon->setAvailable(true);
    QTest::qWait(50);
    QVERIFY(catchers.empty());

    startOn();
    surface->setAvailable(false); // the surface restarts: no windows over nothing
    QVERIFY(catchers.empty());
}

void TestInteractController::hardCapEndsTheModeWhateverThePointerDoes()
{
    controller->setHardCapMs(250);
    controller->setIdleTimeoutMs(100);
    startOn();
    for (int i = 0; i < 20 && controller->active(); ++i) {
        QTest::qWait(30);
        if (catchers.count(u"DP-1"_s) != 0)
            catchers.at(u"DP-1"_s)->point(i, i); // movement keeps the idle timer from ever firing
    }
    QVERIFY(!controller->active());
    QCOMPARE(daemon->callCount(u"SetWallpaperInteract"_s), 1);
    QVERIFY(catchers.empty());
}

void TestInteractController::aTransientOutputListWithoutInteractiveWallpaperIsForgiven()
{
    controller->setGraceMs(300);
    startOn();
    surface->answerJson(u"Outputs"_s, outputs({{u"DP-1"_s, true, false}}));
    surface->emitSignal(u"OutputsChanged"_s);
    QTRY_VERIFY(catchers.empty());
    QVERIFY(controller->active()); // not yet
    surface->answerJson(u"Outputs"_s, outputs({{u"DP-1"_s, true, true}}));
    surface->emitSignal(u"OutputsChanged"_s);
    QTRY_VERIFY(!catchers.empty());
    QTest::qWait(450);
    QVERIFY(controller->active());
    QCOMPARE(daemon->callCount(u"SetWallpaperInteract"_s), 0);

    // Staying without one ends it.
    surface->answerJson(u"Outputs"_s, outputs({{u"DP-1"_s, true, false}}));
    surface->emitSignal(u"OutputsChanged"_s);
    QTRY_COMPARE_WITH_TIMEOUT(daemon->callCount(u"SetWallpaperInteract"_s), 1, 2000);
    QVERIFY(!controller->active());
}

QTEST_MAIN(TestInteractController)
#include "tst_interactcontroller.moc"
