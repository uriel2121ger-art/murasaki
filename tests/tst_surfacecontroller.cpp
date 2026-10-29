// SurfaceController against a fake murasakid on a private session bus and fake OutputSinks: what it asks the
// daemon, what it hands each sink, and how the host mode differs from the layer-shell mode.

#include "constants.h"
#include "itemschemehandler.h"
#include "idlesource.h"
#include "sessionmonitor.h"
#ifdef MURASAKI_X11_TESTS
#include "fakewm.h"
#endif
#include "surfacecontroller.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QScopeGuard>
#include <QScreen>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <optional>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

#include <vector>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

// The part of the daemon the controller talks to.
class FakeDaemon : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.uriel2121ger_art.Murasaki1")
    Q_PROPERTY(QString WallpaperEngineAssetsDir READ assetsDir CONSTANT)
    Q_PROPERTY(uint ApiVersion READ apiVersion)

public:
    QString reply;    // ResolveAssignment's answer
    QString settings = u"{}"_s;
    QStringList asked; // "output@desktop" of every ResolveAssignment

    uint apiVersion_ = kMinDBusApiVersion;

    QString assets; // WallpaperEngineAssetsDir; empty like a machine without Wallpaper Engine
    QString assetsDir() const { return assets; }
    uint apiVersion() const { return apiVersion_; }

public slots:
    Q_SCRIPTABLE QString ResolveAssignment(const QString &output, const QString &desktop)
    {
        asked << output + u'@' + desktop;
        return reply;
    }
    Q_SCRIPTABLE QString Settings() { return settings; }
};

// A daemon that owns its name while it is still starting: the first answer comes late (QDBus serves it on a thread of its own, so the
// controller's wait runs out meanwhile).
class SlowDaemon : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.uriel2121ger_art.Murasaki1")
    Q_PROPERTY(QString WallpaperEngineAssetsDir READ assetsDir CONSTANT)
    Q_PROPERTY(uint ApiVersion READ apiVersion)

public:
    QString reply;
    QString assets;
    int delayMs = 0;
    QString assetsDir() const
    {
        QThread::msleep(static_cast<unsigned long>(delayMs));
        return assets;
    }
    uint apiVersion() const { return kMinDBusApiVersion; }

public slots:
    Q_SCRIPTABLE QString ResolveAssignment(const QString &, const QString &) { return reply; }
    Q_SCRIPTABLE QString Settings() { return u"{}"_s; }
};

// The policy server's Surface1 as a host process sees it.
class FakePolicyServer : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.uriel2121ger_art.Murasaki.Surface1")

public:
    QString state = QStringLiteral(R"({"desktop":"*","paused":false,"audible":true,"frameRateLimit":30})");
    QStringList registered;
    QStringList unregistered;
    QList<std::pair<QString, bool>> visibility;
    QString lastCaller;
    int refusals = 0; // the next RegisterHost calls fail with LimitsExceeded
    int attempts = 0;

public slots:
    Q_SCRIPTABLE void RegisterHost(const QString &output)
    {
        lastCaller = message().service();
        ++attempts;
        if (refusals > 0) {
            --refusals;
            sendErrorReply(QDBusError::LimitsExceeded, u"too many hosted outputs"_s);
            return;
        }
        registered << output;
    }
    Q_SCRIPTABLE void UnregisterHost(const QString &output) { unregistered << output; }
    Q_SCRIPTABLE void SetHostVisible(const QString &output, bool visible) { visibility.append({output, visible}); }
    Q_SCRIPTABLE QString HostState(const QString &) { return state; }

signals:
    Q_SCRIPTABLE void HostStateChanged(const QString &output);
    Q_SCRIPTABLE void HostCursor(const QString &output, double x, double y, double width, double height);
    Q_SCRIPTABLE void HostCursorLeft(const QString &output);
    Q_SCRIPTABLE void HostPointer(const QString &output, double x, double y, double width, double height, uint buttons, int wheelX, int wheelY);
};

// A compositor monitor the test drives.
class FakeMonitor : public SessionMonitor
{
public:
    void set(const SessionState &state) { setState(state); }
    bool knowsOcclusion() const override { return occlusion; }
    bool occlusion = false;
};

// The compositor's idle time, driven by the test.
class FakeIdle : public IdleSource
{
public:
    int afterMs() const override { return 45'000; }
};

// GameMode and UPower, with the one property each of them is asked for.
class FakeGameMode : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.feralinteractive.GameMode")
    Q_PROPERTY(int ClientCount MEMBER clientCount)

public:
    int clientCount = 0;
};

class FakeUPower : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.UPower")
    Q_PROPERTY(bool OnBattery MEMBER onBattery)

public:
    bool onBattery = false;
};

struct SinkState {
    QString name;
    std::vector<QVariantMap> contents;
    std::vector<std::pair<bool, bool>> playback; // paused, audible
    std::vector<int> fps;
    std::vector<bool> interactive;
    std::vector<QPointF> cursor; // fractions of the output
    struct Pointer {
        QPointF at;
        unsigned buttons;
        int wheelX, wheelY;
    };
    std::vector<Pointer> pointer; // injectPointerFraction
    std::optional<double> position; // what the video on it reports
    std::vector<double> seeks;
    std::vector<int> caps; // setPlaybackCap
    std::function<void(const SinkHealthEvent &)> health; // what the wallpaper says about itself (drives the listener)
    bool destroyed = false;
};

class FakeSink : public OutputSink
{
public:
    FakeSink(QString name, SinkState *state) : m_name(std::move(name)), m_state(state)
    {
        m_state->name = m_name;
        m_state->health = [this](const SinkHealthEvent &event) { notifyHealth(event); };
    }
    ~FakeSink() override { m_state->destroyed = true; }

    QString outputName() const override { return m_name; }
    void setContent(const QVariantMap &content) override { m_state->contents.push_back(content); }
    void setPlayback(bool paused, bool audible) override { m_state->playback.emplace_back(paused, audible); }
    void setFrameRateLimit(int fps) override { m_state->fps.push_back(fps); }
    void setInteractive(bool interactive) override { m_state->interactive.push_back(interactive); }
    bool wantsAudio() const override { return false; }
    void setAudioSpectrum(const QList<float> &) override {}
    void setCursorFraction(double x, double y) override { m_state->cursor.emplace_back(x, y); }
    void injectPointerFraction(double x, double y, unsigned buttons, int wheelX, int wheelY) override
    {
        m_state->pointer.push_back({QPointF(x, y), buttons, wheelX, wheelY});
    }
    std::optional<double> videoPosition() const override { return m_state->position; }
    void seekVideo(double seconds) override { m_state->seeks.push_back(seconds); }
    void setPlaybackCap(int fps) override { m_state->caps.push_back(fps); }

private:
    QString m_name;
    SinkState *m_state;
};

QString videoReply(const QString &itemId, bool muted = false)
{
    const QJsonObject reply{
        {u"assignment"_s, QJsonObject{{u"itemId"_s, itemId}, {u"fill"_s, u"fit"_s}, {u"muted"_s, muted}}},
        {u"item"_s, QJsonObject{{u"kind"_s, u"video"_s},
                                {u"status"_s, u"ok"_s},
                                {u"contentDir"_s, u"/tmp/murasaki-test"_s},
                                {u"entryFile"_s, u"/tmp/murasaki-test/clip.mp4"_s}}},
    };
    return QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact));
}

QString spanReply(const QString &itemId)
{
    QJsonObject reply = QJsonDocument::fromJson(videoReply(itemId).toUtf8()).object();
    QJsonObject assignment = reply.value(u"assignment"_s).toObject();
    assignment.insert(u"span"_s, true);
    reply.insert(u"assignment"_s, assignment);
    return QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact));
}

QString sceneReply(const QString &itemId)
{
    const QJsonObject reply{
        {u"assignment"_s, QJsonObject{{u"itemId"_s, itemId}, {u"fill"_s, u"fill"_s}}},
        {u"item"_s, QJsonObject{{u"kind"_s, u"scene"_s},
                                {u"status"_s, u"ok"_s},
                                {u"contentDir"_s, u"/tmp/murasaki-test-scene"_s},
                                {u"entryFile"_s, u"/tmp/murasaki-test-scene/scene.pkg"_s}}},
    };
    return QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact));
}

bool waitFor(const std::function<bool()> &condition, int timeoutMs = 5000)
{
    for (int waited = 0; waited <= timeoutMs; waited += 10) {
        if (condition())
            return true;
        QTest::qWait(10);
    }
    return condition();
}

} // namespace

class TestSurfaceController : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void hostModeManagesExactlyOneOutput();
    void hostModeIgnoresTheScreensAndTheSurfaceName();
    void contentFollowsAssignmentChanges();
    void settingsReachTheSink();
    void sceneEngineSettingReachesSceneContent();
    void sceneRenderScaleSettingReachesSceneContent();
    void sandboxSettingReachesSceneContent();
    void aLateAssetsAnswerStillCounts();
    void aReportedFailureIsPublishedOnce();
    void aNewKeyClearsAFailureAndALateFrameClearsOnlyNoFrame();
    void retryMakesANewDelegate();
    void externalPauseSilencesTheOutput();
    void unknownHostSinkFailsStart();
    void layerShellModeCreatesOneSinkPerScreen();
    void anX11SessionPausesWhenAWindowIsFullscreen(); // only under xcb (its own ctest entry on Xvfb)

    // Policy server
    void hostedOutputLosesItsWindow();
    void hostsGoingAwayBringTheWindowBack();
    void spanContentCarriesTheViewportOfItsScreen();
    void aRunningGamePausesEveryOutput();
    void aGamePausesOnlyTheScreenItCovers();
    void onBatteryTheWallpaperPausesOrSlowsDown();
    void anExcludedApplicationPausesAndSaysWhy();
    void thePetsAreToldToMuteForAFullscreenWindowOrAListedAppOnly();
    void theSessionFactsSayWhichOutputHasAFullscreenWindow();
    void theSessionFactsCarryTheIdleOnlyWhenTheCompositorKnowsIt();
    void pauseRulesFollowTheSettings();
    void presenterPlasmaCreatesNoWindows();
    void presenterLayerShellIgnoresHosts();
    void presenterChangeAtRuntime();
    void hostVisibilityPausesTheHostedOutput();
    void cursorReachesOnlyTheHostOfTheOutputUnderIt();

    // Host
    void hostFollowsThePolicyServer();
    void hostRefusesAnOldDaemon();
    void hostPlaysWithoutAPolicyServerAndRegistersLater();
    void hostForwardsItsVisibilityAndUnregistersOnExit();
    void hostRetriesARefusedRegistration();
    void hostHoversFromTheCursorSignals();
    void hostTakesTheInteractionPointerForItsOutput();
    void fractionsMapToThePixelsOfTheRenderSize();

