// The restart policy of the puppet engine's host (src/overlay/puppetenginehost.*): a fake engine (a shell script) that crashes, refuses or runs, with the real CrashLedger. No GPU.
#include "crashledger.h"
#include "puppet/exitcodes.h"
#include "puppetenginehost.h"
#include "puppetlimits.h"
#include "puppetnames.h"
#include "testutil.h"

#include <QDir>
#include <QSignalSpy>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>

#include <sys/socket.h>
#include <signal.h>

#include <thread>
#include <unistd.h>

using namespace Qt::StringLiterals;
using namespace murasaki;
namespace puppet = murasaki::puppet;

class TestPuppetEngineHost : public QObject
{
    Q_OBJECT
private:
    // A fake engine: it records every launch (its arguments) in <item>/launches.log, then does what <item>/mode says.
    //   crash          dies by SIGSEGV at once
    //   exit N         exits with status N
    //   run            lives until <item>/quit appears, then exits 0
    //   crash-late     lives 0.3 s, then dies by SIGSEGV
    //   stubborn       ignores SIGTERM and lives until <item>/quit appears; writes its pid to <item>/pid
    QString fakeEngine(const QTemporaryDir &dir)
    {
        const QString path = dir.filePath(u"engine.sh"_s);
        testutil::writeFile(path, R"(#!/bin/sh
# --engine --item DIR --file F ...
item="$3"
echo "$@" >> "$item/launches.log"
mode=$(cat "$item/mode")
case "$mode" in
crash) kill -SEGV $$ ;;
exit*) exit ${mode#exit } ;;
run) while [ ! -e "$item/quit" ]; do sleep 0.05; done; exit 0 ;;
crash-late) sleep 0.3; kill -SEGV $$ ;;
stubborn) trap '' TERM; echo $$ > "$item/pid"; while [ ! -e "$item/quit" ]; do sleep 0.05; done; exit 0 ;;
esac
exit 0
)");
        QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::ExeOwner);
        return path;
    }
    static int launchesOf(const QString &item)
    {
        QFile f(item + u"/launches.log"_s);
        return f.open(QIODevice::ReadOnly) ? f.readAll().count('\n') : 0;
    }
    static QString itemWith(const QTemporaryDir &dir, const QString &name, const QByteArray &mode)
    {
        const QString item = dir.filePath(name);
        testutil::writeFile(item + u"/mode"_s, mode);
        testutil::writeFile(item + u"/p.inp"_s, "model");
        return item;
    }
    PuppetEngineHost::Config config(const QString &engine, bool sandbox = false)
    {
        PuppetEngineHost::Config c;
        c.enginePath = engine;
        c.requireSandbox = sandbox;
        c.restartDelayMs = 10;
        return c;
    }

private slots:
    void theCommandLine();
    void threeCrashesBlockTheModel();
    void theBlockSurvivesARestartOfTheOverlayAndAChangedFile();
    void aCrashAfterTheFirstFrameCountsToo();
    void refusalsAreNotCrashes();
    void aRunningEngineStopsOnRequest();
    void eachStartGetsItsOwnSocket();
    void theMarkerIsClearedWhenThePictureShows();
    void requiringTheSandboxWithoutOneRefuses();
    void noProgramIsRefusedNotCounted();
    void aLongRunBeforeACrashStartsTheCountAgain();
    void startingAndStoppingDoNotWait();
    void aDescriptorNobodyTakesIsClosed();
    void startingFromAnotherThreadIsRefused();
    void aStartRightAfterAStopLeavesOneEngine();
};

