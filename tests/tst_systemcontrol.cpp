#include "buildinfo.h"
#include "systemcontrol.h"

#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

// Never talks to the real systemd: every test installs a fake runner or launcher, or puts a fake systemctl first in PATH.
class TestSystemControl : public QObject
{
    Q_OBJECT

private slots:
    void cleanup()
    {
        SystemControl::setAsyncRunner({});
        SystemControl::setLauncher({});
        SystemControl::setPlasmaRunner({});
        qputenv("XDG_CURRENT_DESKTOP", m_desktop);
    }
    void init() { m_desktop = qgetenv("XDG_CURRENT_DESKTOP"); }

    void localFileTurnsFileUrlsIntoPaths();
    void startSurfaceRunsSystemctl();
    void startSurfaceReportsFailure();
    void refreshReadsAutostart();
    void setAutostartUpdatesState();
    void setAutostartFailureKeepsStateAndReports();
    void versionComesFromBuildInfo();
    void answersAreAsynchronous();
    void busyIgnoresOverlappingCalls();
    void realRunnerUsesSystemctlFromPath();
    void realRunnerTimesOut();
    void sandboxStatusSaysWhatConfinesTheEngines();
    void plasmaToggleFollowsTheRunner();
    void plasmaToggleReportsFailure();
    void plasmaToggleIsHiddenOutsidePlasma();

private:
    QByteArray m_desktop;
};

void TestSystemControl::localFileTurnsFileUrlsIntoPaths()
{
    SystemControl control;
    QCOMPARE(control.localFile(QUrl(u"file:///home/me/My%20Pets/cat%23%C3%B1.gif"_s)), u"/home/me/My Pets/cat#ñ.gif"_s);
    QVERIFY(control.localFile(QUrl(u"https://example.org/cat.gif"_s)).isEmpty());
    QVERIFY(control.localFile(QUrl()).isEmpty());
}

void TestSystemControl::startSurfaceRunsSystemctl()
{
    QString program;
    QStringList args;
    SystemControl::setLauncher([&](const QString &p, const QStringList &a) {
        program = p;
        args = a;
        return true;
    });
    SystemControl control;
    QSignalSpy errors(&control, &SystemControl::error);
    QVERIFY(control.startSurface());
    QCOMPARE(program, u"systemctl"_s);
    QCOMPARE(args, QStringList({u"--user"_s, u"start"_s, u"murasaki-surface.service"_s}));
    QCOMPARE(errors.count(), 0);
}

void TestSystemControl::startSurfaceReportsFailure()
{
    SystemControl::setLauncher([](const QString &, const QStringList &) { return false; });
    SystemControl control;
    QSignalSpy errors(&control, &SystemControl::error);
    QVERIFY(!control.startSurface());
    QCOMPARE(errors.count(), 1);
}

void TestSystemControl::refreshReadsAutostart()
{
    int code = 0;
    SystemControl::setAsyncRunner([&](const QStringList &, int, std::function<void(int)> done) { done(code); });
    SystemControl control;
    QSignalSpy changed(&control, &SystemControl::autostartEnabledChanged);
    QVERIFY(!control.autostartEnabled());
    control.refreshAutostart();
    QVERIFY(control.autostartEnabled());
    QCOMPARE(changed.count(), 1);
    control.refreshAutostart(); // no change, no signal
    QCOMPARE(changed.count(), 1);
    code = 1;
    control.refreshAutostart();
    QVERIFY(!control.autostartEnabled());
    QCOMPARE(changed.count(), 2);
}

void TestSystemControl::setAutostartUpdatesState()
{
    QList<QStringList> calls;
    SystemControl::setAsyncRunner([&](const QStringList &args, int, std::function<void(int)> done) {
        calls << args;
        done(0);
    });
    SystemControl control;
    QSignalSpy errors(&control, &SystemControl::error);
    control.setAutostart(true);
    QVERIFY(control.autostartEnabled());
    control.setAutostart(false);
    QVERIFY(!control.autostartEnabled());
    QCOMPARE(errors.count(), 0);
    QCOMPARE(calls.size(), 2);
    QCOMPARE(calls.at(0), QStringList({u"--user"_s, u"enable"_s, u"--now"_s, u"murasaki-surface.service"_s}));
    QCOMPARE(calls.at(1), QStringList({u"--user"_s, u"disable"_s, u"--now"_s, u"murasaki-surface.service"_s}));
}