private:
    // The test platform's screens have no name: give the first one a connector-like name.
    static QString screenName() { return u"TEST-1"_s; }
    QDBusMessage callSurface(QDBusConnection &client, const QString &method, const QVariantList &args = {});
    QJsonObject outputEntry(QDBusConnection &client, const QString &name);
    SurfaceOptions policyOptions()
    {
        SurfaceOptions options;
        options.sinkFactory = hostOptions({}).sinkFactory;
        options.screenName = [](QScreen *screen) {
            return screen == QGuiApplication::primaryScreen() ? screenName() : u"TEST-X%1"_s.arg(quintptr(screen));
        };
        return options;
    }

    SurfaceOptions hostOptions(const QString &output);
public slots:
    void hostStateChangedSlot(const QString &) { ++m_hostStateSignals; }
    void hostCursorSlot(const QString &output, double x, double y, double w, double h)
    {
        m_cursorSeen.append({output, QPointF(x, y), QSizeF(w, h)});
    }
    void hostCursorLeftSlot(const QString &output) { m_cursorLeft.append(output); }

private:
    int m_hostStateSignals = 0;
    struct SeenCursor {
        QString output;
        QPointF local;
        QSizeF size;
    };
    QList<SeenCursor> m_cursorSeen;
    QStringList m_cursorLeft;

    FakeDaemon m_daemon;
    QDBusConnection m_fakeBus = QDBusConnection(u"unconnected"_s);
    std::vector<std::unique_ptr<SinkState>> m_states;
    std::vector<SinkRequest> m_requests;
};

void TestSurfaceController::initTestCase()
{
    if (qEnvironmentVariableIntValue("MURASAKI_TEST_PRIVATE_BUS") != 1)
        QSKIP("needs a private session bus (run through ctest, or set MURASAKI_TEST_PRIVATE_BUS=1 under dbus-run-session)");
    qunsetenv("SWAYSOCK");
    qunsetenv("HYPRLAND_INSTANCE_SIGNATURE");
    m_fakeBus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"fakedaemon"_s);
    QVERIFY(m_fakeBus.isConnected());
    QVERIFY(m_fakeBus.registerObject(QString(kDBusPath), &m_daemon,
                                     QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties));
    QVERIFY(m_fakeBus.registerService(QString(kDBusService)));
}

void TestSurfaceController::init()
{
    m_daemon.reply = videoReply(u"we:1"_s);
    m_daemon.settings = u"{}"_s;
    m_daemon.asked.clear();
    m_daemon.apiVersion_ = kMinDBusApiVersion;
    m_states.clear();
    m_requests.clear();
}

void TestSurfaceController::cleanup()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

SurfaceOptions TestSurfaceController::hostOptions(const QString &output)
{
    SurfaceOptions options;
    options.hostOutput = output;
    options.sinkFactory = [this](const SinkRequest &request) -> std::unique_ptr<OutputSink> {
        m_requests.push_back(request);
        m_states.push_back(std::make_unique<SinkState>());
        return std::make_unique<FakeSink>(request.name, m_states.back().get());
    };
    return options;
}

void TestSurfaceController::hostModeManagesExactlyOneOutput()
{
    SurfaceController controller(hostOptions(u"DP-7"_s));
    QVERIFY(controller.isHost());
    QVERIFY(controller.start());

    QCOMPARE(m_requests.size(), size_t(1));
    QCOMPARE(m_requests[0].name, u"DP-7"_s);
    QVERIFY(!m_requests[0].screen); // no QScreen: the host output is not a monitor of this process
    QVERIFY(m_requests[0].engine);
    QVERIFY(m_requests[0].webProfile);

    SinkState &sink = *m_states[0];
    QVERIFY(waitFor([&] { return !sink.contents.empty() && !sink.playback.empty(); }));
    QCOMPARE(sink.contents.back().value(u"kind"_s).toString(), u"video"_s);
    QCOMPARE(sink.contents.back().value(u"itemId"_s).toString(), u"we:1"_s);
    QCOMPARE(sink.contents.back().value(u"fill"_s).toString(), u"fit"_s);
    QCOMPARE(sink.interactive.back(), false); // a video lets the pointer through
    QCOMPARE(sink.playback.back(), std::make_pair(false, true)); // always playing, audible: no policy server yet
    QCOMPARE(sink.fps.front(), kDefaultWebFpsLimit);
    // The daemon is asked about that output only, on the wildcard desktop.
    QVERIFY(!m_daemon.asked.isEmpty());
    for (const QString &asked : std::as_const(m_daemon.asked))
        QCOMPARE(asked, u"DP-7@*"_s);
}

void TestSurfaceController::hostModeIgnoresTheScreensAndTheSurfaceName()
{
    // Another controller owning Surface1 must not stop a host from starting, and a host must not take the name.
    SurfaceController host(hostOptions(u"HOST-1"_s));
    QVERIFY(host.start());
    QVERIFY(!QDBusConnection::sessionBus().interface()->isServiceRegistered(QString(kSurfaceDBusService)));
    QCOMPARE(m_states.size(), size_t(1)); // one sink although the platform has a screen
}

void TestSurfaceController::contentFollowsAssignmentChanges()
{
    SurfaceController controller(hostOptions(u"DP-1"_s));
    QVERIFY(controller.start());
    SinkState &sink = *m_states[0];
    QVERIFY(waitFor([&] { return !sink.contents.empty(); }));
    const QString firstKey = sink.contents.back().value(u"key"_s).toString();

    m_daemon.reply = videoReply(u"we:2"_s);
    m_fakeBus.send(QDBusMessage::createSignal(QString(kDBusPath), QString(kDBusInterface), u"AssignmentsChanged"_s));
    QVERIFY(waitFor([&] { return sink.contents.back().value(u"itemId"_s).toString() == u"we:2"_s; }));
    QVERIFY(sink.contents.back().value(u"key"_s).toString() != firstKey);

    // Muting keeps the key (the video must not reload) and silences the output.
    m_daemon.reply = videoReply(u"we:2"_s, true);
    m_fakeBus.send(QDBusMessage::createSignal(QString(kDBusPath), QString(kDBusInterface), u"AssignmentsChanged"_s));
    QVERIFY(waitFor([&] { return sink.playback.back() == std::make_pair(false, false); }));

    // Nothing assigned: black.
    m_daemon.reply = u"{}"_s;
    m_fakeBus.send(QDBusMessage::createSignal(QString(kDBusPath), QString(kDBusInterface), u"AssignmentsChanged"_s));
    QVERIFY(waitFor([&] { return sink.contents.back().value(u"kind"_s).toString() == u"none"_s; }));
}

void TestSurfaceController::settingsReachTheSink()
{
    m_daemon.settings = u"{\"webFpsLimit\":12}"_s;
    SurfaceController controller(hostOptions(u"DP-1"_s));
    QVERIFY(controller.start());
    SinkState &sink = *m_states[0];
    QVERIFY(waitFor([&] { return !sink.fps.empty() && sink.fps.back() == 12; }));
}

void TestSurfaceController::sandboxSettingReachesSceneContent()
{
    m_daemon.assets = u"/tmp/murasaki-test-assets"_s;
    m_daemon.reply = sceneReply(u"we:9"_s);
    m_daemon.settings = u"{\"sceneEngine\":\"auto\"}"_s; // a daemon without the setting: sandboxed
    SurfaceController controller(hostOptions(u"DP-1"_s));
    QVERIFY(controller.start());
    SinkState &sink = *m_states[0];
    const auto last = [&](const QString &key) { return sink.contents.empty() ? QVariant() : sink.contents.back().value(key); };
    QVERIFY(waitFor([&] { return last(u"kind"_s).toString() == u"scene"; }));
    QCOMPARE(last(u"sandboxEngines"_s).toBool(), true);
    const QString sandboxedKey = last(u"key"_s).toString();

    // Turned off while the scene plays: the same scene is pushed again with another key, so the engine restarts.
    m_daemon.settings = u"{\"sandboxEngines\":false}"_s;
    m_fakeBus.send(QDBusMessage::createSignal(QString(kDBusPath), QString(kDBusInterface), u"SettingsChanged"_s));
    QVERIFY(waitFor([&] { return last(u"sandboxEngines"_s).toBool() == false; }));
    QVERIFY(last(u"key"_s).toString() != sandboxedKey);
    QVERIFY(last(u"key"_s).toString().contains(u"|nosandbox"_s));

    m_daemon.settings = u"{\"sandboxEngines\":true}"_s;
    m_fakeBus.send(QDBusMessage::createSignal(QString(kDBusPath), QString(kDBusInterface), u"SettingsChanged"_s));
    QVERIFY(waitFor([&] { return last(u"sandboxEngines"_s).toBool(); }));
    QCOMPARE(last(u"key"_s).toString(), sandboxedKey);
    QCOMPARE(last(u"requireSandbox"_s).toBool(), false);

    // Requiring the sandbox restarts the engine too, with the requirement in the content.
    m_daemon.settings = u"{\"requireSandbox\":true}"_s;
    m_fakeBus.send(QDBusMessage::createSignal(QString(kDBusPath), QString(kDBusInterface), u"SettingsChanged"_s));
    QVERIFY(waitFor([&] { return last(u"requireSandbox"_s).toBool(); }));
    QVERIFY(last(u"key"_s).toString().contains(u"|requiresandbox"_s));
}

