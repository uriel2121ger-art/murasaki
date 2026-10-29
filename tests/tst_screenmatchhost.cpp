// The host of murasaki-screenmatch (src/overlay/screenmatchhost.*): a fake helper (a shell script) that crashes, refuses, runs or talks, with the real CrashLedger. No PipeWire, no sandbox.
#include "crashledger.h"
#include "screenmatch/exitcodes.h"
#include "screenmatchhost.h"
#include "testutil.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

using namespace Qt::StringLiterals;
using namespace murasaki;
namespace sm = murasaki::screenmatch;

Q_DECLARE_METATYPE(murasaki::screenmatch::Reply)

class TestScreenMatchHost : public QObject
{
    Q_OBJECT
private:
    // The fake helper: it logs its arguments to <dir>/launches.log, then does what <dir>/mode says.
    //   crash       dies by SIGSEGV at once            exit N     exits with status N
    //   run         says hello, answers every line with `ok <word>` until it reads `quit`, then exits 0
    //   noise       says hello, then garbage and one good reply
    //   crash-late  lives 0.3 s, then dies by SIGSEGV
    //   stubborn    ignores SIGTERM and never reads; lives until <dir>/quit appears
    //   fds         records whether its two descriptors are open, then exits 0
    QString fakeHelper(const QTemporaryDir &dir)
    {
        const QString path = dir.filePath(u"helper.sh"_s);
        testutil::writeFile(path, R"(#!/bin/bash
# --chan-fd N --pw-fd M --nodes IDS ...
d=$(dirname "$0")
chan=$2; pw=$4
echo "$@" >> "$d/launches.log"
mode=$(cat "$d/mode")
case "$mode" in
crash) kill -SEGV $$ ;;
exit*) exit ${mode#exit } ;;
run) echo "hello 1" >&$chan
     while read -r -u $chan line; do
       [ "$line" = quit ] && exit 0
       echo "ok ${line%% *}" >&$chan
     done; exit 0 ;;
noise) printf 'hello 1\nthis is not a reply\n\x01\x02\n' >&$chan; echo "pong" >&$chan; sleep 0.3; exit 0 ;;
crash-late) sleep 0.3; kill -SEGV $$ ;;
stubborn) trap '' TERM; echo $$ > "$d/pid"; while [ ! -e "$d/quit" ]; do sleep 0.05; done; exit 0 ;;
fds) { [ -e /proc/self/fd/$chan ] && [ -e /proc/self/fd/$pw ] && echo both || echo missing; } > "$d/fds.txt"; exit 0 ;;
esac
exit 0
)");
        QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::ExeOwner);
        testutil::writeFile(dir.filePath(u"mode"_s), "run");
        return path;
    }
    static void setMode(const QTemporaryDir &dir, const QByteArray &mode) { testutil::writeFile(dir.filePath(u"mode"_s), mode); }
    static int launchesOf(const QTemporaryDir &dir)
    {
        QFile f(dir.filePath(u"launches.log"_s));
        return f.open(QIODevice::ReadOnly) ? f.readAll().count('\n') : 0;
    }
    ScreenMatchHost::Config config(const QString &helper)
    {
        ScreenMatchHost::Config c;
        c.helperPath = helper;
        c.requireSandbox = false;
        c.stopGraceMs = 300;
        return c;
    }
    struct Pipe {
        int fds[2] = {-1, -1};
        Pipe() { QVERIFY(::pipe(fds) == 0); }
        ~Pipe() { ::close(fds[0]); ::close(fds[1]); }
    };

