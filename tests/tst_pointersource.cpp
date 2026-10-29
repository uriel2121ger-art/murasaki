// The pointer sources: which backend a session gets, the polling and Hyprland sources against fakes, the KWin script in a fake KWin
// (QJSEngine) and the KWin source against a fake org.kde.KWin on a private bus.

#include "hyprlandpointersource.h"
#include "kwinpointersource.h"
#include "pointercapability.h"
#include "petbehaviours.h"
#include "pollingpointersource.h"

#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusError>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QFile>
#include <QJSEngine>
#include <QJSValue>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <climits>
#include <cmath>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

CompositorProfile profileOf(CompositorFamily family)
{
    CompositorProfile p;
    p.family = family;
    return p;
}

bool has(const PointerCapability &c, std::string_view name)
{
    return std::find(c.degraded.begin(), c.degraded.end(), name) != c.degraded.end();
}

// A stand-in for org.kde.KWin's scripting interface.
class FakeScripting : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kwin.Scripting")

public:
    QStringList calls;
    bool loaded = false;
    int loadResult = 7;
    int startDelayMs = 0;    // the reply to start() comes this long after the call
    bool failStart = false;  // start() answers with an error
    int failIsLoaded = 0;    // the next this-many isScriptLoaded() answer with an error
    QString lastPath;
    QByteArray lastScript;

public slots:
    Q_SCRIPTABLE bool isScriptLoaded(const QString &name)
    {
        calls << u"isScriptLoaded "_s + name;
        if (failIsLoaded > 0) {
            --failIsLoaded;
            sendErrorReply(QDBusError::Failed, u"boom"_s);
            return false;
        }
        return loaded;
    }
    Q_SCRIPTABLE int loadScript(const QString &path, const QString &name)
    {
        calls << u"loadScript "_s + name;
        lastPath = path;
        QFile f(path);
        lastScript = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        if (loadResult >= 0)
            loaded = true;
        return loadResult;
    }
    Q_SCRIPTABLE bool unloadScript(const QString &name)
    {
        calls << u"unloadScript "_s + name;
        const bool was = loaded;
        loaded = false;
        return was;
    }
    Q_SCRIPTABLE void start()
    {
        calls << u"start"_s;
        if (failStart) {
            sendErrorReply(QDBusError::Failed, u"cannot start"_s);
            return;
        }
        if (startDelayMs > 0) {
            setDelayedReply(true);
            const QDBusMessage request = message();
            const QDBusConnection bus = connection();
            QTimer::singleShot(startDelayMs, this, [request, bus] { QDBusConnection(bus).send(request.createReply()); });
        }
    }
};

} // namespace

class TstPointerSource : public QObject
{
    Q_OBJECT

private slots:
    // capability
    void kdeWaylandGetsTheKWinScript();
    void x11IsPolledWhateverTheDesktop();
    void hyprlandNeedsItsSockets();
    void everyOtherCompositorIsLocalAndNamed();
    void theOverrideCanOnlyForceLocal();
    // sources
    void theLocalSourceReportsNothing();
    void thePollingSourceReportsChangesWhileWanted();
    void aLazySourceIsMadeOnTheFirstRequestOnly();
    void hyprlandReplyParser_data();
    void hyprlandReplyParser();
    void hyprlandSocketDirFollowsTheMonitorsRule();
    void hyprlandSourceAsksOnlyWhileWanted();
    void hyprlandSourceGivesUpOnASilentSocket();
    void hyprlandSourceSurvivesAShortOutage();
    void hyprlandSocketsMustBeOurs();
    void hyprlandSourceNeverPilesUpRequests();
    // the script
    void theScriptTargetsTheOverlay();
    void theScriptThrottlesLikeTheSurfaceOne();
    void theScriptStopsWhenTheOverlayIsGone();
    // the KWin source
    void kwinSourceLoadsAndUnloadsTheScript();
    void kwinSourceAcceptsReportsOnlyFromKWin();
    void kwinSourceCoalescesAFlood();
    void kwinSourceSaysWhenKWinRefuses();
    void kwinSourceWithoutKWinIsUnavailable();
    void kwinSourceWantedFalseDuringTheLoadLeavesNothingRunning();
    void kwinSourceRetargetsTheScript();
    void kwinSourceReloadsAfterKWinRestarts();
    void kwinSourceRefusesAServiceNameThatIsNotAName();
    void theScriptNeverOutlivesTheOverlay();
    void kwinSourceNeedsItsObjectToBeExported();
    void kwinSourceRetriesOnceWhenKWinIsBack();
    void kwinSourceUnloadsAScriptThatCouldNotBeStarted();
    void kwinSourceKnowsWhoKWinIsWithoutAskingTheBus();
    void kwinSourceIgnoresAbsurdCoordinates();

private:
    static QJSValue loadScript(QJSEngine &engine);
    static int count(QJSValue &api) { return api.property(u"calls"_s).property(u"length"_s).toInt(); } // (no NameHasOwner is sent before the watchdog fires)
};

// ---- capability --------------------------------------------------------------------------------------------------------------

void TstPointerSource::kdeWaylandGetsTheKWinScript()
{
    PointerEnvironment env;
    env.kwinOnBus = true;
    const auto c = choosePointerBackend(profileOf(CompositorFamily::Kde), env);
    QCOMPARE(int(c.backend), int(PointerBackend::KWinScript));
    QCOMPARE(int(c.level), int(PointerSource::Level::Global));
    QVERIFY(c.degraded.empty());
    QVERIFY(!c.reason.isEmpty());
    // KDE without KWin on the bus (a nested session, a broken one): local.
    env.kwinOnBus = false;
    const auto none = choosePointerBackend(profileOf(CompositorFamily::Kde), env);
    QCOMPARE(int(none.backend), int(PointerBackend::Local));
    QVERIFY(has(none, degrade::kPointerLocal));
}

