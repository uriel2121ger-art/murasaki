#include "surfaceservice.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestSurfaceService : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        if (qEnvironmentVariableIntValue("MURASAKI_TEST_PRIVATE_BUS") != 1)
            QSKIP("needs a private session bus (run through ctest, or set MURASAKI_TEST_PRIVATE_BUS=1 under dbus-run-session)");
    }
    void emitsOnlyWhenOutputsDiffer();
    void hostRegistrationIsBookkeptPerCaller();
    void shellReportsComeFromGnomeShellOnly();
    void registrationsGoWithTheBusName();
    void hostVisibilityNeedsARegistration();
    void hostStateIsJsonAndAnnouncedOnChange();
    void invalidOutputNamesAreRefused();
    void anOutputHasOneHost();
    void cursorGoesOnlyToTheVisibleHostOfTheOutput();
    void cursorReportsComeFromKWinOnly();
    void hostPointerComesFromTheOverlayAndGoesToTheHostOnly();
    void sessionFactsAreCurrentAtOnceAndAnnouncedAtMostFourTimesASecond();
    void onlyMurasakisOwnProcessesSeeWhichApplications();

private:
    // A service on the (private) session bus and a client connection of its own, so that calls carry a unique name.
    struct Fixture {
        SurfaceService service;
        QDBusConnection client{QStringLiteral("unconnected")};
        QString name;
        QString path = QStringLiteral("/test/surface");
        QString iface = QStringLiteral("io.github.uriel2121ger_art.Murasaki.Surface1");
    };
public slots:
    void onHostCursor(const QString &output, double x, double y, double w, double h)
    {
        m_cursor.append({output, x, y, w, h});
    }
    void onHostCursorLeft(const QString &output) { m_left.append(output); }
    void onAnyCursor() { ++m_heard; }
    void onHostPointer(const QString &output, double x, double y, double w, double h, uint buttons, int wheelX, int wheelY)
    {
        m_pointer.append({output, x, y, w, h, buttons, wheelX, wheelY});
    }

private:
    struct Cursor {
        QString output;
        double x, y, w, h;
    };
    struct Pointer {
        QString output;
        double x, y, w, h;
        uint buttons;
        int wheelX, wheelY;
    };
    QList<Pointer> m_pointer;
    QList<Cursor> m_cursor;
    QStringList m_left;
    int m_heard = 0;

    static bool setUp(Fixture &f, const QString &clientName);
    static QDBusMessage call(Fixture &f, const QString &method, const QVariantList &args = {});
};

bool TestSurfaceService::setUp(Fixture &f, const QString &clientName)
{
    QDBusConnection server = QDBusConnection::sessionBus();
    if (!server.isConnected())
        return false;
    f.name = server.baseService();
    if (!server.registerObject(f.path, &f.service, QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals))
        return false;
    f.client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, clientName);
    return f.client.isConnected();
}

QDBusMessage TestSurfaceService::call(Fixture &f, const QString &method, const QVariantList &args)
{
    QDBusMessage message = QDBusMessage::createMethodCall(f.name, f.path, f.iface, method);
    message.setArguments(args);
    // A second connection plus a spinning event loop: a blocking call would starve the server side.
    QDBusPendingCall pending = f.client.asyncCall(message);
    if (!QTest::qWaitFor([&] { return pending.isFinished(); }, 5000))
        qWarning("D-Bus call timed out");
    return pending.reply();
}

void TestSurfaceService::emitsOnlyWhenOutputsDiffer()
{
    SurfaceService service;
    QJsonArray outputs;
    service.outputsProvider = [&outputs] { return outputs; };
    QSignalSpy spy(&service, &SurfaceService::OutputsChanged);

    service.notifyOutputsChanged(); // still the empty list: nothing to announce
    QCOMPARE(spy.count(), 0);

    outputs.append(QJsonObject{{u"name"_s, u"DP-1"_s}, {u"playing"_s, true}});
    service.notifyOutputsChanged();
    QCOMPARE(spy.count(), 1);
    service.notifyOutputsChanged();
    QCOMPARE(spy.count(), 1);

    outputs = QJsonArray{QJsonObject{{u"name"_s, u"DP-1"_s}, {u"playing"_s, false}}};
    service.notifyOutputsChanged();
    QCOMPARE(spy.count(), 2);
    QCOMPARE(service.Outputs(), u"[{\"name\":\"DP-1\",\"playing\":false}]"_s);

    outputs = {};
    service.notifyOutputsChanged();
    QCOMPARE(spy.count(), 3);
}

