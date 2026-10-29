// PuppetItem (src/overlay/puppetitem.*) without a window or an engine: how it tells a refusal from a crash, when it may start, and the command line it launches.
#include "puppetitem.h"
#include "bridgesupport.h"
#include "puppetnames.h"
#include "testutil.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

class Probe : public PuppetItem
{
public:
    using PuppetItem::buildLaunch;
    using PuppetItem::bufferSize;
    using PuppetItem::canStart;
    using PuppetItem::hostExited;
    using PuppetItem::adoptHost;
    using PuppetItem::onHostLost;
    using PuppetItem::stopHost;
    using PuppetItem::setExitWaitMs;
};

QString executable(const QString &path)
{
    testutil::writeFile(path, "#!/bin/sh\nexit 0\n");
    QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::ExeOwner);
    return path;
}

} // namespace

class TestPuppetItem : public QObject
{
    Q_OBJECT
private slots:
    void anEndIsARefusalOrACrash_data();
    void anEndIsARefusalOrACrash();
    void aRefusalIsNotRetriedUntilTheModelChanges();
    void aBlockedModelDoesNotStart();
    void theCommandLineIsTheHosts();
    void aBigPictureIsDrawnSmallerThanTheSetting();
    void theLaunchNeedsTheEngineAndThePromisedSandbox();
    void aPendingExitKillIsCancelledByAStop();
    void anEngineThatFallsOverWhileItEndsStillCounts();
};

void TestPuppetItem::anEndIsARefusalOrACrash_data()
{
    QTest::addColumn<int>("code");
    QTest::addColumn<bool>("signal");
    QTest::addColumn<QString>("refused");
    QTest::addColumn<int>("crashes");
    QTest::newRow("a clean end") << 0 << false << QString() << 0;
    QTest::newRow("the model is refused") << 3 << false << u"model refused"_s << 0;
    QTest::newRow("no GPU") << 4 << false << QString::fromLatin1(puppet::kNoGpu) << 0;
    QTest::newRow("no sandbox") << 125 << false << u"sandbox unavailable"_s << 0;
    QTest::newRow("the program is not there") << 127 << false << u"sandbox unavailable"_s << 0;
    QTest::newRow("a failure") << 1 << false << QString() << 1;
    QTest::newRow("a usage error") << 2 << false << QString() << 1;
    QTest::newRow("the device was lost") << 5 << false << QString() << 1;
    QTest::newRow("bwrap reports a SIGSEGV") << 139 << false << QString() << 1;
    QTest::newRow("killed by a signal") << 11 << true << QString() << 1;
    QTest::newRow("a refusal code after a signal is still a crash") << 3 << true << QString() << 1;
}

void TestPuppetItem::anEndIsARefusalOrACrash()
{
    QFETCH(int, code);
    QFETCH(bool, signal);
    QFETCH(QString, refused);
    QFETCH(int, crashes);
    Probe item;
    item.setItemDir(u"/m/haru"_s);
    item.setModelFile(u"model.inp"_s);
    QSignalSpy crashed(&item, &PuppetItem::engineCrashed);
    QSignalSpy refusals(&item, &PuppetItem::engineRefused);
    item.hostExited(code, signal ? QProcess::CrashExit : QProcess::NormalExit);
    QCOMPARE(crashed.count(), crashes);
    if (crashes)
        QCOMPARE(crashed.at(0).at(0).toBool(), false); // (it had not been drawing: not after a long run)
    QCOMPARE(refusals.count(), refused.isEmpty() ? 0 : 1);
    if (!refused.isEmpty())
        QCOMPARE(refusals.at(0).at(0).toString(), refused);
    QCOMPARE(item.canStart(), refused.isEmpty()); // a refusal is not retried
}

void TestPuppetItem::aRefusalIsNotRetriedUntilTheModelChanges()
{
    Probe item;
    item.setItemDir(u"/m/haru"_s);
    item.setModelFile(u"model.inp"_s);
    item.hostExited(3, QProcess::NormalExit);
    QVERIFY(!item.canStart());
    item.setModelFile(u"other.inp"_s); // the user replaced the model
    QVERIFY(item.canStart());
    item.hostExited(4, QProcess::NormalExit);
    QVERIFY(!item.canStart());
    item.setItemDir(u"/m/kei"_s);
    QVERIFY(item.canStart());
}