private slots:
    void initTestCase() { qRegisterMetaType<murasaki::screenmatch::Reply>(); qRegisterMetaType<murasaki::ScreenMatchHost::State>(); }

    void theCommandLine()
    {
        ScreenMatchHost::Config c;
        c.helperPath = u"/usr/bin/murasaki-screenmatch"_s;
        const QStringList plain = ScreenMatchHost::command(c, 5, 6, {12, 34});
        QCOMPARE(plain, (QStringList{u"/usr/bin/murasaki-screenmatch"_s, u"--chan-fd"_s, u"5"_s, u"--pw-fd"_s, u"6"_s, u"--nodes"_s, u"12,34"_s}));
        c.sandboxPath = u"/usr/bin/murasaki-sandbox"_s;
        c.allowCapture = true;
        c.allowDmaBuf = true;
        const QStringList boxed = ScreenMatchHost::command(c, 5, 6, {12});
        QCOMPARE(boxed.mid(0, 5), (QStringList{u"/usr/bin/murasaki-sandbox"_s, u"--profile"_s, u"screenmatch"_s, u"--require"_s, u"--"_s}));
        QCOMPARE(boxed.mid(5), (QStringList{u"/usr/bin/murasaki-screenmatch"_s, u"--chan-fd"_s, u"5"_s, u"--pw-fd"_s, u"6"_s, u"--nodes"_s, u"12"_s, u"--buffers"_s, u"any"_s, u"--allow-capture"_s}));
        c.requireSandbox = false;
        QVERIFY(!ScreenMatchHost::command(c, 5, 6, {12}).contains(u"--require"_s));
        // no option names a file or a frame source
        for (const QString &arg : boxed)
            QVERIFY2(arg != u"--file"_s && arg != u"--source"_s && arg != u"--src"_s && arg != u"--frames"_s && arg != u"--path"_s && arg != u"--fake"_s, qPrintable(arg));
    }

    void aHelperTalksAndIsTold()
    {
        QTemporaryDir dir;
        const QString helper = fakeHelper(dir);
        CrashLedger ledger(dir.filePath(u"state"_s));
        ledger.load();
        ScreenMatchHost host(config(helper), &ledger);
        QSignalSpy replies(&host, &ScreenMatchHost::reply);
        Pipe pw;
        host.start(pw.fds[0], {7, 9});
        QCOMPARE(host.state(), ScreenMatchHost::State::Running);
        QVERIFY(QTest::qWaitFor([&] { return replies.size() >= 1; }, 10000));
        QCOMPARE(replies.at(0).at(0).value<sm::Reply>().type, sm::ReplyType::Hello);
        sm::Command c;
        c.type = sm::CommandType::Rate;
        c.rate = 4;
        host.send(c);
        c.type = sm::CommandType::Start;
        host.send(c);
        QVERIFY(QTest::qWaitFor([&] { return replies.size() >= 3; }, 10000));
        QCOMPARE(replies.at(1).at(0).value<sm::Reply>().word, u"rate"_s);
        QCOMPARE(replies.at(2).at(0).value<sm::Reply>().word, u"start"_s);
        QVERIFY(QFile::exists(dir.filePath(u"launches.log"_s)));
        host.stop(); // `quit`: the helper ends by itself
        QVERIFY(QTest::qWaitFor([&] { return host.state() == ScreenMatchHost::State::Idle; }, 5000));
        QCOMPARE(host.crashes(), 0);
    }

    void garbageFromTheHelperIsCountedAndNeverPassedOn()
    {
        QTemporaryDir dir;
        const QString helper = fakeHelper(dir);
        setMode(dir, "noise");
        ScreenMatchHost host(config(helper), nullptr);
        QSignalSpy replies(&host, &ScreenMatchHost::reply);
        Pipe pw;
        host.start(pw.fds[0], {7});
        QVERIFY(QTest::qWaitFor([&] { return replies.size() >= 2; }, 10000));
        QCOMPARE(replies.size(), 2); // hello and pong
        QCOMPARE(host.badLines(), 2);
        QVERIFY(QTest::qWaitFor([&] { return host.state() == ScreenMatchHost::State::Idle; }, 5000));
    }

    void bothDescriptorsReachTheHelperAndNoOtherDoes()
    {
        QTemporaryDir dir;
        const QString helper = fakeHelper(dir);
        setMode(dir, "fds");
        ScreenMatchHost host(config(helper), nullptr);
        Pipe pw;
        host.start(pw.fds[0], {7});
        QVERIFY(QTest::qWaitFor([&] { return host.state() == ScreenMatchHost::State::Idle; }, 10000));
        QFile f(dir.filePath(u"fds.txt"_s));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll().trimmed(), QByteArray("both"));
        QVERIFY(::fcntl(pw.fds[0], F_GETFD) >= 0); // the caller keeps its own descriptor
    }

    void threeCrashesBlockTheHelperAcrossRestartsOfTheDaemon()
    {
        QTemporaryDir dir;
        const QString helper = fakeHelper(dir);
        setMode(dir, "crash");
        CrashLedger ledger(dir.filePath(u"state"_s));
        ledger.load();
        {
            ScreenMatchHost host(config(helper), &ledger);
            QSignalSpy degraded(&host, &ScreenMatchHost::degraded);
            Pipe pw;
            for (int i = 1; i <= 3; ++i) {
                host.start(pw.fds[0], {7});
                QVERIFY(QTest::qWaitFor([&] { return host.state() == (i < 3 ? ScreenMatchHost::State::Crashed : ScreenMatchHost::State::Blocked); }, 10000));
                QCOMPARE(host.crashes(), i);
            }
            QCOMPARE(degraded.at(0).at(0).toString(), u"screen: helper crashed"_s);
            QCOMPARE(degraded.at(2).at(0).toString(), u"screen: helper keeps crashing, turned off"_s);
            QCOMPARE(host.launches(), 3);
            // a fourth start is refused without launching
            host.start(pw.fds[0], {7});
            QCOMPARE(host.state(), ScreenMatchHost::State::Blocked);
            QCOMPARE(host.launches(), 3);
            QCOMPARE(launchesOf(dir), 3);
        }
        // the block is in the ledger: a new daemon starts blocked
        CrashLedger again(dir.filePath(u"state"_s));
        again.load();
        ScreenMatchHost fresh(config(helper), &again);
        Pipe pw;
        fresh.start(pw.fds[0], {7});
        QCOMPARE(fresh.state(), ScreenMatchHost::State::Blocked);
        QCOMPARE(launchesOf(dir), 3);
        // turned on again by the user: forgotten, started
        setMode(dir, "run");
        fresh.resetCrashes();
        QCOMPARE(fresh.crashes(), 0);
        fresh.start(pw.fds[0], {7});
        QCOMPARE(fresh.state(), ScreenMatchHost::State::Running);
        QVERIFY(QTest::qWaitFor([&] { return launchesOf(dir) == 4; }, 10000));
        fresh.stop();
    }

    void refusalsAndTheDesktopEndingTheShareAreNotCrashes_data()
    {
        QTest::addColumn<QByteArray>("mode");
        QTest::addColumn<int>("state");
        QTest::addColumn<QString>("why");
        QTest::newRow("clean") << QByteArray("exit 0") << int(ScreenMatchHost::State::Idle) << QString();
        QTest::newRow("no pipewire") << QByteArray("exit 3") << int(ScreenMatchHost::State::NoPipeWire) << u"screen: no PipeWire"_s;
        QTest::newRow("stream lost") << QByteArray("exit 4") << int(ScreenMatchHost::State::StreamLost) << u"screen: stopped by the desktop"_s;
        QTest::newRow("sandbox unavailable") << QByteArray("exit 125") << int(ScreenMatchHost::State::Refused) << u"sandbox unavailable"_s;
        QTest::newRow("program not found") << QByteArray("exit 127") << int(ScreenMatchHost::State::Refused) << u"sandbox unavailable"_s;
    }
    void refusalsAndTheDesktopEndingTheShareAreNotCrashes()
    {
        QFETCH(QByteArray, mode);
        QFETCH(int, state);
        QFETCH(QString, why);
        QTemporaryDir dir;
        const QString helper = fakeHelper(dir);
        setMode(dir, mode);
        CrashLedger ledger(dir.filePath(u"state"_s));
        ledger.load();
        ScreenMatchHost host(config(helper), &ledger);
        QSignalSpy degraded(&host, &ScreenMatchHost::degraded);
        Pipe pw;
        host.start(pw.fds[0], {7});
        QVERIFY(QTest::qWaitFor([&] { return host.state() == ScreenMatchHost::State(state); }, 10000));
        QCOMPARE(host.crashes(), 0);
        QCOMPARE(degraded.isEmpty() ? QString() : degraded.last().at(0).toString(), why);
    }

    void anyOtherExitStatusIsACrash()
    {
        QTemporaryDir dir;
        const QString helper = fakeHelper(dir);
        setMode(dir, "exit 42");
        CrashLedger ledger(dir.filePath(u"state"_s));
        ledger.load();
        ScreenMatchHost host(config(helper), &ledger);
        Pipe pw;
        host.start(pw.fds[0], {7});
        QVERIFY(QTest::qWaitFor([&] { return host.state() == ScreenMatchHost::State::Crashed; }, 10000));
        QCOMPARE(host.crashes(), 1);
    }

    void theExitClassesAreTheDocumentedOnes()
    {
        QCOMPARE(sm::classifyExit(true, 0), sm::ExitClass::Crash);
        QCOMPARE(sm::classifyExit(false, sm::kExitOk), sm::ExitClass::Clean);
        QCOMPARE(sm::classifyExit(false, sm::kExitFailure), sm::ExitClass::Crash);
        QCOMPARE(sm::classifyExit(false, sm::kExitUsage), sm::ExitClass::Crash);
        QCOMPARE(sm::classifyExit(false, sm::kExitNoPipeWire), sm::ExitClass::NoPipeWire);
        QCOMPARE(sm::classifyExit(false, sm::kExitStreamLost), sm::ExitClass::StreamLost);
        QCOMPARE(sm::classifyExit(false, sm::kExitChannelStalled), sm::ExitClass::ChannelStalled); // (a stalled daemon is not the helper's crash)
        QCOMPARE(sm::classifyExit(false, 125), sm::ExitClass::SandboxUnavailable);
        QCOMPARE(sm::classifyExit(false, 127), sm::ExitClass::SandboxUnavailable);
        QCOMPARE(sm::classifyExit(false, 77), sm::ExitClass::Crash);
    }

    void aLongRunBeforeACrashStartsTheCountAgain()
    {
        QTemporaryDir dir;
        const QString helper = fakeHelper(dir);
        setMode(dir, "crash");
        CrashLedger ledger(dir.filePath(u"state"_s));
        ledger.load();
        ScreenMatchHost::Config c = config(helper);
        c.longRunMs = 100;
        ScreenMatchHost host(c, &ledger);
        Pipe pw;
        host.start(pw.fds[0], {7});
        QVERIFY(QTest::qWaitFor([&] { return host.state() == ScreenMatchHost::State::Crashed; }, 10000));
        QCOMPARE(host.crashes(), 1);
        setMode(dir, "crash-late");
        for (int i = 0; i < 4; ++i) { // each ran 0.3 s, over the 100 ms: the count restarts every time
            host.start(pw.fds[0], {7});
            QVERIFY(QTest::qWaitFor([&] { return host.state() == ScreenMatchHost::State::Crashed && host.launches() == 2 + i; }, 10000));
            QCOMPARE(host.crashes(), 1);
        }
    }

    void aStubbornHelperIsKilledAfterTheGrace()
    {
        QTemporaryDir dir;
        const QString helper = fakeHelper(dir);
        setMode(dir, "stubborn");
        ScreenMatchHost host(config(helper), nullptr);
        Pipe pw;
        host.start(pw.fds[0], {7});
        QVERIFY(QTest::qWaitFor([&] { return QFile::exists(dir.filePath(u"pid"_s)); }, 10000));
        QFile f(dir.filePath(u"pid"_s));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const int pid = f.readAll().trimmed().toInt();
        QVERIFY(pid > 1);
        host.stop();
        QCOMPARE(host.state(), ScreenMatchHost::State::Idle); // at once
        QVERIFY2(QTest::qWaitFor([&] { return ::kill(pid, 0) != 0; }, 5000), "the helper survived the grace period");
    }

    void refusalsBeforeLaunching()
    {
        QTemporaryDir dir;
        const QString helper = fakeHelper(dir);
        Pipe pw;
        {
            ScreenMatchHost::Config c = config(helper);
            c.requireSandbox = true; // and no sandbox path
            ScreenMatchHost host(c, nullptr);
            host.start(pw.fds[0], {7});
            QCOMPARE(host.state(), ScreenMatchHost::State::Refused);
            QCOMPARE(host.launches(), 0);
        }
        ScreenMatchHost host(config(helper), nullptr);
        host.start(pw.fds[0], {});
        QCOMPARE(host.state(), ScreenMatchHost::State::Refused);
        host.start(-1, {7});
        QCOMPARE(host.state(), ScreenMatchHost::State::Refused);
        host.start(pw.fds[0], QList<unsigned>(sm::kMaxStreams + 1, 3));
        QCOMPARE(host.state(), ScreenMatchHost::State::Refused);
        QCOMPARE(host.launches(), 0);
        ScreenMatchHost::Config missing = config(dir.filePath(u"nope"_s));
        ScreenMatchHost lost(missing, nullptr);
        lost.start(pw.fds[0], {7});
        QVERIFY(QTest::qWaitFor([&] { return lost.state() == ScreenMatchHost::State::Refused; }, 5000));
    }

    void sendingWhileNotRunningDoesNothing()
    {
        QTemporaryDir dir;
        ScreenMatchHost host(config(fakeHelper(dir)), nullptr);
        sm::Command c;
        c.type = sm::CommandType::Ping;
        host.send(c); // (no crash, nothing to write to)
        QCOMPARE(host.state(), ScreenMatchHost::State::Idle);
    }
};

QTEST_MAIN(TestScreenMatchHost)
#include "tst_screenmatchhost.moc"