// The daemon was restarted with the surface and owns its name before it answers: the assets come after the controller's one-second wait.
// The late answer is taken (it used to be dropped, and every scene stayed a preview with "assets not found" until the next restart).
void TestSurfaceController::aLateAssetsAnswerStillCounts()
{
    QVERIFY(m_fakeBus.unregisterService(QString(kDBusService)));
    const auto restore = qScopeGuard([&] { QVERIFY(m_fakeBus.registerService(QString(kDBusService))); });
    QThread thread;
    SlowDaemon slow;
    slow.reply = sceneReply(u"we:9"_s);
    slow.assets = u"/tmp/murasaki-test-assets"_s;
    slow.delayMs = 1500;
    slow.moveToThread(&thread);
    thread.start();
    const auto stop = qScopeGuard([&] {
        QDBusConnection::disconnectFromBus(u"slowdaemon"_s);
        thread.quit();
        thread.wait();
    });
    // (the connection is made on that thread, so its objects are served there)
    QDBusConnection bus(u"unconnected"_s);
    QMetaObject::invokeMethod(&slow, [&] {
        bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"slowdaemon"_s);
        bus.registerObject(QString(kDBusPath), &slow, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties);
        bus.registerService(QString(kDBusService));
    }, Qt::BlockingQueuedConnection);
    QVERIFY(bus.isConnected());

    SurfaceController controller(hostOptions(u"DP-1"_s));
    QVERIFY(controller.start());
    SinkState &sink = *m_states[0];
    const auto last = [&](const QString &key) { return sink.contents.empty() ? QVariant() : sink.contents.back().value(key); };
    QVERIFY(waitFor([&] { return last(u"kind"_s).toString() == u"scene"; }, 8000));
    QCOMPARE(last(u"assetsDir"_s).toString(), u"/tmp/murasaki-test-assets"_s);
}

// A scene that could not be drawn: the sink's report reaches Outputs() as {kind, reason, engine, canTryOther}, once.
void TestSurfaceController::aReportedFailureIsPublishedOnce()
{
    m_daemon.assets = u"/tmp/murasaki-test-assets"_s;
    m_daemon.reply = sceneReply(u"we:9"_s);
    SurfaceController controller(policyOptions());
    QVERIFY(controller.start());
    const QString name = screenName();
    QVERIFY(waitFor([&] { return !m_states.empty() && !m_states[0]->contents.empty(); }));
    SinkState &sink = *m_states[0];
    const QString key = sink.contents.back().value(u"key"_s).toString();
    QDBusConnection observer = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"health-observer"_s);
    const auto cleanup = qScopeGuard([&] { QDBusConnection::disconnectFromBus(u"health-observer"_s); });
    QVERIFY(outputEntry(observer, name).value(u"failure"_s).isNull());

    sink.health({key, u"failed"_s, u"scene engine exited with code 1"_s, u"Vulkan"_s});
    const QJsonObject failure = outputEntry(observer, name).value(u"failure"_s).toObject();
    QCOMPARE(failure.value(u"kind"_s).toString(), u"crashed"_s);
    QCOMPARE(failure.value(u"reason"_s).toString(), u"scene engine exited with code 1"_s);
    QCOMPARE(failure.value(u"engine"_s).toString(), u"Vulkan"_s);
    QVERIFY(failure.contains(u"canTryOther"_s));

    // A report for content that is no longer on screen is ignored.
    sink.health({u"stale"_s, u"failed"_s, u"no first frame within 15 s"_s, QString()});
    QCOMPARE(outputEntry(observer, name).value(u"failure"_s).toObject().value(u"kind"_s).toString(), u"crashed"_s);
}

void TestSurfaceController::aNewKeyClearsAFailureAndALateFrameClearsOnlyNoFrame()
{
    m_daemon.assets = u"/tmp/murasaki-test-assets"_s;
    m_daemon.reply = sceneReply(u"we:9"_s);
    SurfaceController controller(policyOptions());
    QVERIFY(controller.start());
    const QString name = screenName();
    QVERIFY(waitFor([&] { return !m_states.empty() && !m_states[0]->contents.empty(); }));
    SinkState &sink = *m_states[0];
    const QString key = sink.contents.back().value(u"key"_s).toString();
    QDBusConnection observer = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"health-observer"_s);
    const auto cleanup = qScopeGuard([&] { QDBusConnection::disconnectFromBus(u"health-observer"_s); });
    const auto kind = [&] { return outputEntry(observer, name).value(u"failure"_s).toObject().value(u"kind"_s).toString(); };

    sink.health({key, u"failed"_s, u"no first frame within 15 s"_s, u"Vulkan"_s});
    QCOMPARE(kind(), u"noframe"_s);
    sink.health({key, u"frame"_s, QString(), QString()}); // it was only slow
    QCOMPARE(kind(), QString());

    sink.health({key, u"failed"_s, u"scene engine exited with code 1"_s, u"Vulkan"_s});
    sink.health({key, u"frame"_s, QString(), QString()}); // a crash that gave up stays
    QCOMPARE(kind(), u"crashed"_s);

    // Another item: whatever failed before is history.
    m_daemon.reply = sceneReply(u"we:10"_s);
    m_fakeBus.send(QDBusMessage::createSignal(QString(kDBusPath), QString(kDBusInterface), u"AssignmentsChanged"_s));
    QVERIFY(waitFor([&] { return sink.contents.back().value(u"itemId"_s).toString() == u"we:10"; }));
    QCOMPARE(kind(), QString());
}

