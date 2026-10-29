// ScreenWatch (src/daemon/screenwatch.*): the daemon's side of the screen triggers against a FAKE portal on a private bus and a FAKE helper (a bash script that speaks the line protocol and
// scores from a file). Nothing here reads a pixel of a real screen. It covers: nothing exists until the user asks, the gates of start(), the consent, what the helper is told (triggers,
// regions in stream pixels, references, rate, pets), scores becoming activation and the end, the lock, the battery cap, a refusal, a crash (no restart), the desktop ending the share,
// stop(), the explicit capture and its confirmation, and the settings going off.

#include "automationstore.h"
#include "crashledger.h"
#include "screenrefstore.h"
#include "screenwatch.h"
#include "securebytes.h"
#include "settings.h"
#include "testutil.h"

#include <QBuffer>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <limits>
#include <memory>

#include <fcntl.h>
#include <unistd.h>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

struct FakeStream {
    uint node = 0;
    QVariantMap props;
};
QDBusArgument &operator<<(QDBusArgument &arg, const FakeStream &s)
{
    arg.beginStructure();
    arg << s.node << s.props;
    arg.endStructure();
    return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, FakeStream &s)
{
    arg.beginStructure();
    arg >> s.node >> s.props;
    arg.endStructure();
    return arg;
}

// (ii) as a value of an a{sv}: a registered struct type, marshalled as a variant.
struct IntPair {
    int a = 0, b = 0;
};
QDBusArgument &operator<<(QDBusArgument &arg, const IntPair &p)
{
    arg.beginStructure();
    arg << p.a << p.b;
    arg.endStructure();
    return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, IntPair &p)
{
    arg.beginStructure();
    arg >> p.a >> p.b;
    arg.endStructure();
    return arg;
}
QVariant pairVariant(int a, int b)
{
    return QVariant::fromValue(IntPair{a, b});
}

FakeStream monitor(uint node, int x, int y, int w, int h, const QString &id = {})
{
    FakeStream s;
    s.node = node;
    s.props = {{u"position"_s, pairVariant(x, y)}, {u"size"_s, pairVariant(w, h)}, {u"source_type"_s, uint(1)}};
    if (!id.isEmpty())
        s.props.insert(u"id"_s, id);
    return s;
}

class FakeSession : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Session")
public:
    int closed = 0;
public slots:
    Q_SCRIPTABLE void Close() { ++closed; }
signals:
    Q_SCRIPTABLE void Closed(const QVariantMap &details);
};

class FakeRequest : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Request")
public:
    FakeRequest(int *counter, QStringList *paths, QString path) : m_counter(counter), m_paths(paths), m_path(std::move(path)) {}
public slots:
    Q_SCRIPTABLE void Close()
    {
        ++*m_counter;
        m_paths->append(m_path);
    }
private:
    int *m_counter;
    QStringList *m_paths;
    QString m_path;
};

class FakePortal : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.ScreenCast")

public:
    explicit FakePortal(QDBusConnection bus) : m_bus(std::move(bus)) {}

    // What the fake does, one knob each.
    uint startCode = 0;                   // the Response code of Start
    bool silentStart = false;             // Start never gets a Response (the dialog is open)
    bool silentCreate = false;            // CreateSession never gets a Response
    QList<FakeStream> streams{monitor(41, 0, 0, 1920, 1080, u"DP-1"_s)};
    QVariant streamsOverride;             // when valid, the `streams` result as this (to send the wrong type)
    QVariant sessionHandle;               // when valid, session_handle as this (wrong type)
    bool noSessionHandle = false;
    QString restoreTokenReply = u"token-from-the-portal"_s;
    bool refuseToken = false;             // SelectSources with a restore_token is an error (or a refused request)
    bool refuseTokenByResponse = false;   //   ... as a Response with code 2 instead of a D-Bus error
    bool failSelect = false;
    bool openRemoteError = false;
    bool omitStreams = false;
    int openRemoteFd = -1;                // the descriptor handed back (a pipe end the test owns)
    bool noMethods = false;
    bool responseFirst = false;           // the Response is sent before the method reply (the race the portal's docs warn about)
    bool lateReplySelect = false;         // SelectSources: the Response at once, the method reply 100 ms later
    int delayOpenMs = 0;                  // OpenPipeWireRemote answers this late
    FakeSession *sessionObject = nullptr; // exported at the path of each CreateSession
    QStringList closedRequests;           // the paths Request.Close was called on
    QString lastStartPath;

    // What it saw.
    QList<QVariantMap> selectOptions;
    QList<QVariantMap> createOptions;
    int creates = 0, selects = 0, starts = 0, opens = 0;
    QString session;                      // the path of the session made by the last CreateSession
    int requestCloses = 0;                // Request.Close calls (a dialog taken away)
    QList<std::shared_ptr<FakeRequest>> requests;