void TestPuppetEngineHost::theCommandLine()
{
    QCOMPARE(qint64(PuppetEngineHost::Config().longRunMs), puppet::kEngineLongRunSeconds * 1000); // (the table's row is the one the host uses)
    PuppetEngineHost::Config c;
    c.enginePath = u"/usr/bin/murasaki-puppet"_s;
    c.fps = 15;
    c.seed = 9;
    const QStringList plain = PuppetEngineHost::command(c, u"/m/haru"_s, u"p.inp"_s, QSize(300, 400), 7);
    QCOMPARE(plain, (QStringList{u"/usr/bin/murasaki-puppet"_s, u"--engine"_s, u"--item"_s, u"/m/haru"_s, u"--file"_s, u"p.inp"_s, u"--murasaki-bridge-fd"_s, u"7"_s, u"--murasaki-size"_s, u"300x400"_s,
                                 u"--fps"_s, u"15"_s, u"--seed"_s, u"9"_s}));
    c.sandboxPath = u"/usr/bin/murasaki-sandbox"_s;
    c.requireSandbox = true;
    const QStringList boxed = PuppetEngineHost::command(c, u"/m/haru"_s, u"p.inp"_s, QSize(300, 400), 7);
    QCOMPARE(boxed.mid(0, 7), (QStringList{u"/usr/bin/murasaki-sandbox"_s, u"--profile"_s, u"puppet"_s, u"--item"_s, u"/m/haru"_s, u"--require"_s, u"--"_s}));
    QCOMPARE(boxed.mid(7), plain);
    c.corePath = u"/opt/live2d/libCubismCore.so"_s;
    c.requireSandbox = false;
    const QStringList withCore = PuppetEngineHost::command(c, u"/m/haru"_s, u"p.inp"_s, QSize(300, 400), 7);
    QVERIFY(withCore.contains(u"--core"_s));
    QCOMPARE(withCore[withCore.indexOf(u"--core"_s) + 1], u"/opt/live2d/libCubismCore.so"_s);
    QVERIFY(!withCore.contains(u"--require"_s));
}

void TestPuppetEngineHost::threeCrashesBlockTheModel()
{
    QTemporaryDir dir;
    const QString item = itemWith(dir, u"item"_s, "crash");
    CrashLedger ledger(dir.filePath(u"state"_s));
    ledger.load();
    PuppetEngineHost host(config(fakeEngine(dir)), &ledger);
    QSignalSpy degraded(&host, &PuppetEngineHost::degraded);
    QSignalSpy fds(&host, &PuppetEngineHost::consumerFd);
    host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
    QVERIFY(QTest::qWaitFor([&] { return host.state() == PuppetEngineHost::State::Blocked; }, 15000));
    // the first crash restarted the engine, the second too, the third blocked it: three launches, never a fourth
    QCOMPARE(host.launches(), 3);
    QCOMPARE(launchesOf(item), 3);
    QCOMPARE(host.crashes(), 3);
    QCOMPARE(degraded.size(), 3);
    QCOMPARE(degraded.at(0).at(0).toString(), QString::fromUtf8(puppet::kEngineRestarted.data(), puppet::kEngineRestarted.size()));
    QCOMPARE(degraded.at(1).at(0).toString(), QString::fromUtf8(puppet::kEngineRestarted.data(), puppet::kEngineRestarted.size()));
    QCOMPARE(degraded.at(2).at(0).toString(), QString::fromUtf8(puppet::kBlockedAfterCrashes.data(), puppet::kBlockedAfterCrashes.size()));
    QCOMPARE(fds.size(), 3);
    QTest::qWait(300);
    QCOMPARE(launchesOf(item), 3);
    // asking again changes nothing
    host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
    QCOMPARE(host.state(), PuppetEngineHost::State::Blocked);
    QCOMPARE(launchesOf(item), 3);
    for (const QVariantList &fd : fds)
        ::close(fd.at(0).toInt());
}