void TestPuppetItem::aBlockedModelDoesNotStart()
{
    Probe item;
    QVERIFY(!item.canStart()); // no model yet
    item.setItemDir(u"/m/haru"_s);
    QVERIFY(!item.canStart());
    item.setModelFile(u"model.inp"_s);
    QVERIFY(item.canStart());
    item.setBlocked(true);
    QVERIFY(!item.canStart());
    item.setBlocked(false);
    QVERIFY(item.canStart());
}

void TestPuppetItem::theCommandLineIsTheHosts()
{
    const QStringList plain = PuppetItem::launchCommand(u"/bin/murasaki-puppet"_s, QString(), false, u"/m/haru"_s, u"model.inp"_s, QSize(200, 300), 7, 30);
    QCOMPARE(plain.mid(0, 5), (QStringList{u"/bin/murasaki-puppet"_s, u"--engine"_s, u"--item"_s, u"/m/haru"_s, u"--file"_s}));
    QVERIFY(plain.contains(u"200x300"_s));
    QCOMPARE(plain[plain.indexOf(u"--murasaki-bridge-fd"_s) + 1], u"7"_s);
    QCOMPARE(plain[plain.indexOf(u"--fps"_s) + 1], u"30"_s);
    const QStringList boxed = PuppetItem::launchCommand(u"/bin/murasaki-puppet"_s, u"/bin/murasaki-sandbox"_s, true, u"/m/haru"_s, u"model.inp"_s, QSize(200, 300), 7, 30);
    QCOMPARE(boxed.mid(0, 7), (QStringList{u"/bin/murasaki-sandbox"_s, u"--profile"_s, u"puppet"_s, u"--item"_s, u"/m/haru"_s, u"--require"_s, u"--"_s}));
    QCOMPARE(boxed.mid(7), plain);
}

void TestPuppetItem::aBigPictureIsDrawnSmallerThanTheSetting()
{
    Probe item;
    item.setMaxPixels(1000000);
    QCOMPARE(item.bufferSize(QSize(800, 600)), QSize(800, 600));
    QVERIFY(!item.resolutionReduced());
    const QSize big = item.bufferSize(QSize(3000, 2000)); // 6 M pixels
    QVERIFY(big.width() * big.height() <= 1000000);
    QVERIFY(big.width() * big.height() > 900000);          // (not smaller than it has to be)
    QVERIFY(std::abs(double(big.width()) / big.height() - 1.5) < 0.01); // the aspect is kept
    QVERIFY(item.resolutionReduced());
    QVERIFY(!item.bufferSize(QSize(1000, 1000)).isEmpty());
    QVERIFY(item.bufferSize(QSize(1, 5000000)).width() >= 1);
    QVERIFY(!item.resolutionReduced() || item.bufferSize(QSize(100, 100)) == QSize(100, 100)); // (a small one is as asked)
    QVERIFY(!item.resolutionReduced());
    item.setMaxPixels(-3); // nonsense is ignored
    QCOMPARE(item.maxPixels(), 1000000.0);
}