// "Retry" makes the delegate again (another key), for this output and this item only.
void TestSurfaceController::retryMakesANewDelegate()
{
    m_daemon.assets = u"/tmp/murasaki-test-assets"_s;
    m_daemon.reply = sceneReply(u"we:9"_s);
    m_daemon.settings = u"{\"sceneEngine\":\"vulkan\"}"_s;
    SurfaceController controller(policyOptions());
    QVERIFY(controller.start());
    const QString name = screenName();
    QVERIFY(waitFor([&] { return !m_states.empty() && !m_states[0]->contents.empty()
                                 && m_states[0]->contents.back().value(u"sceneEngine"_s).toString() == u"vulkan"; }));
    SinkState &sink = *m_states[0];
    const auto last = [&](const QString &field) { return sink.contents.back().value(field).toString(); };
    QDBusConnection client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"health-client"_s);
    const auto cleanup = qScopeGuard([&] { QDBusConnection::disconnectFromBus(u"health-client"_s); });
    const QString first = last(u"key"_s);

    sink.health({first, u"failed"_s, u"scene engine exited with code 1"_s, u"Vulkan"_s});
    QCOMPARE(callSurface(client, u"RetryOutput"_s, {name}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(waitFor([&] { return last(u"key"_s) != first; }));
    QVERIFY(last(u"key"_s).endsWith(u"|retry:1"_s));
    QCOMPARE(last(u"sceneEngine"_s), u"vulkan"_s);
    QVERIFY(outputEntry(client, name).value(u"failure"_s).isNull()); // the new attempt starts clean


    // The choice belongs to the item: another one plays with the setting again.
    m_daemon.reply = sceneReply(u"we:10"_s);
    m_fakeBus.send(QDBusMessage::createSignal(QString(kDBusPath), QString(kDBusInterface), u"AssignmentsChanged"_s));
    QVERIFY(waitFor([&] { return last(u"itemId"_s) == u"we:10" && last(u"sceneEngine"_s) == u"vulkan"; }));
    QVERIFY(!last(u"key"_s).contains(u"|retry:"_s));
}

void TestSurfaceController::sceneRenderScaleSettingReachesSceneContent()
{
    m_daemon.assets = u"/tmp/murasaki-test-assets"_s;
    m_daemon.reply = sceneReply(u"we:9"_s);
    m_daemon.settings = u"{}"_s;
    SurfaceController controller(hostOptions(u"DP-1"_s));
    QVERIFY(controller.start());
    SinkState &sink = *m_states[0];
    const auto last = [&](const QString &key) { return sink.contents.empty() ? QVariant() : sink.contents.back().value(key); };
    QVERIFY(waitFor([&] { return last(u"kind"_s).toString() == u"scene"; }));
    QCOMPARE(last(u"sceneRenderScale"_s).toDouble(), 0.0); // automatic
    QVERIFY(!last(u"key"_s).toString().contains(u"|scale:"_s));

    // A lower resolution restarts the scene: another key, and the value in the content.
    m_daemon.settings = u"{\"sceneRenderScale\":0.5}"_s;
    m_fakeBus.send(QDBusMessage::createSignal(QString(kDBusPath), QString(kDBusInterface), u"SettingsChanged"_s));
    QVERIFY(waitFor([&] { return last(u"sceneRenderScale"_s).toDouble() == 0.5; }));
    QVERIFY(last(u"key"_s).toString().contains(u"|scale:0.5"_s));
}

void TestSurfaceController::sceneEngineSettingReachesSceneContent()
{
    m_daemon.assets = u"/tmp/murasaki-test-assets"_s;
    m_daemon.reply = sceneReply(u"we:9"_s);
    m_daemon.settings = u"{\"sceneEngine\":\"auto\"}"_s;
    SurfaceController controller(hostOptions(u"DP-1"_s));
    QVERIFY(controller.start());
    SinkState &sink = *m_states[0];
    const auto last = [&](const QString &key) { return sink.contents.empty() ? QString() : sink.contents.back().value(key).toString(); };
    QVERIFY(waitFor([&] { return last(u"kind"_s) == u"scene" && last(u"sceneEngine"_s) == u"auto"; }));
    QVERIFY(last(u"key"_s).endsWith(u"|engine:auto"_s));
    const QString autoKey = last(u"key"_s);

    // The setting changes while the scene plays: the same scene is pushed again with another key, so the surface
    // replaces its delegate and the newly chosen engine starts.
    m_daemon.settings = u"{\"sceneEngine\":\"vulkan\"}"_s;
    m_fakeBus.send(QDBusMessage::createSignal(QString(kDBusPath), QString(kDBusInterface), u"SettingsChanged"_s));
    QVERIFY(waitFor([&] { return last(u"sceneEngine"_s) == u"vulkan"; }));
    QVERIFY(last(u"key"_s).endsWith(u"|engine:vulkan"_s));
    QVERIFY(last(u"key"_s) != autoKey);
    QCOMPARE(last(u"itemId"_s), u"we:9"_s);

    // An unknown value (a newer daemon) reads as the default.
    m_daemon.settings = u"{\"sceneEngine\":\"metal\"}"_s;
    m_fakeBus.send(QDBusMessage::createSignal(QString(kDBusPath), QString(kDBusInterface), u"SettingsChanged"_s));
    QVERIFY(waitFor([&] { return last(u"sceneEngine"_s) == QString(kDefaultSceneEngine); }));

    // Other kinds keep their key whatever the engine setting is.
    m_daemon.reply = videoReply(u"we:2"_s);
    m_fakeBus.send(QDBusMessage::createSignal(QString(kDBusPath), QString(kDBusInterface), u"AssignmentsChanged"_s));
    QVERIFY(waitFor([&] { return last(u"itemId"_s) == u"we:2"; }));
    QVERIFY(!last(u"key"_s).contains(u"|engine:"_s));
}

void TestSurfaceController::externalPauseSilencesTheOutput()
{
    SurfaceController controller(hostOptions(u"DP-1"_s));
    QVERIFY(controller.start());
    SinkState &sink = *m_states[0];
    QVERIFY(waitFor([&] { return !sink.playback.empty() && sink.playback.back() == std::make_pair(false, true); }));

    controller.setExternalPause(true);
    QVERIFY(controller.externalPause());
    QCOMPARE(sink.playback.back(), std::make_pair(true, false));

    // A new assignment arriving while paused stays paused.
    m_daemon.reply = videoReply(u"we:3"_s);
    m_fakeBus.send(QDBusMessage::createSignal(QString(kDBusPath), QString(kDBusInterface), u"AssignmentsChanged"_s));
    QVERIFY(waitFor([&] { return sink.contents.back().value(u"itemId"_s).toString() == u"we:3"_s; }));
    QCOMPARE(sink.playback.back(), std::make_pair(true, false));

    controller.setExternalPause(false);
    QCOMPARE(sink.playback.back(), std::make_pair(false, true));
}

void TestSurfaceController::unknownHostSinkFailsStart()
{
    SurfaceOptions options;
    options.hostOutput = u"DP-1"_s;
    options.sinkFactory = [](const SinkRequest &) -> std::unique_ptr<OutputSink> { return nullptr; };
    SurfaceController controller(std::move(options));
    QVERIFY(!controller.start());
}

void TestSurfaceController::layerShellModeCreatesOneSinkPerScreen()
{
    SurfaceOptions options;
    options.sinkFactory = hostOptions({}).sinkFactory;
    {
        SurfaceController controller(std::move(options));
        QVERIFY(!controller.isHost());
        QVERIFY(controller.start());
        // This mode owns the Surface1 name, like the real surface.
        QVERIFY(QDBusConnection::sessionBus().interface()->isServiceRegistered(QString(kSurfaceDBusService)));
        const auto screens = QGuiApplication::screens();
        // The windows wait for the presenter setting, so they appear a moment after start().
        QVERIFY(waitFor([&] { return m_states.size() == size_t(screens.size()); }));
        for (size_t i = 0; i < m_requests.size(); ++i) {
            QVERIFY(m_requests[i].screen);
            QCOMPARE(m_requests[i].name, m_requests[i].screen->name());
        }
        SinkState &sink = *m_states[0];
        QVERIFY(waitFor([&] { return !sink.contents.empty(); }));
        QVERIFY(!m_daemon.asked.isEmpty());
        QVERIFY(m_daemon.asked.front().startsWith(m_requests[0].name + u'@'));
    }
    QVERIFY(m_states[0]->destroyed); // the controller owns and destroys its sinks
}

// On an X11 session the controller follows the window manager's EWMH state by itself (X11Monitor): a fullscreen window
// pauses the output it covers, and closing it resumes. Runs only on the xcb platform, against a fake window manager.
void TestSurfaceController::anX11SessionPausesWhenAWindowIsFullscreen()
{
#ifdef MURASAKI_X11_TESTS
    if (!QGuiApplication::platformName().startsWith(u"xcb"_s))
        QSKIP("needs the xcb platform (its own ctest entry runs it on Xvfb)");
    FakeWm wm;
    QVERIFY(wm.ok());
    wm.desktops(1, 0, {});
    SurfaceOptions options;
    options.sinkFactory = hostOptions({}).sinkFactory;
    SurfaceController controller(std::move(options));
    QVERIFY(controller.start());
    QVERIFY(waitFor([&] { return !m_states.empty() && !m_states[0]->playback.empty(); }));
    SinkState &sink = *m_states[0];
    const auto paused = [&] { return !sink.playback.empty() && sink.playback.back().first; };
    QTest::qWait(300);
    QVERIFY(!paused());
    const xcb_window_t game = wm.addWindow(u"steam_app_620"_s, QRect(0, 0, 1280, 1024), {u"_NET_WM_STATE_FULLSCREEN"_s});
    QVERIFY(waitFor([&] { return paused(); }));
    wm.removeWindow(game);
    QVERIFY(waitFor([&] { return !paused(); }));
#else
    QSKIP("built without the X11 presenter");
#endif
}

QDBusMessage TestSurfaceController::callSurface(QDBusConnection &client, const QString &method, const QVariantList &args)
{
    QDBusMessage message = QDBusMessage::createMethodCall(QString(kSurfaceDBusService), QString(kSurfaceDBusPath),
                                                          QString(kSurfaceDBusInterface), method);
    message.setArguments(args);
    QDBusPendingCall pending = client.asyncCall(message);
    if (!QTest::qWaitFor([&] { return pending.isFinished(); }, 5000))
        qWarning("D-Bus call timed out");
    return pending.reply();
}

QJsonObject TestSurfaceController::outputEntry(QDBusConnection &client, const QString &name)
{
    const QDBusMessage reply = callSurface(client, u"Outputs"_s);
    const QJsonArray outputs = QJsonDocument::fromJson(reply.arguments().value(0).toString().toUtf8()).array();
    for (const QJsonValue &value : outputs) {
        if (value.toObject().value(u"name"_s).toString() == name)
            return value.toObject();
    }
    return {};
}

void TestSurfaceController::hostedOutputLosesItsWindow()
{
    SurfaceController controller(policyOptions());
    QVERIFY(controller.start());
    const QString name = screenName();
    QVERIFY(waitFor([&] { return !m_states.empty() && !m_states[0]->contents.empty() && !m_states[0]->playback.empty(); }));
    QVERIFY(!m_states[0]->destroyed);

    QDBusConnection host = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"policy-host-1"_s);
    const auto cleanup = qScopeGuard([&] {
        host = QDBusConnection(u"unconnected"_s);
        QDBusConnection::disconnectFromBus(u"policy-host-1"_s);
    });
    QVERIFY(host.isConnected());
    QCOMPARE(outputEntry(host, name).value(u"hosted"_s).toBool(true), false);
    QCOMPARE(callSurface(host, u"RegisterHost"_s, {name}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(waitFor([&] { return m_states[0]->destroyed; })); // no layer-shell window for a hosted output
    const QJsonObject entry = outputEntry(host, name);
    QCOMPARE(entry.value(u"hosted"_s).toBool(), true);
    QCOMPARE(entry.value(u"itemId"_s).toString(), u"we:1"_s); // still resolved: the policy keeps running
    QVERIFY(entry.contains(u"paused"_s) && entry.contains(u"audible"_s));

    // Unregistering brings the window back, with the content already resolved.
    QCOMPARE(callSurface(host, u"UnregisterHost"_s, {name}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(waitFor([&] { return m_states.size() == 2 && !m_states[1]->contents.empty() && !m_states[1]->playback.empty(); }));
    QCOMPARE(m_states[1]->contents.back().value(u"itemId"_s).toString(), u"we:1"_s);
    QCOMPARE(outputEntry(host, name).value(u"hosted"_s).toBool(true), false);
}

void TestSurfaceController::hostsGoingAwayBringTheWindowBack()
{
    SurfaceController controller(policyOptions());
    QVERIFY(controller.start());
    const QString name = screenName();
    QVERIFY(waitFor([&] { return !m_states.empty() && !m_states[0]->contents.empty(); }));

    QDBusConnection host = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"policy-host-2"_s);
    QDBusConnection observer = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"policy-observer"_s);
    const auto cleanup = qScopeGuard([&] { QDBusConnection::disconnectFromBus(u"policy-observer"_s); });
    QCOMPARE(callSurface(host, u"RegisterHost"_s, {name}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(waitFor([&] { return m_states[0]->destroyed; }));

    // The host process dies without unregistering.
    host = QDBusConnection(u"unconnected"_s);
    QDBusConnection::disconnectFromBus(u"policy-host-2"_s);
    QVERIFY(waitFor([&] { return m_states.size() == 2; }));
    QCOMPARE(outputEntry(observer, name).value(u"hosted"_s).toBool(true), false);
}

// The viewport of a spanned wallpaper is the output's own rectangle of the canvas of all outputs: with the test
// platform's screens laid out as they are, it is that screen's geometry moved to the canvas corner. Plain
// assignments carry none.
void TestSurfaceController::spanContentCarriesTheViewportOfItsScreen()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    QRectF bounds;
    for (QScreen *other : QGuiApplication::screens())
        bounds = bounds.united(QRectF(other->geometry()));
    const QRectF own = QRectF(screen->geometry()).translated(-bounds.topLeft());

    m_daemon.reply = spanReply(u"we:1"_s);
    SurfaceController controller(policyOptions());
    QVERIFY(controller.start());
    QVERIFY(waitFor([&] { return !m_states.empty() && !m_states[0]->contents.empty(); }));
    SinkState *state = nullptr;
    for (auto &candidate : m_states) {
        if (candidate->name == screenName())
            state = candidate.get();
    }
    QVERIFY(state);
    QVERIFY(waitFor([&] { return !state->contents.empty(); }));
    const QVariantMap span = state->contents.back().value(u"span"_s).toMap();
    QCOMPARE(span.value(u"canvasW"_s).toDouble(), bounds.width());
    QCOMPARE(span.value(u"canvasH"_s).toDouble(), bounds.height());
    QCOMPARE(span.value(u"x"_s).toDouble(), own.x());
    QCOMPARE(span.value(u"y"_s).toDouble(), own.y());
    QCOMPARE(span.value(u"w"_s).toDouble(), own.width());
    QCOMPARE(span.value(u"h"_s).toDouble(), own.height());

    // The same screen shows no span for a plain assignment.
    m_daemon.reply = videoReply(u"we:1"_s);
    m_fakeBus.send(QDBusMessage::createSignal(QString(kDBusPath), QString(kDBusInterface), u"AssignmentsChanged"_s));
    QVERIFY(waitFor([&] { return state->contents.back().value(u"span"_s).toMap().isEmpty() && state->contents.size() > 1; }));
}

namespace {

// The state of the screen's sink: its last playback decision, and what it was last capped to.
struct Watch {
    SinkState *state = nullptr;
    bool paused() const { return !state->playback.empty() && state->playback.back().first; }
    bool audible() const { return !state->playback.empty() && state->playback.back().second; }
    int cap() const { return state->caps.empty() ? 0 : state->caps.back(); }
};

} // namespace

void TestSurfaceController::aRunningGamePausesEveryOutput()
{
    FakeGameMode gameMode;
    QVERIFY(m_fakeBus.registerObject(u"/com/feralinteractive/GameMode"_s, &gameMode, QDBusConnection::ExportAllProperties));
    const auto cleanup = qScopeGuard([&] {
        m_fakeBus.unregisterService(u"com.feralinteractive.GameMode"_s);
        m_fakeBus.unregisterObject(u"/com/feralinteractive/GameMode"_s);
    });
    QVERIFY(m_fakeBus.registerService(u"com.feralinteractive.GameMode"_s));

    SurfaceController controller(policyOptions());
    QVERIFY(controller.start());
    QVERIFY(waitFor([&] { return !m_states.empty() && !m_states[0]->playback.empty(); }));
    const Watch screen{m_states[0].get()};
    QVERIFY(waitFor([&] { return screen.audible(); }));
    QVERIFY(!screen.paused());

    gameMode.clientCount = 1;
    m_fakeBus.send(QDBusMessage::createSignal(u"/com/feralinteractive/GameMode"_s, u"com.feralinteractive.GameMode"_s, u"GameRegistered"_s)
                   << 4242 << QVariant::fromValue(QDBusObjectPath(u"/com/feralinteractive/GameMode/Game1"_s)));
    QVERIFY(waitFor([&] { return screen.paused() && !screen.audible(); }));
    // the reason is listed for the Screens page
    QDBusConnection client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"pause-client"_s);
    const auto close = qScopeGuard([] { QDBusConnection::disconnectFromBus(u"pause-client"_s); });
    QCOMPARE(outputEntry(client, screenName()).value(u"pauseReason"_s).toString(), u"game"_s);

    gameMode.clientCount = 0;
    m_fakeBus.send(QDBusMessage::createSignal(u"/com/feralinteractive/GameMode"_s, u"com.feralinteractive.GameMode"_s, u"GameUnregistered"_s)
                   << 4242 << QVariant::fromValue(QDBusObjectPath(u"/com/feralinteractive/GameMode/Game1"_s)));
    QVERIFY(waitFor([&] { return !screen.paused() && screen.audible(); }));
    QCOMPARE(outputEntry(client, screenName()).value(u"pauseReason"_s).toString(), QString());
}

// A compositor that says which outputs are covered: the game pauses the screen it is on (covered), and a wallpaper that shows
// goes on (a clock on the other screen keeps the time).
void TestSurfaceController::aGamePausesOnlyTheScreenItCovers()
{
    FakeGameMode gameMode;
    gameMode.clientCount = 1;
    QVERIFY(m_fakeBus.registerObject(u"/com/feralinteractive/GameMode"_s, &gameMode, QDBusConnection::ExportAllProperties));
    const auto cleanup = qScopeGuard([&] {
        m_fakeBus.unregisterService(u"com.feralinteractive.GameMode"_s);
        m_fakeBus.unregisterObject(u"/com/feralinteractive/GameMode"_s);
    });
    QVERIFY(m_fakeBus.registerService(u"com.feralinteractive.GameMode"_s));
    m_daemon.settings = u"{\"pauseWhenCovered\":false}"_s; // (so that only the game rule can pause a covered screen)
    auto monitor = std::make_unique<FakeMonitor>();
    FakeMonitor *fake = monitor.get();
    fake->occlusion = true;
    SurfaceOptions options = policyOptions();
    options.monitorFactory = [&monitor] { return std::move(monitor); };
    SurfaceController controller(std::move(options));
    QVERIFY(controller.start());
    QVERIFY(waitFor([&] { return !m_states.empty() && !m_states[0]->playback.empty(); }));
    const Watch screen{m_states[0].get()};
    QDBusConnection client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"pause-client-covered"_s);
    const auto close = qScopeGuard([] { QDBusConnection::disconnectFromBus(u"pause-client-covered"_s); });

    SessionState state;
    state.outputs.insert(screenName(), OutputSession(u"*"_s, false));
    fake->set(state);
    QTest::qWait(300);
    QVERIFY(!screen.paused());
    QCOMPARE(outputEntry(client, screenName()).value(u"pauseReason"_s).toString(), QString());

    state.outputs.insert(screenName(), OutputSession(u"*"_s, true)); // the game's window covers this screen
    fake->set(state);
    QVERIFY(waitFor([&] { return screen.paused() && !screen.audible(); }));
    QCOMPARE(outputEntry(client, screenName()).value(u"pauseReason"_s).toString(), u"game"_s);
}

void TestSurfaceController::onBatteryTheWallpaperPausesOrSlowsDown()
{
    FakeUPower power;
    power.onBattery = true;
    QVERIFY(m_fakeBus.registerObject(u"/org/freedesktop/UPower"_s, &power, QDBusConnection::ExportAllProperties));
    const auto cleanup = qScopeGuard([&] {
        m_fakeBus.unregisterService(u"org.freedesktop.UPower"_s);
        m_fakeBus.unregisterObject(u"/org/freedesktop/UPower"_s);
    });
    QVERIFY(m_fakeBus.registerService(u"org.freedesktop.UPower"_s));

    // off by default: on battery changes nothing
    m_daemon.settings = u"{}"_s;
    SurfaceOptions options = policyOptions();
    options.powerBus = QDBusConnection::sessionBus(); // this test's "system bus"
    SurfaceController controller(std::move(options));
    QVERIFY(controller.start());
    QVERIFY(waitFor([&] { return !m_states.empty() && !m_states[0]->playback.empty(); }));
    const Watch screen{m_states[0].get()};
    QTest::qWait(300);
    QVERIFY(!screen.paused());
    QCOMPARE(screen.cap(), 0);

    const auto setMode = [&](const QString &mode) {
        m_daemon.settings = u"{\"batteryMode\":\"%1\"}"_s.arg(mode);
        m_fakeBus.send(QDBusMessage::createSignal(QString(kDBusPath), QString(kDBusInterface), u"SettingsChanged"_s));
    };
    setMode(u"lowfps"_s);
    QVERIFY(waitFor([&] { return screen.cap() == 15; }));
    QVERIFY(!screen.paused());
    setMode(u"pause"_s);
    QVERIFY(waitFor([&] { return screen.paused(); }));
    QCOMPARE(screen.cap(), 0);

    // plugged in again
    power.onBattery = false;
    m_fakeBus.send(QDBusMessage::createSignal(u"/org/freedesktop/UPower"_s, u"org.freedesktop.DBus.Properties"_s, u"PropertiesChanged"_s)
                   << u"org.freedesktop.UPower"_s << QVariantMap{{u"OnBattery"_s, false}} << QStringList());
    QVERIFY(waitFor([&] { return !screen.paused(); }));
    setMode(u"lowfps"_s);
    QTest::qWait(200);
    QCOMPARE(screen.cap(), 0);
}

void TestSurfaceController::anExcludedApplicationPausesAndSaysWhy()
{
    m_daemon.settings = u"{\"pauseApps\":[\"obs\",\"steam_app_*\"]}"_s;
    auto monitor = std::make_unique<FakeMonitor>();
    FakeMonitor *fake = monitor.get();
    SurfaceOptions options = policyOptions();
    options.monitorFactory = [&monitor] { return std::move(monitor); };
    SurfaceController controller(std::move(options));
    QVERIFY(controller.start());
    QVERIFY(waitFor([&] { return !m_states.empty() && !m_states[0]->playback.empty(); }));
    const Watch screen{m_states[0].get()};
    QDBusConnection client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"pause-client-2"_s);
    const auto close = qScopeGuard([] { QDBusConnection::disconnectFromBus(u"pause-client-2"_s); });

    SessionState state;
    state.activeApp = u"konsole"_s;
    fake->set(state);
    QTest::qWait(200);
    QVERIFY(!screen.paused());

    state.activeApp = u"OBS"_s; // focus
    fake->set(state);
    QVERIFY(waitFor([&] { return screen.paused(); }));
    QJsonObject entry = outputEntry(client, screenName());
    QCOMPARE(entry.value(u"pauseReason"_s).toString(), u"app"_s);
    QCOMPARE(entry.value(u"pauseDetail"_s).toString(), u"obs"_s);

    state.activeApp = u"konsole"_s;
    state.fullscreenApps = {u"steam_app_620"_s}; // fullscreen elsewhere
    fake->set(state);
    QTest::qWait(100);
    QVERIFY(screen.paused());
    QCOMPARE(outputEntry(client, screenName()).value(u"pauseDetail"_s).toString(), u"steam_app_*"_s);

    state.fullscreenApps.clear();
    fake->set(state);
    QVERIFY(waitFor([&] { return !screen.paused(); }));
}