void TestPuppetEngineHost::theBlockSurvivesARestartOfTheOverlayAndAChangedFile()
{
    QTemporaryDir dir;
    const QString item = itemWith(dir, u"item"_s, "crash");
    const QString state = dir.filePath(u"state"_s);
    {
        CrashLedger ledger(state);
        ledger.load();
        PuppetEngineHost host(config(fakeEngine(dir)), &ledger);
        QSignalSpy fds(&host, &PuppetEngineHost::consumerFd);
        host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
        QVERIFY(QTest::qWaitFor([&] { return host.state() == PuppetEngineHost::State::Blocked; }, 15000));
        for (const QVariantList &fd : fds)
            ::close(fd.at(0).toInt());
    }
    // a new run of the overlay: a new ledger on the same state directory
    CrashLedger again(state);
    again.load();
    PuppetEngineHost host(config(fakeEngine(dir)), &again);
    host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
    QCOMPARE(host.state(), PuppetEngineHost::State::Blocked);
    QCOMPARE(host.launches(), 0);
    QCOMPARE(launchesOf(item), 3);
    // the user replaces the model file: it is another file, tried again
    testutil::writeFile(item + u"/p.inp"_s, "a different and longer model file");
    testutil::writeFile(item + u"/mode"_s, "run");
    QSignalSpy fds(&host, &PuppetEngineHost::consumerFd);
    host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
    QCOMPARE(host.state(), PuppetEngineHost::State::Running);
    QVERIFY(QTest::qWaitFor([&] { return launchesOf(item) == 4; }, 10000));
    host.stop();
    QCOMPARE(host.state(), PuppetEngineHost::State::Idle);
    for (const QVariantList &fd : fds)
        ::close(fd.at(0).toInt());
}

void TestPuppetEngineHost::aCrashAfterTheFirstFrameCountsToo()
{
    QTemporaryDir dir;
    const QString item = itemWith(dir, u"item"_s, "crash-late");
    CrashLedger ledger(dir.filePath(u"state"_s));
    ledger.load();
    PuppetEngineHost host(config(fakeEngine(dir)), &ledger);
    QSignalSpy fds(&host, &PuppetEngineHost::consumerFd);
    QSignalSpy degraded(&host, &PuppetEngineHost::degraded);
    host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
    // each run shows its first picture, and then the engine dies: the crashes still add up
    int shown = 0;
    while (host.state() != PuppetEngineHost::State::Blocked && shown < 5) {
        if (host.state() == PuppetEngineHost::State::Running)
            host.firstFrameShown();
        ++shown;
        QTest::qWait(150);
    }
    QVERIFY(QTest::qWaitFor([&] { return host.state() == PuppetEngineHost::State::Blocked; }, 15000));
    QCOMPARE(host.crashes(), 3);
    QCOMPARE(launchesOf(item), 3);
    QVERIFY(degraded.size() == 3);
    for (const QVariantList &fd : fds)
        ::close(fd.at(0).toInt());
}