void TstPointerSource::x11IsPolledWhateverTheDesktop()
{
    PointerEnvironment env;
    env.x11Session = true;
    env.kwinOnBus = true; // KWin's X11 session is on the bus too: the script is still not used
    for (const auto family : {CompositorFamily::Kde, CompositorFamily::Gnome, CompositorFamily::X11, CompositorFamily::Unknown}) {
        const auto c = choosePointerBackend(profileOf(family), env);
        QCOMPARE(int(c.backend), int(PointerBackend::X11Poll));
        QCOMPARE(int(c.level), int(PointerSource::Level::Global));
    }
    // The family alone (the profile says X11) is enough.
    QCOMPARE(int(choosePointerBackend(profileOf(CompositorFamily::X11), {}).backend), int(PointerBackend::X11Poll));
}

void TstPointerSource::hyprlandNeedsItsSockets()
{
    PointerEnvironment env;
    env.hyprlandSocketDir = u"/run/user/1000/hypr/abc"_s;
    QCOMPARE(int(choosePointerBackend(profileOf(CompositorFamily::Hyprland), env).backend), int(PointerBackend::HyprlandIpc));
    env.hyprlandSocketDir.clear();
    const auto c = choosePointerBackend(profileOf(CompositorFamily::Hyprland), env);
    QCOMPARE(int(c.backend), int(PointerBackend::Local));
    QVERIFY(has(c, degrade::kPointerLocal));
}

void TstPointerSource::everyOtherCompositorIsLocalAndNamed()
{
    PointerEnvironment env;
    env.kwinOnBus = true;                 // irrelevant outside KDE
    env.hyprlandSocketDir = u"/x"_s;      // irrelevant outside Hyprland
    for (const auto family : {CompositorFamily::Sway, CompositorFamily::Wlroots, CompositorFamily::Cosmic, CompositorFamily::Gnome,
                              CompositorFamily::Unknown}) {
        const auto c = choosePointerBackend(profileOf(family), env);
        QCOMPARE(int(c.backend), int(PointerBackend::Local));
        QCOMPARE(int(c.level), int(PointerSource::Level::Local));
        QCOMPARE(int(c.degraded.size()), 1);
        QVERIFY(has(c, degrade::kPointerLocal));
        QVERIFY(!c.reason.isEmpty());
    }
}

void TstPointerSource::theOverrideCanOnlyForceLocal()
{
    PointerEnvironment env;
    env.kwinOnBus = true;
    env.override = u"local"_s;
    QCOMPARE(int(choosePointerBackend(profileOf(CompositorFamily::Kde), env).backend), int(PointerBackend::Local));
    env.override = u" LOCAL "_s;
    QCOMPARE(int(choosePointerBackend(profileOf(CompositorFamily::Kde), env).backend), int(PointerBackend::Local));
    for (const QString &value : {u""_s, u"auto"_s, u"global"_s, u"nonsense"_s})
        QCOMPARE(int(choosePointerBackend(profileOf(CompositorFamily::Kde), PointerEnvironment{true, false, {}, value}).backend), int(PointerBackend::KWinScript));
    // "global" can not invent a source where there is none.
    QCOMPARE(int(choosePointerBackend(profileOf(CompositorFamily::Sway), PointerEnvironment{false, false, {}, u"global"_s}).backend), int(PointerBackend::Local));
}

// ---- sources -----------------------------------------------------------------------------------------------------------------

void TstPointerSource::theLocalSourceReportsNothing()
{
    LocalPointerSource source;
    QSignalSpy moved(&source, &PointerSource::moved);
    QCOMPARE(int(source.level()), int(PointerSource::Level::Local));
    source.wanted(true);
    QVERIFY(source.isWanted());
    QTest::qWait(50);
    QCOMPARE(moved.count(), 0);
    source.wanted(false);
    QVERIFY(!source.isWanted());
}

void TstPointerSource::aLazySourceIsMadeOnTheFirstRequestOnly()
{
    int made = 0;
    ManualPointerSource *real = nullptr;
    LazyPointerSource source(PointerSource::Level::Global, [&]() -> std::unique_ptr<PointerSource> {
        ++made;
        auto manual = std::make_unique<ManualPointerSource>(PointerSource::Level::Global);
        real = manual.get();
        return manual;
    });
    QSignalSpy moved(&source, &PointerSource::moved);
    QSignalSpy gone(&source, &PointerSource::unavailable);
    QCOMPARE(int(source.level()), int(PointerSource::Level::Global)); // known without a backend
    source.wanted(false);                                              // nothing to stop: nothing is made for it
    QCOMPARE(made, 0);
    QVERIFY(!source.created());
    source.wanted(true);
    QCOMPARE(made, 1);
    QVERIFY(source.created());
    QVERIFY(real->isWanted());
    real->moveTo({3, 4});
    QCOMPARE(moved.count(), 1);
    QCOMPARE(moved.at(0).at(0).toPointF(), QPointF(3, 4));
    emit real->unavailable(u"gone"_s);
    QCOMPARE(gone.count(), 1);
    source.wanted(false);
    QVERIFY(!real->isWanted());
    source.wanted(true); // again: the same backend, not a second one
    QCOMPARE(made, 1);
    // A factory that makes nothing is not an error.
    LazyPointerSource empty(PointerSource::Level::Global, [] { return std::unique_ptr<PointerSource>(); });
    empty.wanted(true);
    QVERIFY(!empty.created());
}

void TstPointerSource::thePollingSourceReportsChangesWhileWanted()
{
    std::optional<QPointF> where = QPointF(10, 20);
    int asked = 0;
    PollingPointerSource source([&] {
        ++asked;
        return where;
    }, 5);
    QSignalSpy moved(&source, &PointerSource::moved);
    QCOMPARE(int(source.level()), int(PointerSource::Level::Global));
    QVERIFY(!source.isRunning());
    QTest::qWait(30);
    QCOMPARE(asked, 0); // nothing runs until wanted
    source.wanted(true);
    QVERIFY(source.isRunning());
    QCOMPARE(moved.count(), 1); // where the cursor rests, at once
    QCOMPARE(moved.at(0).at(0).toPointF(), QPointF(10, 20));
    QTest::qWait(40);
    QCOMPARE(moved.count(), 1); // unchanged: nothing
    QVERIFY(asked > 2);
    where = QPointF(11, 20);
    QTRY_COMPARE(moved.count(), 2);
    where = std::nullopt; // the provider has nothing (no display): ignored
    QTest::qWait(30);
    QCOMPARE(moved.count(), 2);
    where = QPointF(std::nan(""), 0);
    QTest::qWait(30);
    QCOMPARE(moved.count(), 2);
    source.wanted(false);
    QVERIFY(!source.isRunning());
    const int before = asked;
    where = QPointF(500, 500);
    QTest::qWait(40);
    QCOMPARE(asked, before); // stopped: not even asked
    source.wanted(true);     // and a new start reports again, even for the same place as before
    QVERIFY(moved.count() >= 3);
    QCOMPARE(moved.last().at(0).toPointF(), QPointF(500, 500));
}