void TestSurfaceController::thePetsAreToldToMuteForAFullscreenWindowOrAListedAppOnly()
{
    m_daemon.settings = u"{\"pauseApps\":[\"obs\"]}"_s;
    auto monitor = std::make_unique<FakeMonitor>();
    FakeMonitor *fake = monitor.get();
    SurfaceOptions options = policyOptions();
    options.monitorFactory = [&monitor] { return std::move(monitor); };
    SurfaceController controller(std::move(options));
    QVERIFY(controller.start());
    QVERIFY(waitFor([&] { return !m_states.empty() && !m_states[0]->playback.empty(); }));
    QDBusConnection client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"pause-client-pets"_s);
    const auto close = qScopeGuard([] { QDBusConnection::disconnectFromBus(u"pause-client-pets"_s); });
    const auto muted = [&] {
        const QDBusMessage reply = callSurface(client, u"PetAudioMuted"_s);
        return reply.type() == QDBusMessage::ReplyMessage && reply.arguments().value(0).toBool();
    };
    QVERIFY(!muted());
    SessionState state;
    state.activeApp = u"konsole"_s;
    fake->set(state);
    QTest::qWait(150);
    QVERIFY(!muted());
    // a window covering an output (maximized) is not a reason: pets are above windows
    // a fullscreen window is, whatever the user's rules say
    state.fullscreenApps = {u"mpv"_s};
    fake->set(state);
    QTRY_VERIFY(muted());
    state.fullscreenApps.clear();
    fake->set(state);
    QTRY_VERIFY(!muted());
    // a listed application with the focus is
    state.activeApp = u"obs"_s;
    fake->set(state);
    QTRY_VERIFY(muted());
    state.activeApp = u"konsole"_s;
    fake->set(state);
    QTRY_VERIFY(!muted());
}