void TestPuppetEngineHost::refusalsAreNotCrashes()
{
    const struct {
        const char *mode;
        PuppetEngineHost::State state;
        const char *degraded;
    } cases[] = {
        {"exit 3", PuppetEngineHost::State::Refused, "model refused"},
        {"exit 4", PuppetEngineHost::State::NoGpu, "puppet: no GPU"},
        {"exit 125", PuppetEngineHost::State::Refused, "sandbox unavailable"},
        {"exit 127", PuppetEngineHost::State::Refused, "sandbox unavailable"},
    };
    for (const auto &c : cases) {
        QTemporaryDir dir;
        const QString item = itemWith(dir, u"item"_s, c.mode);
        CrashLedger ledger(dir.filePath(u"state"_s));
        ledger.load();
        PuppetEngineHost host(config(fakeEngine(dir)), &ledger);
        QSignalSpy fds(&host, &PuppetEngineHost::consumerFd);
        QSignalSpy degraded(&host, &PuppetEngineHost::degraded);
        host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
        QVERIFY2(QTest::qWaitFor([&] { return host.state() == c.state; }, 15000), c.mode);
        QTest::qWait(200);
        QCOMPARE(launchesOf(item), 1);   // no retry
        QCOMPARE(host.crashes(), 0);     // and nothing charged to the model
        QCOMPARE(degraded.size(), 1);
        QCOMPARE(degraded.at(0).at(0).toString(), QString::fromLatin1(c.degraded));
        for (const QVariantList &fd : fds)
            ::close(fd.at(0).toInt());
    }
    // other statuses (1, 2, 5, 77) and a plain exit 0 after a start: failures are crashes, 0 is a clean end
    for (const char *mode : {"exit 1", "exit 2", "exit 5", "exit 77"}) {
        QTemporaryDir dir;
        const QString item = itemWith(dir, u"item"_s, mode);
        CrashLedger ledger(dir.filePath(u"state"_s));
        ledger.load();
        PuppetEngineHost host(config(fakeEngine(dir)), &ledger);
        QSignalSpy fds(&host, &PuppetEngineHost::consumerFd);
        host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
        QVERIFY2(QTest::qWaitFor([&] { return host.state() == PuppetEngineHost::State::Blocked; }, 15000), mode);
        QCOMPARE(host.crashes(), 3);
        for (const QVariantList &fd : fds)
            ::close(fd.at(0).toInt());
    }
    QTemporaryDir dir;
    const QString item = itemWith(dir, u"item"_s, "exit 0");
    CrashLedger ledger(dir.filePath(u"state"_s));
    ledger.load();
    PuppetEngineHost host(config(fakeEngine(dir)), &ledger);
    QSignalSpy fds(&host, &PuppetEngineHost::consumerFd);
    host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
    QVERIFY(QTest::qWaitFor([&] { return host.state() == PuppetEngineHost::State::Idle && launchesOf(item) == 1; }, 15000));
    QCOMPARE(host.crashes(), 0);
    for (const QVariantList &fd : fds)
        ::close(fd.at(0).toInt());
}

void TestPuppetEngineHost::aRunningEngineStopsOnRequest()
{
    QTemporaryDir dir;
    const QString item = itemWith(dir, u"item"_s, "run");
    CrashLedger ledger(dir.filePath(u"state"_s));
    ledger.load();
    PuppetEngineHost host(config(fakeEngine(dir)), &ledger);
    QSignalSpy fds(&host, &PuppetEngineHost::consumerFd);
    host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
    QCOMPARE(host.state(), PuppetEngineHost::State::Running);
    QVERIFY(QTest::qWaitFor([&] { return launchesOf(item) == 1; }, 10000));
    host.stop();
    QCOMPARE(host.state(), PuppetEngineHost::State::Idle);
    QTest::qWait(300);
    QCOMPARE(launchesOf(item), 1);   // terminating it is not a crash: no restart
    QCOMPARE(host.crashes(), 0);
    // and a stopped host can be started again
    host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
    QVERIFY(QTest::qWaitFor([&] { return launchesOf(item) == 2; }, 10000));
    QFile quit(item + u"/quit"_s);
    QVERIFY(quit.open(QIODevice::WriteOnly)); // the engine ends by itself, cleanly
    QVERIFY(QTest::qWaitFor([&] { return host.state() == PuppetEngineHost::State::Idle; }, 10000));
    QCOMPARE(host.crashes(), 0);
    for (const QVariantList &fd : fds)
        ::close(fd.at(0).toInt());
}

void TestPuppetEngineHost::eachStartGetsItsOwnSocket()
{
    QTemporaryDir dir;
    const QString item = itemWith(dir, u"item"_s, "crash");
    CrashLedger ledger(dir.filePath(u"state"_s));
    ledger.load();
    PuppetEngineHost host(config(fakeEngine(dir)), &ledger);
    QSignalSpy fds(&host, &PuppetEngineHost::consumerFd);
    host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
    QVERIFY(QTest::qWaitFor([&] { return host.state() == PuppetEngineHost::State::Blocked; }, 15000));
    QCOMPARE(fds.size(), 3);
    // three descriptors, all open sockets of ours, the engine's end of each gone with its process: reading sees the end
    QSet<int> seen;
    for (const QVariantList &fd : fds) {
        const int n = fd.at(0).toInt();
        QVERIFY(n >= 0);
        seen.insert(n);
        char byte;
        QCOMPARE(::recv(n, &byte, 1, MSG_DONTWAIT), ssize_t(0)); // closed by the peer: end of file, not "no data yet"
        ::close(n);
    }
    QCOMPARE(seen.size(), 3);
}

