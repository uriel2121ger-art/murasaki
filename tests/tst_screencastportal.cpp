// The client of the screen cast portal (src/daemon/screencastportal.*) against a FAKE portal on a private bus: the happy path with one and several monitors, a refusal, the desktop closing the
// session, a dialog nobody answers, the restore token (sent only with "remember", used, refused and dropped), the descriptor (owned, close-on-exec, handed over once), cancellation, and replies that
// are not as documented. The fake takes the well-known name of the user's portal, so it only ever runs on an isolated bus (MURASAKI_TEST_PRIVATE_BUS=1).

#include "screencastportal.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QElapsedTimer>
#include <QSignalSpy>
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
    Q_PROPERTY(uint AvailableCursorModes READ cursorModes SCRIPTABLE true)

public:
    uint cursorModes() const { return availableCursorModes; }
    uint availableCursorModes = 7;        // hidden, embedded and metadata
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

Q_DECLARE_METATYPE(murasaki::ScreenCastPortal::State)

class TstScreenCastPortal : public QObject
{
    Q_OBJECT

private:
    QDBusConnection server = QDBusConnection::sessionBus();
    QDBusConnection client{u"unused"_s};
    std::unique_ptr<FakePortal> portal;
    FakeSession session;
    bool m_up = false;
    int m_pipe[2] = {-1, -1};

    bool exportProperty = true; // false: a portal that does not have AvailableCursorModes
    void bringUp()
    {
        QVERIFY(server.registerObject(u"/org/freedesktop/portal/desktop"_s, portal.get(),
                                      QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals | (exportProperty ? QDBusConnection::ExportScriptableProperties : QDBusConnection::ExportScriptableSlots)));
        QVERIFY(server.registerService(u"org.freedesktop.portal.Desktop"_s));
        portal->sessionObject = &session;
        m_up = true;
    }
    static bool waitState(ScreenCastPortal &p, ScreenCastPortal::State s, int ms = 5000) { return QTest::qWaitFor([&] { return p.state() == s; }, ms); }

private slots:
    void initTestCase()
    {
        if (qEnvironmentVariableIntValue("MURASAKI_TEST_PRIVATE_BUS") != 1)
            QSKIP("needs a private session bus (run through ctest, or set MURASAKI_TEST_PRIVATE_BUS=1 under dbus-run-session)");
        QVERIFY(server.isConnected());
        qDBusRegisterMetaType<IntPair>();
        qDBusRegisterMetaType<FakeStream>();
        qDBusRegisterMetaType<QList<FakeStream>>();
        qRegisterMetaType<murasaki::ScreenCastPortal::State>();
        client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"murasaki-test-screencast-client"_s);
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
        QTest::qWait(100); // (Close calls still on their way belong to this test, not to the next)
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