void TestSystemControl::setAutostartFailureKeepsStateAndReports()
{
    SystemControl::setAsyncRunner([](const QStringList &, int, std::function<void(int)> done) { done(1); });
    SystemControl control;
    QSignalSpy changed(&control, &SystemControl::autostartEnabledChanged);
    QSignalSpy errors(&control, &SystemControl::error);
    control.setAutostart(true);
    QVERIFY(!control.autostartEnabled());
    QCOMPARE(errors.count(), 1);
    QCOMPARE(changed.count(), 1); // lets a bound switch fall back
}

void TestSystemControl::versionComesFromBuildInfo()
{
    SystemControl control;
    QCOMPARE(control.appVersion(), versionString());
    QVERIFY(!control.appVersion().isEmpty());
}

void TestSystemControl::answersAreAsynchronous()
{
    std::function<void(int)> pending;
    SystemControl::setAsyncRunner([&](const QStringList &, int, std::function<void(int)> done) { pending = std::move(done); });
    SystemControl control;
    control.refreshAutostart();
    QVERIFY(control.autostartBusy());
    QVERIFY(!control.autostartEnabled()); // nothing known until the answer comes
    pending(0);
    QVERIFY(!control.autostartBusy());
    QVERIFY(control.autostartEnabled());
}

void TestSystemControl::busyIgnoresOverlappingCalls()
{
    int runs = 0;
    std::function<void(int)> pending;
    SystemControl::setAsyncRunner([&](const QStringList &, int, std::function<void(int)> done) {
        ++runs;
        pending = std::move(done);
    });
    SystemControl control;
    QSignalSpy changed(&control, &SystemControl::autostartEnabledChanged);
    control.setAutostart(true);
    control.setAutostart(false); // ignored, but the switch is told to fall back
    control.refreshAutostart();  // ignored
    QCOMPARE(runs, 1);
    QCOMPARE(changed.count(), 1);
    pending(0);
    QVERIFY(control.autostartEnabled());
}

namespace {

// A systemctl in PATH that records its arguments and exits with the code in "exit", after "delay" seconds.
class FakeSystemctl
{
public:
    FakeSystemctl()
    {
        QFile file(m_dir.filePath(u"systemctl"_s));
        if (!file.open(QIODevice::WriteOnly))
            qFatal("cannot write fake systemctl");
        const QByteArray dir = m_dir.path().toUtf8();
        file.write("#!/bin/sh\necho \"$*\" >> \"" + dir + "/args\"\n[ -f \"" + dir + "/delay\" ] && sleep $(cat \"" + dir
                   + "/delay\")\ncode=0\n[ -f \"" + dir + "/exit\" ] && code=$(cat \"" + dir + "/exit\")\nexit $code\n");
        file.close();
        file.setPermissions(file.permissions() | QFileDevice::ExeOwner);
        m_oldPath = qgetenv("PATH");
        qputenv("PATH", dir + ':' + m_oldPath);
    }
    ~FakeSystemctl() { qputenv("PATH", m_oldPath); }
    void write(const QString &name, const QByteArray &data)
    {
        QFile file(m_dir.filePath(name));
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size())
            qFatal("cannot write %s", qPrintable(name));
    }
    QStringList calls() const
    {
        QFile file(m_dir.filePath(u"args"_s));
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(file.readAll()).split(u'\n', Qt::SkipEmptyParts);
    }

private:
    QTemporaryDir m_dir;
    QByteArray m_oldPath;
};

} // namespace

void TestSystemControl::realRunnerUsesSystemctlFromPath()
{
    FakeSystemctl fake;
    SystemControl control;
    control.refreshAutostart();
    QTRY_VERIFY_WITH_TIMEOUT(control.autostartEnabled() && !control.autostartBusy(), 5000);
    fake.write(u"exit"_s, "1");
    control.refreshAutostart();
    QTRY_VERIFY_WITH_TIMEOUT(!control.autostartEnabled() && !control.autostartBusy(), 5000);
    fake.write(u"exit"_s, "0");
    control.setAutostart(true);
    QTRY_VERIFY_WITH_TIMEOUT(control.autostartEnabled() && !control.autostartBusy(), 5000);
    QCOMPARE(fake.calls(), QStringList({u"--user is-enabled --quiet murasaki-surface.service"_s,
                                         u"--user is-enabled --quiet murasaki-surface.service"_s,
                                         u"--user enable --now murasaki-surface.service"_s}));
}

void TestSystemControl::realRunnerTimesOut()
{
    // The real runner kills a systemctl that takes too long and reports a failure, without blocking.
    FakeSystemctl fake;
    fake.write(u"delay"_s, "30");
    SystemControl::setTimeouts(300, 300);
    SystemControl control;
    QSignalSpy errors(&control, &SystemControl::error);
    QElapsedTimer clock;
    clock.start();
    control.setAutostart(true);
    QVERIFY(control.autostartBusy()); // returned at once
    QVERIFY(clock.elapsed() < 200);
    QTRY_VERIFY_WITH_TIMEOUT(!control.autostartBusy(), 5000);
    QVERIFY(clock.elapsed() < 5000);
    QVERIFY(!control.autostartEnabled());
    QCOMPARE(errors.count(), 1);
    SystemControl::setTimeouts(5000, 30000);
}