void TestSurfaceController::theSessionFactsSayWhichOutputHasAFullscreenWindow()
{
    m_daemon.settings = u"{\"pauseApps\":[\"obs\"]}"_s;
    auto monitor = std::make_unique<FakeMonitor>();
    FakeMonitor *fake = monitor.get();
    SurfaceOptions options = policyOptions();
    options.monitorFactory = [&monitor] { return std::move(monitor); };
    SurfaceController controller(std::move(options));
    QVERIFY(controller.start());
    QVERIFY(waitFor([&] { return !m_states.empty() && !m_states[0]->playback.empty(); }));
    QDBusConnection client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"facts-client"_s);
    const auto close = qScopeGuard([] { QDBusConnection::disconnectFromBus(u"facts-client"_s); });
    // Murasaki's own overlay (the owner of its bus name) reads the whole facts; the plain client only where a window is fullscreen
    QDBusConnection overlay = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"facts-overlay"_s);
    const auto closeOverlay = qScopeGuard([] { QDBusConnection::disconnectFromBus(u"facts-overlay"_s); });
    QVERIFY(overlay.registerService(QString(kOverlayDBusService)));
    const auto facts = [&] {
        const QDBusMessage reply = callSurface(overlay, u"SessionFacts"_s);
        return QJsonDocument::fromJson(reply.arguments().value(0).toString().toUtf8()).object();
    };
    const auto perOutput = [&] {
        const QDBusMessage reply = callSurface(overlay, u"OutputFullscreen"_s);
        return QJsonDocument::fromJson(reply.arguments().value(0).toString().toUtf8()).object();
    };
    QVERIFY(waitFor([&] { return facts().contains(u"outputs"_s); }));
    QVERIFY(!facts().value(u"anyFullscreen"_s).toBool());
    // a fullscreen window on one output only
    SessionState state;
    state.outputs[u"DP-1"_s].fullscreen = true;
    state.outputs[u"DP-1"_s].fullscreenApp = u"mpv"_s;
    state.outputs[u"DP-1"_s].occluded = true;
    state.openApps = {u"mpv"_s, u"konsole"_s};
    state.fullscreenApps = {u"mpv"_s};
    state.activeApp = u"mpv"_s;
    fake->set(state);
    QVERIFY(waitFor([&] { return facts().value(u"anyFullscreen"_s).toBool(); }));
    const QJsonObject json = facts();
    QCOMPARE(json.value(u"outputs"_s).toObject().value(u"DP-1"_s).toObject().value(u"app"_s).toString(), u"mpv"_s);
    QCOMPARE(json.value(u"activeApp"_s).toString(), u"mpv"_s);
    QCOMPARE(json.value(u"openApps"_s).toArray().size(), 2);
    QVERIFY(json.value(u"known"_s).isBool());
    QVERIFY(!json.value(u"quiet"_s).toBool());
    QCOMPARE(perOutput().value(u"DP-1"_s).toObject().value(u"fullscreen"_s).toBool(), true);
    // a listed application is a different fact: `quiet`, with no fullscreen window
    state = SessionState();
    state.activeApp = u"obs"_s;
    fake->set(state);
    QVERIFY(waitFor([&] { return facts().value(u"quiet"_s).toBool(); }));
    QVERIFY(!facts().value(u"anyFullscreen"_s).toBool());
    QVERIFY(!perOutput().value(u"DP-1"_s).toObject().value(u"fullscreen"_s).toBool());
    // the plain client sees no application at all
    state = SessionState();
    state.outputs[u"DP-1"_s].fullscreen = true;
    state.outputs[u"DP-1"_s].fullscreenApp = u"secretgame"_s;
    state.openApps = {u"secretgame"_s};
    state.activeApp = u"secretgame"_s;
    fake->set(state);
    QVERIFY(waitFor([&] { return facts().value(u"anyFullscreen"_s).toBool(); }));
    const QString plain = callSurface(client, u"SessionFacts"_s).arguments().value(0).toString();
    QVERIFY(plain.contains(u"\"fullscreen\":true"_s));
    QVERIFY(!plain.contains(u"secretgame"_s));
    QVERIFY(!callSurface(client, u"OutputFullscreen"_s).arguments().value(0).toString().contains(u"secretgame"_s));
}

void TestSurfaceController::theSessionFactsCarryTheIdleOnlyWhenTheCompositorKnowsIt()
{
    auto monitor = std::make_unique<FakeMonitor>();
    auto idleSource = std::make_unique<FakeIdle>();
    FakeIdle *idle = idleSource.get();
    SurfaceOptions options = policyOptions();
    options.monitorFactory = [&monitor] { return std::move(monitor); };
    options.idleFactory = [&idleSource] { return std::move(idleSource); };
    SurfaceController controller(std::move(options));
    QVERIFY(controller.start());
    QVERIFY(waitFor([&] { return !m_states.empty() && !m_states[0]->playback.empty(); }));
    QDBusConnection overlay = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"facts-idle-overlay"_s);
    const auto closeOverlay = qScopeGuard([] { QDBusConnection::disconnectFromBus(u"facts-idle-overlay"_s); });
    QVERIFY(overlay.registerService(QString(kOverlayDBusService)));
    const auto facts = [&] {
        const QDBusMessage reply = callSurface(overlay, u"SessionFacts"_s);
        return QJsonDocument::fromJson(reply.arguments().value(0).toString().toUtf8()).object();
    };
    QVERIFY(waitFor([&] { return facts().contains(u"outputs"_s); }));
    // the compositor has not said yet: the fact is absent, not "false"
    QVERIFY(!facts().contains(u"idleKnown"_s) && !facts().contains(u"idle"_s));
    idle->setIdle(true, false);
    QVERIFY(waitFor([&] { return facts().value(u"idleKnown"_s).toBool(); }));
    QVERIFY(!facts().value(u"idle"_s).toBool(true));
    QCOMPARE(facts().value(u"idleAfterMs"_s).toInt(), 45'000);
    idle->setIdle(true, true);
    QVERIFY(waitFor([&] { return facts().value(u"idle"_s).toBool(); }));
    // it is not for everyone: a plain client of the bus does not learn when the user is away
    QDBusConnection client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"facts-idle-client"_s);
    const auto close = qScopeGuard([] { QDBusConnection::disconnectFromBus(u"facts-idle-client"_s); });
    QVERIFY(!callSurface(client, u"SessionFacts"_s).arguments().value(0).toString().contains(u"idle"_s));
    // and unknown again (the connection to the compositor broke): the fact goes away
    idle->setIdle(false, false);
    QVERIFY(waitFor([&] { return !facts().contains(u"idleKnown"_s); }));
}

void TestSurfaceController::pauseRulesFollowTheSettings()
{
    auto monitor = std::make_unique<FakeMonitor>();
    FakeMonitor *fake = monitor.get();
    SurfaceOptions options = policyOptions();
    options.monitorFactory = [&monitor] { return std::move(monitor); };
    SurfaceController controller(std::move(options));
    QVERIFY(controller.start());
    QVERIFY(waitFor([&] { return !m_states.empty() && !m_states[0]->playback.empty(); }));
    const Watch screen{m_states[0].get()};
    SessionState state;
    state.activeApp = u"obs"_s;
    fake->set(state);
    QTest::qWait(200);
    QVERIFY(!screen.paused()); // no rule yet

    m_daemon.settings = u"{\"pauseApps\":[\"obs\"]}"_s;
    m_fakeBus.send(QDBusMessage::createSignal(QString(kDBusPath), QString(kDBusInterface), u"SettingsChanged"_s));
    QVERIFY(waitFor([&] { return screen.paused(); })); // the rule arrives with the settings, the state was already there
    m_daemon.settings = u"{}"_s;
    m_fakeBus.send(QDBusMessage::createSignal(QString(kDBusPath), QString(kDBusInterface), u"SettingsChanged"_s));
    QVERIFY(waitFor([&] { return !screen.paused(); }));
}

void TestSurfaceController::presenterPlasmaCreatesNoWindows()
{
    m_daemon.settings = u"{\"presenter\":\"plasma\"}"_s;
    SurfaceController controller(policyOptions());
    QVERIFY(controller.start());
    const QString name = screenName();
    QDBusConnection client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"policy-client-3"_s);
    const auto cleanup = qScopeGuard([&] {
        client = QDBusConnection(u"unconnected"_s);
        QDBusConnection::disconnectFromBus(u"policy-client-3"_s);
    });
    // The policy still resolves and decides for every output ...
    QVERIFY(waitFor([&] { return outputEntry(client, name).value(u"itemId"_s).toString() == u"we:1"_s; }));
    const QJsonObject entry = outputEntry(client, name);
    QCOMPARE(entry.value(u"paused"_s).toBool(true), false);
    QCOMPARE(entry.value(u"audible"_s).toBool(false), true);
    // ... but no window was ever made.
    QTest::qWait(200);
    QVERIFY(m_states.empty());
    QVERIFY(m_requests.empty());
}