    void theCursorModeFollowsWhatThePortalOffers()
    {
        struct Case {
            uint offered;
            bool property;
            int expected; // 0: the option is left out
        };
        for (const Case c : {Case{7, true, 1}, Case{1, true, 1}, Case{6, true, 4}, Case{4, true, 4}, Case{2, true, 2}, Case{0, true, 0}, Case{0, false, 1}}) {
            init();
            exportProperty = c.property;
            portal->availableCursorModes = c.offered;
            bringUp();
            ScreenCastPortal p(client);
            p.start({});
            QVERIFY2(waitState(p, ScreenCastPortal::State::Watching), qPrintable(QStringLiteral("offered %1").arg(c.offered)));
            QCOMPARE(portal->selectOptions.size(), 1);
            if (c.expected == 0)
                QVERIFY(!portal->selectOptions.at(0).contains(u"cursor_mode"_s));
            else
                QCOMPARE(portal->selectOptions.at(0).value(u"cursor_mode"_s).toUInt(), uint(c.expected));
            ::close(p.takeRemoteFd());
            cleanup();
        }
        exportProperty = true;
    }
    void oneMonitorIsSharedAndTheRemoteHandedOverOnce()
    {
        bringUp();
        ScreenCastPortal p(client);
        QSignalSpy states(&p, &ScreenCastPortal::stateChanged);
        QCOMPARE(p.state(), ScreenCastPortal::State::Idle);
        p.start({});
        QCOMPARE(p.state(), ScreenCastPortal::State::Asking);
        QVERIFY(waitState(p, ScreenCastPortal::State::Watching));
        QCOMPARE(states.size(), 2); // Asking, Watching
        QCOMPARE(p.streams().size(), 1);
        QCOMPARE(p.streams().at(0), (ScreenCastPortal::Stream{41, 0, 0, 1920, 1080, u"DP-1"_s, true}));
        QVERIFY(p.reason().isEmpty());
        // what the portal was asked: monitors, several, no cursor; and nothing about remembering
        QCOMPARE(portal->selectOptions.size(), 1);
        const QVariantMap o = portal->selectOptions.at(0);
        QCOMPARE(o.value(u"types"_s).toUInt(), 1u);
        QCOMPARE(o.value(u"multiple"_s).toBool(), true);
        QCOMPARE(o.value(u"cursor_mode"_s).toUInt(), 1u);
        QVERIFY(!o.contains(u"persist_mode"_s) && !o.contains(u"restore_token"_s));
        // the descriptor: ours, close-on-exec, not the portal's, once
        const int pending = p.pendingRemoteFd();
        QVERIFY(pending >= 3);
        QVERIFY(pending != m_pipe[0]);
        QVERIFY(::fcntl(pending, F_GETFD) & FD_CLOEXEC);
        const int fd = p.takeRemoteFd();
        QCOMPARE(fd, pending);
        QCOMPARE(p.takeRemoteFd(), -1);
        QCOMPARE(p.pendingRemoteFd(), -1);
        // it is a duplicate of the pipe end the fake sent: data written to the pipe is readable from it
        const char byte = 'x';
        QCOMPARE(::write(m_pipe[1], &byte, 1), ssize_t(1));
        char got = 0;
        QCOMPARE(::read(fd, &got, 1), ssize_t(1));
        QCOMPARE(got, 'x');
        ::close(fd);
        // nothing was sent that remembers the choice
        QCOMPARE(portal->creates, 1);
        QCOMPARE(portal->starts, 1);
        QCOMPARE(portal->opens, 1);
    }

    void severalMonitorsKeepTheirGeometryInOrder()
    {
        portal->streams = {monitor(41, 0, 0, 1920, 1080, u"DP-1"_s), monitor(42, 1920, -200, 2560, 1440, u"HDMI-1"_s), monitor(43, -1080, 0, 1080, 1920)};
        bringUp();
        ScreenCastPortal p(client);
        p.start({});
        QVERIFY(waitState(p, ScreenCastPortal::State::Watching));
        const auto s = p.streams();
        QCOMPARE(s.size(), 3);
        QCOMPARE(s.at(1), (ScreenCastPortal::Stream{42, 1920, -200, 2560, 1440, u"HDMI-1"_s, true}));
        QCOMPARE(s.at(2), (ScreenCastPortal::Stream{43, -1080, 0, 1080, 1920, QString(), true}));
        ::close(p.takeRemoteFd());
    }

    void theUserRefusing()
    {
        portal->startCode = 1;
        bringUp();
        ScreenCastPortal p(client);
        p.start({});
        QVERIFY(waitState(p, ScreenCastPortal::State::Failed));
        QCOMPARE(p.reason(), QString(kScreenPermissionRefused));
        QCOMPARE(p.pendingRemoteFd(), -1);
        QTRY_COMPARE(session.closed, 1); // the session is closed on the portal's side
        QCOMPARE(portal->opens, 0);
        // and a new attempt works
        portal->startCode = 0;
        p.start({});
        QVERIFY(waitState(p, ScreenCastPortal::State::Watching));
        QVERIFY(p.reason().isEmpty());
        ::close(p.takeRemoteFd());
    }