void TstPointerSource::hyprlandReplyParser_data()
{
    QTest::addColumn<QByteArray>("reply");
    QTest::addColumn<bool>("valid");
    QTest::addColumn<double>("x");
    QTest::addColumn<double>("y");
    QTest::newRow("json ints") << QByteArray("{\"x\": 1234, \"y\": 567}") << true << 1234.0 << 567.0;
    QTest::newRow("json floats") << QByteArray("{\"x\": 12.5, \"y\": -3.25}\n") << true << 12.5 << -3.25;
    QTest::newRow("json other keys kept out") << QByteArray("{\"y\": 2, \"x\": 1, \"z\": 3}") << true << 1.0 << 2.0;
    QTest::newRow("text") << QByteArray("1234, 567\n") << true << 1234.0 << 567.0;
    QTest::newRow("text negative") << QByteArray("-1, -2") << true << -1.0 << -2.0;
    QTest::newRow("text fractional") << QByteArray("10.5,20.5") << true << 10.5 << 20.5;
    QTest::newRow("empty") << QByteArray() << false << 0.0 << 0.0;
    QTest::newRow("blank") << QByteArray("  \n") << false << 0.0 << 0.0;
    QTest::newRow("json missing y") << QByteArray("{\"x\": 1}") << false << 0.0 << 0.0;
    QTest::newRow("json strings") << QByteArray("{\"x\": \"1\", \"y\": \"2\"}") << false << 0.0 << 0.0;
    QTest::newRow("json array") << QByteArray("[1, 2]") << false << 0.0 << 0.0;
    QTest::newRow("json broken") << QByteArray("{\"x\": 1, \"y\":") << false << 0.0 << 0.0;
    QTest::newRow("json null") << QByteArray("{\"x\": null, \"y\": null}") << false << 0.0 << 0.0;
    QTest::newRow("text three numbers") << QByteArray("1, 2, 3") << false << 0.0 << 0.0;
    QTest::newRow("text one number") << QByteArray("1") << false << 0.0 << 0.0;
    QTest::newRow("text words") << QByteArray("unknown request") << false << 0.0 << 0.0;
    QTest::newRow("nan") << QByteArray("nan, 1") << false << 0.0 << 0.0;
    QTest::newRow("inf") << QByteArray("1, inf") << false << 0.0 << 0.0;
    QTest::newRow("huge json") << QByteArray("{\"x\": 1e300, \"y\": 1}") << false << 0.0 << 0.0;
    QTest::newRow("huge text") << QByteArray("2000000, 1") << false << 0.0 << 0.0;
    QTest::newRow("too long") << (QByteArray("{\"x\": 1, \"y\": 2, \"pad\": \"") + QByteArray(400, 'a') + "\"}") << false << 0.0 << 0.0;
}

void TstPointerSource::hyprlandReplyParser()
{
    QFETCH(QByteArray, reply);
    QFETCH(bool, valid);
    QFETCH(double, x);
    QFETCH(double, y);
    const auto p = HyprlandPointerSource::parseCursor(reply);
    QCOMPARE(p.has_value(), valid);
    if (valid) {
        QCOMPARE(p->x(), x);
        QCOMPARE(p->y(), y);
    }
}

void TstPointerSource::hyprlandSocketDirFollowsTheMonitorsRule()
{
    QTemporaryDir runtime;
    QVERIFY(QDir().mkpath(runtime.path() + u"/hypr/sig1"_s));
    QFile socket(runtime.path() + u"/hypr/sig1/.socket.sock"_s);
    QVERIFY(socket.open(QIODevice::WriteOnly));
    socket.close();
    QProcessEnvironment env;
    env.insert(u"XDG_RUNTIME_DIR"_s, runtime.path());
    QVERIFY(HyprlandPointerSource::socketDir(env).isEmpty()); // no signature
    env.insert(u"HYPRLAND_INSTANCE_SIGNATURE"_s, u"sig1"_s);
    QCOMPARE(HyprlandPointerSource::socketDir(env), runtime.path() + u"/hypr/sig1"_s);
    env.insert(u"HYPRLAND_INSTANCE_SIGNATURE"_s, u"nothere"_s);
    QVERIFY(HyprlandPointerSource::socketDir(env).isEmpty());
    for (const QString &evil : {u"../sig1"_s, u"sig1/.."_s, u"a/b"_s, u".."_s})
        env.insert(u"HYPRLAND_INSTANCE_SIGNATURE"_s, evil);
    QVERIFY(HyprlandPointerSource::socketDir(env).isEmpty());
}

namespace {

// A Hyprland request socket: answers "j/cursorpos" with the current JSON and closes, like Hyprland.
struct FakeHyprland {
    QTemporaryDir dir;
    QLocalServer server;
    QByteArray reply = "{\"x\": 100, \"y\": 200}";
    int connections = 0;
    int requests = 0;
    int delayMs = 0;
    QStringList seen;

