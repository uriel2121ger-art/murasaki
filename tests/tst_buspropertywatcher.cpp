// The optional-service watchers (GameMode on the session bus, UPower on the system bus) against fakes on a private
// bus: absent, present, changing, vanishing, and never started by a read.

#include "buspropertywatcher.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

// A service with one property, which the test changes and announces.
class FakeService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "test.Fake1")
    Q_PROPERTY(int ClientCount MEMBER clientCount)
    Q_PROPERTY(bool OnBattery MEMBER onBattery)

public:
    int clientCount = 0;
    bool onBattery = false;
};

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
    bool onBattery = true; // already on battery when the surface starts
};

class FakeModernPowerProfiles : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.UPower.PowerProfiles")
    Q_PROPERTY(QString ActiveProfile MEMBER activeProfile)

public:
    QString activeProfile = u"balanced"_s;
};

class FakePowerProfiles : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "net.hadess.PowerProfiles")
    Q_PROPERTY(QString ActiveProfile MEMBER activeProfile)

public:
    QString activeProfile = u"balanced"_s;
};

} // namespace

class TestBusPropertyWatcher : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        if (qEnvironmentVariableIntValue("MURASAKI_TEST_PRIVATE_BUS") != 1)
            QSKIP("needs a private session bus (run through ctest, or set MURASAKI_TEST_PRIVATE_BUS=1 under dbus-run-session)");
    }

    void anAbsentServiceIsNotAnError()
    {
        BusPropertyWatcher watcher(QDBusConnection::sessionBus(), u"test.Nobody"_s, u"/x"_s, u"test.Fake1"_s, u"ClientCount"_s);
        QSignalSpy spy(&watcher, &BusPropertyWatcher::changed);
        watcher.start();
        QTest::qWait(100);
        QVERIFY(!watcher.available());
        QCOMPARE(spy.count(), 0);
        QVERIFY(!QDBusConnection::sessionBus().interface()->isServiceRegistered(u"test.Nobody"_s)); // nothing was started
    }

    void followsAServiceThatAppearsChangesAndLeaves()
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        const QString name = u"test.Watched%1"_s.arg(QCoreApplication::applicationPid());
        BusPropertyWatcher watcher(bus, name, u"/obj"_s, u"test.Fake1"_s, u"ClientCount"_s, {u"Poked"_s});
        QSignalSpy spy(&watcher, &BusPropertyWatcher::changed);
        watcher.start();
        QVERIFY(!watcher.available());

        FakeService service;
        service.clientCount = 2;
        QVERIFY(bus.registerObject(u"/obj"_s, &service, QDBusConnection::ExportAllProperties));
        const auto cleanup = qScopeGuard([&] {
            bus.unregisterService(name);
            bus.unregisterObject(u"/obj"_s);
        });
        QVERIFY(bus.registerService(name));
        QTRY_VERIFY(watcher.available());
        QCOMPARE(watcher.value().toInt(), 2);

        // PropertiesChanged re-reads
        service.clientCount = 5;
        bus.send(QDBusMessage::createSignal(u"/obj"_s, u"org.freedesktop.DBus.Properties"_s, u"PropertiesChanged"_s)
                     << u"test.Fake1"_s << QVariantMap{{u"ClientCount"_s, 5}} << QStringList());
        QTRY_COMPARE(watcher.value().toInt(), 5);

        // so do the extra signals
        service.clientCount = 0;
        bus.send(QDBusMessage::createSignal(u"/obj"_s, u"test.Fake1"_s, u"Poked"_s));
        QTRY_COMPARE(watcher.value().toInt(), 0);
        QVERIFY(watcher.available());

        const int before = spy.count();
        bus.unregisterService(name);
        QTRY_VERIFY(!watcher.available());
        QVERIFY(spy.count() > before);
    }

    void gameModeIsActiveWhileItHasClients()
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        GameModeWatcher watcher(bus);
        QSignalSpy spy(&watcher, &GameModeWatcher::changed);
        watcher.start();
        QTest::qWait(100);
        QVERIFY(!watcher.active()); // gamemoded is not here

        FakeGameMode fake;
        QVERIFY(bus.registerObject(u"/com/feralinteractive/GameMode"_s, &fake, QDBusConnection::ExportAllProperties));
        const auto cleanup = qScopeGuard([&] {
            bus.unregisterService(u"com.feralinteractive.GameMode"_s);
            bus.unregisterObject(u"/com/feralinteractive/GameMode"_s);
        });
        QVERIFY(bus.registerService(u"com.feralinteractive.GameMode"_s));
        QTest::qWait(150);
        QVERIFY(!watcher.active()); // present, no clients: not a change of the answer
        QCOMPARE(spy.count(), 0);

        fake.clientCount = 1;
        bus.send(QDBusMessage::createSignal(u"/com/feralinteractive/GameMode"_s, u"com.feralinteractive.GameMode"_s, u"GameRegistered"_s)
                 << 1234 << QVariant::fromValue(QDBusObjectPath(u"/com/feralinteractive/GameMode/Game1"_s)));
        QTRY_VERIFY(watcher.active());
        QCOMPARE(spy.count(), 1);

        fake.clientCount = 0;
        bus.send(QDBusMessage::createSignal(u"/com/feralinteractive/GameMode"_s, u"com.feralinteractive.GameMode"_s, u"GameUnregistered"_s)
                 << 1234 << QVariant::fromValue(QDBusObjectPath(u"/com/feralinteractive/GameMode/Game1"_s)));
        QTRY_VERIFY(!watcher.active());
        QCOMPARE(spy.count(), 2);
    }

    void powerFollowsOnBattery()
    {
        // The "system bus" of this test is the private session bus: the watcher takes the connection it is given.
        QDBusConnection bus = QDBusConnection::sessionBus();
        PowerWatcher watcher(bus);
        QSignalSpy spy(&watcher, &PowerWatcher::changed);
        watcher.start();
        QTest::qWait(100);
        QVERIFY(!watcher.onBattery()); // no UPower: treated as mains

        FakeUPower fake;
        QVERIFY(bus.registerObject(u"/org/freedesktop/UPower"_s, &fake, QDBusConnection::ExportAllProperties));
        const auto cleanup = qScopeGuard([&] {
            bus.unregisterService(u"org.freedesktop.UPower"_s);
            bus.unregisterObject(u"/org/freedesktop/UPower"_s);
        });
        QVERIFY(bus.registerService(u"org.freedesktop.UPower"_s));
        QTRY_VERIFY(watcher.onBattery());
        QCOMPARE(spy.count(), 1);

        fake.onBattery = false;
        bus.send(QDBusMessage::createSignal(u"/org/freedesktop/UPower"_s, u"org.freedesktop.DBus.Properties"_s, u"PropertiesChanged"_s)
                 << u"org.freedesktop.UPower"_s << QVariantMap{{u"OnBattery"_s, false}} << QStringList());
        QTRY_VERIFY(!watcher.onBattery());
        QCOMPARE(spy.count(), 2);
    }

    void powerProfileIsSaverOnlyWhenTheDaemonSaysSo()
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        PowerProfileWatcher watcher(bus);
        QSignalSpy spy(&watcher, &PowerProfileWatcher::changed);
        watcher.start();
        QTest::qWait(100);
        QVERIFY(!watcher.powerSaver()); // no power-profiles-daemon: not saving

        FakePowerProfiles fake;
        QVERIFY(bus.registerObject(u"/net/hadess/PowerProfiles"_s, &fake, QDBusConnection::ExportAllProperties));
        const auto cleanup = qScopeGuard([&] {
            bus.unregisterService(u"net.hadess.PowerProfiles"_s);
            bus.unregisterObject(u"/net/hadess/PowerProfiles"_s);
        });
        QVERIFY(bus.registerService(u"net.hadess.PowerProfiles"_s));
        QTest::qWait(150);
        QVERIFY(!watcher.powerSaver()); // balanced: not a change of the answer
        QCOMPARE(spy.count(), 0);

        fake.activeProfile = u"power-saver"_s;
        bus.send(QDBusMessage::createSignal(u"/net/hadess/PowerProfiles"_s, u"org.freedesktop.DBus.Properties"_s, u"PropertiesChanged"_s)
                 << u"net.hadess.PowerProfiles"_s << QVariantMap{{u"ActiveProfile"_s, u"power-saver"_s}} << QStringList());
        QTRY_VERIFY(watcher.powerSaver());
        QCOMPARE(spy.count(), 1);

        fake.activeProfile = u"performance"_s;
        bus.send(QDBusMessage::createSignal(u"/net/hadess/PowerProfiles"_s, u"org.freedesktop.DBus.Properties"_s, u"PropertiesChanged"_s)
                 << u"net.hadess.PowerProfiles"_s << QVariantMap{{u"ActiveProfile"_s, u"performance"_s}} << QStringList());
        QTRY_VERIFY(!watcher.powerSaver());
        QCOMPARE(spy.count(), 2);
    }

    void thePowerProfileIsAlsoReadFromTheNewerName()
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        PowerProfileWatcher watcher(bus);
        QSignalSpy spy(&watcher, &PowerProfileWatcher::changed);
        watcher.start();
        FakeModernPowerProfiles fake;
        fake.activeProfile = u"power-saver"_s;
        QVERIFY(bus.registerObject(u"/org/freedesktop/UPower/PowerProfiles"_s, &fake, QDBusConnection::ExportAllProperties));
        const auto cleanup = qScopeGuard([&] {
            bus.unregisterService(u"org.freedesktop.UPower.PowerProfiles"_s);
            bus.unregisterObject(u"/org/freedesktop/UPower/PowerProfiles"_s);
        });
        QVERIFY(bus.registerService(u"org.freedesktop.UPower.PowerProfiles"_s));
        QTRY_VERIFY(watcher.powerSaver());
        QCOMPARE(spy.count(), 1);
        fake.activeProfile = u"performance"_s;
        bus.send(QDBusMessage::createSignal(u"/org/freedesktop/UPower/PowerProfiles"_s, u"org.freedesktop.DBus.Properties"_s, u"PropertiesChanged"_s)
                 << u"org.freedesktop.UPower.PowerProfiles"_s << QVariantMap{{u"ActiveProfile"_s, u"performance"_s}} << QStringList());
        QTRY_VERIFY(!watcher.powerSaver());
    }
};

QTEST_GUILESS_MAIN(TestBusPropertyWatcher)
#include "tst_buspropertywatcher.moc"