    void anotherEndOfTheRequestIsAnErrorNotARefusal()
    {
        portal->startCode = 2;
        bringUp();
        ScreenCastPortal p(client);
        p.start({});
        QVERIFY(waitState(p, ScreenCastPortal::State::Failed));
        QCOMPARE(p.reason(), QString(kScreenPortalError));
    }

    void theDesktopClosingTheSessionWhileWatching()
    {
        bringUp();
        ScreenCastPortal p(client);
        p.start({});
        QVERIFY(waitState(p, ScreenCastPortal::State::Watching));
        emit session.Closed({});
        QVERIFY(waitState(p, ScreenCastPortal::State::Failed));
        QCOMPARE(p.reason(), QString(kScreenStoppedByDesktop));
        QCOMPARE(p.pendingRemoteFd(), -1); // an untaken remote is closed with the session
        QVERIFY(p.streams().isEmpty());
        QCOMPARE(session.closed, 0); // (it is already closed: nothing is asked of it)
    }

    void theSessionClosingBeforeTheDialogIsAnswered()
    {
        portal->silentStart = true;
        bringUp();
        ScreenCastPortal p(client);
        p.start({});
        QTRY_COMPARE_WITH_TIMEOUT(portal->starts, 1, 5000);
        QCOMPARE(p.state(), ScreenCastPortal::State::Asking);
        emit session.Closed({});
        QVERIFY(waitState(p, ScreenCastPortal::State::Failed));
        QCOMPARE(p.reason(), QString(kScreenStoppedByDesktop));
    }

    void aDialogNobodyAnswersEndsInAskingThenFailed()
    {
        portal->silentStart = true;
        bringUp();
        ScreenCastPortal p(client);
        ScreenCastPortal::Options o;
        o.askTimeoutMs = 400;
        p.start(o);
        QTRY_COMPARE_WITH_TIMEOUT(portal->starts, 1, 5000);
        QCOMPARE(p.state(), ScreenCastPortal::State::Asking);
        QTest::qWait(200);
        QCOMPARE(p.state(), ScreenCastPortal::State::Asking); // still waiting for a person
        QVERIFY(waitState(p, ScreenCastPortal::State::Failed));
        QCOMPARE(p.reason(), QString(kScreenPortalSilent));
        QTRY_COMPARE(session.closed, 1);
        QTRY_COMPARE(portal->requestCloses, 1); // the dialog is taken away, not left open
        QCOMPARE(portal->closedRequests, QStringList{portal->lastStartPath});
    }

    void aStepTheDesktopNeverAnswersTimesOutSooner()
    {
        portal->silentCreate = true;
        bringUp();
        ScreenCastPortal p(client);
        ScreenCastPortal::Options o;
        o.stepTimeoutMs = 300;
        o.askTimeoutMs = 60000;
        QElapsedTimer t;
        t.start();
        p.start(o);
        QVERIFY(waitState(p, ScreenCastPortal::State::Failed));
        QCOMPARE(p.reason(), QString(kScreenPortalSilent));
        QVERIFY(t.elapsed() < 5000);
        QCOMPARE(portal->starts, 0);
    }

    void noPortalAtAll()
    {
        // (nothing is registered under the portal's name)
        ScreenCastPortal p(client);
        p.start({});
        QVERIFY(waitState(p, ScreenCastPortal::State::Failed));
        QCOMPARE(p.reason(), QString(kScreenPortalMissing));
        ScreenCastPortal gone{QDBusConnection(u"never-connected"_s)};
        gone.start({});
        QCOMPARE(gone.state(), ScreenCastPortal::State::Failed);
        QCOMPARE(gone.reason(), QString(kScreenPortalMissing));
    }

