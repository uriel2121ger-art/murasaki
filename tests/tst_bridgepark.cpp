// BridgeItem parking (src/bridge/qml/bridgeitem.*): a host that stays paused is stopped to give its memory back, and the end of the pause
// takes it out of that state again; a short pause, a spanned picture or an item that does not ask for it keeps its host.
#include "bridgeitem.h"

#include <QProcess>
#include <QTest>

#include <csignal>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

class ParkingItem : public BridgeItem
{
public:
    using BridgeItem::adoptHost;
    using BridgeItem::parked;
    using BridgeItem::setParkAfterMs;

    // A host that never ends by itself.
    QProcess *startSleeper()
    {
        auto *process = new QProcess(this);
        process->start(u"sleep"_s, {u"30"_s});
        if (!process->waitForStarted(5000))
            return nullptr;
        adoptHost(process);
        return process;
    }
};

constexpr int kParkMs = 50;

} // namespace

class TestBridgePark : public QObject
{
    Q_OBJECT
private slots:
    void aHostPausedLongEnoughIsStoppedAndThePauseEndingLetsItStart()
    {
        ParkingItem item;
        item.setParkAfterMs(kParkMs);
        QProcess *host = item.startSleeper();
        QVERIFY(host);
        const qint64 pid = host->processId();
        item.setPaused(true);
        QTRY_VERIFY_WITH_TIMEOUT(item.parked(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(::kill(pid_t(pid), 0) != 0, 5000); // the process is gone
        item.setPaused(false);
        QVERIFY(!item.parked());
    }
    void aShortPauseKeepsTheHost()
    {
        ParkingItem item;
        item.setParkAfterMs(kParkMs * 4);
        QProcess *host = item.startSleeper();
        QVERIFY(host);
        item.setPaused(true);
        QTest::qWait(kParkMs);
        item.setPaused(false);
        QTest::qWait(kParkMs * 6);
        QVERIFY(!item.parked());
        QCOMPARE(host->state(), QProcess::Running);
        host->kill();
        host->waitForFinished();
    }
    void anItemThatDoesNotAskKeepsItsHost()
    {
        ParkingItem item;
        QProcess *host = item.startSleeper();
        QVERIFY(host);
        item.setPaused(true);
        QTest::qWait(kParkMs * 4);
        QVERIFY(!item.parked());
        QCOMPARE(host->state(), QProcess::Running);
        host->kill();
        host->waitForFinished();
    }
    void aSpannedPictureKeepsItsHost()
    {
        ParkingItem item;
        item.setParkAfterMs(kParkMs);
        item.setSpan({{u"canvasW"_s, 200}, {u"canvasH"_s, 100}, {u"x"_s, 0}, {u"y"_s, 0}, {u"w"_s, 100}, {u"h"_s, 100}});
        QProcess *host = item.startSleeper();
        QVERIFY(host);
        item.setPaused(true);
        QTest::qWait(kParkMs * 4);
        QVERIFY(!item.parked());
        QCOMPARE(host->state(), QProcess::Running);
        host->kill();
        host->waitForFinished();
    }
};

QTEST_MAIN(TestBridgePark)
#include "tst_bridgepark.moc"