void TestPuppetEngineHost::theMarkerIsClearedWhenThePictureShows()
{
    QTemporaryDir dir;
    const QString item = itemWith(dir, u"item"_s, "run");
    const QString state = dir.filePath(u"state"_s);
    CrashLedger ledger(state);
    ledger.load();
    PuppetEngineHost host(config(fakeEngine(dir)), &ledger);
    QSignalSpy fds(&host, &PuppetEngineHost::consumerFd);
    host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
    QCOMPARE(QDir(state).entryList({u"opening-*.json"_s}, QDir::Files).size(), 1); // while the model opens, a marker
    host.firstFrameShown();
    QCOMPARE(QDir(state).entryList({u"opening-*.json"_s}, QDir::Files).size(), 0);
    QVERIFY(QTest::qWaitFor([&] { return launchesOf(item) == 1; }, 10000));
    host.stop();
    // a marker left by a run of the overlay that died while opening is a crash of the model at the next start
    host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
    QCOMPARE(QDir(state).entryList({u"opening-*.json"_s}, QDir::Files).size(), 1);
    QVERIFY(QTest::qWaitFor([&] { return launchesOf(item) == 2; }, 10000));
    CrashLedger next(state); // (the overlay is killed here: nobody cleared the marker)
    next.load();
    QCOMPARE(next.crashes(item + u"/p.inp"_s), 1);
    host.stop();
    for (const QVariantList &fd : fds)
        ::close(fd.at(0).toInt());
}

void TestPuppetEngineHost::requiringTheSandboxWithoutOneRefuses()
{
    QTemporaryDir dir;
    const QString item = itemWith(dir, u"item"_s, "run");
    CrashLedger ledger(dir.filePath(u"state"_s));
    ledger.load();
    PuppetEngineHost host(config(fakeEngine(dir), /*requireSandbox=*/true), &ledger); // no helper given
    QSignalSpy degraded(&host, &PuppetEngineHost::degraded);
    host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
    QCOMPARE(host.state(), PuppetEngineHost::State::Refused);
    QCOMPARE(degraded.size(), 1);
    QCOMPARE(degraded.at(0).at(0).toString(), u"sandbox unavailable"_s);
    QCOMPARE(launchesOf(item), 0);
}

void TestPuppetEngineHost::noProgramIsRefusedNotCounted()
{
    QTemporaryDir dir;
    const QString item = itemWith(dir, u"item"_s, "run");
    CrashLedger ledger(dir.filePath(u"state"_s));
    ledger.load();
    PuppetEngineHost host(config(dir.filePath(u"missing-engine"_s)), &ledger);
    QSignalSpy fds(&host, &PuppetEngineHost::consumerFd);
    QSignalSpy degraded(&host, &PuppetEngineHost::degraded);
    host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
    QVERIFY(QTest::qWaitFor([&] { return host.state() == PuppetEngineHost::State::Refused; }, 10000)); // (reported by a signal: start() does not wait)
    QCOMPARE(degraded.size(), 1);
    QCOMPARE(degraded.at(0).at(0).toString(), u"cannot start the engine"_s);
    QCOMPARE(host.crashes(), 0);
    QCOMPARE(QDir(dir.filePath(u"state"_s)).entryList({u"opening-*.json"_s}, QDir::Files).size(), 0);
    for (const QVariantList &fd : fds)
        ::close(fd.at(0).toInt());
}

namespace {

int openDescriptors()
{
    return QDir(u"/proc/self/fd"_s).entryList(QDir::NoDotAndDotDot).size();
}

} // namespace