    void stoppingCancelsTheDialogAndTheSession()
    {
        portal->silentStart = true;
        bringUp();
        ScreenCastPortal p(client);
        p.start({});
        QTRY_COMPARE_WITH_TIMEOUT(portal->starts, 1, 5000);
        p.stop(); // Close on the request of Start is what takes the dialog away
        QCOMPARE(p.state(), ScreenCastPortal::State::Idle);
        QVERIFY(p.reason().isEmpty());
        QTRY_COMPARE(session.closed, 1);
        QTRY_COMPARE(portal->requestCloses, 1);
        // late answers of the old run do nothing
        QTest::qWait(100);
        QCOMPARE(p.state(), ScreenCastPortal::State::Idle);
    }

    void stoppingClosesAnUntakenDescriptor()
    {
        bringUp();
        ScreenCastPortal p(client);
        p.start({});
        QVERIFY(waitState(p, ScreenCastPortal::State::Watching));
        const int fd = p.pendingRemoteFd();
        QVERIFY(::fcntl(fd, F_GETFD) >= 0);
        p.stop();
        QVERIFY(::fcntl(fd, F_GETFD) < 0);
        QCOMPARE(errno, EBADF);
        QCOMPARE(p.takeRemoteFd(), -1);
        QCOMPARE(p.state(), ScreenCastPortal::State::Idle);
    }

    void aSecondStartReplacesTheFirst()
    {
        portal->silentStart = true;
        bringUp();
        ScreenCastPortal p(client);
        p.start({});
        QTRY_COMPARE_WITH_TIMEOUT(portal->starts, 1, 5000);
        portal->silentStart = false;
        p.start({});
        QVERIFY(waitState(p, ScreenCastPortal::State::Watching));
        QCOMPARE(portal->creates, 2);
        QCOMPARE(p.streams().size(), 1);
        ::close(p.takeRemoteFd());
    }

