// SessionWatch: the lock screen (org.freedesktop.ScreenSaver) and the screen-off input pause the pets together;
// nothing else does. Fakes the ScreenSaver service on a private bus.

#include "sessionwatch.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QSignalSpy>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

class FakeScreenSaver : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.ScreenSaver")

public:
    bool active = false;

public slots:
    Q_SCRIPTABLE bool GetActive() { return active; }

signals:
    Q_SCRIPTABLE void ActiveChanged(bool active);
};

} // namespace

class TstSessionWatch : public QObject
{
    Q_OBJECT

private slots:
    void lockAndScreenOffPauseTogether();
    void followsTheScreenSaverSignal();
    void startsPausedWhenAlreadyLocked();
};

void TstSessionWatch::lockAndScreenOffPauseTogether()
{
    SessionWatch watch;
    QSignalSpy changed(&watch, &SessionWatch::pausedChanged);
    QVERIFY(!watch.paused());
    watch.setLocked(true);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.at(0).at(0).toBool(), true);
    watch.setScreenOff(true); // already paused: no new signal
    QCOMPARE(changed.count(), 1);
    watch.setLocked(false); // the screen is still off
    QVERIFY(watch.paused());
    QCOMPARE(changed.count(), 1);
    watch.setScreenOff(false);
    QCOMPARE(changed.count(), 2);
    QCOMPARE(changed.at(1).at(0).toBool(), false);
    watch.setScreenOff(false); // no change, no signal
    QCOMPARE(changed.count(), 2);
}

void TstSessionWatch::followsTheScreenSaverSignal()
{
    if (qEnvironmentVariableIntValue("MURASAKI_TEST_PRIVATE_BUS") != 1)
        QSKIP("needs a private session bus");
    QDBusConnection bus = QDBusConnection::sessionBus();
    FakeScreenSaver saver;
    QVERIFY(bus.registerObject(u"/org/freedesktop/ScreenSaver"_s, &saver, QDBusConnection::ExportScriptableSignals
                                                                              | QDBusConnection::ExportScriptableSlots));
    SessionWatch watch;
    QSignalSpy changed(&watch, &SessionWatch::pausedChanged);
    watch.watchScreenSaver(bus);
    emit saver.ActiveChanged(true);
    QTRY_VERIFY(watch.locked());
    QCOMPARE(changed.count(), 1);
    emit saver.ActiveChanged(false);
    QTRY_VERIFY(!watch.locked());
    QCOMPARE(changed.count(), 2);
    bus.unregisterObject(u"/org/freedesktop/ScreenSaver"_s);
}

void TstSessionWatch::startsPausedWhenAlreadyLocked()
{
    if (qEnvironmentVariableIntValue("MURASAKI_TEST_PRIVATE_BUS") != 1)
        QSKIP("needs a private session bus");
    QDBusConnection bus = QDBusConnection::sessionBus();
    FakeScreenSaver saver;
    saver.active = true;
    QVERIFY(bus.registerObject(u"/org/freedesktop/ScreenSaver"_s, &saver, QDBusConnection::ExportScriptableSlots));
    QVERIFY(bus.registerService(u"org.freedesktop.ScreenSaver"_s));
    SessionWatch watch;
    watch.watchScreenSaver(bus);
    QTRY_VERIFY(watch.locked());
    bus.unregisterService(u"org.freedesktop.ScreenSaver"_s);
    bus.unregisterObject(u"/org/freedesktop/ScreenSaver"_s);
}

QTEST_MAIN(TstSessionWatch)
#include "tst_sessionwatch.moc"