void TestPuppetEngineHost::aLongRunBeforeACrashStartsTheCountAgain()
{
    // the engine lives 0.3 s and then crashes; a run counts as long from 0.2 s here (60 s in the overlay)
    QTemporaryDir dir;
    const QString item = itemWith(dir, u"item"_s, "crash-late");
    CrashLedger ledger(dir.filePath(u"state"_s));
    ledger.load();
    PuppetEngineHost::Config c = config(fakeEngine(dir));
    c.longRunMs = 200;
    PuppetEngineHost host(c, &ledger);
    QSignalSpy fds(&host, &PuppetEngineHost::consumerFd);
    host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
    QVERIFY(QTest::qWaitFor([&] {
        host.firstFrameShown(); // (every run shows its picture at once; the clock starts there)
        return launchesOf(item) >= 5;
    }, 20000));
    // five crashes, none of them blocks: each one started the count again
    QVERIFY(host.state() != PuppetEngineHost::State::Blocked);
    QCOMPARE(host.crashes(), 1);
    host.stop();
    for (const QVariantList &fd : fds)
        ::close(fd.at(0).toInt());
    // the same engine with the long run out of reach is blocked at the third
    QTemporaryDir other;
    const QString item2 = itemWith(other, u"item"_s, "crash-late");
    CrashLedger ledger2(other.filePath(u"state"_s));
    ledger2.load();
    PuppetEngineHost::Config c2 = config(fakeEngine(other));
    c2.longRunMs = 1800000;
    PuppetEngineHost host2(c2, &ledger2);
    QSignalSpy fds2(&host2, &PuppetEngineHost::consumerFd);
    host2.start(u"pet1"_s, item2, u"p.inp"_s, QSize(64, 64));
    QVERIFY(QTest::qWaitFor([&] { return host2.state() == PuppetEngineHost::State::Blocked; }, 20000));
    QCOMPARE(host2.crashes(), 3);
    for (const QVariantList &fd : fds2)
        ::close(fd.at(0).toInt());
}

void TestPuppetEngineHost::startingAndStoppingDoNotWait()
{
    QTemporaryDir dir;
    const QString item = itemWith(dir, u"item"_s, "stubborn");
    CrashLedger ledger(dir.filePath(u"state"_s));
    ledger.load();
    PuppetEngineHost::Config c = config(fakeEngine(dir));
    c.stopGraceMs = 400;
    PuppetEngineHost host(c, &ledger);
    QSignalSpy fds(&host, &PuppetEngineHost::consumerFd);
    QElapsedTimer timer;
    timer.start();
    host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
    QVERIFY2(timer.elapsed() < 500, qPrintable(QString::number(timer.elapsed()))); // (it used to wait for the process to start)
    QVERIFY(QTest::qWaitFor([&] { return QFile::exists(item + u"/pid"_s); }, 10000));
    QFile pidFile(item + u"/pid"_s);
    QVERIFY(pidFile.open(QIODevice::ReadOnly));
    const pid_t pid = pid_t(pidFile.readAll().trimmed().toLongLong());
    QVERIFY(pid > 1);
    timer.restart();
    host.stop(); // SIGTERM is ignored by this engine: stop() still returns at once
    QVERIFY2(timer.elapsed() < 200, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(host.state(), PuppetEngineHost::State::Idle);
    QVERIFY(::kill(pid, 0) == 0); // still there for now
    // and the grace period ends it
    QVERIFY(QTest::qWaitFor([&] { return ::kill(pid, 0) != 0; }, 10000));
    QCOMPARE(host.crashes(), 0); // (stopping is not a crash)
    QCOMPARE(launchesOf(item), 1);
    for (const QVariantList &fd : fds)
        ::close(fd.at(0).toInt());
}

void TestPuppetEngineHost::aDescriptorNobodyTakesIsClosed()
{
    QTemporaryDir dir;
    const QString item = itemWith(dir, u"item"_s, "run");
    CrashLedger ledger(dir.filePath(u"state"_s));
    ledger.load();
    PuppetEngineHost host(config(fakeEngine(dir)), &ledger);
    // nobody connected to consumerFd: the consumer end does not stay open in this process
    const int before = openDescriptors();
    host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
    QVERIFY(QTest::qWaitFor([&] { return launchesOf(item) == 1; }, 10000));
    const int without = openDescriptors() - before;
    host.stop();
    QVERIFY(QTest::qWaitFor([&] { return openDescriptors() <= before + 1; }, 10000));
    // with a receiver that keeps it, there is exactly one more
    QSignalSpy fds(&host, &PuppetEngineHost::consumerFd);
    const int before2 = openDescriptors();
    host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
    QVERIFY(QTest::qWaitFor([&] { return launchesOf(item) == 2; }, 10000));
    const int with = openDescriptors() - before2;
    QCOMPARE(fds.size(), 1);
    QCOMPARE(with, without + 1);
    host.stop();
    for (const QVariantList &fd : fds)
        ::close(fd.at(0).toInt());
}

void TestPuppetEngineHost::startingFromAnotherThreadIsRefused()
{
    // PR_SET_PDEATHSIG would fire when that thread ended: the engine would die with a worker
    QTemporaryDir dir;
    const QString item = itemWith(dir, u"item"_s, "run");
    CrashLedger ledger(dir.filePath(u"state"_s));
    ledger.load();
    PuppetEngineHost host(config(fakeEngine(dir)), &ledger);
    QSignalSpy degraded(&host, &PuppetEngineHost::degraded);
    std::thread worker([&] { host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64)); });
    worker.join();
    QCOMPARE(host.state(), PuppetEngineHost::State::Refused);
    QCOMPARE(degraded.size(), 1);
    QCOMPARE(degraded.at(0).at(0).toString(), u"start from the main thread"_s);
    QCOMPARE(launchesOf(item), 0);
}