void TestSystemControl::sandboxStatusSaysWhatConfinesTheEngines()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString helper = dir.filePath(u"helper"_s);
    QFile file(helper);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("#!/bin/sh\necho '{\"landlockAbi\":8,\"tcpDeny\":true,\"scopes\":true,\"bwrap\":true}'\n");
    file.close();
    QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    SystemControl control;
    qputenv("MURASAKI_SANDBOX", helper.toUtf8());
    QCOMPARE(control.sandboxStatus(), u"Landlock ABI 8, TCP refused, mount namespace yes"_s);
    qputenv("MURASAKI_SANDBOX", dir.filePath(u"missing"_s).toUtf8());
    QVERIFY(control.sandboxStatus().isEmpty()); // not installed
    qunsetenv("MURASAKI_SANDBOX");
}

void TestSystemControl::plasmaToggleFollowsTheRunner()
{
    qputenv("XDG_CURRENT_DESKTOP", "KDE");
    QList<SystemControl::PlasmaOperation> ops;
    bool active = false;
    SystemControl::setPlasmaRunner([&](SystemControl::PlasmaOperation op, std::function<void(const plasma::Result &)> done) {
        ops << op;
        if (op == SystemControl::PlasmaOperation::Enable)
            active = true;
        if (op == SystemControl::PlasmaOperation::Disable)
            active = false;
        plasma::Result result;
        result.ok = true;
        result.active = active;
        done(result);
    });
    SystemControl control;
    QVERIFY(control.plasmaSupported());
    QSignalSpy changed(&control, &SystemControl::plasmaEnabledChanged);
    QSignalSpy errors(&control, &SystemControl::error);

    control.refreshPlasma();
    QVERIFY(control.plasmaBusy()); // the answer arrives from the event loop
    QVERIFY(QTest::qWaitFor([&] { return changed.count() == 1; }, 2000));
    QVERIFY(!control.plasmaBusy());
    QVERIFY(!control.plasmaEnabled());

    control.setPlasma(true);
    QVERIFY(QTest::qWaitFor([&] { return changed.count() == 2; }, 2000));
    QVERIFY(control.plasmaEnabled());
    control.setPlasma(false);
    QVERIFY(QTest::qWaitFor([&] { return changed.count() == 3; }, 2000));
    QVERIFY(!control.plasmaEnabled());
    using Op = SystemControl::PlasmaOperation;
    QCOMPARE(ops, (QList<Op>{Op::Status, Op::Enable, Op::Disable}));
    QCOMPARE(errors.count(), 0);
}

void TestSystemControl::plasmaToggleReportsFailure()
{
    qputenv("XDG_CURRENT_DESKTOP", "KDE");
    SystemControl::setPlasmaRunner([](SystemControl::PlasmaOperation, std::function<void(const plasma::Result &)> done) {
        plasma::Result result;
        result.error = u"org.kde.plasmashell is not running"_s;
        done(result);
    });
    SystemControl control;
    QSignalSpy changed(&control, &SystemControl::plasmaEnabledChanged);
    QSignalSpy errors(&control, &SystemControl::error);
    control.setPlasma(true);
    QVERIFY(QTest::qWaitFor([&] { return changed.count() == 1; }, 2000));
    QVERIFY(!control.plasmaEnabled()); // a bound switch falls back
    QCOMPARE(errors.count(), 1);
    QVERIFY(errors.first().first().toString().contains(u"plasmashell"_s));
    // A failed status query is not worth an error.
    control.refreshPlasma();
    QVERIFY(QTest::qWaitFor([&] { return changed.count() == 2; }, 2000));
    QCOMPARE(errors.count(), 1);
}

void TestSystemControl::plasmaToggleIsHiddenOutsidePlasma()
{
    qputenv("XDG_CURRENT_DESKTOP", "GNOME");
    int calls = 0;
    SystemControl::setPlasmaRunner([&](SystemControl::PlasmaOperation, std::function<void(const plasma::Result &)>) { ++calls; });
    SystemControl control;
    QVERIFY(!control.plasmaSupported());
    control.refreshPlasma();
    control.setPlasma(true);
    QCOMPARE(calls, 0);
    QVERIFY(!control.plasmaBusy());
}

QTEST_GUILESS_MAIN(TestSystemControl)
#include "tst_systemcontrol.moc"
