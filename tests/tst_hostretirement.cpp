// HostRetirement (src/bridge/qml/bridgesupport.*): a stopped host is not waited for, is killed after a moment, and counts as alive until it is gone.
#include "bridgesupport.h"
#include "testutil.h"

#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <csignal>
#include <unistd.h>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

// A host that ignores SIGTERM and never ends by itself; it writes its pid first.
QProcess *stubborn(const QTemporaryDir &dir, QObject *parent)
{
    const QString script = dir.filePath(u"host.sh"_s);
    testutil::writeFile(script, "#!/bin/sh\ntrap '' TERM HUP\necho $$ > \"$(dirname \"$0\")/pid\"\nwhile :; do sleep 0.05; done\n");
    QFile::setPermissions(script, QFile::permissions(script) | QFileDevice::ExeOwner);
    auto *process = new QProcess(parent);
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->start(script, {});
    return process;
}

bool alive(pid_t pid)
{
    return ::kill(pid, 0) == 0;
}

pid_t pidOf(const QTemporaryDir &dir)
{
    QFile f(dir.filePath(u"pid"_s));
    return f.open(QIODevice::ReadOnly) ? pid_t(f.readAll().trimmed().toLongLong()) : 0;
}

} // namespace

class TestHostRetirement : public QObject
{
    Q_OBJECT
private slots:
    void aRetiredHostIsNotWaitedForAndIsKilledAfterAMoment();
    void aHostThatEndsByItselfLeavesAtOnce();
    void nothingLeavingMeansNothingToWaitFor();
    void aRetiredHostOutlivesTheObjectThatStartedIt();
};

void TestHostRetirement::aRetiredHostIsNotWaitedForAndIsKilledAfterAMoment()
{
    QTemporaryDir dir;
    QProcess *host = stubborn(dir, nullptr);
    QVERIFY(host->waitForStarted(5000));
    QVERIFY(QTest::qWaitFor([&] { return pidOf(dir) > 0; }, 5000));
    const pid_t pid = pidOf(dir);
    QElapsedTimer timer;
    timer.start();
    HostRetirement::retire(host, 300);
    QVERIFY2(timer.elapsed() < 200, qPrintable(QString::number(timer.elapsed()))); // returns at once
    QVERIFY(HostRetirement::anyLeaving());                                      // and it counts as alive: a new host waits
    QVERIFY(alive(pid));
    QVERIFY(QTest::qWaitFor([&] { return !HostRetirement::anyLeaving(); }, 5000));
    QVERIFY(timer.elapsed() >= 250);                                             // (it was given its moment)
    QVERIFY(QTest::qWaitFor([&] { return !alive(pid); }, 5000));                 // killed, not left behind
}

void TestHostRetirement::aHostThatEndsByItselfLeavesAtOnce()
{
    QTemporaryDir dir;
    auto *host = new QProcess;
    host->start(u"/bin/sh"_s, {u"-c"_s, u"sleep 0.1"_s});
    QVERIFY(host->waitForStarted(5000));
    HostRetirement::retire(host, 20000);
    QVERIFY(HostRetirement::anyLeaving());
    QVERIFY(QTest::qWaitFor([&] { return !HostRetirement::anyLeaving(); }, 5000)); // long before the kill
}

void TestHostRetirement::nothingLeavingMeansNothingToWaitFor()
{
    QVERIFY(!HostRetirement::anyLeaving());
    HostRetirement::retire(nullptr, 100);              // nothing
    auto *never = new QProcess;                         // never started
    HostRetirement::retire(never, 100);
    QVERIFY(!HostRetirement::anyLeaving());
}

void TestHostRetirement::aRetiredHostOutlivesTheObjectThatStartedIt()
{
    QTemporaryDir dir;
    auto *owner = new QObject;
    QProcess *host = stubborn(dir, owner);
    QVERIFY(host->waitForStarted(5000));
    QVERIFY(QTest::qWaitFor([&] { return pidOf(dir) > 0; }, 5000));
    const pid_t pid = pidOf(dir);
    HostRetirement::retire(host, 400);
    delete owner; // the item (the window) is destroyed: the process is not taken down with it, and is still tracked
    QVERIFY(alive(pid));
    QVERIFY(HostRetirement::anyLeaving());
    QVERIFY(QTest::qWaitFor([&] { return !alive(pid); }, 5000));
    QVERIFY(QTest::qWaitFor([&] { return !HostRetirement::anyLeaving(); }, 5000));
}

QTEST_MAIN(TestHostRetirement)
#include "tst_hostretirement.moc"