void TestPuppetEngineHost::aStartRightAfterAStopLeavesOneEngine()
{
    // the first engine ignores SIGTERM and would live through the grace period: a start() at once kills it first
    QTemporaryDir dir;
    const QString item = itemWith(dir, u"item"_s, "stubborn");
    CrashLedger ledger(dir.filePath(u"state"_s));
    ledger.load();
    PuppetEngineHost::Config c = config(fakeEngine(dir));
    c.stopGraceMs = 60000;
    PuppetEngineHost host(c, &ledger);
    QSignalSpy fds(&host, &PuppetEngineHost::consumerFd);
    host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64));
    QVERIFY(QTest::qWaitFor([&] { return QFile::exists(item + u"/pid"_s); }, 10000));
    QFile pidFile(item + u"/pid"_s);
    QVERIFY(pidFile.open(QIODevice::ReadOnly));
    const pid_t first = pid_t(pidFile.readAll().trimmed().toLongLong());
    pidFile.close();
    QFile::remove(item + u"/pid"_s);
    host.stop();
    host.start(u"pet1"_s, item, u"p.inp"_s, QSize(64, 64)); // (right away)
    QVERIFY2(::kill(first, 0) != 0, "the first engine is gone before the second starts");
    QVERIFY(QTest::qWaitFor([&] { return QFile::exists(item + u"/pid"_s); }, 10000));
    QFile second(item + u"/pid"_s);
    QVERIFY(second.open(QIODevice::ReadOnly));
    QVERIFY(pid_t(second.readAll().trimmed().toLongLong()) != first);
    QFile quit(item + u"/quit"_s);
    QVERIFY(quit.open(QIODevice::WriteOnly));
    QVERIFY(QTest::qWaitFor([&] { return host.state() == PuppetEngineHost::State::Idle; }, 10000));
    for (const QVariantList &fd : fds)
        ::close(fd.at(0).toInt());
}

QTEST_GUILESS_MAIN(TestPuppetEngineHost)
#include "tst_puppetenginehost.moc"