public slots:
    Q_SCRIPTABLE QDBusObjectPath CreateSession(const QVariantMap &options)
    {
        ++creates;
        createOptions << options;
        QString sender = message().service();
        sender.remove(u':').replace(u'.', u'_');
        if (!session.isEmpty())
            m_bus.unregisterObject(session);
        session = u"/org/freedesktop/portal/desktop/session/%1/%2"_s.arg(sender, options.value(u"session_handle_token"_s).toString());
        if (sessionObject)
            m_bus.registerObject(session, sessionObject, QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals);
        QVariantMap results;
        if (!noSessionHandle)
            results.insert(u"session_handle"_s, sessionHandle.isValid() ? sessionHandle : QVariant(session));
        return respond(options, results, silentCreate ? 99 : 0);
    }
    Q_SCRIPTABLE QDBusObjectPath SelectSources(const QDBusObjectPath &, const QVariantMap &options)
    {
        ++selects;
        selectOptions << options;
        if (failSelect || (refuseToken && options.contains(u"restore_token"_s))) {
            sendErrorReply(QDBusError::InvalidArgs, u"bad restore token"_s);
            return {};
        }
        const QDBusObjectPath path = respond(options, {}, refuseTokenByResponse && options.contains(u"restore_token"_s) ? 2 : 0);
        if (lateReplySelect) {
            const QDBusMessage call = message();
            setDelayedReply(true);
            QTimer::singleShot(100, this, [this, call, path] { m_bus.send(call.createReply(QVariant::fromValue(path))); });
            return {};
        }
        return path;
    }
    Q_SCRIPTABLE QDBusObjectPath Start(const QDBusObjectPath &, const QString &, const QVariantMap &options)
    {
        ++starts;
        {
            QString sender = message().service();
            sender.remove(u':').replace(u'.', u'_');
            lastStartPath = u"/org/freedesktop/portal/desktop/request/%1/%2"_s.arg(sender, options.value(u"handle_token"_s).toString());
        }
        QVariantMap results;
        if (!omitStreams)
            results.insert(u"streams"_s, streamsOverride.isValid() ? streamsOverride : QVariant::fromValue(streams));
        if (!restoreTokenReply.isEmpty()) {
            results.insert(u"restore_token"_s, restoreTokenReply);
            results.insert(u"persist_mode"_s, uint(2));
        }
        return respond(options, results, silentStart ? 99 : startCode);
    }
    Q_SCRIPTABLE QDBusUnixFileDescriptor OpenPipeWireRemote(const QDBusObjectPath &, const QVariantMap &)
    {
        ++opens;
        if (openRemoteError) {
            sendErrorReply(QDBusError::Failed, u"no remote"_s);
            return {};
        }
        if (delayOpenMs > 0) {
            const QDBusMessage call = message();
            setDelayedReply(true);
            QTimer::singleShot(delayOpenMs, this, [this, call] { m_bus.send(call.createReply(QVariant::fromValue(QDBusUnixFileDescriptor(openRemoteFd)))); });
            return {};
        }
        return QDBusUnixFileDescriptor(openRemoteFd);
    }

private:
    QDBusObjectPath respond(const QVariantMap &options, const QVariantMap &results, uint code)
    {
        QString sender = message().service();
        sender.remove(u':').replace(u'.', u'_');
        const QString path = u"/org/freedesktop/portal/desktop/request/%1/%2"_s.arg(sender, options.value(u"handle_token"_s).toString());
        auto request = std::make_shared<FakeRequest>(&requestCloses, &closedRequests, path);
        if (m_bus.registerObject(path, request.get(), QDBusConnection::ExportScriptableSlots))
            requests.append(request);
        if (code != 99) { // 99: the fake never answers (a dialog nobody touches)
            const auto send = [this, results, code, path] {
                QDBusMessage response = QDBusMessage::createSignal(path, u"org.freedesktop.portal.Request"_s, u"Response"_s);
                response.setArguments({code, results});
                m_bus.send(response);
            };
            if (responseFirst)
                send();
            else
                QTimer::singleShot(10, this, send);
        }
        return QDBusObjectPath(path);
    }
    QDBusConnection m_bus;
};

} // namespace

namespace {

QString fakeHelper(const QTemporaryDir &dir)
{
    const QString path = dir.filePath(u"helper.sh"_s);
    testutil::writeFile(path, R"SH(#!/bin/bash
# --chan-fd N --pw-fd M --nodes IDS ...
d=$(dirname "$0")
chan=$2
echo "$@" >> "$d/launches.log"
mode=$(cat "$d/mode")
[ "$mode" = crash ] && kill -SEGV $$
echo "hello 1" >&$chan
ids=(); running=0; n=0
while true; do
  if read -r -t 0.04 -u $chan line; then
    echo "$line" >> "$d/lines.log"
    case "$line" in
      "trigger "*) set -- $line; ids+=("$2") ;;
      clear) ids=() ;;
      start) running=1 ;;
      stop) running=0 ;;
      quit) exit 0 ;;
      "capture "*) set -- $line; req=$2
         [ "$mode" = crashoncapture ] && kill -SEGV $$
         [ "$mode" = silentcapture ] && continue
         echo "shot $req 2 2 12" >&$chan; echo "chunk $req 0 AAAAAAAAAAAAAAAA" >&$chan; echo "shotend $req" >&$chan ;;
    esac
    echo "ok ${line%% *}" >&$chan
  else
    [ $? -gt 128 ] || exit 0
  fi
  if [ $running = 1 ]; then
    for id in "${ids[@]}"; do echo "s $id 0 $(cat "$d/score" 2>/dev/null || echo 0)" >&$chan; done
    n=$((n+1)); echo "t $n" >&$chan
  fi
done
)SH");
    QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::ExeOwner);
    testutil::writeFile(dir.filePath(u"mode"_s), "run");
    testutil::writeFile(dir.filePath(u"score"_s), "0");
    return path;
}

QByteArray greyPng(int seed)
{
    QImage image(32, 32, QImage::Format_RGB888);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x)
            image.setPixel(x, y, qRgb((x * 8 + seed) & 255, (y * 8) & 255, 40));
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return png;
}

} // namespace

class TstScreenWatch : public QObject
{
    Q_OBJECT

private:
    QDBusConnection server = QDBusConnection::sessionBus();
    QDBusConnection client{u"unused"_s};
    std::unique_ptr<FakePortal> portal;
    FakeSession session;
    bool m_up = false;
    int m_pipe[2] = {-1, -1};

    struct World {
        QTemporaryDir dir;
        SettingsStore settings{dir.filePath(u"settings.json"_s)};
        TriggerStore triggers{dir.filePath(u"triggers.json"_s)};
        ScreenRefStore refs{dir.filePath(u"refs"_s)};
        QString helper;
        QString triggerId;
        std::unique_ptr<ScreenWatch> watch;
        bool running = false;
        QList<screenmatch::LogicalRect> pets;
        bool requireSandbox = false;  // (a fake helper behind no sandbox, unless a test asks for the real requirement)
        QString crashDir;
        std::function<double(int, int, int, int)> scaleOf;
        int pendingMs = 60000, jobMs = 4000;
    };

    static QString linesOf(const World &w)
    {
        QFile f(w.dir.filePath(u"lines.log"_s));
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    }
    static int launches(const World &w)
    {
        QFile f(w.dir.filePath(u"launches.log"_s));
        return f.open(QIODevice::ReadOnly) ? f.readAll().count('\n') : 0;
    }
    static void score(const World &w, int permille) { testutil::writeFile(w.dir.filePath(u"score"_s), QByteArray::number(permille)); }

