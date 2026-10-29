// BusLease (src/daemon/buslease.h): a thing that lives as long as the bus connection that asked for it. Needs a private session bus (run through ctest).

#include "buslease.h"

#include <QDBusConnection>
#include <QSignalSpy>
#include <QTest>

using namespace Qt::StringLiterals;
using murasaki::BusLease;

class TstBusLease : public QObject
{
    Q_OBJECT

private:
    // The unique name of a new connection to the bus; the connection stays open (by its name) until disconnectFromBus, which needs that no copy of it is held here.
    static QString peer(const QString &name)
    {
        const QDBusConnection connection = QDBusConnection::connectToBus(QDBusConnection::SessionBus, name);
        return connection.isConnected() ? connection.baseService() : QString();
    }

private slots:
    void initTestCase()
    {
        if (qEnvironmentVariableIntValue("MURASAKI_TEST_PRIVATE_BUS") != 1)
            QSKIP("needs a private session bus (run through ctest, or set MURASAKI_TEST_PRIVATE_BUS=1 under dbus-run-session)");
    }

    void theLeaseIsLostWhenItsOwnerLeavesTheBus()
    {
        BusLease lease(QDBusConnection::sessionBus());
        QSignalSpy lost(&lease, &BusLease::lost);
        const QString owner = peer(u"lease-owner"_s);
        QVERIFY(!owner.isEmpty());
        lease.take(owner);
        QVERIFY(lease.held());
        QTest::qWait(50);
        QCOMPARE(lost.size(), 0); // (alive: nothing)
        QDBusConnection::disconnectFromBus(u"lease-owner"_s);
        QTRY_COMPARE(lost.size(), 1);
        QVERIFY(!lease.held());
    }

    void aReleasedLeaseIsNeverLostAndAnotherOwnersExitIsNoMatter()
    {
        BusLease lease(QDBusConnection::sessionBus());
        QSignalSpy lost(&lease, &BusLease::lost);
        const QString mine = peer(u"lease-mine"_s);
        peer(u"lease-other"_s);
        lease.take(mine);
        QDBusConnection::disconnectFromBus(u"lease-other"_s);
        QTest::qWait(100);
        QCOMPARE(lost.size(), 0);
        lease.release();
        QVERIFY(!lease.held());
        QDBusConnection::disconnectFromBus(u"lease-mine"_s);
        QTest::qWait(100);
        QCOMPARE(lost.size(), 0);
    }

    void anOwnerThatIsAlreadyGoneIsLostAtOnce()
    {
        BusLease lease(QDBusConnection::sessionBus());
        QSignalSpy lost(&lease, &BusLease::lost);
        lease.take(u":1.99999"_s);
        QTRY_COMPARE(lost.size(), 1);
    }

    void takingAgainReplacesTheOwner()
    {
        BusLease lease(QDBusConnection::sessionBus());
        QSignalSpy lost(&lease, &BusLease::lost);
        const QString first = peer(u"lease-first"_s);
        const QString second = peer(u"lease-second"_s);
        lease.take(first);
        lease.take(second);
        QDBusConnection::disconnectFromBus(u"lease-first"_s);
        QTest::qWait(100);
        QCOMPARE(lost.size(), 0);
        QDBusConnection::disconnectFromBus(u"lease-second"_s);
        QTRY_COMPARE(lost.size(), 1);
    }
};

QTEST_GUILESS_MAIN(TstBusLease)
#include "tst_buslease.moc"