void TestPuppetItem::theLaunchNeedsTheEngineAndThePromisedSandbox()
{
    QTemporaryDir dir;
    Probe item;
    item.setItemDir(dir.path());
    item.setModelFile(u"model.inp"_s);
    qputenv("MURASAKI_PUPPET", dir.filePath(u"missing-puppet"_s).toUtf8());
    QVERIFY(!item.buildLaunch(5, QSize(10, 10)).error.isEmpty()); // no engine
    qputenv("MURASAKI_PUPPET", executable(dir.filePath(u"murasaki-puppet"_s)).toUtf8());
    qputenv("MURASAKI_SANDBOX", dir.filePath(u"missing-sandbox"_s).toUtf8());
    item.setRequireSandbox(true);
    BridgeLaunch launch = item.buildLaunch(5, QSize(10, 10));
    QVERIFY2(launch.error.contains(u"sandbox is required"_s), qPrintable(launch.error)); // required and missing: it does not run unconfined
    item.setRequireSandbox(false);
    item.setSandbox(false);
    launch = item.buildLaunch(5, QSize(10, 10));
    QVERIFY(launch.error.isEmpty());
    QCOMPARE(launch.program, dir.filePath(u"murasaki-puppet"_s));
    QVERIFY(!launch.arguments.contains(u"--profile"_s));
    qputenv("MURASAKI_SANDBOX", executable(dir.filePath(u"murasaki-sandbox"_s)).toUtf8());
    item.setSandbox(true);
    launch = item.buildLaunch(5, QSize(10, 10));
    QCOMPARE(launch.program, dir.filePath(u"murasaki-sandbox"_s));
    QVERIFY(launch.arguments.contains(u"puppet"_s));
    QVERIFY(!launch.arguments.contains(u"--require"_s));
    item.setRequireSandbox(true);
    QVERIFY(item.buildLaunch(5, QSize(10, 10)).arguments.contains(u"--require"_s));
    qunsetenv("MURASAKI_PUPPET");
    qunsetenv("MURASAKI_SANDBOX");
}

namespace {
QProcess *shell(const QString &script)
{
    auto *process = new QProcess;
    process->start(u"/bin/sh"_s, {u"-c"_s, script});
    return process;
}
} // namespace

void TestPuppetItem::aPendingExitKillIsCancelledByAStop()
{
    // The socket of an engine is lost while it lives on (the exit timer is armed), the item is stopped (a new configuration) and the old engine ends by itself; the next engine must not be killed by the old timer.
    Probe item;
    item.setItemDir(u"/m/haru"_s);
    item.setModelFile(u"model.inp"_s);
    item.setExitWaitMs(200);
    QSignalSpy crashed(&item, &PuppetItem::engineCrashed);
    QProcess *old = shell(u"sleep 0.1"_s);
    QVERIFY(old->waitForStarted(5000));
    item.adoptHost(old);
    item.onHostLost(u"socket lost"_s); // the process lives: the timer is armed, nothing is decided
    item.stopHost();                   // retires the old engine and has to disarm the timer
    QVERIFY(QTest::qWaitFor([&] { return !HostRetirement::anyLeaving(); }, 5000));
    QProcess *next = shell(u"sleep 5"_s);
    QVERIFY(next->waitForStarted(5000));
    item.adoptHost(next);
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 600) // (a negative: the old timer would have fired inside this time)
        QTest::qWait(20);
    QCOMPARE(next->state(), QProcess::Running);
    QCOMPARE(crashed.count(), 0);
    item.stopHost();
    QVERIFY(QTest::qWaitFor([&] { return !HostRetirement::anyLeaving(); }, 5000));
}

void TestPuppetItem::anEngineThatFallsOverWhileItEndsStillCounts()
{
    Probe item;
    item.setItemDir(u"/m/haru"_s);
    item.setModelFile(u"model.inp"_s);
    item.setExitWaitMs(2000);
    QSignalSpy crashed(&item, &PuppetItem::engineCrashed);
    QProcess *falling = shell(u"sleep 0.2; kill -SEGV $$"_s);
    QVERIFY(falling->waitForStarted(5000));
    item.adoptHost(falling);
    item.stopHost();
    QVERIFY(HostRetirement::anyLeaving());
    QTRY_COMPARE_WITH_TIMEOUT(crashed.count(), 1, 5000);

    // an engine the retirement itself kills is not a crash of the model
    crashed.clear();
    QProcess *stubborn = shell(u"trap '' TERM; while :; do sleep 0.05; done"_s);
    QVERIFY(stubborn->waitForStarted(5000));
    item.setExitWaitMs(150);
    item.adoptHost(stubborn);
    item.stopHost();
    QVERIFY(QTest::qWaitFor([&] { return !HostRetirement::anyLeaving(); }, 5000));
    QCOMPARE(crashed.count(), 0);
}

QTEST_MAIN(TestPuppetItem)
#include "tst_puppetitem.moc"