    bool start()
    {
        if (!dir.isValid())
            return false;
        QObject::connect(&server, &QLocalServer::newConnection, &server, [this] {
            while (QLocalSocket *socket = server.nextPendingConnection()) {
                ++connections;
                QObject::connect(socket, &QLocalSocket::readyRead, socket, [this, socket] {
                    seen << QString::fromUtf8(socket->readAll());
                    ++requests;
                    const auto answer = [this, socket] {
                        socket->write(reply);
                        socket->disconnectFromServer();
                    };
                    if (delayMs > 0)
                        QTimer::singleShot(delayMs, socket, answer);
                    else
                        answer();
                });
                QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        return server.listen(dir.path() + u"/.socket.sock"_s);
    }
};

} // namespace

void TstPointerSource::hyprlandSourceAsksOnlyWhileWanted()
{
    FakeHyprland hypr;
    QVERIFY(hypr.start());
    HyprlandPointerSource source(hypr.dir.path(), 10);
    QSignalSpy moved(&source, &PointerSource::moved);
    QCOMPARE(int(source.level()), int(PointerSource::Level::Global));
    QTest::qWait(60);
    QCOMPARE(hypr.requests, 0);
    source.wanted(true);
    QTRY_COMPARE(moved.count(), 1);
    QCOMPARE(moved.at(0).at(0).toPointF(), QPointF(100, 200));
    QCOMPARE(hypr.seen.first(), u"j/cursorpos"_s);
    QTest::qWait(80);
    QCOMPARE(moved.count(), 1); // unchanged: polled, not reported
    QVERIFY(hypr.requests > 2);
    hypr.reply = "{\"x\": 101, \"y\": 200}";
    QTRY_COMPARE(moved.count(), 2);
    QCOMPARE(moved.at(1).at(0).toPointF(), QPointF(101, 200));
    hypr.reply = "garbage"; // a bad reply is not a movement
    QTest::qWait(60);
    QCOMPARE(moved.count(), 2);
    hypr.reply = "{\"x\": 101, \"y\": 200}";
    source.wanted(false);
    QVERIFY(!source.isRunning());
    QTest::qWait(30); // an answer in flight is dropped
    const int stopped = hypr.requests;
    QTest::qWait(80);
    QCOMPARE(hypr.requests, stopped);
    QCOMPARE(moved.count(), 2);
}

void TstPointerSource::hyprlandSourceGivesUpOnASilentSocket()
{
    QTemporaryDir dir; // no server in it
    HyprlandPointerSource source(dir.path(), 3);
    source.setGiveUpWindowMs(200); // (3 s by default)
    QSignalSpy gone(&source, &PointerSource::unavailable);
    QSignalSpy moved(&source, &PointerSource::moved);
    source.wanted(true);
    QTRY_COMPARE_WITH_TIMEOUT(gone.count(), 1, 5000);
    QVERIFY(!source.isRunning());
    QCOMPARE(moved.count(), 0);
    QVERIFY(!gone.at(0).at(0).toString().isEmpty());
    QTest::qWait(40);
    QCOMPARE(gone.count(), 1); // once
}

void TstPointerSource::hyprlandSourceSurvivesAShortOutage()
{
    FakeHyprland hypr;
    QVERIFY(hypr.dir.isValid());
    HyprlandPointerSource source(hypr.dir.path(), 3); // no server yet: every request fails at once
    source.setGiveUpWindowMs(1500);
    QSignalSpy gone(&source, &PointerSource::unavailable);
    QSignalSpy moved(&source, &PointerSource::moved);
    source.wanted(true);
    QTest::qWait(300); // well over 10 failures, well under the window
    QCOMPARE(gone.count(), 0);
    QVERIFY(source.isRunning());
    QVERIFY(hypr.start()); // Hyprland is back
    QTRY_COMPARE(moved.count(), 1);
    QTest::qWait(1700); // longer than the window: the failures were forgotten at the first success
    QCOMPARE(gone.count(), 0);
}

void TstPointerSource::hyprlandSocketsMustBeOurs()
{
    QTemporaryDir dir;
    QVERIFY(HyprlandPointerSource::ownedByCurrentUser(dir.path()));
    QVERIFY(!HyprlandPointerSource::ownedByCurrentUser(u"/"_s)); // root's (the tests do not run as root)
    QVERIFY(!HyprlandPointerSource::ownedByCurrentUser(dir.path() + u"/missing"_s));
    // No runtime directory: the base is not "/hypr/".
    QProcessEnvironment env;
    env.insert(u"HYPRLAND_INSTANCE_SIGNATURE"_s, u"whatever"_s);
    QVERIFY(HyprlandPointerSource::socketDir(env).isEmpty());
    env.insert(u"XDG_RUNTIME_DIR"_s, QString());
    QVERIFY(HyprlandPointerSource::socketDir(env).isEmpty());
    QCOMPARE(int(HyprlandPointerSource::kDefaultIntervalMs), 34);
}

void TstPointerSource::hyprlandSourceNeverPilesUpRequests()
{
    FakeHyprland hypr;
    hypr.delayMs = 150; // a slow Hyprland
    QVERIFY(hypr.start());
    HyprlandPointerSource source(hypr.dir.path(), 5);
    source.wanted(true);
    QTest::qWait(400);
    // About three replies fit in 400 ms; at 5 ms intervals without the guard there would be dozens of requests.
    QVERIFY2(hypr.requests <= 5, qPrintable(QString::number(hypr.requests)));
    QVERIFY(hypr.requests >= 1);
}

// ---- the script ----------------------------------------------------------------------------------------------------------------

QJSValue TstPointerSource::loadScript(QJSEngine &engine)
{
    QFile file(QStringLiteral(KWIN_PET_CURSOR_SCRIPT));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QString head = QStringLiteral(
        "(function () {"
        "  const calls = []; const handlers = []; const timers = [];"
        "  const workspace = { cursorPos: { x: 640, y: 360 }, cursorPosChanged: { connect: f => handlers.push(f), disconnect: f => { const i = handlers.indexOf(f); if (i >= 0) handlers.splice(i, 1); } } };"
        "  function callDBus(...args) { calls.push(args); }"
        "  function QTimer() {"
        "    this.interval = 0; this.singleShot = false; this.running = false; this.callbacks = [];"
        "    this.timeout = { connect: f => this.callbacks.push(f) };"
        "    this.start = () => { this.running = true; };"
        "    this.stop = () => { this.running = false; };"
        "    timers.push(this);"
        "  }");
    const QString tail = QStringLiteral(
        ";\n  return {"
        "    calls: calls,"
        "    move: (x, y) => { workspace.cursorPos = { x: x, y: y }; handlers.forEach(f => f()); },"
        "    tick: () => timers.filter(t => t.running && t.singleShot).forEach(t => { t.running = false; t.callbacks.forEach(f => f()); }),"
        "    running: () => timers.filter(t => t.running && t.singleShot).length,"
        "    interval: () => timers[0].interval,"
        "    handlers: () => handlers.length,"
        "    watchdogs: () => timers.filter(t => !t.singleShot && t.running).length,"
        "    watchdogInterval: () => timers.filter(t => !t.singleShot)[0].interval,"
        "    watchdogTick: () => timers.filter(t => !t.singleShot && t.running).forEach(t => t.callbacks.forEach(f => f())),"
        "    answerOwner: (has) => { const c = calls.filter(a => a[3] === 'NameHasOwner'); c[c.length - 1][c[c.length - 1].length - 1](has); },"
        "    ownerQueries: () => calls.filter(a => a[3] === 'NameHasOwner').length,"
        "  };"
        "})()");
    return engine.evaluate(head + u"\n"_s + QString::fromUtf8(file.readAll()) + tail, u"kwin-pet-cursor-test.js"_s);
}

void TstPointerSource::theScriptTargetsTheOverlay()
{
    QJSEngine engine;
    QJSValue api = loadScript(engine);
    QVERIFY2(!api.isError(), qPrintable(api.toString()));
    QCOMPARE(api.property(u"handlers"_s).callWithInstance(api).toInt(), 1); // it listens to cursorPosChanged
    QCOMPARE(count(api), 1);                                                 // and reports where the cursor rests at load
    const QJSValue call = api.property(u"calls"_s).property(0);
    QCOMPARE(call.property(0).toString(), QString::fromLatin1(KWinPointerSource::kDefaultServiceName));
    QCOMPARE(call.property(1).toString(), QString::fromLatin1(KWinPointerSource::kObjectPath));
    QCOMPARE(call.property(2).toString(), u"io.github.uriel2121ger_art.Murasaki.Overlay.Pointer1"_s);
    QCOMPARE(call.property(3).toString(), u"ReportPointer"_s);
    QCOMPARE(call.property(4).toInt(), 640);
    QCOMPARE(call.property(5).toInt(), 360);
}

void TstPointerSource::theScriptThrottlesLikeTheSurfaceOne()
{
    QJSEngine engine;
    QJSValue api = loadScript(engine);
    QVERIFY(api.property(u"interval"_s).callWithInstance(api).toInt() >= 33); // <= 30 Hz
    const int start = count(api);
    api.property(u"move"_s).callWithInstance(api, {1, 1}); // first goes out at once
    QCOMPARE(count(api), start + 1);
    for (int i = 2; i <= 40; ++i)
        api.property(u"move"_s).callWithInstance(api, {i, i});
    QCOMPARE(count(api), start + 1); // the burst waits
    api.property(u"tick"_s).callWithInstance(api);
    QCOMPARE(count(api), start + 2); // one trailing report with the latest position
    QCOMPARE(api.property(u"calls"_s).property(start + 1).property(4).toInt(), 40);
    api.property(u"tick"_s).callWithInstance(api); // nothing moved: quiet, and no timer left running
    api.property(u"tick"_s).callWithInstance(api);
    QCOMPARE(count(api), start + 2);
    QCOMPARE(api.property(u"running"_s).callWithInstance(api).toInt(), 0);
}

void TstPointerSource::theScriptStopsWhenTheOverlayIsGone()
{
    QJSEngine engine;
    QJSValue api = loadScript(engine);
    QVERIFY2(!api.isError(), qPrintable(api.toString()));
    QCOMPARE(api.property(u"watchdogs"_s).callWithInstance(api).toInt(), 1);
    QVERIFY(api.property(u"watchdogInterval"_s).callWithInstance(api).toInt() >= 1000); // cheap: a check every few seconds
    // The overlay is there: the checks go on, the script reports.
    for (int i = 0; i < 3; ++i) {
        api.property(u"watchdogTick"_s).callWithInstance(api);
        api.property(u"answerOwner"_s).callWithInstance(api, {true});
    }
    api.property(u"move"_s).callWithInstance(api, {5, 5});
    const int before = count(api);
    QVERIFY(before >= 2);
    QCOMPARE(api.property(u"handlers"_s).callWithInstance(api).toInt(), 1);
    // Gone once: not yet (a restart in progress). Back: the count starts again.
    api.property(u"watchdogTick"_s).callWithInstance(api);
    api.property(u"answerOwner"_s).callWithInstance(api, {false});
    api.property(u"watchdogTick"_s).callWithInstance(api);
    api.property(u"answerOwner"_s).callWithInstance(api, {true});
    api.property(u"watchdogTick"_s).callWithInstance(api);
    api.property(u"answerOwner"_s).callWithInstance(api, {false});
    QCOMPARE(api.property(u"handlers"_s).callWithInstance(api).toInt(), 1);
    // Gone twice in a row: the cursor connection is dropped and the checks stop; nothing more is ever sent.
    api.property(u"watchdogTick"_s).callWithInstance(api);
    api.property(u"answerOwner"_s).callWithInstance(api, {false});
    QCOMPARE(api.property(u"handlers"_s).callWithInstance(api).toInt(), 0);
    QCOMPARE(api.property(u"watchdogs"_s).callWithInstance(api).toInt(), 0);
    const int queries = api.property(u"ownerQueries"_s).callWithInstance(api).toInt();
    api.property(u"move"_s).callWithInstance(api, {9, 9}); // (the connection is gone; a stray call is ignored as well)
    api.property(u"tick"_s).callWithInstance(api);
    api.property(u"watchdogTick"_s).callWithInstance(api);
    QCOMPARE(api.property(u"ownerQueries"_s).callWithInstance(api).toInt(), queries);
    // The queries go to the bus daemon about the overlay's name.
    const QJSValue last = api.property(u"calls"_s).property(count(api) - 1);
    QVERIFY(last.property(u"length"_s).toInt() >= 5);
}

// ---- the KWin source ----------------------------------------------------------------------------------------------------------------

#define NEEDS_PRIVATE_BUS                                                                                                              \
    if (qEnvironmentVariableIntValue("MURASAKI_TEST_PRIVATE_BUS") != 1)                                                               \
        QSKIP("needs a private session bus (run through ctest, or set MURASAKI_TEST_PRIVATE_BUS=1 under dbus-run-session)");

namespace {

// Two more connections to the private bus: "a" is the overlay's, "kwin" owns org.kde.KWin, "stranger" is anybody else.
struct Bus {
    QDBusConnection a, kwin, stranger;
    FakeScripting scripting;
    Bus(const QString &tag)
        : a(QDBusConnection::connectToBus(qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS"), tag + u"-a"_s))
        , kwin(QDBusConnection::connectToBus(qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS"), tag + u"-kwin"_s))
        , stranger(QDBusConnection::connectToBus(qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS"), tag + u"-stranger"_s))
        , m_tag(tag)
    {
    }
    ~Bus()
    {
        kwin.unregisterObject(u"/Scripting"_s);
        for (const QString &n : {u"-a"_s, u"-kwin"_s, u"-stranger"_s})
            QDBusConnection::disconnectFromBus(m_tag + n);
    }
    bool ownKWin(const QString &name = u"org.kde.KWin"_s)
    {
        return kwin.registerObject(u"/Scripting"_s, &scripting, QDBusConnection::ExportAllSlots) && kwin.registerService(name);
    }
    void report(QDBusConnection &from, int x, int y)
    {
        QDBusMessage m = QDBusMessage::createMethodCall(a.baseService(), QString::fromLatin1(KWinPointerSource::kObjectPath),
                                                        u"io.github.uriel2121ger_art.Murasaki.Overlay.Pointer1"_s, u"ReportPointer"_s);
        m.setArguments({x, y});
        from.asyncCall(m);
    }

private:
    QString m_tag;
};

KWinPointerSource::Options optionsFor(Bus &bus, const QString &runtime)
{
    KWinPointerSource::Options o;
    o.bus = bus.a;
    o.runtimeDir = runtime;
    o.callTimeoutMs = 2000;
    return o;
}

} // namespace

void TstPointerSource::kwinSourceLoadsAndUnloadsTheScript()
{
    NEEDS_PRIVATE_BUS
    Bus bus(u"load"_s);
    QVERIFY(bus.ownKWin());
    QTemporaryDir runtime;
    KWinPointerSource source(optionsFor(bus, runtime.path()));
    QCOMPARE(int(source.level()), int(PointerSource::Level::Global));
    QTest::qWait(50);
    // Nothing is loaded until a pet wants the cursor; the only call is the clean-up of a script a crashed overlay might have left.
    QCOMPARE(bus.scripting.calls, (QStringList{u"unloadScript murasaki-pet-cursor"_s}));
    bus.scripting.calls.clear();
    source.wanted(true);
    QTRY_VERIFY(source.scriptLoaded());
    QCOMPARE(bus.scripting.calls,
             (QStringList{u"isScriptLoaded murasaki-pet-cursor"_s, u"loadScript murasaki-pet-cursor"_s, u"start"_s}));
    QCOMPARE(bus.scripting.lastPath, runtime.path() + u"/kwin-pet-cursor.js"_s);
    QVERIFY(bus.scripting.lastScript.contains("ReportPointer"));
    QVERIFY(bus.scripting.lastScript.contains("io.github.uriel2121ger_art.Murasaki.Overlay"));
    source.wanted(true); // again: nothing new
    QTest::qWait(50);
    QCOMPARE(bus.scripting.calls.size(), 3);
    source.wanted(false);
    QTRY_VERIFY(!bus.scripting.loaded);
    QVERIFY(!source.scriptLoaded());
    QCOMPARE(bus.scripting.calls.last(), u"unloadScript murasaki-pet-cursor"_s);
    // A script left behind by a crashed overlay is replaced.
    bus.scripting.calls.clear();
    bus.scripting.loaded = true;
    source.wanted(true);
    QTRY_VERIFY(source.scriptLoaded());
    QCOMPARE(bus.scripting.calls, (QStringList{u"isScriptLoaded murasaki-pet-cursor"_s, u"unloadScript murasaki-pet-cursor"_s,
                                               u"loadScript murasaki-pet-cursor"_s, u"start"_s}));
}

void TstPointerSource::kwinSourceAcceptsReportsOnlyFromKWin()
{
    NEEDS_PRIVATE_BUS
    Bus bus(u"sender"_s);
    QVERIFY(bus.ownKWin());
    QTemporaryDir runtime;
    KWinPointerSource source(optionsFor(bus, runtime.path()));
    QSignalSpy moved(&source, &PointerSource::moved);
    bus.report(bus.kwin, 5, 6); // not wanted yet: ignored
    QTest::qWait(60);
    QCOMPARE(moved.count(), 0);
    source.wanted(true);
    QTRY_VERIFY(source.scriptLoaded());
    bus.report(bus.stranger, 1, 2); // anybody else: ignored
    QTest::qWait(80);
    QCOMPARE(moved.count(), 0);
    bus.report(bus.kwin, 300, 400);
    QTRY_COMPARE(moved.count(), 1);
    QCOMPARE(moved.at(0).at(0).toPointF(), QPointF(300, 400));
    source.wanted(false);
    bus.report(bus.kwin, 7, 8); // stopped: ignored
    QTest::qWait(80);
    QCOMPARE(moved.count(), 1);
}

void TstPointerSource::kwinSourceCoalescesAFlood()
{
    NEEDS_PRIVATE_BUS
    Bus bus(u"flood"_s);
    QVERIFY(bus.ownKWin());
    QTemporaryDir runtime;
    KWinPointerSource source(optionsFor(bus, runtime.path()));
    QSignalSpy moved(&source, &PointerSource::moved);
    source.wanted(true);
    QTRY_VERIFY(source.scriptLoaded());
    // (The bus allows 128 unanswered calls to one connection, so the flood is 100 calls; a real KWin sends 30 a second.)
    for (int i = 1; i <= 100; ++i)
        bus.report(bus.kwin, i, 2 * i);
    QTRY_VERIFY_WITH_TIMEOUT(!moved.isEmpty() && moved.last().at(0).toPointF() == QPointF(100, 200), 5000); // the last one is never lost
    QTest::qWait(60);
    QVERIFY2(moved.count() < 60, qPrintable(QString::number(moved.count()))); // and the flood is much thinner than 100
    QCOMPARE(moved.last().at(0).toPointF(), QPointF(100, 200));
}

void TstPointerSource::kwinSourceSaysWhenKWinRefuses()
{
    NEEDS_PRIVATE_BUS
    Bus bus(u"refuse"_s);
    QVERIFY(bus.ownKWin());
    bus.scripting.loadResult = -1;
    QTemporaryDir runtime;
    KWinPointerSource source(optionsFor(bus, runtime.path()));
    QSignalSpy gone(&source, &PointerSource::unavailable);
    source.wanted(true);
    QTRY_COMPARE(gone.count(), 1);
    QVERIFY(!source.scriptLoaded());
    QVERIFY(!bus.scripting.calls.contains(u"start"_s));
}

void TstPointerSource::kwinSourceWithoutKWinIsUnavailable()
{
    NEEDS_PRIVATE_BUS
    Bus bus(u"nokwin"_s);
    QTemporaryDir runtime;
    auto o = optionsFor(bus, runtime.path());
    o.kwinService = u"org.kde.NotThere"_s;
    o.callTimeoutMs = 500;
    {
        KWinPointerSource source(o);
        QSignalSpy gone(&source, &PointerSource::unavailable);
        source.wanted(true);
        QTRY_COMPARE_WITH_TIMEOUT(gone.count(), 1, 5000);
        QVERIFY(!source.scriptLoaded());
    }
    // An unwritable runtime directory is unavailable as well.
    auto bad = optionsFor(bus, u"/proc/nope/murasaki"_s);
    bad.kwinService = u"org.kde.NotThere"_s;
    KWinPointerSource source2(bad);
    QSignalSpy gone2(&source2, &PointerSource::unavailable);
    source2.wanted(true);
    QTRY_COMPARE(gone2.count(), 1);
}

void TstPointerSource::kwinSourceWantedFalseDuringTheLoadLeavesNothingRunning()
{
    NEEDS_PRIVATE_BUS
    Bus bus(u"race"_s);
    QVERIFY(bus.ownKWin());
    QTemporaryDir runtime;
    KWinPointerSource source(optionsFor(bus, runtime.path()));
    source.wanted(true);
    source.wanted(false); // before KWin has answered anything
    QTest::qWait(300);
    QVERIFY(!bus.scripting.loaded);
    QVERIFY(!source.scriptLoaded());
    // On, off, on in a row: ends loaded exactly once.
    bus.scripting.calls.clear();
    source.wanted(true);
    source.wanted(false);
    source.wanted(true);
    QTRY_VERIFY(source.scriptLoaded());
    QTest::qWait(100);
    QVERIFY(bus.scripting.loaded);
    QVERIFY(source.isWanted());
    source.wanted(false);
    QTRY_VERIFY(!bus.scripting.loaded);
}

void TstPointerSource::kwinSourceRetargetsTheScript()
{
    NEEDS_PRIVATE_BUS
    Bus bus(u"retarget"_s);
    QVERIFY(bus.ownKWin());
    QTemporaryDir runtime;
    auto o = optionsFor(bus, runtime.path());
    o.serviceName = u"test.pointer.sink"_s;
    KWinPointerSource source(o);
    source.wanted(true);
    QTRY_VERIFY(source.scriptLoaded());
    // Only the service line changes: the interface name begins with the default service name and must stay as it is (the call would be
    // refused otherwise), and so must the object path.
    QVERIFY(bus.scripting.lastScript.contains("const service = \"test.pointer.sink\";"));
    QVERIFY(!bus.scripting.lastScript.contains("const service = \"io.github"));
    QVERIFY(bus.scripting.lastScript.contains("const iface = \"io.github.uriel2121ger_art.Murasaki.Overlay.Pointer1\";"));
    QVERIFY(bus.scripting.lastScript.contains("const path = \"/io/github/uriel2121ger_art/Murasaki/Overlay/Pointer\";"));
}

void TstPointerSource::kwinSourceRefusesAServiceNameThatIsNotAName()
{
    NEEDS_PRIVATE_BUS
    Bus bus(u"badname"_s);
    QVERIFY(bus.ownKWin());
    QTemporaryDir runtime;
    for (const QString &bad : {u"a\"; evil(); //"_s, u"with space"_s, u""_s, u"a/b"_s, QString(300, u'a')}) {
        auto o = optionsFor(bus, runtime.path());
        o.serviceName = bad;
        KWinPointerSource source(o);
        QSignalSpy gone(&source, &PointerSource::unavailable);
        bus.scripting.calls.clear();
        source.wanted(true);
        QTRY_COMPARE(gone.count(), 1);
        QVERIFY2(!bus.scripting.calls.contains(u"start"_s) && !bus.scripting.calls.contains(u"loadScript murasaki-pet-cursor"_s), qPrintable(bad));
    }
}

void TstPointerSource::theScriptNeverOutlivesTheOverlay()
{
    NEEDS_PRIVATE_BUS
    Bus bus(u"outlive"_s);
    QVERIFY(bus.ownKWin());
    QTemporaryDir runtime;
    // A script left in KWin by an overlay that was killed: the next overlay removes it at construction, though nothing wants the cursor.
    bus.scripting.loaded = true;
    {
        KWinPointerSource source(optionsFor(bus, runtime.path()));
        QTRY_VERIFY(!bus.scripting.loaded);
        QVERIFY(!source.isWanted());
    }
    // Destroyed while the load is under way (start() not answered yet): the script is taken out anyway.
    bus.scripting.startDelayMs = 500;
    bus.scripting.calls.clear();
    auto *source = new KWinPointerSource(optionsFor(bus, runtime.path()));
    source->wanted(true);
    QTRY_VERIFY(bus.scripting.calls.contains(u"start"_s));
    QVERIFY(bus.scripting.loaded);
    QVERIFY(!source->scriptLoaded()); // (still loading)
    delete source;
    QVERIFY(!bus.scripting.loaded);
    QCOMPARE(bus.scripting.calls.last(), u"unloadScript murasaki-pet-cursor"_s);
    // Destroyed after loadScript was sent and before it was answered.
    bus.scripting.startDelayMs = 0;
    bus.scripting.calls.clear();
    bus.scripting.loaded = false;
    source = new KWinPointerSource(optionsFor(bus, runtime.path()));
    source->wanted(true);
    delete source; // nothing has had time to answer
    QTest::qWait(200);
    QVERIFY(!bus.scripting.loaded);
    // A normal wanted(true) .. destroy.
    source = new KWinPointerSource(optionsFor(bus, runtime.path()));
    source->wanted(true);
    QTRY_VERIFY(source->scriptLoaded());
    delete source;
    QVERIFY(!bus.scripting.loaded);
}

void TstPointerSource::kwinSourceNeedsItsObjectToBeExported()
{
    NEEDS_PRIVATE_BUS
    Bus bus(u"unreg"_s);
    QVERIFY(bus.ownKWin());
    QObject squatter; // somebody already exports the path
    QVERIFY(bus.a.registerObject(QString::fromLatin1(KWinPointerSource::kObjectPath), &squatter));
    QTemporaryDir runtime;
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"cannot export"_s));
    KWinPointerSource source(optionsFor(bus, runtime.path()));
    QSignalSpy gone(&source, &PointerSource::unavailable);
    source.wanted(true);
    QCOMPARE(gone.count(), 1); // at once, not after some timeout
    QVERIFY(!source.isWanted());
    QTest::qWait(100);
    QVERIFY(!bus.scripting.calls.contains(u"loadScript murasaki-pet-cursor"_s)); // no script is loaded for a source nobody can call
    bus.a.unregisterObject(QString::fromLatin1(KWinPointerSource::kObjectPath));
}

void TstPointerSource::kwinSourceRetriesOnceWhenKWinIsBack()
{
    NEEDS_PRIVATE_BUS
    Bus bus(u"retry"_s);
    QVERIFY(bus.ownKWin());
    QTemporaryDir runtime;
    {
        bus.scripting.failIsLoaded = 1; // the first attempt fails (KWin was still starting), the retry works
        KWinPointerSource source(optionsFor(bus, runtime.path()));
        QSignalSpy gone(&source, &PointerSource::unavailable);
        source.wanted(true);
        QTRY_VERIFY(source.scriptLoaded());
        QCOMPARE(gone.count(), 0);
    }
    {
        bus.scripting.failIsLoaded = 2; // two failures: reported, once
        KWinPointerSource source(optionsFor(bus, runtime.path()));
        QSignalSpy gone(&source, &PointerSource::unavailable);
        source.wanted(true);
        QTRY_COMPARE(gone.count(), 1);
        QTest::qWait(100);
        QCOMPARE(gone.count(), 1);
        QVERIFY(!source.scriptLoaded());
    }
}

void TstPointerSource::kwinSourceUnloadsAScriptThatCouldNotBeStarted()
{
    NEEDS_PRIVATE_BUS
    Bus bus(u"nostart"_s);
    QVERIFY(bus.ownKWin());
    bus.scripting.failStart = true;
    QTemporaryDir runtime;
    KWinPointerSource source(optionsFor(bus, runtime.path()));
    QSignalSpy gone(&source, &PointerSource::unavailable);
    source.wanted(true);
    QTRY_COMPARE(gone.count(), 1);
    QVERIFY(!bus.scripting.loaded); // loaded by loadScript, then taken out again
    QVERIFY(bus.scripting.calls.contains(u"unloadScript murasaki-pet-cursor"_s));
    QVERIFY(!source.scriptLoaded());
}

void TstPointerSource::kwinSourceKnowsWhoKWinIsWithoutAskingTheBus()
{
    NEEDS_PRIVATE_BUS
    Bus bus(u"owner"_s);
    QVERIFY(bus.ownKWin());
    QTemporaryDir runtime;
    KWinPointerSource source(optionsFor(bus, runtime.path()));
    QSignalSpy moved(&source, &PointerSource::moved);
    source.wanted(true);
    QTRY_VERIFY(source.scriptLoaded());
    bus.report(bus.kwin, 1, 1);
    QTRY_COMPARE(moved.count(), 1);
    // KWin loses the name: its calls (same unique name) are not accepted any more, until it owns the name again.
    QVERIFY(bus.kwin.unregisterService(u"org.kde.KWin"_s));
    QTest::qWait(100);
    bus.report(bus.kwin, 2, 2);
    QTest::qWait(100);
    QCOMPARE(moved.count(), 1);
    QVERIFY(bus.kwin.registerService(u"org.kde.KWin"_s));
    QTRY_VERIFY(bus.scripting.calls.count(u"start"_s) >= 2); // and the script is loaded again
    QTest::qWait(60);
    bus.report(bus.kwin, 3, 3);
    QTRY_COMPARE(moved.count(), 2);
    QCOMPARE(moved.last().at(0).toPointF(), QPointF(3, 3));
}

void TstPointerSource::kwinSourceIgnoresAbsurdCoordinates()
{
    NEEDS_PRIVATE_BUS
    Bus bus(u"range"_s);
    QVERIFY(bus.ownKWin());
    QTemporaryDir runtime;
    KWinPointerSource source(optionsFor(bus, runtime.path()));
    QSignalSpy moved(&source, &PointerSource::moved);
    source.wanted(true);
    QTRY_VERIFY(source.scriptLoaded());
    bus.report(bus.kwin, 2000000, 5);
    bus.report(bus.kwin, 5, -2000000);
    bus.report(bus.kwin, INT_MAX, INT_MIN);
    QTest::qWait(100);
    QCOMPARE(moved.count(), 0);
    bus.report(bus.kwin, -1000000, 1000000); // the bounds themselves are fine
    QTRY_COMPARE(moved.count(), 1);
}

void TstPointerSource::kwinSourceReloadsAfterKWinRestarts()
{
    NEEDS_PRIVATE_BUS
    Bus bus(u"restart"_s);
    QVERIFY(bus.ownKWin());
    QTemporaryDir runtime;
    KWinPointerSource source(optionsFor(bus, runtime.path()));
    source.wanted(true);
    QTRY_VERIFY(source.scriptLoaded());
    // KWin goes away and comes back with no scripts.
    QVERIFY(bus.kwin.unregisterService(u"org.kde.KWin"_s));
    bus.scripting.loaded = false;
    bus.scripting.calls.clear();
    QVERIFY(bus.kwin.registerService(u"org.kde.KWin"_s));
    QTRY_VERIFY(bus.scripting.calls.contains(u"start"_s));
    QTRY_VERIFY(source.scriptLoaded());
    QVERIFY(bus.scripting.loaded);
}

QTEST_MAIN(TstPointerSource)
#include "tst_pointersource.moc"