void TestSurfaceService::hostRegistrationIsBookkeptPerCaller()
{
    Fixture f;
    if (!setUp(f, u"surfsvc-a"_s))
        QSKIP("no D-Bus session bus; run the tests under dbus-run-session");
    const auto cleanup = qScopeGuard([&] {
        QDBusConnection::sessionBus().unregisterObject(f.path);
        QDBusConnection::disconnectFromBus(u"surfsvc-a"_s);
    });
    QSignalSpy changed(&f.service, &SurfaceService::hostsChanged);

    QVERIFY(!f.service.isHosted(u"DP-1"_s));
    QCOMPARE(call(f, u"RegisterHost"_s, {u"DP-1"_s}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(f.service.isHosted(u"DP-1"_s));
    QVERIFY(!f.service.isHosted(u"DP-2"_s));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.last().at(0).toString(), u"DP-1"_s);

    // Twice from the same name changes nothing.
    QCOMPARE(call(f, u"RegisterHost"_s, {u"DP-1"_s}).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(changed.count(), 1);

    // Unregistering an output one never registered is a no-op.
    QCOMPARE(call(f, u"UnregisterHost"_s, {u"DP-2"_s}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(f.service.isHosted(u"DP-1"_s));
    QCOMPARE(changed.count(), 1);

    QCOMPARE(call(f, u"UnregisterHost"_s, {u"DP-1"_s}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(!f.service.isHosted(u"DP-1"_s));
    QCOMPARE(changed.count(), 2);
}

void TestSurfaceService::registrationsGoWithTheBusName()
{
    Fixture f;
    if (!setUp(f, u"surfsvc-b"_s))
        QSKIP("no D-Bus session bus; run the tests under dbus-run-session");
    const auto cleanup = qScopeGuard([&] { QDBusConnection::sessionBus().unregisterObject(f.path); });
    // A second host process for another output on a connection of its own.
    QDBusConnection other = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"surfsvc-b2"_s);
    QVERIFY(other.isConnected());

    QCOMPARE(call(f, u"RegisterHost"_s, {u"DP-1"_s}).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(call(f, u"RegisterHost"_s, {u"DP-2"_s}).type(), QDBusMessage::ReplyMessage);
    {
        QDBusMessage message = QDBusMessage::createMethodCall(f.name, f.path, f.iface, u"RegisterHost"_s);
        message.setArguments({u"DP-3"_s});
        QDBusPendingCall pending = other.asyncCall(message);
        QVERIFY(QTest::qWaitFor([&] { return pending.isFinished(); }, 5000));
    }
    QVERIFY(f.service.isHosted(u"DP-1"_s) && f.service.isHosted(u"DP-2"_s) && f.service.isHosted(u"DP-3"_s));
    QCOMPARE(call(f, u"SetHostVisible"_s, {u"DP-1"_s, false}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(!f.service.hostVisible(u"DP-1"_s));

    QSignalSpy changed(&f.service, &SurfaceService::hostsChanged);
    // The first host process dies: every handle to its connection has to go for the bus name to vanish.
    f.client = QDBusConnection(u"unconnected"_s);
    QDBusConnection::disconnectFromBus(u"surfsvc-b"_s);
    QVERIFY(QTest::qWaitFor([&] { return !f.service.isHosted(u"DP-1"_s); }, 5000));
    QVERIFY(!f.service.isHosted(u"DP-2"_s));
    QVERIFY(f.service.isHosted(u"DP-3"_s)); // another name's registration stays
    QVERIFY(f.service.hostVisible(u"DP-1"_s)); // and the "hidden" flag does not outlive its host
    QVERIFY(changed.count() >= 2);

    other = QDBusConnection(u"unconnected"_s);
    QDBusConnection::disconnectFromBus(u"surfsvc-b2"_s);
    QVERIFY(QTest::qWaitFor([&] { return !f.service.isHosted(u"DP-3"_s); }, 5000));
}

void TestSurfaceService::hostVisibilityNeedsARegistration()
{
    Fixture f;
    if (!setUp(f, u"surfsvc-c"_s))
        QSKIP("no D-Bus session bus; run the tests under dbus-run-session");
    const auto cleanup = qScopeGuard([&] {
        QDBusConnection::sessionBus().unregisterObject(f.path);
        QDBusConnection::disconnectFromBus(u"surfsvc-c"_s);
    });
    QSignalSpy changed(&f.service, &SurfaceService::hostsChanged);

    // Not registered: ignored.
    QCOMPARE(call(f, u"SetHostVisible"_s, {u"DP-1"_s, false}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(f.service.hostVisible(u"DP-1"_s));
    QCOMPARE(changed.count(), 0);

    QCOMPARE(call(f, u"RegisterHost"_s, {u"DP-1"_s}).type(), QDBusMessage::ReplyMessage);
    changed.clear();
    QVERIFY(f.service.hostVisible(u"DP-1"_s)); // visible until said otherwise
    QCOMPARE(call(f, u"SetHostVisible"_s, {u"DP-1"_s, false}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(!f.service.hostVisible(u"DP-1"_s));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(call(f, u"SetHostVisible"_s, {u"DP-1"_s, false}).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(changed.count(), 1); // no change, no signal
    QCOMPARE(call(f, u"SetHostVisible"_s, {u"DP-1"_s, true}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(f.service.hostVisible(u"DP-1"_s));
    QCOMPARE(changed.count(), 2);

    // Unregistering resets the flag.
    QCOMPARE(call(f, u"SetHostVisible"_s, {u"DP-1"_s, false}).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(call(f, u"UnregisterHost"_s, {u"DP-1"_s}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(f.service.hostVisible(u"DP-1"_s));
}

void TestSurfaceService::hostStateIsJsonAndAnnouncedOnChange()
{
    Fixture f;
    if (!setUp(f, u"surfsvc-d"_s))
        QSKIP("no D-Bus session bus; run the tests under dbus-run-session");
    const auto cleanup = qScopeGuard([&] {
        QDBusConnection::sessionBus().unregisterObject(f.path);
        QDBusConnection::disconnectFromBus(u"surfsvc-d"_s);
    });
    bool paused = false;
    f.service.hostStateProvider = [&paused](const QString &output) {
        return QJsonObject{{u"desktop"_s, output + u"-desk"_s}, {u"paused"_s, paused}, {u"audible"_s, true},
                           {u"frameRateLimit"_s, 24}};
    };

    const QDBusMessage reply = call(f, u"HostState"_s, {u"DP-1"_s});
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(QJsonDocument::fromJson(reply.arguments().value(0).toString().toUtf8()).object(),
             (QJsonObject{{u"desktop"_s, u"DP-1-desk"_s}, {u"paused"_s, false}, {u"audible"_s, true},
                          {u"frameRateLimit"_s, 24}}));

    QSignalSpy state(&f.service, &SurfaceService::HostStateChanged);
    f.service.notifyHostStates(); // nobody hosts anything: silent
    QCOMPARE(state.count(), 0);

    QCOMPARE(call(f, u"RegisterHost"_s, {u"DP-1"_s}).type(), QDBusMessage::ReplyMessage);
    f.service.notifyHostStates(); // unchanged since the registration
    QCOMPARE(state.count(), 0);
    paused = true;
    f.service.notifyHostStates();
    QCOMPARE(state.count(), 1);
    QCOMPARE(state.last().at(0).toString(), u"DP-1"_s);
    f.service.notifyHostStates();
    QCOMPARE(state.count(), 1);
}

void TestSurfaceService::invalidOutputNamesAreRefused()
{
    Fixture f;
    if (!setUp(f, u"surfsvc-e"_s))
        QSKIP("no D-Bus session bus; run the tests under dbus-run-session");
    const auto cleanup = qScopeGuard([&] {
        QDBusConnection::sessionBus().unregisterObject(f.path);
        QDBusConnection::disconnectFromBus(u"surfsvc-e"_s);
    });
    for (const QString &method : {u"RegisterHost"_s, u"UnregisterHost"_s}) {
        QCOMPARE(call(f, method, {QString()}).type(), QDBusMessage::ErrorMessage);
        QCOMPARE(call(f, method, {QString(201, u'x')}).type(), QDBusMessage::ErrorMessage);
    }
    QCOMPARE(call(f, u"HostState"_s, {QString()}).type(), QDBusMessage::ErrorMessage);
    QVERIFY(!f.service.isHosted(QString()));

    // One connection can host only a few outputs.
    int accepted = 0;
    for (int i = 0; i < 20; ++i) {
        const QDBusMessage reply = call(f, u"RegisterHost"_s, {u"OUT-%1"_s.arg(i)});
        if (reply.type() == QDBusMessage::ReplyMessage)
            ++accepted;
        else
            QCOMPARE(reply.errorName(), QDBusError::errorString(QDBusError::LimitsExceeded));
    }
    QCOMPARE(accepted, 8);
}

void TestSurfaceService::anOutputHasOneHost()
{
    Fixture f;
    if (!setUp(f, u"surfsvc-f"_s))
        QSKIP("no D-Bus session bus; run the tests under dbus-run-session");
    const auto cleanup = qScopeGuard([&] {
        QDBusConnection::sessionBus().unregisterObject(f.path);
        QDBusConnection::disconnectFromBus(u"surfsvc-f"_s);
    });
    QDBusConnection other = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"surfsvc-f2"_s);
    QVERIFY(other.isConnected());
    const auto callOther = [&](const QString &method, const QVariantList &args) {
        QDBusMessage message = QDBusMessage::createMethodCall(f.name, f.path, f.iface, method);
        message.setArguments(args);
        QDBusPendingCall pending = other.asyncCall(message);
        if (!QTest::qWaitFor([&] { return pending.isFinished(); }, 5000))
            qWarning("D-Bus call timed out");
        return pending.reply();
    };

    QCOMPARE(call(f, u"RegisterHost"_s, {u"DP-1"_s}).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(call(f, u"SetHostVisible"_s, {u"DP-1"_s, false}).type(), QDBusMessage::ReplyMessage);
    // Another connection cannot take the output, nor unregister it, nor change its visibility.
    const QDBusMessage refused = callOther(u"RegisterHost"_s, {u"DP-1"_s});
    QCOMPARE(refused.type(), QDBusMessage::ErrorMessage);
    QCOMPARE(refused.errorName(), QDBusError::errorString(QDBusError::AccessDenied));
    QVERIFY(refused.errorMessage().contains(u"DP-1"_s));
    QCOMPARE(callOther(u"SetHostVisible"_s, {u"DP-1"_s, true}).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(callOther(u"UnregisterHost"_s, {u"DP-1"_s}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(f.service.isHosted(u"DP-1"_s));
    QVERIFY(!f.service.hostVisible(u"DP-1"_s));

    // When the owner leaves, the hidden flag goes with it and the next host starts visible.
    QCOMPARE(call(f, u"UnregisterHost"_s, {u"DP-1"_s}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(f.service.hostVisible(u"DP-1"_s));
    QCOMPARE(callOther(u"RegisterHost"_s, {u"DP-1"_s}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(f.service.isHosted(u"DP-1"_s));
    QVERIFY(f.service.hostVisible(u"DP-1"_s));

    other = QDBusConnection(u"unconnected"_s);
    QDBusConnection::disconnectFromBus(u"surfsvc-f2"_s);
    QVERIFY(QTest::qWaitFor([&] { return !f.service.isHosted(u"DP-1"_s); }, 5000));
}

void TestSurfaceService::cursorGoesOnlyToTheVisibleHostOfTheOutput()
{
    Fixture f;
    if (!setUp(f, u"surfsvc-g"_s))
        QSKIP("no D-Bus session bus; run the tests under dbus-run-session");
    f.service.dbusPath = f.path;
    const auto cleanup = qScopeGuard([&] {
        QDBusConnection::sessionBus().unregisterObject(f.path);
        QDBusConnection::disconnectFromBus(u"surfsvc-g"_s);
        QDBusConnection::disconnectFromBus(u"surfsvc-g2"_s);
    });
    QDBusConnection other = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"surfsvc-g2"_s);
    QVERIFY(other.isConnected());
    m_cursor.clear();
    m_left.clear();
    QVERIFY(f.client.connect(f.name, f.path, f.iface, u"HostCursor"_s, this,
                             SLOT(onHostCursor(QString,double,double,double,double))));
    QVERIFY(f.client.connect(f.name, f.path, f.iface, u"HostCursorLeft"_s, this, SLOT(onHostCursorLeft(QString))));
    // A second connection listens to the same signals: it must hear nothing, the signals are addressed.
    m_heard = 0;
    QVERIFY(other.connect(f.name, f.path, f.iface, u"HostCursor"_s, this, SLOT(onAnyCursor())));
    QVERIFY(other.connect(f.name, f.path, f.iface, u"HostCursorLeft"_s, this, SLOT(onAnyCursor())));

    QVERIFY(!f.service.hasVisibleHost());
    QVERIFY(!f.service.sendHostCursor(u"DP-1"_s, QPointF(1, 2), QSizeF(3, 4))); // nobody hosts it
    QCOMPARE(call(f, u"RegisterHost"_s, {u"DP-1"_s}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(f.service.hasVisibleHost());
    QVERIFY(!f.service.sendHostCursor(u"DP-2"_s, QPointF(1, 2), QSizeF(3, 4))); // another output

    QVERIFY(f.service.sendHostCursor(u"DP-1"_s, QPointF(10.5, 20), QSizeF(1920, 1080)));
    QVERIFY(f.service.sendHostCursorLeft(u"DP-1"_s));
    QVERIFY(QTest::qWaitFor([&] { return m_cursor.size() == 1 && m_left.size() == 1; }, 5000));
    QCOMPARE(m_cursor[0].output, u"DP-1"_s);
    QCOMPARE(m_cursor[0].x, 10.5);
    QCOMPARE(m_cursor[0].y, 20.0);
    QCOMPARE(m_cursor[0].w, 1920.0);
    QCOMPARE(m_cursor[0].h, 1080.0);
    QCOMPARE(m_left, QStringList{u"DP-1"_s});
    QTest::qWait(100);
    QCOMPARE(m_heard, 0);

    // A hidden host has no use for the cursor.
    QCOMPARE(call(f, u"SetHostVisible"_s, {u"DP-1"_s, false}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(!f.service.hasVisibleHost());
    QVERIFY(!f.service.sendHostCursor(u"DP-1"_s, QPointF(1, 2), QSizeF(3, 4)));
    QVERIFY(!f.service.sendHostCursorLeft(u"DP-1"_s));
    QCOMPARE(call(f, u"SetHostVisible"_s, {u"DP-1"_s, true}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(f.service.hasVisibleHost());
    QCOMPARE(call(f, u"UnregisterHost"_s, {u"DP-1"_s}).type(), QDBusMessage::ReplyMessage);
    QVERIFY(!f.service.hasVisibleHost());
}

void TestSurfaceService::cursorReportsComeFromKWinOnly()
{
    Fixture f;
    if (!setUp(f, u"surfsvc-h"_s))
        QSKIP("no D-Bus session bus; run the tests under dbus-run-session");
    const auto cleanup = qScopeGuard([&] {
        f.client.unregisterService(u"org.kde.KWin"_s);
        QDBusConnection::sessionBus().unregisterObject(f.path);
        QDBusConnection::disconnectFromBus(u"surfsvc-h"_s);
    });
    QSignalSpy spy(&f.service, &SurfaceService::kwinCursorReported);

    // A direct (in-process) call is trusted.
    f.service.ReportKWinCursor(1, 2);
    QCOMPARE(spy.count(), 1);

    // Nobody owns org.kde.KWin: a bus caller is not KWin.
    QCOMPARE(call(f, u"ReportKWinCursor"_s, {3, 4}).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(spy.count(), 1);

    // The caller owns the name now.
    QVERIFY(f.client.registerService(u"org.kde.KWin"_s));
    QCOMPARE(call(f, u"ReportKWinCursor"_s, {3, 4}).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.last().at(0).toPointF(), QPointF(3, 4));
}

void TestSurfaceService::hostPointerComesFromTheOverlayAndGoesToTheHostOnly()
{
    Fixture f;
    if (!setUp(f, u"surfsvc-i"_s))
        QSKIP("no D-Bus session bus; run the tests under dbus-run-session");
    f.service.dbusPath = f.path;
    const auto cleanup = qScopeGuard([&] {
        QDBusConnection::sessionBus().unregisterObject(f.path);
        QDBusConnection::disconnectFromBus(u"surfsvc-i"_s);
        QDBusConnection::disconnectFromBus(u"surfsvc-i2"_s);
    });
    // The client is the host of DP-1; the overlay is another connection.
    QDBusConnection overlay = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"surfsvc-i2"_s);
    QVERIFY(overlay.isConnected());
    m_pointer.clear();
    QVERIFY(f.client.connect(f.name, f.path, f.iface, u"HostPointer"_s, this,
                             SLOT(onHostPointer(QString,double,double,double,double,uint,int,int))));
    QCOMPARE(call(f, u"RegisterHost"_s, {u"DP-1"_s}).type(), QDBusMessage::ReplyMessage);

    const auto report = [&](const QString &output, double x, uint buttons, int wheelY) {
        QDBusMessage message = QDBusMessage::createMethodCall(f.name, f.path, f.iface, u"ReportHostPointer"_s);
        message.setArguments({output, x, 20.0, 1920.0, 1080.0, buttons, 0, wheelY});
        QDBusPendingCall pending = overlay.asyncCall(message);
        if (!QTest::qWaitFor([&] { return pending.isFinished(); }, 5000))
            qWarning("D-Bus call timed out");
    };

    // Direct (in-process) use: addressed to the host, buttons masked to the three that exist, an absurd wheel clamped.
    QVERIFY(f.service.sendHostPointer(u"DP-1"_s, QPointF(10.5, 20), QSizeF(1920, 1080), 0xff, 0, 1'000'000));
    QVERIFY(!f.service.sendHostPointer(u"DP-1"_s, QPointF(1, 2), QSizeF(0, 4), 1, 0, 0));       // no size
    QVERIFY(!f.service.sendHostPointer(u"DP-1"_s, QPointF(qQNaN(), 2), QSizeF(3, 4), 1, 0, 0)); // no position
    QVERIFY(!f.service.sendHostPointer(u"DP-2"_s, QPointF(1, 2), QSizeF(3, 4), 1, 0, 0));       // nobody hosts it
    QVERIFY(QTest::qWaitFor([&] { return m_pointer.size() == 1; }, 5000));
    QCOMPARE(m_pointer[0].output, u"DP-1"_s);
    QCOMPARE(m_pointer[0].buttons, 7u);
    QCOMPARE(m_pointer[0].wheelY, 3840);

    // A bus caller that is not the overlay is ignored.
    report(u"DP-1"_s, 5, 1, 0);
    QTest::qWait(100);
    QCOMPARE(m_pointer.size(), 1);

    // The overlay's name makes it the overlay.
    QVERIFY(overlay.registerService(QString(kOverlayDBusService)));
    report(u"DP-1"_s, 5, 1, 0);
    QVERIFY(QTest::qWaitFor([&] { return m_pointer.size() == 2; }, 5000));
    QCOMPARE(m_pointer[1].x, 5.0);
    QCOMPARE(m_pointer[1].buttons, 1u);
    report(u"DP-2"_s, 6, 1, 0); // another output: nobody hears it
    report(QString(), 7, 1, 0);
    QTest::qWait(100);
    QCOMPARE(m_pointer.size(), 2);

    // A button down when the overlay disappears is let go in the host (a crash would leave it down).
    report(u"DP-1"_s, 9, 1, 0);
    QVERIFY(QTest::qWaitFor([&] { return m_pointer.size() == 3; }, 5000));
    QCOMPARE(m_pointer[2].buttons, 1u);
    QVERIFY(overlay.unregisterService(QString(kOverlayDBusService)));
    QVERIFY(QTest::qWaitFor([&] { return m_pointer.size() == 4; }, 5000));
    QCOMPARE(m_pointer[3].buttons, 0u);
    QCOMPARE(m_pointer[3].x, 9.0);
    QTest::qWait(100);
    QCOMPARE(m_pointer.size(), 4); // once
    QVERIFY(overlay.registerService(QString(kOverlayDBusService)));

    // A hidden host takes no pointer.
    QCOMPARE(call(f, u"SetHostVisible"_s, {u"DP-1"_s, false}).type(), QDBusMessage::ReplyMessage);
    report(u"DP-1"_s, 8, 1, 0);
    QTest::qWait(100);
    QCOMPARE(m_pointer.size(), 4);
}

void TestSurfaceService::shellReportsComeFromGnomeShellOnly()
{
    Fixture f;
    if (!setUp(f, u"surfsvc-i"_s))
        QSKIP("no D-Bus session bus; run the tests under dbus-run-session");
    const auto cleanup = qScopeGuard([&] {
        f.client.unregisterService(u"org.gnome.Shell"_s);
        QDBusConnection::sessionBus().unregisterObject(f.path);
        QDBusConnection::disconnectFromBus(u"surfsvc-i"_s);
    });
    QSignalSpy spy(&f.service, &SurfaceService::shellStateReported);

    f.service.ReportShellState(u"{}"_s); // in-process: trusted
    QCOMPARE(spy.count(), 1);

    // Nobody owns org.gnome.Shell: a bus caller is not the shell.
    QCOMPARE(call(f, u"ReportShellState"_s, {u"{}"_s}).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(spy.count(), 1);
    // KWin's name does not make the caller the shell either.
    QVERIFY(f.client.registerService(u"org.kde.KWin"_s));
    QCOMPARE(call(f, u"ReportShellState"_s, {u"{}"_s}).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(spy.count(), 1);
    f.client.unregisterService(u"org.kde.KWin"_s);

    QVERIFY(f.client.registerService(u"org.gnome.Shell"_s));
    QCOMPARE(call(f, u"ReportShellState"_s, {u"{\"currentDesktop\":\"0\"}"_s}).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.last().at(0).toString(), u"{\"currentDesktop\":\"0\"}"_s);
    // A report that is far too big is dropped.
    f.service.ReportShellState(QString(1024 * 1024 + 1, u'x'));
    QCOMPARE(spy.count(), 2);
}

void TestSurfaceService::sessionFactsAreCurrentAtOnceAndAnnouncedAtMostFourTimesASecond()
{
    SurfaceService service;
    service.setSessionFactsInterval(120);
    QSignalSpy facts(&service, &SurfaceService::SessionFactsChanged);
    QSignalSpy outputs(&service, &SurfaceService::OutputFullscreenChanged);
    QCOMPARE(service.SessionFacts(), u"{\"known\":false,\"outputs\":{}}"_s); // before the first publication: unknown, not \"nothing is fullscreen\"
    const QString quiet = u"{\"DP-1\":{\"fullscreen\":false,\"app\":\"\"}}"_s;
    // the first change goes out at once
    service.setSessionFacts({u"{\"n\":1}"_s, quiet, u"{}"_s, u"{}"_s});
    QCOMPARE(facts.count(), 1);
    QCOMPARE(outputs.count(), 1);
    // a burst inside the interval: the getters are current, the signal waits and goes out once
    for (int i = 2; i <= 20; ++i)
        service.setSessionFacts({QStringLiteral("{\"n\":%1}").arg(i), quiet, u"{}"_s, u"{}"_s});
    QCOMPARE(service.SessionFacts(), u"{\"n\":20}"_s);
    QCOMPARE(facts.count(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(facts.count(), 2, 1000);
    QCOMPARE(outputs.count(), 1); // OutputFullscreen did not change in the burst: no signal
    // the same value again is nothing
    service.setSessionFacts({u"{\"n\":20}"_s, quiet, u"{}"_s, u"{}"_s});
    QTest::qWait(300);
    QCOMPARE(facts.count(), 2);
    // a fullscreen change reaches both
    const QString full = u"{\"DP-1\":{\"fullscreen\":true,\"app\":\"mpv\"}}"_s;
    service.setSessionFacts({u"{\"n\":21}"_s, full, u"{}"_s, u"{}"_s});
    QTRY_COMPARE_WITH_TIMEOUT(outputs.count(), 2, 1000);
    QCOMPARE(service.OutputFullscreen(), full);
}

void TestSurfaceService::onlyMurasakisOwnProcessesSeeWhichApplications()
{
    Fixture f;
    if (!setUp(f, u"surfsvc-facts"_s))
        QSKIP("no D-Bus session bus; run the tests under dbus-run-session");
    const auto cleanup = qScopeGuard([&] {
        QDBusConnection::sessionBus().unregisterObject(f.path);
        QDBusConnection::disconnectFromBus(u"surfsvc-facts"_s);
        QDBusConnection::disconnectFromBus(u"surfsvc-facts-overlay"_s);
    });
    f.service.setSessionFacts({u"{\"outputs\":{\"DP-1\":{\"fullscreen\":true,\"app\":\"steam_app_620\",\"occluded\":true}},\"openApps\":[\"firefox\"],\"activeApp\":\"firefox\",\"gameRunning\":true,\"known\":true}"_s,
                               u"{\"DP-1\":{\"fullscreen\":true,\"app\":\"steam_app_620\"}}"_s,
                               u"{\"outputs\":{\"DP-1\":{\"fullscreen\":true,\"occluded\":true}},\"anyFullscreen\":true,\"known\":true}"_s,
                               u"{\"DP-1\":{\"fullscreen\":true}}"_s});
    // A process that is none of Murasaki's: where, never which application, game or battery
    const QString outsider = call(f, u"SessionFacts"_s).arguments().value(0).toString();
    QVERIFY(outsider.contains(u"\"fullscreen\":true"_s));
    for (const char *secret : {"steam_app_620", "firefox", "openApps", "activeApp", "gameRunning"})
        QVERIFY2(!outsider.contains(QLatin1String(secret)), secret);
    const QString outsiderOutputs = call(f, u"OutputFullscreen"_s).arguments().value(0).toString();
    QVERIFY(outsiderOutputs.contains(u"\"fullscreen\":true"_s) && !outsiderOutputs.contains(u"steam_app"_s));
    // The overlay (the owner of its bus name) reads everything
    QDBusConnection overlay = QDBusConnection::connectToBus(QDBusConnection::SessionBus, u"surfsvc-facts-overlay"_s);
    QVERIFY(overlay.isConnected());
    QVERIFY(overlay.registerService(QString(kOverlayDBusService)));
    const auto asOverlay = [&](const QString &method) {
        QDBusMessage message = QDBusMessage::createMethodCall(f.name, f.path, f.iface, method);
        QDBusPendingCall pending = overlay.asyncCall(message);
        (void)QTest::qWaitFor([&] { return pending.isFinished(); }, 5000);
        return pending.reply().arguments().value(0).toString();
    };
    const QString full = asOverlay(u"SessionFacts"_s);
    QVERIFY(full.contains(u"steam_app_620"_s) && full.contains(u"openApps"_s) && full.contains(u"gameRunning"_s));
    QVERIFY(asOverlay(u"OutputFullscreen"_s).contains(u"steam_app_620"_s));
    // The change signal carries no argument at all
    QSignalSpy spy(&f.service, &SurfaceService::SessionFactsChanged);
    f.service.setSessionFactsInterval(10);
    f.service.setSessionFacts({u"{\"known\":true,\"x\":1}"_s, u"{}"_s, u"{}"_s, u"{}"_s});
    QTRY_VERIFY(spy.count() >= 1);
    QCOMPARE(spy.first().size(), 0);
    // An outsider is told when the public facts did not change only as far as the signal goes (focus changes stay a timing, not a name)
    QVERIFY(overlay.unregisterService(QString(kOverlayDBusService)));
}

QTEST_GUILESS_MAIN(TestSurfaceService)
#include "tst_surfaceservice.moc"