void TestSurfaceController::presenterLayerShellIgnoresHosts()
{
    m_daemon.settings = u"{\"presenter\":\"layer-shell\"}"_s;
    SurfaceController controller(policyOptions());
    QVERIFY(controller.start());
    const QString name = screenName();
    QVERIFY(waitFor([&] { return !m_states.empty() && !m_states[0]->contents.empty(); }));
    QDBusConnection host = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"policy-host-4"_s);
    const auto cleanup = qScopeGuard([&] {
        host = QDBusConnection(u"unconnected"_s);
        QDBusConnection::disconnectFromBus(u"policy-host-4"_s);
    });
    QCOMPARE(callSurface(host, u"RegisterHost"_s, {name}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(waitFor([&] { return outputEntry(host, name).value(u"hosted"_s).toBool(); }));
    QTest::qWait(100);
    QVERIFY(!m_states[0]->destroyed); // the window stays
    QCOMPARE(m_states.size(), size_t(1));
}

void TestSurfaceController::presenterChangeAtRuntime()
{
    SurfaceController controller(policyOptions());
    QVERIFY(controller.start());
    QVERIFY(waitFor([&] { return !m_states.empty() && !m_states[0]->contents.empty(); }));

    m_daemon.settings = u"{\"presenter\":\"plasma\"}"_s;
    m_fakeBus.send(QDBusMessage::createSignal(QString(kDBusPath), QString(kDBusInterface), u"SettingsChanged"_s));
    QVERIFY(waitFor([&] { return m_states[0]->destroyed; }));

    m_daemon.settings = u"{\"presenter\":\"auto\"}"_s;
    m_fakeBus.send(QDBusMessage::createSignal(QString(kDBusPath), QString(kDBusInterface), u"SettingsChanged"_s));
    QVERIFY(waitFor([&] { return m_states.size() == 2 && !m_states[1]->contents.empty(); }));
}

void TestSurfaceController::hostVisibilityPausesTheHostedOutput()
{
    m_daemon.settings = u"{\"presenter\":\"plasma\"}"_s;
    SurfaceController controller(policyOptions());
    QVERIFY(controller.start());
    const QString name = screenName();
    QDBusConnection host = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"policy-host-5"_s);
    const auto cleanup = qScopeGuard([&] {
        host = QDBusConnection(u"unconnected"_s);
        QDBusConnection::disconnectFromBus(u"policy-host-5"_s);
    });
    QVERIFY(waitFor([&] { return outputEntry(host, name).value(u"itemId"_s).toString() == u"we:1"_s; }));
    QCOMPARE(callSurface(host, u"RegisterHost"_s, {name}).type(), QDBusMessage::ReplyMessage);

    const auto hostState = [&] {
        const QDBusMessage reply = callSurface(host, u"HostState"_s, {name});
        return QJsonDocument::fromJson(reply.arguments().value(0).toString().toUtf8()).object();
    };
    QVERIFY(waitFor([&] { return hostState().value(u"audible"_s).toBool(); }));
    const QJsonObject playing = hostState();
    QCOMPARE(playing.value(u"paused"_s).toBool(true), false);
    QCOMPARE(playing.value(u"desktop"_s).toString(), u"*"_s);
    QCOMPARE(playing.value(u"frameRateLimit"_s).toInt(), kDefaultWebFpsLimit);

    QVERIFY(host.connect(QString(kSurfaceDBusService), QString(kSurfaceDBusPath), QString(kSurfaceDBusInterface),
                         u"HostStateChanged"_s, this, SLOT(hostStateChangedSlot(QString))));
    m_hostStateSignals = 0;
    QCOMPARE(callSurface(host, u"SetHostVisible"_s, {name, false}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(waitFor([&] { return hostState().value(u"paused"_s).toBool(); }));
    QCOMPARE(hostState().value(u"audible"_s).toBool(true), false);
    QVERIFY(waitFor([&] { return m_hostStateSignals >= 1; }));
    const QJsonObject entry = outputEntry(host, name);
    QCOMPARE(entry.value(u"paused"_s).toBool(false), true);
    QCOMPARE(entry.value(u"playing"_s).toBool(true), false);

    QCOMPARE(callSurface(host, u"SetHostVisible"_s, {name, true}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(waitFor([&] { return !hostState().value(u"paused"_s).toBool(); }));
    QCOMPARE(hostState().value(u"audible"_s).toBool(false), true);
}

void TestSurfaceController::cursorReachesOnlyTheHostOfTheOutputUnderIt()
{
    m_daemon.settings = u"{\"presenter\":\"plasma\"}"_s;
    SurfaceController controller(policyOptions());
    QVERIFY(controller.start());
    const QString name = screenName();
    const QRect geometry = QGuiApplication::primaryScreen()->geometry();
    QDBusConnection host = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"policy-host-6"_s);
    QDBusConnection bystander = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"policy-host-6b"_s);
    // The sender check of ReportKWinCursor: this connection plays KWin.
    QVERIFY(m_fakeBus.registerService(u"org.kde.KWin"_s));
    const auto cleanup = qScopeGuard([&] {
        m_fakeBus.unregisterService(u"org.kde.KWin"_s);
        host = QDBusConnection(u"unconnected"_s);
        bystander = QDBusConnection(u"unconnected"_s);
        QDBusConnection::disconnectFromBus(u"policy-host-6"_s);
        QDBusConnection::disconnectFromBus(u"policy-host-6b"_s);
    });
    m_cursorSeen.clear();
    m_cursorLeft.clear();
    const auto report = [&](int x, int y) {
        QDBusMessage message = QDBusMessage::createMethodCall(QString(kSurfaceDBusService), QString(kSurfaceDBusPath),
                                                              QString(kSurfaceDBusInterface), u"ReportKWinCursor"_s);
        message.setArguments({x, y});
        QDBusPendingCall pending = m_fakeBus.asyncCall(message);
        if (!QTest::qWaitFor([&] { return pending.isFinished(); }, 5000))
            qWarning("ReportKWinCursor timed out");
    };
    QVERIFY(waitFor([&] { return outputEntry(host, name).value(u"itemId"_s).toString() == u"we:1"_s; }));
    QVERIFY(host.connect(QString(kSurfaceDBusService), QString(kSurfaceDBusPath), QString(kSurfaceDBusInterface),
                         u"HostCursor"_s, this, SLOT(hostCursorSlot(QString,double,double,double,double))));
    QVERIFY(host.connect(QString(kSurfaceDBusService), QString(kSurfaceDBusPath), QString(kSurfaceDBusInterface),
                         u"HostCursorLeft"_s, this, SLOT(hostCursorLeftSlot(QString))));

    // Nobody hosts: a report goes nowhere.
    report(10, 10);
    QTest::qWait(150);
    QVERIFY(m_cursorSeen.isEmpty());

    QCOMPARE(callSurface(host, u"RegisterHost"_s, {name}).type(), QDBusMessage::ReplyMessage);
    // A resting cursor is not waited for: the host that arrives gets the last position of the report before it.
    // (There was none while nobody was hosting, so nothing yet.)
    report(geometry.x() + 100, geometry.y() + 50);
    QVERIFY(waitFor([&] { return m_cursorSeen.size() == 1; }));
    QCOMPARE(m_cursorSeen[0].output, name);
    QCOMPARE(m_cursorSeen[0].local, QPointF(100, 50));
    QCOMPARE(m_cursorSeen[0].size, QSizeF(geometry.size()));

    // A burst is throttled: the first goes out at once, the rest collapses into the latest.
    report(geometry.x() + 200, geometry.y() + 60);
    report(geometry.x() + 210, geometry.y() + 61);
    report(geometry.x() + 220, geometry.y() + 62);
    QVERIFY(waitFor([&] { return m_cursorSeen.last().local == QPointF(220, 62); }));
    QVERIFY(m_cursorSeen.size() <= 3); // never one message per report

    // Off every output: the host is told, and nothing else.
    const qsizetype before = m_cursorSeen.size();
    report(geometry.right() + 5000, geometry.y());
    QVERIFY(waitFor([&] { return m_cursorLeft == QStringList{name}; }));
    QCOMPARE(m_cursorSeen.size(), before);

    // A hidden host gets nothing.
    QCOMPARE(callSurface(host, u"SetHostVisible"_s, {name, false}).type(), QDBusMessage::ReplyMessage);
    QTest::qWait(60);
    report(geometry.x() + 5, geometry.y() + 5);
    QTest::qWait(150);
    QCOMPARE(m_cursorSeen.size(), before);
}

void TestSurfaceController::hostRefusesAnOldDaemon()
{
    m_daemon.apiVersion_ = kMinDBusApiVersion - 1;
    SurfaceController controller(hostOptions(u"DP-1"_s));
    QSignalSpy spy(&controller, &SurfaceController::incompatibleDaemon);
    QTest::ignoreMessage(QtCriticalMsg, QRegularExpression(u"murasakid speaks API revision"_s));
    QVERIFY(controller.start());
    QTRY_COMPARE(spy.size(), 1);
    QCOMPARE(spy.at(0).at(0).toUInt(), kMinDBusApiVersion - 1);
}

void TestSurfaceController::hostFollowsThePolicyServer()
{
    FakePolicyServer policy;
    policy.state = u"{\"desktop\":\"desk-2\",\"paused\":true,\"audible\":false,\"frameRateLimit\":15}"_s;
    QVERIFY(m_fakeBus.registerObject(QString(kSurfaceDBusPath), &policy,
                                     QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
    QVERIFY(m_fakeBus.registerService(QString(kSurfaceDBusService)));
    const auto cleanup = qScopeGuard([&] {
        m_fakeBus.unregisterService(QString(kSurfaceDBusService));
        m_fakeBus.unregisterObject(QString(kSurfaceDBusPath));
    });

    SurfaceController controller(hostOptions(u"DP-1"_s));
    QVERIFY(controller.start());
    SinkState &sink = *m_states[0];
    QVERIFY(waitFor([&] { return policy.registered == QStringList{u"DP-1"_s}; }));
    QVERIFY(waitFor([&] { return !sink.playback.empty() && sink.playback.back() == std::make_pair(true, false); }));
    QCOMPARE(sink.fps.back(), 15);
    // The content is the one of the policy's desktop.
    QVERIFY(waitFor([&] { return m_daemon.asked.contains(u"DP-1@desk-2"_s); }));

    policy.state = u"{\"desktop\":\"desk-2\",\"paused\":false,\"audible\":true,\"frameRateLimit\":45}"_s;
    emit policy.HostStateChanged(u"OTHER-1"_s); // another output's change is not ours
    QTest::qWait(100);
    QCOMPARE(sink.playback.back(), std::make_pair(true, false));
    emit policy.HostStateChanged(u"DP-1"_s);
    QVERIFY(waitFor([&] { return sink.playback.back() == std::make_pair(false, true); }));
    QCOMPARE(sink.fps.back(), 45);

    // The plugin hides the wallpaper: paused locally right away, whatever the policy says.
    controller.setExternalPause(true);
    QCOMPARE(sink.playback.back(), std::make_pair(true, false));
    QVERIFY(waitFor([&] { return !policy.visibility.isEmpty() && !policy.visibility.back().second; }));
}

void TestSurfaceController::hostPlaysWithoutAPolicyServerAndRegistersLater()
{
    SurfaceController controller(hostOptions(u"DP-1"_s));
    QVERIFY(controller.start());
    SinkState &sink = *m_states[0];
    QVERIFY(waitFor([&] { return !sink.playback.empty(); }));
    QCOMPARE(sink.playback.back(), std::make_pair(false, true)); // the P4 fallback: always playing

    FakePolicyServer policy;
    policy.state = u"{\"desktop\":\"*\",\"paused\":true,\"audible\":false,\"frameRateLimit\":20}"_s;
    QVERIFY(m_fakeBus.registerObject(QString(kSurfaceDBusPath), &policy,
                                     QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
    QVERIFY(m_fakeBus.registerService(QString(kSurfaceDBusService)));
    QVERIFY(waitFor([&] { return policy.registered == QStringList{u"DP-1"_s}; }));
    QVERIFY(waitFor([&] { return sink.playback.back() == std::make_pair(true, false); }));

    // The policy server leaves: back to always playing. It returns: registered again.
    m_fakeBus.unregisterService(QString(kSurfaceDBusService));
    QVERIFY(waitFor([&] { return sink.playback.back() == std::make_pair(false, true); }));
    QCOMPARE(sink.fps.back(), kDefaultWebFpsLimit);
    QVERIFY(m_fakeBus.registerService(QString(kSurfaceDBusService)));
    QVERIFY(waitFor([&] { return policy.registered.size() == 2; }));
    QVERIFY(waitFor([&] { return sink.playback.back() == std::make_pair(true, false); }));
    m_fakeBus.unregisterService(QString(kSurfaceDBusService));
    m_fakeBus.unregisterObject(QString(kSurfaceDBusPath));
}

void TestSurfaceController::hostRetriesARefusedRegistration()
{
    FakePolicyServer policy;
    policy.refusals = 1;
    policy.state = u"{\"desktop\":\"*\",\"paused\":true,\"audible\":false,\"frameRateLimit\":30}"_s;
    QVERIFY(m_fakeBus.registerObject(QString(kSurfaceDBusPath), &policy,
                                     QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
    QVERIFY(m_fakeBus.registerService(QString(kSurfaceDBusService)));
    const auto cleanup = qScopeGuard([&] {
        m_fakeBus.unregisterService(QString(kSurfaceDBusService));
        m_fakeBus.unregisterObject(QString(kSurfaceDBusPath));
    });
    SurfaceController controller(hostOptions(u"DP-1"_s));
    QVERIFY(controller.start());
    SinkState &sink = *m_states[0];
    // Refused: the output plays unconditionally ...
    QVERIFY(waitFor([&] { return policy.attempts == 1; }));
    QVERIFY(waitFor([&] { return !sink.playback.empty(); }));
    QCOMPARE(sink.playback.back(), std::make_pair(false, true));
    // ... and the host asks again by itself (backoff of a second), then follows the policy.
    QVERIFY(waitFor([&] { return policy.attempts == 2; }, 5000));
    QVERIFY(waitFor([&] { return sink.playback.back() == std::make_pair(true, false); }));
}

void TestSurfaceController::hostForwardsItsVisibilityAndUnregistersOnExit()
{
    FakePolicyServer policy;
    QVERIFY(m_fakeBus.registerObject(QString(kSurfaceDBusPath), &policy,
                                     QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
    QVERIFY(m_fakeBus.registerService(QString(kSurfaceDBusService)));
    const auto cleanup = qScopeGuard([&] {
        m_fakeBus.unregisterService(QString(kSurfaceDBusService));
        m_fakeBus.unregisterObject(QString(kSurfaceDBusPath));
    });
    {
        SurfaceController controller(hostOptions(u"DP-9"_s));
        QVERIFY(controller.start());
        QVERIFY(waitFor([&] { return !policy.registered.isEmpty(); }));
        QVERIFY(!policy.lastCaller.isEmpty());
        // Once registered, the host states its visibility (visible) before anything else.
        QVERIFY(waitFor([&] { return !policy.visibility.isEmpty(); }));
        QCOMPARE(policy.visibility.front(), std::make_pair(u"DP-9"_s, true));
        controller.setExternalPause(true);
        controller.setExternalPause(false);
        QVERIFY(waitFor([&] { return policy.visibility.size() >= 3; }));
        QCOMPARE(policy.visibility.at(1), std::make_pair(u"DP-9"_s, false));
        QCOMPARE(policy.visibility.at(2), std::make_pair(u"DP-9"_s, true));
    }
    QVERIFY(waitFor([&] { return policy.unregistered == QStringList{u"DP-9"_s}; }));
}

void TestSurfaceController::hostHoversFromTheCursorSignals()
{
    FakePolicyServer policy;
    QVERIFY(m_fakeBus.registerObject(QString(kSurfaceDBusPath), &policy,
                                     QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
    QVERIFY(m_fakeBus.registerService(QString(kSurfaceDBusService)));
    const auto cleanup = qScopeGuard([&] {
        m_fakeBus.unregisterService(QString(kSurfaceDBusService));
        m_fakeBus.unregisterObject(QString(kSurfaceDBusPath));
    });

    SurfaceController controller(hostOptions(u"DP-1"_s));
    QVERIFY(controller.start());
    SinkState &sink = *m_states[0];
    QVERIFY(waitFor([&] { return policy.registered == QStringList{u"DP-1"_s}; }));

    // Logical position and output size become a fraction of the render size, whatever the output scale.
    emit policy.HostCursor(u"DP-1"_s, 480, 270, 1920, 1080);
    QVERIFY(waitFor([&] { return sink.cursor.size() == 1; }));
    QCOMPARE(sink.cursor[0], QPointF(0.25, 0.25));
    // Another output's cursor, a bad size and the "left" message are not hovers.
    emit policy.HostCursor(u"DP-2"_s, 1, 1, 100, 100);
    emit policy.HostCursor(u"DP-1"_s, 1, 1, 0, 100);
    emit policy.HostCursorLeft(u"DP-1"_s);
    emit policy.HostCursor(u"DP-1"_s, 960, 540, 1920, 1080);
    QVERIFY(waitFor([&] { return sink.cursor.size() == 2; }));
    QCOMPARE(sink.cursor[1], QPointF(0.5, 0.5));
}

void TestSurfaceController::hostTakesTheInteractionPointerForItsOutput()
{
    FakePolicyServer policy;
    QVERIFY(m_fakeBus.registerObject(QString(kSurfaceDBusPath), &policy,
                                     QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
    QVERIFY(m_fakeBus.registerService(QString(kSurfaceDBusService)));
    const auto cleanup = qScopeGuard([&] {
        m_fakeBus.unregisterService(QString(kSurfaceDBusService));
        m_fakeBus.unregisterObject(QString(kSurfaceDBusPath));
    });

    SurfaceController controller(hostOptions(u"DP-1"_s));
    QVERIFY(controller.start());
    SinkState &sink = *m_states[0];
    QVERIFY(waitFor([&] { return policy.registered == QStringList{u"DP-1"_s}; }));

    // Position (a fraction of the output, as the cursor), buttons and wheel reach the sink of this host's output.
    emit policy.HostPointer(u"DP-1"_s, 480, 270, 1920, 1080, 1, 0, 0);
    QVERIFY(waitFor([&] { return sink.pointer.size() == 1; }));
    QCOMPARE(sink.pointer[0].at, QPointF(0.25, 0.25));
    QCOMPARE(sink.pointer[0].buttons, 1u);
    // Another output, a bad size: nothing. A wheel turn comes with its position.
    emit policy.HostPointer(u"DP-2"_s, 1, 1, 100, 100, 1, 0, 0);
    emit policy.HostPointer(u"DP-1"_s, 1, 1, 0, 100, 1, 0, 0);
    emit policy.HostPointer(u"DP-1"_s, 960, 540, 1920, 1080, 0, 0, -120);
    QVERIFY(waitFor([&] { return sink.pointer.size() == 2; }));
    QCOMPARE(sink.pointer[1].at, QPointF(0.5, 0.5));
    QCOMPARE(sink.pointer[1].buttons, 0u);
    QCOMPARE(sink.pointer[1].wheelY, -120);
    QTest::qWait(50);
    QCOMPARE(sink.pointer.size(), size_t(2));
}

void TestSurfaceController::fractionsMapToThePixelsOfTheRenderSize()
{
    const QSize size(1920, 1080);
    QCOMPARE(pixelOfFraction(0.5, 0.5, size), std::optional<QPoint>(QPoint(960, 540)));
    QCOMPARE(pixelOfFraction(0, 0, size), std::optional<QPoint>(QPoint(0, 0)));
    QCOMPARE(pixelOfFraction(1.0, 1.0, size), std::optional<QPoint>(QPoint(1919, 1079))); // the last pixel, not one past it
    QCOMPARE(pixelOfFraction(-3, 7, size), std::optional<QPoint>(QPoint(0, 1079)));
    QVERIFY(!pixelOfFraction(qQNaN(), 0.5, size));
    QVERIFY(!pixelOfFraction(0.5, 0.5, QSize(0, 100)));
}

int main(int argc, char *argv[])
{
    ItemSchemeHandler::registerScheme();
    QtWebEngineQuick::initialize();
    QGuiApplication app(argc, argv);
    TestSurfaceController test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_surfacecontroller.moc"