    // A world with one screen trigger (two ticks to activate, 150 ms of grace, no cooldown) with one region and its reference.
    void make(World &w, bool enabledSetting = true, bool withReference = true)
    {
        w.settings.load();
        w.triggers.load();
        if (enabledSetting)
            QVERIFY(w.settings.merge(QJsonObject{{u"screenTriggersEnabled"_s, true}}));
        Trigger t;
        t.name = u"Victory"_s;
        t.enabled = true;
        t.source.kind = TriggerKind::Screen;
        t.source.screen = QJsonObject{{u"regions"_s, QJsonArray{QJsonObject{{u"kind"_s, u"rect"_s}, {u"x"_s, 100}, {u"y"_s, 200}, {u"w"_s, 300}, {u"h"_s, 120}}}},
                                      {u"match"_s, QJsonObject{{u"mode"_s, u"hash"_s}, {u"threshold"_s, 0.8}, {u"startFramesToCheck"_s, 2}, {u"stopGraceMs"_s, 150}, {u"cooldownMs"_s, 0}}}};
        t.action.type = TriggerActionType::HideAll;
        QString error;
        w.triggerId = w.triggers.add(t, &error);
        QVERIFY2(!w.triggerId.isEmpty(), qPrintable(error));
        if (withReference)
            QVERIFY(w.refs.putFrame(w.triggerId, 0, 0, greyPng(0)));
        w.helper = fakeHelper(w.dir);
        ScreenWatch::Config config;
        config.helperPath = w.helper;
        config.requireSandbox = w.requireSandbox;
        config.crashDir = w.crashDir;
        config.pendingMs = w.pendingMs;
        config.jobMs = w.jobMs;
        config.bus = client;
        config.statePath = w.dir.filePath(u"screen-state.json"_s);
        config.petsDebounceMs = 20;
        config.refreshDebounceMs = 20;
        ScreenWatch::Hooks hooks;
        hooks.petRects = [&w] { return w.pets; };
        hooks.scaleOf = [&w](int x, int y, int width, int height) { return w.scaleOf ? w.scaleOf(x, y, width, height) : 1.0; };
        hooks.actionRunning = [&w](const QString &) { return w.running; };
        w.watch = std::make_unique<ScreenWatch>(&w.triggers, &w.settings, &w.refs, config, hooks);
    }
    void bringUp()
    {
        QVERIFY(server.registerObject(u"/org/freedesktop/portal/desktop"_s, portal.get(), QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals));
        QVERIFY(server.registerService(u"org.freedesktop.portal.Desktop"_s));
        portal->sessionObject = &session;
        m_up = true;
    }
    static bool waitState(ScreenWatch &w, ScreenWatch::State s, int ms = 8000) { return QTest::qWaitFor([&] { return w.state() == s; }, ms); }
    bool waitLines(const World &w, const QString &needle, int ms = 6000)
    {
        return QTest::qWaitFor([&] { return linesOf(w).contains(needle); }, ms);
    }

private slots:
    void initTestCase()
    {
        if (qEnvironmentVariableIntValue("MURASAKI_TEST_PRIVATE_BUS") != 1)
            QSKIP("needs a private session bus (run through ctest, or set MURASAKI_TEST_PRIVATE_BUS=1 under dbus-run-session)");
        qDBusRegisterMetaType<IntPair>();
        qDBusRegisterMetaType<FakeStream>();
        qDBusRegisterMetaType<QList<FakeStream>>();
        client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"murasaki-test-screenwatch-client"_s);
        QVERIFY(client.isConnected());
    }
    void init()
    {
        if (qEnvironmentVariableIntValue("MURASAKI_TEST_PRIVATE_BUS") != 1)
            QSKIP("needs a private session bus");
        portal = std::make_unique<FakePortal>(server);
        session.closed = 0;
        QVERIFY(::pipe2(m_pipe, O_CLOEXEC) == 0);
        portal->openRemoteFd = m_pipe[0];
    }
    void cleanup()
    {
        QTest::qWait(100);
        if (m_up) {
            server.unregisterService(u"org.freedesktop.portal.Desktop"_s);
            server.unregisterObject(u"/org/freedesktop/portal/desktop"_s);
            if (!portal->session.isEmpty())
                server.unregisterObject(portal->session);
            m_up = false;
        }
        portal.reset();
        for (int &fd : m_pipe) {
            if (fd >= 0)
                ::close(fd);
            fd = -1;
        }
    }

    void nothingExistsUntilTheUserAsks()
    {
        bringUp();
        World w;
        make(w);
        QVERIFY(w.watch->idle());
        QCOMPARE(w.watch->state(), ScreenWatch::State::Off);
        QCOMPARE(w.watch->stateJson().value(u"state"_s).toString(), u"off"_s);
        QTest::qWait(300);
        QCOMPARE(portal->creates, 0); // no portal call
        QCOMPARE(launches(w), 0);     // no helper
        QVERIFY(w.watch->idle());
        // settings, triggers and pets changing while off start nothing either
        w.watch->settingsChanged();
        w.watch->triggersChanged();
        w.watch->petsMoved();
        QVERIFY(w.watch->idle());
    }
    void startNeedsTheSettingATriggerAnUnlockedSessionAndAHelper()
    {
        bringUp();
        QString why;
        {
            World w;
            make(w, false);
            QVERIFY(!w.watch->start(&why));
            QVERIFY2(why.contains(u"screenTriggersEnabled"_s), qPrintable(why));
            QVERIFY(w.watch->idle());
        }
        {
            World w;
            make(w);
            w.settings.merge(QJsonObject{{u"automationEnabled"_s, false}});
            QVERIFY(!w.watch->start(&why)); // the master switch of the automation
        }
        {
            World w;
            make(w);
            Trigger t = *w.triggers.trigger(w.triggerId);
            t.enabled = false;
            w.triggers.update(t);
            QVERIFY(!w.watch->start(&why));
            QVERIFY2(why.contains(u"no screen trigger"_s), qPrintable(why));
        }
        {
            World w;
            make(w);
            w.watch->setLocked(true);
            QVERIFY(!w.watch->start(&why));
            QVERIFY2(why.contains(u"locked"_s), qPrintable(why));
        }
        QCOMPARE(portal->creates, 0);
    }
    void consentThenTheHelperIsToldEverything()
    {
        bringUp();
        World w;
        make(w);
        QSignalSpy changed(w.watch.get(), &ScreenWatch::changed);
        QVERIFY(w.watch->start());
        QCOMPARE(w.watch->state(), ScreenWatch::State::Asking);
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QVERIFY(!w.watch->idle());
        // the portal was asked not to remember
        QVERIFY(!portal->selectOptions.at(0).contains(u"persist_mode"_s));
        QVERIFY(waitLines(w, u"start"_s));
        const QString lines = linesOf(w);
        QVERIFY2(lines.contains(u"clear\n"_s), qPrintable(lines));
        QVERIFY2(lines.contains(u"trigger %1 hash\n"_s.arg(w.triggerId)), qPrintable(lines));
        QVERIFY2(lines.contains(u"region %1 0 0 100 200 300 120\n"_s.arg(w.triggerId)), qPrintable(lines)); // stream pixels of the first monitor
        QVERIFY2(lines.contains(u"ref %1 0 0 "_s.arg(w.triggerId)), qPrintable(lines));
        QVERIFY2(lines.contains(u"rate 5\n"_s), qPrintable(lines));
        QVERIFY2(lines.contains(u"pets 0\n"_s), qPrintable(lines));
        QVERIFY2(!lines.contains(u"--buffers"_s), qPrintable(lines)); // (no argument is a command; the default is shared memory, never `any`)
        QFile args(w.dir.filePath(u"launches.log"_s));
        QVERIFY(args.open(QIODevice::ReadOnly));
        const QString launch = QString::fromUtf8(args.readAll());
        QVERIFY2(!launch.contains(u"--buffers"_s), qPrintable(launch));
        const QJsonObject state = w.watch->stateJson();
        QCOMPARE(state.value(u"state"_s).toString(), u"watching"_s);
        QCOMPARE(state.value(u"regions"_s).toInt(), 1);
        QCOMPARE(state.value(u"ticksPerSecond"_s).toInt(), 5);
        QVERIFY(state.value(u"since"_s).toDouble() > 0);
        // starting again while watching is harmless
        QVERIFY(w.watch->start());
        QCOMPARE(launches(w), 1);
        QVERIFY(changed.size() >= 2);
    }
    void scoresBecomeActivationAndTheEndAfterTheGrace()
    {
        bringUp();
        World w;
        make(w);
        QSignalSpy activated(w.watch.get(), &ScreenWatch::activated);
        QSignalSpy deactivated(w.watch.get(), &ScreenWatch::deactivated);
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QTest::qWait(300);
        QCOMPARE(activated.size(), 0); // the picture is not there (score 0)
        QVERIFY(w.watch->triggerJson(w.triggerId).value(u"score"_s).toArray().at(0).toInt() == 0);
        score(w, 950);
        QTRY_COMPARE_WITH_TIMEOUT(activated.size(), 1, 5000);
        QCOMPARE(activated.first().first().toString(), w.triggerId);
        const QJsonObject live = w.watch->triggerJson(w.triggerId);
        QVERIFY(live.value(u"active"_s).toBool());
        QVERIFY(live.value(u"detected"_s).toBool());
        QCOMPARE(live.value(u"score"_s).toArray().at(0).toInt(), 950);
        score(w, 100);
        QTRY_COMPARE_WITH_TIMEOUT(deactivated.size(), 1, 5000);
        QVERIFY(!w.watch->triggerJson(w.triggerId).value(u"active"_s).toBool());
        QCOMPARE(activated.size(), 1);
    }
    void stopEndsTheHelperTheSessionAndWhatWasActive()
    {
        bringUp();
        World w;
        make(w);
        QSignalSpy deactivated(w.watch.get(), &ScreenWatch::deactivated);
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        score(w, 990);
        QTRY_VERIFY_WITH_TIMEOUT(w.watch->triggerJson(w.triggerId).value(u"active"_s).toBool(), 5000);
        w.watch->stop();
        QCOMPARE(w.watch->state(), ScreenWatch::State::Off);
        QCOMPARE(deactivated.size(), 1); // what was active is released (the action is undone)
        QVERIFY(w.watch->idle());
        QTRY_COMPARE(session.closed, 1); // the portal session is closed
        QCOMPARE(launches(w), 1);
        // there is no automatic restart
        QTest::qWait(300);
        QCOMPARE(launches(w), 1);
        QCOMPARE(portal->creates, 1);
    }
    // The guided walk's watch is leased to the connection that started it (StartScreenWatchLeased): a GUI that dies must not leave the screen being read.
    static QString newPeer(const QString &name)
    {
        const QDBusConnection connection = QDBusConnection::connectToBus(QDBusConnection::SessionBus, name);
        return connection.isConnected() ? connection.baseService() : QString();
    }
    void aLeasedWatchStopsWhenItsConnectionLeavesTheBus()
    {
        bringUp();
        World w;
        make(w);
        const QString owner = newPeer(u"screenwatch-lease-owner"_s);
        QVERIFY(!owner.isEmpty());
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        w.watch->lease(owner);
        QTest::qWait(100);
        QCOMPARE(w.watch->state(), ScreenWatch::State::Watching); // (alive: it goes on)
        QDBusConnection::disconnectFromBus(u"screenwatch-lease-owner"_s);
        QTRY_COMPARE_WITH_TIMEOUT(w.watch->state(), ScreenWatch::State::Off, 5000);
        QVERIFY(w.watch->idle()); // the helper is gone and the portal session closed: the sharing indicator goes off
        QTRY_COMPARE(session.closed, 1);
        QVERIFY(w.watch->stateJson().value(u"reason"_s).toString().contains(u"gone"_s));
    }
    void aWatchThatWasNotLeasedOrWasStoppedOutlivesTheConnection()
    {
        bringUp();
        World w;
        make(w);
        // never leased (it was the user's own start)
        const QString stranger = newPeer(u"screenwatch-lease-stranger"_s);
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QDBusConnection::disconnectFromBus(u"screenwatch-lease-stranger"_s);
        QTest::qWait(150);
        QCOMPARE(w.watch->state(), ScreenWatch::State::Watching);
        // leased, then stopped on purpose and started again by somebody else: the old owner leaving is no matter
        const QString owner = newPeer(u"screenwatch-lease-owner2"_s);
        w.watch->lease(owner);
        w.watch->stop();
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QDBusConnection::disconnectFromBus(u"screenwatch-lease-owner2"_s);
        QTest::qWait(150);
        QCOMPARE(w.watch->state(), ScreenWatch::State::Watching);
        // leasing while nothing runs binds nothing
        w.watch->stop();
        const QString late = newPeer(u"screenwatch-lease-late"_s);
        w.watch->lease(late);
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QDBusConnection::disconnectFromBus(u"screenwatch-lease-late"_s);
        QTest::qWait(150);
        QCOMPARE(w.watch->state(), ScreenWatch::State::Watching);
    }
    void theLockPausesAndTheBatteryCapsTheRate()
    {
        bringUp();
        World w;
        make(w);
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QVERIFY(waitLines(w, u"start"_s));
        w.watch->setOnBattery(true);
        QVERIFY(waitLines(w, u"rate 2\n"_s));
        w.watch->setOnBattery(false);
        QTRY_VERIFY_WITH_TIMEOUT(linesOf(w).count(u"rate 5\n"_s) >= 2, 5000);
        score(w, 990);
        QTRY_VERIFY_WITH_TIMEOUT(w.watch->triggerJson(w.triggerId).value(u"active"_s).toBool(), 5000);
        const int stops = linesOf(w).count(u"stop\n"_s);
        QSignalSpy deactivated(w.watch.get(), &ScreenWatch::deactivated);
        w.watch->setLocked(true);
        QCOMPARE(deactivated.size(), 1); // locked: nothing stays active
        QVERIFY(w.watch->stateJson().value(u"paused"_s).toBool());
        QTRY_COMPARE_WITH_TIMEOUT(linesOf(w).count(u"stop\n"_s), stops + 1, 5000);
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching)); // (still the same session)
        const int starts = linesOf(w).count(u"start\n"_s);
        w.watch->setLocked(false);
        QTRY_COMPARE_WITH_TIMEOUT(linesOf(w).count(u"start\n"_s), starts + 1, 5000);
        QVERIFY(!w.watch->stateJson().value(u"paused"_s).toBool());
        QCOMPARE(launches(w), 1);
    }
    void aRefusalIsNamedAndNothingStarts()
    {
        portal->startCode = 1;
        bringUp();
        World w;
        make(w);
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Refused));
        QCOMPARE(w.watch->reason(), QString(kScreenPermissionRefused));
        QCOMPARE(launches(w), 0);
        QVERIFY(w.watch->idle() == false); // (a state to show: refused)
        QCOMPARE(w.watch->stateJson().value(u"state"_s).toString(), u"refused"_s);
    }
    void aHelperThatCrashesIsNotRestarted()
    {
        bringUp();
        World w;
        make(w);
        testutil::writeFile(w.dir.filePath(u"mode"_s), "crash");
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Failed));
        QVERIFY2(w.watch->reason().contains(u"crash"_s), qPrintable(w.watch->reason()));
        QTest::qWait(400);
        QCOMPARE(launches(w), 1);
        QTRY_COMPARE(session.closed, 1); // the portal session does not stay open for a dead helper
    }
    void settingsGoingOffStopsEverything()
    {
        bringUp();
        World w;
        make(w);
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QVERIFY(w.settings.merge(QJsonObject{{u"screenTriggersEnabled"_s, false}}));
        w.watch->settingsChanged();
        QCOMPARE(w.watch->state(), ScreenWatch::State::Off);
        QTRY_COMPARE(session.closed, 1);
        QVERIFY(w.watch->idle());
    }
    void theLastScreenTriggerGoingAwayStopsTheWatch()
    {
        bringUp();
        World w;
        make(w);
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QVERIFY(w.triggers.remove(w.triggerId));
        w.watch->triggersChanged();
        QTRY_COMPARE_WITH_TIMEOUT(w.watch->state(), ScreenWatch::State::Off, 5000);
    }
    void aRegionOutsideTheSharedScreensAndOneWithoutAReferenceAreNamed()
    {
        bringUp();
        World w;
        make(w, true, false); // no reference picture
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QVERIFY(waitLines(w, u"trigger "_s));
        QCOMPARE(w.watch->stateJson().value(u"regions"_s).toInt(), 0);
        QVERIFY(w.watch->stateJson().value(u"notes"_s).toArray().at(0).toString().contains(u"no reference"_s));
        QVERIFY(!linesOf(w).contains(u"region "_s)); // not declared to the helper
        // a trigger that is moved off the screens
        Trigger t = *w.triggers.trigger(w.triggerId);
        t.source.screen.insert(u"regions"_s, QJsonArray{QJsonObject{{u"kind"_s, u"rect"_s}, {u"x"_s, 9000}, {u"y"_s, 9000}, {u"w"_s, 50}, {u"h"_s, 50}}});
        w.triggers.update(t);
        w.watch->triggersChanged();
        QTRY_VERIFY_WITH_TIMEOUT(w.watch->stateJson().value(u"notes"_s).toArray().at(0).toString().contains(u"outside the shared screens"_s), 5000);
    }
    void petsAreSentToTheHelperInStreamPixels()
    {
        bringUp();
        World w;
        make(w);
        w.pets = {{150, 220, 80, 60}, {5000, 5000, 10, 10}}; // the second is on no screen
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QVERIFY(waitLines(w, u"pets 1 0 150 220 80 60\n"_s));
        w.pets = {{150, 220, 80, 60}, {400, 400, 100, 100}};
        w.watch->petsMoved();
        QVERIFY(waitLines(w, u"pets 2 "_s));
        // with the setting off nothing is sent
        QVERIFY(w.settings.merge(QJsonObject{{u"screenIgnorePets"_s, false}}));
        w.watch->settingsChanged();
        QVERIFY(waitLines(w, u"pets 0\n"_s));
    }
    void aCaptureIsHeldUntilItIsConfirmed()
    {
        bringUp();
        World w;
        make(w);
        QString why;
        QVERIFY(w.watch->capture(w.triggerId, 0, 1, &why).isEmpty()); // not watching yet
        QVERIFY2(why.contains(u"start watching"_s), qPrintable(why));
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QVERIFY(w.watch->capture(QStringLiteral("nope"), 0, 1, &why).isEmpty());
        QVERIFY(w.watch->capture(w.triggerId, 3, 1, &why).isEmpty());
        QVERIFY(w.watch->capture(w.triggerId, 0, 65, &why).isEmpty());
        QSignalSpy captured(w.watch.get(), &ScreenWatch::captured);
        const QString request = w.watch->capture(w.triggerId, 0, 1, &why);
        QVERIFY2(!request.isEmpty(), qPrintable(why));
        QVERIFY(w.watch->capture(w.triggerId, 0, 1, &why).isEmpty()); // one at a time
        QTRY_COMPARE_WITH_TIMEOUT(captured.size(), 1, 5000);
        const QJsonObject result = QJsonDocument::fromJson(captured.first().at(1).toString().toUtf8()).object();
        QCOMPARE(result.value(u"request"_s).toString(), request);
        const QJsonObject step = result.value(u"steps"_s).toArray().at(0).toObject();
        QCOMPARE(step.value(u"width"_s).toInt(), 2);
        QVERIFY(QByteArray::fromBase64(step.value(u"png"_s).toString().toLatin1()).startsWith("\x89PNG"));
        // nothing was kept
        QCOMPARE(w.refs.steps(w.triggerId, 0), (QList<int>{0})); // (only the reference the test put there)
        const QByteArray before = w.refs.frame(w.triggerId, 0, 0);
        QVERIFY(!w.watch->commitCapture(QStringLiteral("capture-999"), &why));
        QVERIFY(w.watch->commitCapture(request, &why));
        QVERIFY(w.refs.frame(w.triggerId, 0, 0) != before); // the confirmed picture replaced step 0
        QVERIFY(!w.watch->commitCapture(request, &why));    // once
    }
    void aHostThatRefusesToStartIsAFailureNotACrash()
    {
        bringUp();
        {
            // a sandbox is required and there is none: the host says so from inside start()
            World w;
            w.requireSandbox = true;
            make(w);
            QVERIFY(w.watch->start());
            QVERIFY(waitState(*w.watch, ScreenWatch::State::Failed));
            QCOMPARE(w.watch->reason(), u"sandbox unavailable"_s);
            QCOMPARE(launches(w), 0);
            QTRY_COMPARE(session.closed, 1);
        }
        {
            // a helper the crash ledger has blocked (three crashes) while the setting stays on
            World w;
            w.crashDir = w.dir.filePath(u"ledger"_s);
            make(w);
            CrashLedger ledger(w.crashDir);
            ledger.load();
            for (int i = 0; i < CrashLedger::kMaxCrashes; ++i)
                ledger.recordCrash(w.helper);
            QVERIFY(ledger.blocked(w.helper));
            portal->requestCloses = 0;
            QVERIFY(w.watch->start());
            QVERIFY(waitState(*w.watch, ScreenWatch::State::Failed));
            QVERIFY2(w.watch->reason().contains(u"keeps crashing"_s), qPrintable(w.watch->reason()));
            QCOMPARE(launches(w), 0);
            // and the user turning the setting off and on again lets it try
            QVERIFY(w.settings.merge(QJsonObject{{u"screenTriggersEnabled"_s, false}}));
            w.watch->settingsChanged();
            QVERIFY(w.settings.merge(QJsonObject{{u"screenTriggersEnabled"_s, true}}));
            w.watch->settingsChanged();
            QVERIFY(w.watch->start());
            QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        }
    }
    void aLockWhileTheDialogIsOpenKeepsTheHelperPausedUntilTheUnlock()
    {
        bringUp();
        World w;
        make(w);
        QVERIFY(w.watch->start());
        w.watch->setLocked(true); // before the dialog is answered
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QVERIFY(waitLines(w, u"rate "_s));
        QTest::qWait(300);
        QVERIFY2(!linesOf(w).contains(u"start\n"_s), "the helper was told to read the screen of a locked session");
        QVERIFY(w.watch->stateJson().value(u"paused"_s).toBool());
        QString why;
        QVERIFY(w.watch->capture(w.triggerId, 0, 1, &why).isEmpty()); // no picture of a locked session
        QVERIFY2(why.contains(u"locked"_s), qPrintable(why));
        w.watch->setLocked(false);
        QVERIFY(waitLines(w, u"start\n"_s));
        QVERIFY(!w.watch->stateJson().value(u"paused"_s).toBool());
    }
    void aTriggerRemovedWhileTheDialogIsOpenStartsNothing()
    {
        bringUp();
        World w;
        make(w);
        QVERIFY(w.watch->start());
        QVERIFY(w.triggers.remove(w.triggerId));
        w.watch->triggersChanged();
        QCOMPARE(w.watch->state(), ScreenWatch::State::Off);
        QTest::qWait(500);
        QCOMPARE(launches(w), 0);
        QVERIFY(w.watch->idle());
    }
    void aCaptureNobodyKeepsIsDroppedAndNothingSurvivesTheStop()
    {
        bringUp();
        World w;
        w.pendingMs = 400;
        make(w);
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QSignalSpy captured(w.watch.get(), &ScreenWatch::captured);
        QString why;
        QString request = w.watch->capture(w.triggerId, 0, 1, &why);
        QVERIFY(!request.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(captured.size(), 1, 5000);
        // expiry
        QTRY_VERIFY_WITH_TIMEOUT(!w.watch->commitCapture(request, &why), 3000);
        QVERIFY2(why.contains(u"no such capture"_s), qPrintable(why));
        QVERIFY(!w.watch->idle());
        // stop drops it
        request = w.watch->capture(w.triggerId, 0, 1, &why);
        QTRY_COMPARE_WITH_TIMEOUT(captured.size(), 2, 5000);
        w.watch->stop();
        QVERIFY(!w.watch->commitCapture(request, &why));
        QVERIFY(w.watch->idle()); // (and the expiry timer is not left running)
        // a lock drops it too, and so does a trigger that goes away
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        request = w.watch->capture(w.triggerId, 0, 1, &why);
        QTRY_COMPARE_WITH_TIMEOUT(captured.size(), 3, 5000);
        w.watch->setLocked(true);
        w.watch->setLocked(false);
        QVERIFY(!w.watch->commitCapture(request, &why));
    }
    void severalMonitorsGetTheirOwnPixelsAndAnUnknownScaleIsNamedNotGuessed()
    {
        // three monitors: 1x at the origin, 2x to the right (and higher), and a portrait one to the left whose output is not known
        portal->streams = {monitor(41, 0, 0, 1920, 1080, u"DP-1"_s), monitor(42, 1920, -200, 2560, 1440, u"HDMI-1"_s), monitor(43, -1080, 0, 1080, 1920, u"DP-2"_s)};
        bringUp();
        World w;
        w.scaleOf = [](int x, int, int, int) { return x == 1920 ? 2.0 : x == 0 ? 1.0 : 0.0; };
        make(w);
        Trigger t = *w.triggers.trigger(w.triggerId);
        const auto rect = [](double x, double y, double wd, double h) { return QJsonObject{{u"kind"_s, u"rect"_s}, {u"x"_s, x}, {u"y"_s, y}, {u"w"_s, wd}, {u"h"_s, h}}; };
        t.source.screen.insert(u"regions"_s, QJsonArray{rect(100, 200, 300, 120), rect(2000, 0, 50, 25), rect(-900, 100, 200, 200)});
        QVERIFY(w.triggers.update(t));
        QVERIFY(w.refs.putFrame(w.triggerId, 1, 0, greyPng(10)));
        QVERIFY(w.refs.putFrame(w.triggerId, 2, 0, greyPng(20)));
        w.pets = {{150, 220, 80, 60}, {2100, 100, 100, 100}, {-800, 300, 100, 100}, {9000, 9000, 10, 10}};
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QVERIFY(waitLines(w, u"start\n"_s));
        const QString lines = linesOf(w);
        QVERIFY2(lines.contains(u"region %1 0 0 100 200 300 120\n"_s.arg(w.triggerId)), qPrintable(lines));  // the 1x monitor: as it is
        QVERIFY2(lines.contains(u"region %1 1 1 160 400 100 50\n"_s.arg(w.triggerId)), qPrintable(lines));   // the 2x monitor, in ITS pixels: 80 in, 200 down, doubled
        QVERIFY2(!lines.contains(u"region %1 2 "_s.arg(w.triggerId)), qPrintable(lines));                    // the monitor of an unknown scale: not guessed
        const QJsonArray notes = w.watch->stateJson().value(u"notes"_s).toArray();
        QStringList all;
        for (const QJsonValue &n : notes)
            all << n.toString();
        QVERIFY2(all.join(u"|"_s).contains(u"scale of that screen is not known"_s), qPrintable(all.join(u"|"_s)));
        QCOMPARE(w.watch->stateJson().value(u"regions"_s).toInt(), 2);
        // our own pets: one on each monitor whose scale is known, in that monitor's pixels; the one of the unknown monitor and the one off every screen are left out
        QVERIFY2(lines.contains(u"pets 2 0 150 220 80 60 1 360 600 200 200\n"_s), qPrintable(lines));
    }
    void aPendingCaptureLivesInDumpFreeMemoryAndIsWipedWhenItEnds()
    {
        bringUp();
        World w;
        w.pendingMs = 600;
        make(w);
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QSignalSpy captured(w.watch.get(), &ScreenWatch::captured);
        int wiped = 0, notZero = 0;
        SecureBytes::setWipeObserver([&](const char *, qsizetype, bool allZero) {
            ++wiped;
            notZero += allZero ? 0 : 1;
        });
        const auto cleanup = qScopeGuard([] { SecureBytes::setWipeObserver({}); });
        QString why;
        const int before = SecureBytes::liveBuffers();
        // held: one buffer for the picture, nothing else of it kept in the daemon; a wipe happens whenever it ends
        const auto capture = [&](int steps) {
            const QString request = w.watch->capture(w.triggerId, 0, steps, &why);
            return QTest::qWaitFor([&] { return captured.size() > 0; }, 5000) ? request : QString();
        };
        QString request = capture(1);
        QVERIFY2(!request.isEmpty(), qPrintable(why));
        QCOMPARE(SecureBytes::liveBuffers(), before + 1);
        // the mapping is kept out of a dump (the flag "dd" in smaps)
        QVERIFY(SecureBytes::liveBytes() > 0);
        // kept (committed): wiped
        captured.clear();
        wiped = 0;
        QVERIFY2(w.watch->commitCapture(request, &why), qPrintable(why));
        QCOMPARE(SecureBytes::liveBuffers(), before);
        QCOMPARE(wiped, 1);
        // discarded
        request = capture(2);
        QVERIFY(!request.isEmpty());
        QCOMPARE(SecureBytes::liveBuffers(), before + 2); // (a sequence: one buffer a picture)
        captured.clear();
        wiped = 0;
        QVERIFY(w.watch->discardCapture(request));
        QCOMPARE(SecureBytes::liveBuffers(), before);
        QCOMPARE(wiped, 2);
        // expired: nobody kept it
        request = capture(1);
        QVERIFY(!request.isEmpty());
        captured.clear();
        wiped = 0;
        QTRY_COMPARE_WITH_TIMEOUT(SecureBytes::liveBuffers(), before, 3000);
        QCOMPARE(wiped, 1);
        // stopped
        request = capture(1);
        QVERIFY(!request.isEmpty());
        captured.clear();
        wiped = 0;
        w.watch->stop();
        QCOMPARE(SecureBytes::liveBuffers(), before);
        QCOMPARE(wiped, 1);
        // a capture that is still running when the watch stops leaves nothing either
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QVERIFY(!w.watch->capture(w.triggerId, 0, 3, &why).isEmpty());
        w.watch->stop();
        QTRY_COMPARE_WITH_TIMEOUT(SecureBytes::liveBuffers(), before, 3000);
        QCOMPARE(notZero, 0); // every buffer was all zeroes when it was given back
    }
    void aCaptureCanBeDiscarded()
    {
        bringUp();
        World w;
        make(w);
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QSignalSpy captured(w.watch.get(), &ScreenWatch::captured);
        QString why;
        const QString request = w.watch->capture(w.triggerId, 0, 1, &why);
        QTRY_COMPARE_WITH_TIMEOUT(captured.size(), 1, 5000);
        QVERIFY(w.watch->discardCapture(request));
        QVERIFY(!w.watch->discardCapture(request));
        QVERIFY(!w.watch->commitCapture(request, &why));
        QVERIFY(!w.watch->capture(w.triggerId, 0, 2, &why).isEmpty());
    }
    void aKeptCaptureReplacesTheWholeSetAndNeedsItsTrigger()
    {
        bringUp();
        World w;
        make(w);
        for (int step = 1; step < 4; ++step)
            QVERIFY(w.refs.putFrame(w.triggerId, 0, step, greyPng(step * 20))); // an old run of four
        QCOMPARE(w.refs.steps(w.triggerId, 0).size(), 4);
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QSignalSpy captured(w.watch.get(), &ScreenWatch::captured);
        QString why;
        QString request = w.watch->capture(w.triggerId, 0, 1, &why);
        QTRY_COMPARE_WITH_TIMEOUT(captured.size(), 1, 5000);
        QVERIFY(w.watch->commitCapture(request, &why));
        QCOMPARE(w.refs.steps(w.triggerId, 0), (QList<int>{0})); // the old steps 1 to 3 are gone, not mixed into the new run
        // the trigger goes between the capture and the keeping: nothing is written
        request = w.watch->capture(w.triggerId, 0, 1, &why);
        QTRY_COMPARE_WITH_TIMEOUT(captured.size(), 2, 5000);
        QVERIFY(w.triggers.remove(w.triggerId));
        QVERIFY(!w.watch->commitCapture(request, &why));
        QVERIFY2(why.contains(u"gone"_s), qPrintable(why));
    }
    void aCaptureTheHelperDoesNotAnswerOrDiesDuringIsFailedNotStuck()
    {
        bringUp();
        {
            World w;
            w.jobMs = 300;
            make(w);
            testutil::writeFile(w.dir.filePath(u"mode"_s), "silentcapture");
            QVERIFY(w.watch->start());
            QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
            QSignalSpy captured(w.watch.get(), &ScreenWatch::captured);
            QString why;
            QVERIFY(!w.watch->capture(w.triggerId, 0, 1, &why).isEmpty());
            QTRY_COMPARE_WITH_TIMEOUT(captured.size(), 1, 5000);
            QVERIFY(captured.first().at(1).toString().contains(u"did not answer"_s));
            QVERIFY(!w.watch->capture(w.triggerId, 0, 1, &why).isEmpty()); // not "already running"
        }
        {
            World w;
            make(w);
            testutil::writeFile(w.dir.filePath(u"mode"_s), "crashoncapture");
            QVERIFY(w.watch->start());
            QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
            QSignalSpy captured(w.watch.get(), &ScreenWatch::captured);
            QString why;
            QVERIFY(!w.watch->capture(w.triggerId, 0, 1, &why).isEmpty());
            QTRY_COMPARE_WITH_TIMEOUT(captured.size(), 1, 5000);
            QVERIFY(captured.first().at(1).toString().contains(u"error"_s));
            QVERIFY(waitState(*w.watch, ScreenWatch::State::Failed));
            QVERIFY(w.watch->capture(w.triggerId, 0, 1, &why).isEmpty()); // not watching
            QVERIFY(!why.contains(u"already running"_s));
        }
    }
    void editingAnotherTriggerDoesNotTouchTheWatch()
    {
        bringUp();
        World w;
        make(w);
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QVERIFY(waitLines(w, u"start\n"_s));
        const int clears = linesOf(w).count(u"clear\n"_s);
        Trigger other;
        other.name = u"fire"_s;
        other.source.kind = TriggerKind::Fire;
        other.source.name = u"x"_s;
        other.action.type = TriggerActionType::HideAll;
        QVERIFY(!w.triggers.add(other).isEmpty());
        w.watch->triggersChanged();
        QTest::qWait(400);
        QCOMPARE(linesOf(w).count(u"clear\n"_s), clears); // no new setup, so nothing held is undone and redone
        Trigger t = *w.triggers.trigger(w.triggerId);
        t.source.screen.insert(u"regions"_s, QJsonArray{QJsonObject{{u"kind"_s, u"rect"_s}, {u"x"_s, 5}, {u"y"_s, 5}, {u"w"_s, 40}, {u"h"_s, 40}}});
        w.triggers.update(t);
        w.watch->triggersChanged();
        QTRY_COMPARE_WITH_TIMEOUT(linesOf(w).count(u"clear\n"_s), clears + 1, 5000);
    }
    void withoutRememberingNoTokenIsAskedForOrKept()
    {
        bringUp();
        World w;
        make(w);
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QVERIFY(!portal->selectOptions.at(0).contains(u"persist_mode"_s));
        QVERIFY(!portal->selectOptions.at(0).contains(u"restore_token"_s));
        QVERIFY(!QFileInfo::exists(w.dir.filePath(u"screen-state.json"_s)) || QFile(w.dir.filePath(u"screen-state.json"_s)).size() <= 4); // (nothing, or an empty object)
    }
    void rememberingKeepsTheTokenAndSendsItNextTime()
    {
        bringUp();
        World w;
        make(w);
        QVERIFY(w.settings.merge(QJsonObject{{u"screenRemember"_s, true}}));
        QVERIFY(w.watch->start());
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QCOMPARE(portal->selectOptions.at(0).value(u"persist_mode"_s).toUInt(), 2u);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(w.dir.filePath(u"screen-state.json"_s)), 3000);
        QFile f(w.dir.filePath(u"screen-state.json"_s));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(f.readAll()).object().value(u"restoreToken"_s).toString(), u"token-from-the-portal"_s);
        w.watch->stop();
        QTRY_COMPARE(session.closed, 1);
        QVERIFY(w.watch->start()); // the next session sends the token it kept
        QVERIFY(waitState(*w.watch, ScreenWatch::State::Watching));
        QCOMPARE(portal->selectOptions.size(), 2);
        QCOMPARE(portal->selectOptions.at(1).value(u"restore_token"_s).toString(), u"token-from-the-portal"_s);
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    TstScreenWatch test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_screenwatch.moc"