    // ---- Senders, stale replies and other orders of events ----
    void aForgedResponseOrClosedFromAnotherClientIsIgnored()
    {
        portal->silentStart = true;
        bringUp();
        ScreenCastPortal p(client);
        p.start({});
        QTRY_COMPARE_WITH_TIMEOUT(portal->starts, 1, 5000);
        QDBusConnection rogue = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"murasaki-test-screencast-rogue"_s);
        QVERIFY(rogue.isConnected());
        // a Response for the very request that is open, with streams and a good code: it is not from the portal
        QDBusMessage response = QDBusMessage::createSignal(portal->lastStartPath, u"org.freedesktop.portal.Request"_s, u"Response"_s);
        response.setArguments({uint(0), QVariantMap{{u"streams"_s, QVariant::fromValue(QList<FakeStream>{monitor(99, 0, 0, 10, 10)})}}});
        QVERIFY(rogue.send(response));
        // and a Closed on our session
        QDBusMessage closed = QDBusMessage::createSignal(portal->session, u"org.freedesktop.portal.Session"_s, u"Closed"_s);
        closed.setArguments({QVariantMap{}});
        QVERIFY(rogue.send(closed));
        QTest::qWait(400);
        QCOMPARE(p.state(), ScreenCastPortal::State::Asking);
        QVERIFY(p.streams().isEmpty());
        QCOMPARE(portal->opens, 0);
        // the real portal answering afterwards still works
        portal->silentStart = false;
        p.start({});
        QVERIFY(waitState(p, ScreenCastPortal::State::Watching));
        QCOMPARE(p.streams().at(0).node, 41u);
        ::close(p.takeRemoteFd());
        QDBusConnection::disconnectFromBus(u"murasaki-test-screencast-rogue"_s);
    }

    void aResponseThatBeatsTheMethodReplyIsAccepted()
    {
        portal->responseFirst = true;
        bringUp();
        ScreenCastPortal p(client);
        p.start({});
        QVERIFY(waitState(p, ScreenCastPortal::State::Watching));
        QCOMPARE(p.streams().size(), 1);
        ::close(p.takeRemoteFd());
    }

    void aLateMethodReplyDoesNotAimTheCloseAtTheWrongRequest()
    {
        portal->lateReplySelect = true; // SelectSources: Response now, method reply 100 ms later, while Start is already open
        portal->silentStart = true;
        bringUp();
        ScreenCastPortal p(client);
        p.start({});
        QTRY_COMPARE_WITH_TIMEOUT(portal->starts, 1, 5000);
        QTest::qWait(300); // the late reply of SelectSources has come
        p.stop();
        QTRY_COMPARE(portal->requestCloses, 1);
        QCOMPARE(portal->closedRequests, QStringList{portal->lastStartPath}); // the dialog's request, not the finished one
    }

    void stoppingBetweenStartAndOpenRemoteLeavesNothing()
    {
        portal->delayOpenMs = 300;
        bringUp();
        const auto openFds = [] {
            int n = 0;
            for (int fd = 3; fd < 256; ++fd)
                n += ::fcntl(fd, F_GETFD) >= 0;
            return n;
        };
        ScreenCastPortal p(client);
        p.start({});
        QTRY_COMPARE_WITH_TIMEOUT(portal->opens, 1, 5000);
        const int before = openFds();
        p.stop();
        QTest::qWait(600); // the reply with its descriptor arrives into nothing
        QCOMPARE(p.state(), ScreenCastPortal::State::Idle);
        QCOMPARE(p.pendingRemoteFd(), -1);
        QCOMPARE(openFds(), before);
        QTRY_COMPARE(session.closed, 1);
    }

    void destroyingWithACallPendingIsSafeAndSilent()
    {
        portal->delayOpenMs = 300;
        bringUp();
        auto *p = new ScreenCastPortal(client);
        QSignalSpy states(p, &ScreenCastPortal::stateChanged);
        p->start({});
        QTRY_COMPARE_WITH_TIMEOUT(portal->opens, 1, 5000);
        delete p;
        QCOMPARE(states.size(), 1); // Asking; no Idle announced from the destructor
        QTest::qWait(600);          // the late reply finds nobody
        auto *q = new ScreenCastPortal(client);
        portal->silentStart = true;
        q->start({});
        QTRY_COMPARE_WITH_TIMEOUT(portal->starts, 2, 5000);
        delete q;
        QTRY_COMPARE(portal->requestCloses, 1); // and a dialog open at that moment is taken away
    }

    void aRestoreTokenThatIsNotPlainTextIsNotKept_data()
    {
        QTest::addColumn<QString>("token");
        QTest::newRow("too long") << QString(300, u'a');
        QTest::newRow("a newline") << u"abc\ndef"_s;
        QTest::newRow("a control byte") << QString(u"abc"_s + QChar(1) + u"def"_s);
    }
    void aRestoreTokenThatIsNotPlainTextIsNotKept()
    {
        QFETCH(QString, token);
        portal->restoreTokenReply = token;
        bringUp();
        ScreenCastPortal p(client);
        QSignalSpy tokens(&p, &ScreenCastPortal::restoreToken);
        ScreenCastPortal::Options o;
        o.remember = true;
        p.start(o);
        QVERIFY(waitState(p, ScreenCastPortal::State::Watching)); // the cast itself is fine
        QCOMPARE(tokens.size(), 0);
        ::close(p.takeRemoteFd());
    }

    void aSingleStreamWithoutGeometryIsTheWholeOutput()
    {
        FakeStream s;
        s.node = 77;
        portal->streams = {s};
        bringUp();
        ScreenCastPortal p(client);
        p.start({});
        QVERIFY(waitState(p, ScreenCastPortal::State::Watching));
        QCOMPARE(p.streams().size(), 1);
        QVERIFY(!p.streams().at(0).hasGeometry);
        QCOMPARE(p.streams().at(0).node, 77u);
        ::close(p.takeRemoteFd());
    }

    void aFailedRunDoesNotKeepASpentToken()
    {
        portal->startCode = 1;
        bringUp();
        ScreenCastPortal p(client);
        ScreenCastPortal::Options o;
        o.remember = true;
        o.restoreToken = u"single-use"_s;
        p.start(o);
        QVERIFY(waitState(p, ScreenCastPortal::State::Failed));
        QCOMPARE(portal->selectOptions.at(0).value(u"restore_token"_s).toString(), u"single-use"_s);
        // the object does not send it again by itself: a new run with no token given asks without one
        portal->startCode = 0;
        ScreenCastPortal::Options again;
        again.remember = true;
        p.start(again);
        QVERIFY(waitState(p, ScreenCastPortal::State::Watching));
        QVERIFY(!portal->selectOptions.last().contains(u"restore_token"_s));
        ::close(p.takeRemoteFd());
    }

    // ---- The restore token ----
    void rememberAsksToPersistAndStoresTheToken()
    {
        bringUp();
        ScreenCastPortal p(client);
        QSignalSpy tokens(&p, &ScreenCastPortal::restoreToken);
        ScreenCastPortal::Options o;
        o.remember = true;
        p.start(o);
        QVERIFY(waitState(p, ScreenCastPortal::State::Watching));
        const QVariantMap sent = portal->selectOptions.at(0);
        QCOMPARE(sent.value(u"persist_mode"_s).toUInt(), 2u);
        QVERIFY(!sent.contains(u"restore_token"_s)); // none stored yet
        QCOMPARE(tokens.size(), 1);
        QCOMPARE(tokens.at(0).at(0).toString(), u"token-from-the-portal"_s);
        ::close(p.takeRemoteFd());
    }

    void withoutRememberNoTokenIsSentOrKept()
    {
        bringUp();
        ScreenCastPortal p(client);
        QSignalSpy tokens(&p, &ScreenCastPortal::restoreToken);
        ScreenCastPortal::Options o;
        o.remember = false;
        o.restoreToken = u"an-old-token"_s; // a stored token is not used when the setting is off
        p.start(o);
        QVERIFY(waitState(p, ScreenCastPortal::State::Watching));
        const QVariantMap sent = portal->selectOptions.at(0);
        QVERIFY(!sent.contains(u"persist_mode"_s) && !sent.contains(u"restore_token"_s));
        QCOMPARE(tokens.size(), 0); // the portal offered one (the fake always does): ignored
        ::close(p.takeRemoteFd());
    }

    void aStoredTokenIsUsed()
    {
        bringUp();
        ScreenCastPortal p(client);
        QSignalSpy refused(&p, &ScreenCastPortal::restoreTokenRefused);
        ScreenCastPortal::Options o;
        o.remember = true;
        o.restoreToken = u"stored-token"_s;
        p.start(o);
        QVERIFY(waitState(p, ScreenCastPortal::State::Watching));
        QCOMPARE(portal->selectOptions.size(), 1);
        QCOMPARE(portal->selectOptions.at(0).value(u"restore_token"_s).toString(), u"stored-token"_s);
        QCOMPARE(refused.size(), 0);
        ::close(p.takeRemoteFd());
    }

    void aRefusedTokenFallsBackToTheDialog_data()
    {
        QTest::addColumn<bool>("byResponse");
        QTest::newRow("a D-Bus error") << false;
        QTest::newRow("a refused request") << true;
    }
    void aRefusedTokenFallsBackToTheDialog()
    {
        QFETCH(bool, byResponse);
        portal->refuseToken = !byResponse;
        portal->refuseTokenByResponse = byResponse;
        bringUp();
        ScreenCastPortal p(client);
        QSignalSpy refused(&p, &ScreenCastPortal::restoreTokenRefused);
        QSignalSpy tokens(&p, &ScreenCastPortal::restoreToken);
        ScreenCastPortal::Options o;
        o.remember = true;
        o.restoreToken = u"expired-token"_s;
        p.start(o);
        QVERIFY(waitState(p, ScreenCastPortal::State::Watching));
        QCOMPARE(refused.size(), 1);
        QCOMPARE(portal->selectOptions.size(), 2);
        QVERIFY(portal->selectOptions.at(0).contains(u"restore_token"_s));
        QVERIFY(!portal->selectOptions.at(1).contains(u"restore_token"_s)); // asked again without it
        QCOMPARE(portal->selectOptions.at(1).value(u"persist_mode"_s).toUInt(), 2u);
        QCOMPARE(portal->creates, 1); // the same session
        QCOMPARE(tokens.size(), 1);   // and a fresh token to keep
        ::close(p.takeRemoteFd());
    }

    void selectSourcesFailingWithoutATokenFails()
    {
        portal->failSelect = true;
        bringUp();
        ScreenCastPortal p(client);
        p.start({});
        QVERIFY(waitState(p, ScreenCastPortal::State::Failed));
        QCOMPARE(p.reason(), QString(kScreenPortalError));
        QCOMPARE(portal->selects, 1); // no retry: there was no token to drop
    }

    void anUnusableStoredTokenIsNeverSent()
    {
        bringUp();
        ScreenCastPortal p(client);
        ScreenCastPortal::Options o;
        o.remember = true;
        o.restoreToken = QString(u"x"_s).repeated(5000) ;
        p.start(o);
        QVERIFY(waitState(p, ScreenCastPortal::State::Watching));
        QVERIFY(!portal->selectOptions.at(0).contains(u"restore_token"_s));
        ::close(p.takeRemoteFd());
    }

    // ---- Replies that are not as documented ----
    void malformedRepliesAreRefusedByName_data()
    {
        QTest::addColumn<int>("what");
        QTest::addColumn<QString>("detail");
        QTest::newRow("no session handle") << 0 << u"session_handle"_s;
        QTest::newRow("session handle of the wrong type") << 1 << u"session_handle"_s;
        QTest::newRow("session handle outside the portal's namespace") << 2 << u"session_handle"_s;
        QTest::newRow("streams of the wrong type") << 3 << u"streams"_s;
        QTest::newRow("streams missing") << 4 << u"streams"_s;
        QTest::newRow("node id zero") << 5 << u"node id"_s;
        QTest::newRow("a position without a size") << 6 << u"but not both"_s;
        QTest::newRow("zero width") << 7 << u"out of range"_s;
        QTest::newRow("negative height") << 8 << u"out of range"_s;
        QTest::newRow("huge size") << 9 << u"out of range"_s;
        QTest::newRow("huge position") << 10 << u"out of range"_s;
        QTest::newRow("nine streams") << 11 << u"more than 8"_s;
        QTest::newRow("a huge array") << 12 << u"more than 8"_s;
        QTest::newRow("a node twice") << 13 << u"twice"_s;
        QTest::newRow("geometry of the wrong type") << 14 << u"not (ii)"_s;
        QTest::newRow("the smallest int as a position") << 16 << u"out of range"_s;
        QTest::newRow("a window instead of a monitor") << 17 << u"not a monitor"_s;
        QTest::newRow("several streams without geometry") << 18 << u"without position and size"_s;
        QTest::newRow("a session handle of another client") << 19 << u"session_handle"_s;
        QTest::newRow("an id that is not text") << 15 << u"id"_s;
    }
    void malformedRepliesAreRefusedByName()
    {
        QFETCH(int, what);
        QFETCH(QString, detail);
        switch (what) {
        case 0: portal->noSessionHandle = true; break;
        case 1: portal->sessionHandle = 42; break;
        case 2: portal->sessionHandle = u"/etc/passwd"_s; break;
        case 3: portal->streamsOverride = u"not an array"_s; break;
        case 4: portal->omitStreams = true; break;
        case 5: portal->streams = {monitor(0, 0, 0, 100, 100)}; break;
        case 6: { FakeStream s; s.node = 5; s.props = {{u"position"_s, pairVariant(0, 0)}}; portal->streams = {s}; break; }
        case 7: portal->streams = {monitor(5, 0, 0, 0, 100)}; break;
        case 8: portal->streams = {monitor(5, 0, 0, 100, -3)}; break;
        case 9: portal->streams = {monitor(5, 0, 0, 100000, 100)}; break;
        case 10: portal->streams = {monitor(5, 99999999, 0, 100, 100)}; break;
        case 11: portal->streams.clear(); for (uint i = 1; i <= 9; ++i) portal->streams << monitor(i, int(i) * 100, 0, 100, 100); break;
        case 12: portal->streams.clear(); for (uint i = 1; i <= 5000; ++i) portal->streams << monitor(i, 0, 0, 10, 10); break;
        case 13: portal->streams = {monitor(7, 0, 0, 100, 100), monitor(7, 100, 0, 100, 100)}; break;
        case 14: { FakeStream s; s.node = 5; s.props = {{u"position"_s, u"0,0"_s}, {u"size"_s, u"100x100"_s}}; portal->streams = {s}; break; }
        case 16: portal->streams = {monitor(5, std::numeric_limits<int>::min(), 0, 100, 100)}; break;
        case 17: { FakeStream s = monitor(5, 0, 0, 100, 100); s.props.insert(u"source_type"_s, uint(2)); portal->streams = {s}; break; }
        case 18: { FakeStream a; a.node = 5; FakeStream b; b.node = 6; portal->streams = {a, b}; break; }
        case 19: portal->sessionHandle = u"/org/freedesktop/portal/desktop/session/other_client/murasaki_session_1"_s; break;
        case 15: { FakeStream s = monitor(5, 0, 0, 100, 100); s.props.insert(u"id"_s, 7); portal->streams = {s}; break; }
        }
        bringUp();
        ScreenCastPortal p(client);
        p.start({});
        QVERIFY2(waitState(p, ScreenCastPortal::State::Failed), qPrintable(QString::number(what)));
        QVERIFY2(p.reason().startsWith(QString(kScreenPortalMalformed)), qPrintable(p.reason()));
        QVERIFY2(p.reason().contains(detail), qPrintable(p.reason()));
        QCOMPARE(p.pendingRemoteFd(), -1);
        QVERIFY(p.streams().isEmpty());
        QCOMPARE(portal->opens, 0); // nothing was asked after a bad reply
    }

    void noStreamsMeansNoMonitorShared()
    {
        portal->streams.clear();
        bringUp();
        ScreenCastPortal p(client);
        p.start({});
        QVERIFY(waitState(p, ScreenCastPortal::State::Failed));
        QCOMPARE(p.reason(), QString(kScreenNoMonitors));
        QCOMPARE(portal->opens, 0);
    }

    void openRemoteFailingIsThePortalsError()
    {
        portal->openRemoteError = true;
        bringUp();
        ScreenCastPortal p(client);
        p.start({});
        QVERIFY(waitState(p, ScreenCastPortal::State::Failed));
        QCOMPARE(p.reason(), QString(kScreenPortalError));
        QCOMPARE(p.pendingRemoteFd(), -1);
    }

    void descriptorsDoNotLeakAcrossRuns()
    {
        bringUp();
        const auto openFds = [] {
            int n = 0;
            for (int fd = 3; fd < 256; ++fd)
                n += ::fcntl(fd, F_GETFD) >= 0;
            return n;
        };
        ScreenCastPortal p(client);
        p.start({});
        QVERIFY(waitState(p, ScreenCastPortal::State::Watching));
        p.stop();
        const int base = openFds();
        for (int i = 0; i < 5; ++i) {
            p.start({});
            QVERIFY(waitState(p, ScreenCastPortal::State::Watching));
            if (i % 2)
                ::close(p.takeRemoteFd());
            else
                p.stop();
        }
        p.stop();
        QCOMPARE(openFds(), base);
    }
};

QTEST_MAIN(TstScreenCastPortal)
#include "tst_screencastportal.moc"
