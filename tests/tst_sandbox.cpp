// murasaki-sandbox, the real binary, around small system programs: what Landlock lets through and what it refuses.
// Tests that need a newer Landlock ABI than the kernel has skip themselves.

#include "bwrap.h"
#include "landlock.h"
#include "seccomp.h"
#include "../src/sandbox/profile.h" // (not "profile.h": src/core has one too)
#include "testutil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cstring>

#include <csignal>
#include <cerrno>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/seccomp.h>
#include <sys/syscall.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace {

struct Run {
    int exitCode = -1;
    QString out;
    QString err;
};

// What every system program needs to start: the dynamic loader, libraries and the binaries themselves.
QStringList systemPolicy()
{
    QStringList args;
    for (const char *path : {"/usr", "/lib", "/lib64", "/bin", "/etc/ld.so.cache", "/proc/self"})
        args << u"--ro"_s << QString::fromLatin1(path);
    return args;
}

// Runs murasaki-sandbox with `policy` around `program args` and waits for it.
Run sandboxed(const QStringList &policy, const QString &program, const QStringList &programArgs = {},
              int inheritFd = -1, const QProcessEnvironment &environment = QProcessEnvironment())
{
    QProcess process;
    if (!environment.isEmpty())
        process.setProcessEnvironment(environment);
    if (inheritFd >= 0)
        process.setChildProcessModifier([inheritFd] { // like the bridge socket
            if (inheritFd == 7)
                ::fcntl(7, F_SETFD, 0); // dup2 onto itself would keep O_CLOEXEC and the program would get nothing
            else
                ::dup2(inheritFd, 7);
        });
    process.setProgram(QStringLiteral(MURASAKI_SANDBOX_PATH));
    process.setArguments(policy + QStringList{u"--"_s, program} + programArgs);
    process.start();
    Run run;
    if (!process.waitForFinished(20000)) {
        process.kill();
        process.waitForFinished();
    }
    run.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -2;
    run.out = QString::fromLocal8Bit(process.readAllStandardOutput());
    run.err = QString::fromLocal8Bit(process.readAllStandardError());
    return run;
}

} // namespace

static bool bwrapUsable(); // (defined below)

class TestSandbox : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void importProfileReadsItsSourceAndWritesItsOutputOnly();
    void importProfileRefusesTheNetworkAndDropsTheEnvironment();
    void importProfileSetsLimitsAndRefusesLooseRules();
    void importProfileHidesPathnameUnixSockets();
    void importProfileUnderRequireNeedsTheMountNamespace();
    void usageErrorsExitWithTheUsageStatus();
    void aMissingProgramExitsWith127();
    void withoutLandlockItRunsUnconfinedAndSaysSo();
    void requireRefusesToRunUnconfined();
    void readsOnlyUnderTheReadOnlyPaths();
    void writesOnlyUnderTheReadWritePaths();
    void aSingleFileCanBeAllowed();
    void aPathThatDoesNotExistIsSkipped();
    void childrenOfTheProgramStayConfined();
    void noNewPrivsIsSet();
    void noNetRefusesTcpConnect();
    void abstractUnixSocketsAndSignalsAreScoped();
    void theBridgeFdSurvives();
    void sceneProfileReadsTheItemAndHidesTheRestOfHome();
    void sceneProfileWritesOnlyTheCaches();
    void sceneProfileForbidsTcpAndKeepsTheGpuNodes();
    void sceneProfileLetsPipeWireClientsConnect();
    void sceneProfileAllowsTheLibrariesNextToTheEngine();
    void theMountNamespaceHidesTheSessionBus();
    void theMountNamespaceKeepsTheBridgeDescriptorAndTheAudioSockets();
    void withoutBwrapLandlockAloneStillConfinesAndSaysSo();
    void reportDescribesWhatTheSystemOffers();
    void requireFailsWhenTcpCannotBeRefused();
    void bwrapBindsPathsThatMerelyStartWithProc();
    void sceneProfileCoversFontconfigAndTheTimezone();
    void sceneProfileLetsTheEngineReadItsVideoPlugin();
    void puppetProfileTable();
    void puppetProfileReadsTheItemAndTheCoreOnly();
    void puppetProfileWritesOnlyTheCaches();
    void puppetProfileForbidsTcpAndKeepsTheGpuNodes();
    void puppetSeccompListNamesTheDangerousCalls();
    void puppetProfileRefusesDangerousSyscalls();
    void puppetFilterProgramAnswersEachCase();
    void puppetProfileRefusesNamespacesButKeepsThreadsAndProcesses();
    void puppetProfileOutlivesNoParent();
    void puppetProfileRefusesLooseOptions();
    void screenmatchProfileTable();
    void screenmatchSeccompFilterAnswersEachCase();
    void screenmatchHelperCannotOpenConnectOrExec();
    void screenmatchProfileRefusesLooseOptions();
    void screenmatchHelperTakesNoFrameSourceOption();
    void theMountNamespaceFollowsASymlinkedItemDirectory();
    void theLocalTimezoneSurvivesTheMountNamespace();

private:
    int m_abi = 0;
    QString m_bash;
    QString m_cat;
    QTemporaryDir m_dir;
};

void TestSandbox::initTestCase()
{
    m_abi = murasaki::sandbox::kernelAbi();
    m_bash = QStandardPaths::findExecutable(u"bash"_s);
    m_cat = QStandardPaths::findExecutable(u"cat"_s);
    if (m_bash.isEmpty() || m_cat.isEmpty())
        QSKIP("needs bash and cat");
    testutil::writeFile(m_dir.filePath(u"allowed/file.txt"_s), "allowed");
    testutil::writeFile(m_dir.filePath(u"secret/file.txt"_s), "secret");
}

void TestSandbox::usageErrorsExitWithTheUsageStatus()
{
    for (const QStringList &args : {QStringList{}, QStringList{u"--ro"_s}, QStringList{u"--bogus"_s, u"--"_s, u"x"_s},
                                    QStringList{u"--abi"_s, u"x"_s, u"--"_s, u"x"_s}, QStringList{u"--no-net"_s}}) {
        QProcess process;
        process.start(QStringLiteral(MURASAKI_SANDBOX_PATH), args);
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitCode(), 125);
    }
}

void TestSandbox::aMissingProgramExitsWith127()
{
    if (m_abi < 1)
        QSKIP("needs Landlock");
    const Run run = sandboxed(systemPolicy(), u"/nonexistent/program"_s);
    QCOMPARE(run.exitCode, 127);
}

void TestSandbox::withoutLandlockItRunsUnconfinedAndSaysSo()
{
    const Run run = sandboxed({u"--abi"_s, u"0"_s}, m_cat, {m_dir.filePath(u"secret/file.txt"_s)});
    QCOMPARE(run.exitCode, 0);
    QCOMPARE(run.out, u"secret"_s);
    QVERIFY2(run.err.contains(u"unconfined"_s), qPrintable(run.err));
    QCOMPARE(run.err.count(u"murasaki-sandbox"_s), 1); // once
}

void TestSandbox::requireRefusesToRunUnconfined()
{
    const Run run = sandboxed({u"--abi"_s, u"0"_s, u"--require"_s}, m_cat, {m_dir.filePath(u"secret/file.txt"_s)});
    QCOMPARE(run.exitCode, 125);
    QVERIFY(run.out.isEmpty());
}

void TestSandbox::readsOnlyUnderTheReadOnlyPaths()
{
    if (m_abi < 1)
        QSKIP("needs Landlock");
    const QStringList policy = systemPolicy() + QStringList{u"--ro"_s, m_dir.filePath(u"allowed"_s)};
    const Run ok = sandboxed(policy, m_cat, {m_dir.filePath(u"allowed/file.txt"_s)});
    QCOMPARE(ok.exitCode, 0);
    QCOMPARE(ok.out, u"allowed"_s);
    const Run denied = sandboxed(policy, m_cat, {m_dir.filePath(u"secret/file.txt"_s)});
    QVERIFY(denied.exitCode != 0);
    QVERIFY(denied.out.isEmpty());
    QVERIFY2(denied.err.contains(u"Permission denied"_s), qPrintable(denied.err));
}

void TestSandbox::writesOnlyUnderTheReadWritePaths()
{
    if (m_abi < 1)
        QSKIP("needs Landlock");
    const QString writable = m_dir.filePath(u"scratch"_s);
    QDir().mkpath(writable);
    const QStringList policy = systemPolicy() + QStringList{u"--ro"_s, m_dir.filePath(u"allowed"_s), u"--rw"_s, writable};
    const Run write = sandboxed(policy, m_bash, {u"-c"_s, u"echo hi > '%1/new.txt'"_s.arg(writable)});
    QCOMPARE(write.exitCode, 0);
    QCOMPARE(QFile(writable + u"/new.txt"_s).size(), 3);
    const QString readOnlyTarget = m_dir.filePath(u"allowed/new.txt"_s);
    const Run denied = sandboxed(policy, m_bash, {u"-c"_s, u"echo hi > '%1'"_s.arg(readOnlyTarget)});
    QVERIFY(denied.exitCode != 0);
    QVERIFY(!QFile::exists(readOnlyTarget));
    const Run overwrite = sandboxed(policy, m_bash, {u"-c"_s, u"echo x > '%1'"_s.arg(m_dir.filePath(u"allowed/file.txt"_s))});
    QVERIFY(overwrite.exitCode != 0);
    QFile file(m_dir.filePath(u"allowed/file.txt"_s));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("allowed"));
}

void TestSandbox::aSingleFileCanBeAllowed()
{
    if (m_abi < 1)
        QSKIP("needs Landlock");
    testutil::writeFile(m_dir.filePath(u"single/one.txt"_s), "one");
    testutil::writeFile(m_dir.filePath(u"single/two.txt"_s), "two");
    const QStringList policy = systemPolicy() + QStringList{u"--ro"_s, m_dir.filePath(u"single/one.txt"_s)};
    QCOMPARE(sandboxed(policy, m_cat, {m_dir.filePath(u"single/one.txt"_s)}).out, u"one"_s);
    QVERIFY(sandboxed(policy, m_cat, {m_dir.filePath(u"single/two.txt"_s)}).exitCode != 0);
}

void TestSandbox::aPathThatDoesNotExistIsSkipped()
{
    if (m_abi < 1)
        QSKIP("needs Landlock");
    const QStringList policy = systemPolicy() + QStringList{u"--ro"_s, u"/nonexistent/xyz"_s, u"--rw"_s, u"/nonexistent/abc"_s};
    QCOMPARE(sandboxed(policy, m_cat, {u"/proc/self/comm"_s}).exitCode, 0);
}

void TestSandbox::childrenOfTheProgramStayConfined()
{
    if (m_abi < 1)
        QSKIP("needs Landlock");
    const Run run = sandboxed(systemPolicy(), m_bash,
                              {u"-c"_s, u"%1 '%2' || echo child-denied"_s.arg(m_cat, m_dir.filePath(u"secret/file.txt"_s))});
    QVERIFY2(run.out.contains(u"child-denied"_s), qPrintable(run.out + run.err));
    QVERIFY(!run.out.contains(u"secret"_s));
}

void TestSandbox::importProfileReadsItsSourceAndWritesItsOutputOnly()
{
    if (m_abi < 1)
        QSKIP("needs Landlock");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString src = dir.filePath(u"src.txt"_s), other = dir.filePath(u"other.txt"_s), out = dir.filePath(u"out"_s), elsewhere = dir.filePath(u"elsewhere"_s);
    QVERIFY(QDir().mkpath(out) && QDir().mkpath(elsewhere));
    testutil::writeFile(src, "source");
    testutil::writeFile(other, "secret");
    const QStringList profile{u"--profile"_s, u"import"_s, u"--src"_s, src, u"--out"_s, out};
    const auto sh = [&](const QString &script) { return sandboxed(profile, m_bash, {u"-c"_s, script}); };
    QVERIFY2(sh(u"cat '%1'"_s.arg(src)).out.contains(u"source"_s), "the named source is readable");
    QVERIFY(!sh(u"cat '%1'"_s.arg(other)).out.contains(u"secret"_s)); // a sibling file is not
    QVERIFY(sh(u"echo x > '%1/result'"_s.arg(out)).exitCode == 0);
    QVERIFY(QFile::exists(out + u"/result"_s));
    QVERIFY(sh(u"echo x > '%1/leak'"_s.arg(elsewhere)).exitCode != 0);
    QVERIFY(!QFile::exists(elsewhere + u"/leak"_s));
    QVERIFY(sh(u"echo x >> '%1'"_s.arg(src)).exitCode != 0); // the source is read only
    QVERIFY(sh(u"rm '%1'"_s.arg(src)).exitCode != 0);
    // no home, no session files
    const QString home = QDir::homePath();
    QVERIFY(!sh(u"ls '%1' 2>&1"_s.arg(home)).out.contains(u".bashrc"_s));
    QVERIFY(sh(u"ls /run/user 2>/dev/null; ls /etc/passwd >/dev/null 2>&1"_s).exitCode != 0); // /etc/passwd is not granted
    QVERIFY(sh(u"cat /etc/hostname 2>&1"_s).exitCode != 0);
}

void TestSandbox::importProfileRefusesTheNetworkAndDropsTheEnvironment()
{
    if (m_abi < 4)
        QSKIP("needs Landlock ABI 4");
    QTemporaryDir dir;
    QVERIFY(QDir().mkpath(dir.filePath(u"out"_s)));
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    const QStringList profile{u"--profile"_s, u"import"_s, u"--out"_s, dir.filePath(u"out"_s)};
    const QString script = u"exec 3<>/dev/tcp/127.0.0.1/%1 && echo connected"_s.arg(server.serverPort());
    const Run net = sandboxed(profile, m_bash, {u"-c"_s, script});
    QVERIFY2(!net.out.contains(u"connected"_s), qPrintable(net.out + net.err));
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(u"QT_PLUGIN_PATH"_s, u"/tmp/evil"_s);
    env.insert(u"LD_PRELOAD"_s, u"/tmp/evil.so"_s);
    env.insert(u"QT_IMAGEIO_MAXALLOC"_s, u"100000"_s);
    env.insert(u"DBUS_SESSION_BUS_ADDRESS"_s, u"unix:path=/tmp/bus"_s);
    const Run printed = sandboxed(profile, m_bash, {u"-c"_s, u"echo \"[$QT_PLUGIN_PATH][$LD_PRELOAD][$QT_IMAGEIO_MAXALLOC][$DBUS_SESSION_BUS_ADDRESS][$QT_QPA_PLATFORM]\""_s}, -1, env);
    QCOMPARE(printed.out.trimmed(), u"[][][][][offscreen]"_s);
}

void TestSandbox::importProfileSetsLimitsAndRefusesLooseRules()
{
    if (m_abi < 1)
        QSKIP("needs Landlock");
    QTemporaryDir dir;
    QVERIFY(QDir().mkpath(dir.filePath(u"out"_s)));
    const QStringList profile{u"--profile"_s, u"import"_s, u"--out"_s, dir.filePath(u"out"_s)};
    const Run limits = sandboxed(profile, m_bash, {u"-c"_s, u"ulimit -n; ulimit -t; ulimit -c"_s});
    QCOMPARE(limits.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts), (QStringList{u"256"_s, u"3600"_s, u"0"_s}));
    // --ro / --rw beside the profile would be a hole in the one reviewed place; a relative --out or --src is not a path of ours either
    QVERIFY(sandboxed(profile + QStringList{u"--ro"_s, u"/etc"_s}, m_bash, {u"-c"_s, u"true"_s}).exitCode == 125);
    QVERIFY(sandboxed(QStringList{u"--profile"_s, u"import"_s, u"--out"_s, u"relative"_s}, m_bash, {u"-c"_s, u"true"_s}).exitCode == 125);
    QVERIFY(sandboxed(QStringList{u"--profile"_s, u"import"_s}, m_bash, {u"-c"_s, u"true"_s}).exitCode == 125);
}

void TestSandbox::importProfileHidesPathnameUnixSockets()
{
    const QString python = QStandardPaths::findExecutable(u"python3"_s);
    if (m_abi < 1 || python.isEmpty() || !bwrapUsable())
        QSKIP("needs Landlock, python3 and a working bwrap");
    QTemporaryDir dir;
    QVERIFY(QDir().mkpath(dir.filePath(u"out"_s)));
    const QString bus = dir.filePath(u"bus"_s);
    QLocalServer server; // a stand-in for the session bus, the Wayland socket, systemd --user: a unix socket by path
    QVERIFY(server.listen(bus));
    const QString connect = u"import socket\ns=socket.socket(socket.AF_UNIX)\ns.connect('%1')\nprint('connected')"_s.arg(bus);
    const QStringList profile{u"--profile"_s, u"import"_s, u"--src"_s, bus, u"--out"_s, dir.filePath(u"out"_s)};
    // Landlock alone (ABI 8) can not refuse the connect, even when the socket is a named source...
    const Run landlockOnly = sandboxed(profile + QStringList{u"--bwrap"_s, u"none"_s}, python, {u"-c"_s, connect});
    if (m_abi < 9)
        QVERIFY2(landlockOnly.out.contains(u"connected"_s), qPrintable(landlockOnly.out + landlockOnly.err));
    // ...the mount namespace of the import profile does: the socket's directory is not there at all, and the runtime directory does not exist.
    const QStringList elsewhere{u"--profile"_s, u"import"_s, u"--out"_s, dir.filePath(u"out"_s)};
    const Run namespaced = sandboxed(elsewhere, python, {u"-c"_s, connect});
    QVERIFY2(!namespaced.out.contains(u"connected"_s), qPrintable(namespaced.out + namespaced.err));
    QVERIFY2(namespaced.err.contains(u"No such file"_s) || namespaced.err.contains(u"FileNotFound"_s), qPrintable(namespaced.err));
    const Run runtime = sandboxed(elsewhere, m_bash, {u"-c"_s, u"ls /run/user /tmp/.X11-unix 2>&1; echo [$XDG_RUNTIME_DIR][$WAYLAND_DISPLAY][$DISPLAY]"_s}, -1,
                                  [] { QProcessEnvironment e = QProcessEnvironment::systemEnvironment(); e.insert(u"XDG_RUNTIME_DIR"_s, u"/run/user/1000"_s); e.insert(u"WAYLAND_DISPLAY"_s, u"wayland-0"_s); e.insert(u"DISPLAY"_s, u":0"_s); return e; }());
    QVERIFY2(runtime.out.contains(u"[][][]"_s), qPrintable(runtime.out));
    QVERIFY2(!runtime.out.contains(u"wayland-0\n"_s) && runtime.out.contains(u"No such file"_s), qPrintable(runtime.out));
}

void TestSandbox::importProfileUnderRequireNeedsTheMountNamespace()
{
    if (m_abi < 4)
        QSKIP("needs Landlock ABI 4");
    QTemporaryDir dir;
    QVERIFY(QDir().mkpath(dir.filePath(u"out"_s)));
    const QStringList profile{u"--profile"_s, u"import"_s, u"--out"_s, dir.filePath(u"out"_s), u"--require"_s};
    // no mount namespace allowed: --require refuses (125) instead of running with the session's sockets reachable
    const Run refused = sandboxed(profile + QStringList{u"--bwrap"_s, u"none"_s}, m_bash, {u"-c"_s, u"echo ran"_s});
    QCOMPARE(refused.exitCode, 125);
    QVERIFY(!refused.out.contains(u"ran"_s));
    QVERIFY(refused.err.contains(u"unix sockets"_s));
    if (bwrapUsable()) {
        const Run ok = sandboxed(profile, m_bash, {u"-c"_s, u"echo ran"_s});
        QVERIFY2(ok.out.contains(u"ran"_s), qPrintable(ok.out + ok.err));
    }
    // and --report says whether the sockets can be hidden here
    QProcess report;
    report.start(QStringLiteral(MURASAKI_SANDBOX_PATH), {u"--report"_s});
    QVERIFY(report.waitForFinished(5000));
    QCOMPARE(QJsonDocument::fromJson(report.readAllStandardOutput()).object().value(u"unixSocketsHidden"_s).toBool(), bwrapUsable());
}

void TestSandbox::noNewPrivsIsSet()
{
    const QString grep = QStandardPaths::findExecutable(u"grep"_s);
    if (grep.isEmpty() || m_abi < 1)
        QSKIP("needs grep and Landlock");
    const Run run = sandboxed(systemPolicy(), grep, {u"NoNewPrivs"_s, u"/proc/self/status"_s});
    QVERIFY2(run.out.contains(QRegularExpression(u"NoNewPrivs:\\s*1"_s)), qPrintable(run.out + run.err));
}

void TestSandbox::noNetRefusesTcpConnect()
{
    if (m_abi < 4)
        QSKIP("needs Landlock ABI 4");
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0)); // a listener the test owns; the kernel completes the connect
    const QString script = u"exec 3<>/dev/tcp/127.0.0.1/%1 && echo connected"_s.arg(server.serverPort());
    // Control: the same program with the filesystem rules only can connect.
    const Run open = sandboxed(systemPolicy(), m_bash, {u"-c"_s, script});
    QVERIFY2(open.out.contains(u"connected"_s), qPrintable(open.out + open.err));
    const Run closed = sandboxed(systemPolicy() + QStringList{u"--no-net"_s}, m_bash, {u"-c"_s, script});
    QVERIFY2(!closed.out.contains(u"connected"_s), qPrintable(closed.out + closed.err));
    QVERIFY(closed.exitCode != 0);
}

void TestSandbox::abstractUnixSocketsAndSignalsAreScoped()
{
    if (m_abi < 6)
        QSKIP("needs Landlock ABI 6");
    const QString python = QStandardPaths::findExecutable(u"python3"_s);
    if (python.isEmpty())
        QSKIP("needs python3");
    QLocalServer server;
    server.setSocketOptions(QLocalServer::AbstractNamespaceOption);
    QVERIFY(server.listen(u"murasaki-sandbox-test-%1"_s.arg(QCoreApplication::applicationPid())));
    const QString connect =
        u"import socket\ns=socket.socket(socket.AF_UNIX)\ns.connect('\\0%1')\nprint('connected')"_s.arg(server.serverName());

    QProcess control; // the same program, not sandboxed: it reaches the server
    control.start(python, {u"-c"_s, connect});
    QVERIFY(control.waitForFinished(10000));
    QVERIFY2(control.readAllStandardOutput().contains("connected"), qPrintable(QString::fromLocal8Bit(control.readAllStandardError())));

    const Run scoped = sandboxed(systemPolicy(), python, {u"-c"_s, connect});
    QVERIFY2(!scoped.out.contains(u"connected"_s), qPrintable(scoped.out + scoped.err));

    // A signal to a process outside the sandbox (the test itself is the parent) is refused too.
    const QString signalOutside = u"kill -0 $PPID && echo signalled"_s;
    QProcess signalControl;
    signalControl.setProgram(m_bash);
    signalControl.setArguments({u"-c"_s, signalOutside});
    signalControl.start();
    QVERIFY(signalControl.waitForFinished(10000));
    QVERIFY(signalControl.readAllStandardOutput().contains("signalled"));
    const Run blocked = sandboxed(systemPolicy(), m_bash, {u"-c"_s, signalOutside});
    QVERIFY2(!blocked.out.contains(u"signalled"_s), qPrintable(blocked.out + blocked.err));
}

void TestSandbox::theBridgeFdSurvives()
{
    if (m_abi < 1)
        QSKIP("needs Landlock");
    // fd 7 is a file opened before the sandbox, like the bridge socket; it stays usable although its path is not allowed.
    const int fd = ::open(m_dir.filePath(u"secret/file.txt"_s).toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
    QVERIFY(fd >= 0);
    // The program reads it through the descriptor it inherited, not through the (forbidden) path.
    const Run viaShell = sandboxed(systemPolicy(), m_bash, {u"-c"_s, u"%1 <&7"_s.arg(m_cat)}, fd);
    ::close(fd);
    QCOMPARE(viaShell.out, u"secret"_s);
}

namespace {

// A throw-away home for the scene profile: the item, an "ssh" secret, and the XDG variables pointing into it.
struct FakeHome {
    QTemporaryDir dir;
    QString item() const { return dir.filePath(u"steam/item"_s); }
    QString secret() const { return dir.filePath(u"home/.ssh/id_test"_s); }
    QProcessEnvironment environment() const
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(u"HOME"_s, dir.filePath(u"home"_s));
        env.remove(u"XDG_CACHE_HOME"_s);
        env.remove(u"XDG_CONFIG_HOME"_s);
        env.insert(u"XDG_RUNTIME_DIR"_s, dir.filePath(u"run"_s));
        return env;
    }
    FakeHome()
    {
        testutil::writeFile(item() + u"/project.json"_s, "{}");
        testutil::writeFile(secret(), "private key");
        QDir().mkpath(dir.filePath(u"run"_s));
        QFile::setPermissions(dir.filePath(u"run"_s), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
};

QStringList sceneProfile(const QString &item) { return {u"--profile"_s, u"scene"_s, u"--item"_s, item, u"--require"_s}; }

} // namespace

namespace {

QStringList puppetProfile(const QString &item, const QString &core = {})
{
    QStringList args{u"--profile"_s, u"puppet"_s, u"--item"_s, item, u"--require"_s};
    if (!core.isEmpty())
        args << u"--core"_s << core;
    return args;
}

bool contains(const std::vector<std::string> &list, const std::string &path)
{
    return std::find(list.begin(), list.end(), path) != list.end();
}

} // namespace

void TestSandbox::puppetProfileTable()
{
    using namespace murasaki::sandbox;
    ProfileEnvironment env;
    env.home = "/home/someone";
    env.xdgRuntimeDir = "/run/user/1000";
    const Policy none = puppetProfile({"/models/haru", "", "/usr/bin/murasaki-puppet"}, env);
    const Policy withCore = puppetProfile({"/models/haru", "/opt/live2d/libCubismCore.so", "/usr/bin/murasaki-puppet"}, env);
    SceneProfileInput sceneInput;
    sceneInput.itemDir = "/models/haru";
    sceneInput.program = "/usr/bin/murasaki-puppet";
    const Policy scene = sceneProfile(sceneInput, env);
    QVERIFY(none.denyTcp);
    // reads: the model folder, the system, the GPU description; the Core file only when one is named
    QVERIFY(contains(none.readOnly, "/models/haru"));
    QVERIFY(contains(none.readOnly, "/usr"));
    QVERIFY(contains(none.readOnly, "/etc/vulkan"));
    QVERIFY(!contains(none.readOnly, "/opt/live2d/libCubismCore.so"));
    QVERIFY(contains(withCore.readOnly, "/opt/live2d/libCubismCore.so"));
    QCOMPARE(withCore.readOnly.size(), none.readOnly.size() + 1); // that one path, nothing else
    QCOMPARE(withCore.readWrite, none.readWrite);
    // nothing of the home, the fonts, the audio or the session
    for (const std::string &path : none.readOnly) {
        QVERIFY2(path.find("/home/someone") == std::string::npos || path == "/home/someone/.local/lib/murasaki" || path == "/home/someone/.local/bin", path.c_str()); // (where a user-installed engine keeps its libraries)
        QVERIFY2(path.find("/run/user") == std::string::npos, path.c_str());
        QVERIFY2(path.find("pipewire") == std::string::npos && path.find("pulse") == std::string::npos && path.find("alsa") == std::string::npos, path.c_str());
        QVERIFY2(path.find("fontconfig") == std::string::npos && path.find("fonts") == std::string::npos, path.c_str());
    }
    // writes: the GPU render nodes (never the whole of /dev/dri: no card node), a few devices, the process's own task entries and the two caches of the puppet engine alone
    QVERIFY(contains(none.readOnly, "/dev/dri")); // (libdrm lists it to find the GPUs)
    QVERIFY(!contains(none.readWrite, "/dev/dri"));
    for (const std::string &path : none.readWrite)
        QVERIFY2(path.rfind("/dev/dri", 0) != 0 || path.rfind("/dev/dri/renderD", 0) == 0, path.c_str());
    QVERIFY(contains(none.readWrite, "/proc/self/task"));
    QVERIFY(contains(none.readWrite, "/home/someone/.cache/murasaki/puppet-shader-cache"));
    QVERIFY(contains(none.readWrite, "/home/someone/.cache/murasaki/puppet-gpu-cache"));
    QVERIFY(!contains(none.readWrite, "/home/someone/.cache/murasaki/shader-cache"));
    QVERIFY(!contains(none.readWrite, "/home/someone/.cache/murasaki/gpu-cache"));
    for (const std::string &path : none.readWrite) {
        QVERIFY2(path.rfind("/dev/", 0) == 0 || path.rfind("/proc/self", 0) == 0 || path.rfind("/home/someone/.cache/murasaki/", 0) == 0, path.c_str());
    }
    // a variation of the scene profile with things removed, never added: every read of the puppet profile is a read of the scene profile
    for (const std::string &path : none.readOnly)
        QVERIFY2(contains(scene.readOnly, path) || path == "/dev/dri", path.c_str()); // (the scene has it as a writable path)
    QVERIFY(none.readOnly.size() < scene.readOnly.size());
    // the drivers' caches are the puppet engine's own, not the scene's: a model can not plant what a scene engine reads later
    QVERIFY(puppetProfileEnvironment(env) != sceneProfileEnvironment(env));
    for (const auto &[name, value] : puppetProfileEnvironment(env)) {
        QVERIFY2(value.find("/puppet-gpu-cache") != std::string::npos, name.c_str());
        QVERIFY(contains(none.readWrite, value));
    }
    for (const std::string &path : none.readWrite) {
        for (const auto &[name, value] : sceneProfileEnvironment(env))
            QVERIFY2(path != value, path.c_str());
    }
}

void TestSandbox::puppetProfileReadsTheItemAndTheCoreOnly()
{
    if (m_abi < 1)
        QSKIP("needs Landlock");
    FakeHome home;
    testutil::writeFile(home.dir.filePath(u"opt/core.so"_s), "core bytes");
    testutil::writeFile(home.dir.filePath(u"opt/other.so"_s), "other bytes");
    const Run item = sandboxed(puppetProfile(home.item()), m_cat, {home.item() + u"/project.json"_s}, -1, home.environment());
    QCOMPARE(item.out, u"{}"_s);
    const Run secret = sandboxed(puppetProfile(home.item()), m_cat, {home.secret()}, -1, home.environment());
    QVERIFY(secret.exitCode != 0);
    QVERIFY2(!secret.out.contains(u"private key"_s), qPrintable(secret.out));
    // the Core file: unreadable unless named, and then only that file
    const Run without = sandboxed(puppetProfile(home.item()), m_cat, {home.dir.filePath(u"opt/core.so"_s)}, -1, home.environment());
    QVERIFY(without.exitCode != 0);
    const Run with = sandboxed(puppetProfile(home.item(), home.dir.filePath(u"opt/core.so"_s)), m_cat, {home.dir.filePath(u"opt/core.so"_s)}, -1, home.environment());
    QCOMPARE(with.out, u"core bytes"_s);
    const Run sibling = sandboxed(puppetProfile(home.item(), home.dir.filePath(u"opt/core.so"_s)), m_cat, {home.dir.filePath(u"opt/other.so"_s)}, -1, home.environment());
    QVERIFY(sibling.exitCode != 0);
    QVERIFY2(!sibling.out.contains(u"other bytes"_s), qPrintable(sibling.out));
    // the user's audio and font configuration are not there as they are for a scene
    testutil::writeFile(home.dir.filePath(u"home/.config/pipewire/client.conf"_s), "pipewire config");
    const Run audio = sandboxed(puppetProfile(home.item()), m_cat, {home.dir.filePath(u"home/.config/pipewire/client.conf"_s)}, -1, home.environment());
    QVERIFY(audio.exitCode != 0);
    const Run sceneAudio = sandboxed(sceneProfile(home.item()), m_cat, {home.dir.filePath(u"home/.config/pipewire/client.conf"_s)}, -1, home.environment());
    QCOMPARE(sceneAudio.out, u"pipewire config"_s); // (the control: a scene does read it)
}

void TestSandbox::puppetProfileWritesOnlyTheCaches()
{
    if (m_abi < 1)
        QSKIP("needs Landlock");
    FakeHome home;
    const QString cache = home.dir.filePath(u"home/.cache/murasaki/puppet-shader-cache"_s);
    const Run inCache = sandboxed(puppetProfile(home.item()), m_bash, {u"-c"_s, u"echo x > '%1/entry.bin'"_s.arg(cache)}, -1, home.environment());
    QCOMPARE(inCache.exitCode, 0);
    QVERIFY(QFile::exists(cache + u"/entry.bin"_s));
    // the scene engines' caches are not the puppet engine's to write
    for (const QString &outside : {home.item() + u"/planted"_s, home.dir.filePath(u"home/planted"_s), home.dir.filePath(u"home/.cache/planted"_s), home.dir.filePath(u"run/planted"_s),
                                   home.dir.filePath(u"home/.cache/murasaki/shader-cache/planted"_s), home.dir.filePath(u"home/.cache/murasaki/gpu-cache/planted"_s)}) {
        const Run run = sandboxed(puppetProfile(home.item()), m_bash, {u"-c"_s, u"echo x > '%1'"_s.arg(outside)}, -1, home.environment());
        QVERIFY2(run.exitCode != 0, qPrintable(outside));
        QVERIFY2(!QFile::exists(outside), qPrintable(outside));
    }
}

void TestSandbox::puppetProfileForbidsTcpAndKeepsTheGpuNodes()
{
    if (m_abi < 4)
        QSKIP("needs Landlock ABI 4");
    FakeHome home;
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    const Run tcp = sandboxed(puppetProfile(home.item()), m_bash, {u"-c"_s, u"exec 3<>/dev/tcp/127.0.0.1/%1 && echo connected"_s.arg(server.serverPort())}, -1, home.environment());
    QVERIFY2(!tcp.out.contains(u"connected"_s), qPrintable(tcp.out + tcp.err));
    if (::access("/dev/dri/renderD128", R_OK | W_OK) == 0) { // (a node this user cannot open proves nothing)
        const Run gpu = sandboxed(puppetProfile(home.item()), m_bash, {u"-c"_s, u"exec 3<>/dev/dri/renderD128 && echo opened"_s}, -1, home.environment());
        QVERIFY2(gpu.out.contains(u"opened"_s), qPrintable(gpu.out + gpu.err));
        // libdrm lists the directory to find the devices; without it RADV sees no GPU at all
        const Run listing = sandboxed(puppetProfile(home.item()), m_bash, {u"-c"_s, u"for f in /dev/dri/*; do echo \"$f\"; done"_s}, -1, home.environment());
        QVERIFY2(listing.out.contains(u"/dev/dri/renderD128"_s), qPrintable(listing.out + listing.err));
    }
    // a card node can not be used even read only: under the mount namespace it is not there, and with Landlock ABI 5 or later an fd opened from the directory's read right has no ioctl right (DRM_IOCTL_VERSION is EACCES);
    // before ABI 5 (kernel 6.10) Landlock alone can not stop that, which is why the mount namespace matters
    if (const QString python = QStandardPaths::findExecutable(u"python3"_s); !python.isEmpty() && QFileInfo::exists(u"/dev/dri/card0"_s) | QFileInfo::exists(u"/dev/dri/card1"_s)) {
        const QString card = QFileInfo::exists(u"/dev/dri/card0"_s) ? u"/dev/dri/card0"_s : u"/dev/dri/card1"_s;
        const QString script = QStringLiteral(
            "import fcntl, os\n"
            "try:\n"
            "    fd = os.open('%1', os.O_RDONLY)\n"
            "except OSError as e:\n"
            "    print('open', e.errno); raise SystemExit\n"
            "buf = bytearray(64)\n"
            "try:\n"
            "    fcntl.ioctl(fd, 0xC0406400, buf)\n"
            "    print('ioctl', 0)\n"
            "except OSError as e:\n"
            "    print('ioctl', e.errno)\n").arg(card);
        const Run run = sandboxed(puppetProfile(home.item()), python, {u"-I"_s, u"-c"_s, script}, -1, home.environment());
        if (m_abi >= 5 || run.out.contains(u"open"_s))
            QVERIFY2(!run.out.contains(u"ioctl 0"_s), qPrintable(run.out + run.err));
    }
    // a card node (display control) is not a render node: the puppet engine does not get it
    for (const QString &card : QDir(u"/dev/dri"_s).entryList({u"card*"_s}, QDir::System)) {
        const QString path = u"/dev/dri/"_s + card;
        if (QFileInfo(path).isReadable() && QFileInfo(path).isWritable()) {
            const Run denied = sandboxed(puppetProfile(home.item()), m_bash, {u"-c"_s, u"exec 3<>'%1' && echo opened"_s.arg(path)}, -1, home.environment());
            QVERIFY2(!denied.out.contains(u"opened"_s), qPrintable(path));
        }
    }
}

void TestSandbox::puppetSeccompListNamesTheDangerousCalls()
{
    const std::vector<long> denied = murasaki::sandbox::puppetDeniedSyscalls();
    const auto has = [&](long number) { return std::find(denied.begin(), denied.end(), number) != denied.end(); };
    for (const long number : {long(SYS_io_uring_setup), long(SYS_io_uring_enter), long(SYS_io_uring_register), long(SYS_bpf), long(SYS_userfaultfd), long(SYS_perf_event_open), long(SYS_ptrace),
                              long(SYS_process_vm_readv), long(SYS_process_vm_writev), long(SYS_keyctl), long(SYS_open_by_handle_at), long(SYS_mount), long(SYS_unshare), long(SYS_setns),
                              long(SYS_kexec_load)})
        QVERIFY2(has(number), qPrintable(QString::number(number)));
    // what a Vulkan driver does every frame stays allowed
    for (const long number : {long(SYS_read), long(SYS_write), long(SYS_ioctl), long(SYS_mmap), long(SYS_futex), long(SYS_openat), long(SYS_sendmsg), long(SYS_recvmsg), long(SYS_epoll_wait)})
        QVERIFY2(!has(number), qPrintable(QString::number(number)));
}

namespace {

// A BPF interpreter for the few instructions the filter uses (load a word of seccomp_data, jump on equal / greater or equal / bit set, return), so the program can be checked without the kernel
// and over inputs a running process could not make (another ABI). Returns the action.
uint32_t runFilter(const std::vector<sock_filter> &program, uint32_t arch, uint32_t nr, uint64_t arg0 = 0)
{
    seccomp_data data{};
    data.arch = arch;
    data.nr = static_cast<int>(nr);
    data.args[0] = arg0;
    uint32_t a = 0;
    size_t pc = 0;
    for (int steps = 0; steps < 10000 && pc < program.size(); ++steps) {
        const sock_filter &i = program[pc];
        switch (BPF_CLASS(i.code)) {
        case BPF_LD:
            if (BPF_MODE(i.code) != BPF_ABS || BPF_SIZE(i.code) != BPF_W || i.k + 4 > sizeof data)
                return 0xFFFFFFFFu;
            std::memcpy(&a, reinterpret_cast<const char *>(&data) + i.k, 4);
            ++pc;
            break;
        case BPF_JMP: {
            bool taken = false;
            switch (BPF_OP(i.code)) {
            case BPF_JEQ: taken = a == i.k; break;
            case BPF_JGE: taken = a >= i.k; break;
            case BPF_JSET: taken = (a & i.k) != 0; break;
            default: return 0xFFFFFFFFu;
            }
            pc += 1 + (taken ? i.jt : i.jf);
            break;
        }
        case BPF_RET:
            return i.k;
        default:
            return 0xFFFFFFFFu;
        }
    }
    return 0xFFFFFFFFu; // fell off the end: a program the kernel would refuse
}

} // namespace

void TestSandbox::puppetFilterProgramAnswersEachCase()
{
    const std::vector<sock_filter> program = murasaki::sandbox::buildPuppetFilter();
    QVERIFY(!program.empty());
#if defined(__x86_64__)
    const uint32_t arch = AUDIT_ARCH_X86_64;
#else
    const uint32_t arch = AUDIT_ARCH_AARCH64;
#endif
    const uint32_t eperm = SECCOMP_RET_ERRNO | EPERM, enosys = SECCOMP_RET_ERRNO | ENOSYS;
    // every call of the list is EPERM, whatever else the program does
    for (const long number : murasaki::sandbox::puppetDeniedSyscalls())
        QCOMPARE(runFilter(program, arch, uint32_t(number)), eperm);
    // what a driver does all day is allowed
    for (const long number : {long(SYS_read), long(SYS_write), long(SYS_ioctl), long(SYS_mmap), long(SYS_futex), long(SYS_openat), long(SYS_sendmsg), long(SYS_recvmsg), long(SYS_epoll_wait),
                              long(SYS_getpid), long(SYS_munmap), long(SYS_mprotect), long(SYS_exit_group)})
        QCOMPARE(runFilter(program, arch, uint32_t(number)), uint32_t(SECCOMP_RET_ALLOW));
    // the new mount API, by number (the same on both architectures), and module loading
    for (const uint32_t number : {428u, 429u, 430u, 431u, 432u, 433u, 442u})
        QCOMPARE(runFilter(program, arch, number), eperm);
    // clone3 is ENOSYS (so glibc falls back to clone); clone is EPERM with any new-namespace flag and allowed without
    QCOMPARE(runFilter(program, arch, uint32_t(SYS_clone3)), enosys);
    for (const uint64_t flag : {0x00020000ull, 0x02000000ull, 0x04000000ull, 0x08000000ull, 0x10000000ull, 0x20000000ull, 0x40000000ull, 0x80ull})
        QCOMPARE(runFilter(program, arch, uint32_t(SYS_clone), flag | 17), eperm);
    const uint64_t thread = 0x00000100 | 0x00000200 | 0x00000400 | 0x00000800 | 0x00010000 | 0x00040000 | 0x00200000 | 0x00400000; // CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM|SETTLS|PARENT_SETTID...
    QCOMPARE(runFilter(program, arch, uint32_t(SYS_clone), thread), uint32_t(SECCOMP_RET_ALLOW));
    QCOMPARE(runFilter(program, arch, uint32_t(SYS_clone), 17), uint32_t(SECCOMP_RET_ALLOW)); // a plain fork()
    // the number is read again after the clone test: a denied call after it is still denied
    QCOMPARE(runFilter(program, arch, uint32_t(SYS_ptrace)), eperm);
    // another ABI is refused whole, with a call number that would be harmless in ours
    QCOMPARE(runFilter(program, AUDIT_ARCH_I386, 3), eperm);       // (read on ia32)
    QCOMPARE(runFilter(program, AUDIT_ARCH_ARM, 3), eperm);
    QCOMPARE(runFilter(program, 0, uint32_t(SYS_read)), eperm);
#if defined(__x86_64__)
    QCOMPARE(runFilter(program, arch, 0x40000000u | uint32_t(SYS_read)), eperm); // the x32 ABI, same architecture value
    QCOMPARE(runFilter(program, arch, 0x40000000u), eperm);
#endif
    // the program ends in a return on every path and fits the kernel's limit
    QVERIFY(program.size() < 4096);
    QVERIFY(BPF_CLASS(program.back().code) == BPF_RET);
}

void TestSandbox::puppetProfileRefusesNamespacesButKeepsThreadsAndProcesses()
{
    const QString python = QStandardPaths::findExecutable(u"python3"_s);
    if (python.isEmpty())
        QSKIP("needs python3");
    FakeHome home;
    // clone(CLONE_NEWUSER | SIGCHLD): EPERM under the filter (a child that appears anyway leaves at once). Threads, fork and subprocess go on working: glibc falls back from clone3 to clone.
    const QString script = QStringLiteral(
        "import ctypes, os, subprocess, threading\n"
        "libc = ctypes.CDLL(None, use_errno=True)\n"
        "r = libc.syscall(%1, 0x10000000 | 17, 0, 0, 0, 0); e = ctypes.get_errno()\n"
        "if r == 0: os._exit(0)\n"
        "out = []\n"
        "t = threading.Thread(target=lambda: out.append('thread')); t.start(); t.join()\n"
        "pid = os.fork()\n"
        "if pid == 0: os._exit(7)\n"
        "_, status = os.waitpid(pid, 0)\n"
        "p = subprocess.run(['/bin/true']).returncode\n"
        "print('newuser', r, e, out[0], 'child', os.WEXITSTATUS(status), 'spawn', p)\n").arg(SYS_clone);
    const Run run = sandboxed(puppetProfile(home.item()), python, {u"-I"_s, u"-c"_s, script}, -1, home.environment());
    QVERIFY2(run.out.contains(QStringLiteral("newuser -1 %1 thread child 7 spawn 0").arg(EPERM)), qPrintable(run.out + run.err));
}

void TestSandbox::puppetProfileRefusesDangerousSyscalls()
{
    const QString python = QStandardPaths::findExecutable(u"python3"_s);
    if (python.isEmpty())
        QSKIP("needs python3");
    FakeHome home;
    // io_uring_setup(0, NULL) is EINVAL without the filter and EPERM with it; PTRACE_TRACEME is 0 without it and EPERM with it; getpid still works
    const QString script = QStringLiteral(
        "import ctypes, os\n"
        "libc = ctypes.CDLL(None, use_errno=True)\n"
        "r1 = libc.syscall(%1, 0, None); e1 = ctypes.get_errno()\n"
        "r2 = libc.ptrace(0, 0, None, None); e2 = ctypes.get_errno()\n"
        "print('uring', r1, e1, 'ptrace', r2, e2, 'pid', os.getpid() > 0)\n").arg(SYS_io_uring_setup);
    QProcess control; // (no sandbox: the calls are real, so the filter is what makes the difference)
    control.start(python, {u"-I"_s, u"-c"_s, script});
    QVERIFY(control.waitForFinished(20000));
    const QString free = QString::fromUtf8(control.readAllStandardOutput());
    QVERIFY2(free.contains(QRegularExpression(u"uring -1 (14|22) ptrace 0 "_s)), qPrintable(free));
    const Run filtered = sandboxed(puppetProfile(home.item()), python, {u"-I"_s, u"-c"_s, script}, -1, home.environment());
    QVERIFY2(filtered.out.contains(QStringLiteral("uring -1 %1").arg(EPERM)), qPrintable(filtered.out + filtered.err));
    QVERIFY2(filtered.out.contains(QStringLiteral("ptrace -1 %1").arg(EPERM)), qPrintable(filtered.out + filtered.err));
    QVERIFY2(filtered.out.contains(u"pid True"_s), qPrintable(filtered.out + filtered.err));
}

void TestSandbox::puppetProfileOutlivesNoParent()
{
    if (m_abi < 1)
        QSKIP("needs Landlock");
    // The engine dies with whoever started it: the helper asks for SIGKILL when its parent goes. A shell that starts the sandboxed program in the background and exits must
    // leave no survivor.
    FakeHome home;
    const QString pidFile = home.dir.filePath(u"home/.cache/murasaki/puppet-shader-cache/pid"_s); // the one place the profile lets a program write
    QProcess outer;
    outer.setProcessEnvironment(home.environment());
    outer.setProgram(m_bash);
    outer.setArguments({u"-c"_s, u"'%1' --profile puppet --item '%2' --no-bwrap -- /bin/sh -c 'echo $$ > %3; sleep 30' & wait_for=0; for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do [ -s '%3' ] && break; sleep 0.1; done; kill -9 $$"_s.arg(QStringLiteral(MURASAKI_SANDBOX_PATH), home.item(), pidFile)});
    outer.start();
    QVERIFY(outer.waitForFinished(15000));
    QVERIFY(QFileInfo(pidFile).size() > 0);
    QFile f(pidFile);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const int pid = f.readAll().trimmed().toInt();
    QVERIFY(pid > 1);
    QVERIFY2(QTest::qWaitFor([&] { return ::kill(pid, 0) != 0; }, 5000), "the sandboxed program survived its parent");
}

void TestSandbox::puppetProfileRefusesLooseOptions()
{
    FakeHome home;
    const auto run = [&](const QStringList &args) { return sandboxed(args, m_cat, {u"/dev/null"_s}, -1, home.environment()); };
    QVERIFY(run({u"--profile"_s, u"puppet"_s}).exitCode == 125);                                                                 // no --item
    QVERIFY(run({u"--profile"_s, u"puppet"_s, u"--item"_s, home.item(), u"--core"_s, u"relative.so"_s}).exitCode == 125);       // --core must be absolute
    QVERIFY(run({u"--profile"_s, u"bogus"_s, u"--item"_s, home.item()}).exitCode == 125);
}

void TestSandbox::sceneProfileReadsTheItemAndHidesTheRestOfHome()
{
    if (m_abi < 1)
        QSKIP("needs Landlock");
    FakeHome home;
    const Run item = sandboxed(sceneProfile(home.item()), m_cat, {home.item() + u"/project.json"_s}, -1, home.environment());
    QCOMPARE(item.out, u"{}"_s);
    const Run secret = sandboxed(sceneProfile(home.item()), m_cat, {home.secret()}, -1, home.environment());
    QVERIFY(secret.exitCode != 0);
    QVERIFY2(!secret.out.contains(u"private key"_s), qPrintable(secret.out));
    // Not even the listing of the home directory.
    const Run listing = sandboxed(sceneProfile(home.item()), m_bash, {u"-c"_s, u"ls '%1/home/.ssh'"_s.arg(home.dir.path())}, -1,
                                  home.environment());
    QVERIFY(listing.exitCode != 0);
}

void TestSandbox::sceneProfileWritesOnlyTheCaches()
{
    if (m_abi < 1)
        QSKIP("needs Landlock");
    FakeHome home;
    const QString cache = home.dir.filePath(u"home/.cache/murasaki/shader-cache"_s);
    const Run inCache = sandboxed(sceneProfile(home.item()), m_bash, {u"-c"_s, u"echo x > '%1/entry.bin'"_s.arg(cache)}, -1,
                                  home.environment());
    QCOMPARE(inCache.exitCode, 0);
    QVERIFY(QFile::exists(cache + u"/entry.bin"_s)); // the helper created the directory before the sandbox closed
    for (const QString &outside : {home.item() + u"/planted"_s, home.dir.filePath(u"home/planted"_s),
                                   home.dir.filePath(u"home/.cache/planted"_s)}) {
        const Run run = sandboxed(sceneProfile(home.item()), m_bash, {u"-c"_s, u"echo x > '%1'"_s.arg(outside)}, -1,
                                  home.environment());
        QVERIFY2(run.exitCode != 0, qPrintable(outside));
        QVERIFY2(!QFile::exists(outside), qPrintable(outside));
    }
    // The GPU drivers are pointed at the same cache directory.
    const Run env = sandboxed(sceneProfile(home.item()), m_bash, {u"-c"_s, u"echo $MESA_SHADER_CACHE_DIR"_s}, -1, home.environment());
    QVERIFY2(env.out.trimmed().endsWith(u"/murasaki/gpu-cache"_s), qPrintable(env.out));
}

void TestSandbox::sceneProfileForbidsTcpAndKeepsTheGpuNodes()
{
    if (m_abi < 4)
        QSKIP("needs Landlock ABI 4");
    FakeHome home;
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    const Run tcp = sandboxed(sceneProfile(home.item()), m_bash,
                              {u"-c"_s, u"exec 3<>/dev/tcp/127.0.0.1/%1 && echo connected"_s.arg(server.serverPort())}, -1,
                              home.environment());
    QVERIFY2(!tcp.out.contains(u"connected"_s), qPrintable(tcp.out + tcp.err));
    if (::access("/dev/dri/renderD128", R_OK | W_OK) == 0) { // the engines open a render node read-write (one this user cannot open proves nothing)
        const Run gpu = sandboxed(sceneProfile(home.item()), m_bash, {u"-c"_s, u"exec 3<>/dev/dri/renderD128 && echo opened"_s}, -1,
                                  home.environment());
        QVERIFY2(gpu.out.contains(u"opened"_s), qPrintable(gpu.out + gpu.err));
    }
}

void TestSandbox::sceneProfileLetsPipeWireClientsConnect()
{
    if (m_abi < 1)
        QSKIP("needs Landlock");
    const QString pipewire = QStandardPaths::findExecutable(u"pipewire"_s);
    const QString pwCli = QStandardPaths::findExecutable(u"pw-cli"_s);
    if (pipewire.isEmpty() || pwCli.isEmpty())
        QSKIP("needs pipewire and pw-cli");
    FakeHome home; // its run/ is the XDG_RUNTIME_DIR of a PipeWire server of our own, never the session's
    QProcessEnvironment env = home.environment();
    env.insert(u"PIPEWIRE_RUNTIME_DIR"_s, home.dir.filePath(u"run"_s));
    QProcess server;
    server.setProcessEnvironment(env);
    server.start(pipewire, {});
    QVERIFY(server.waitForStarted(5000));
    const QString socketPath = home.dir.filePath(u"run/pipewire-0"_s);
    QVERIFY2(QTest::qWaitFor([&] { return QFileInfo::exists(socketPath); }, 10000), "private PipeWire did not start");
    const Run info = sandboxed(sceneProfile(home.item()), pwCli, {u"info"_s, u"0"_s}, -1, env);
    server.terminate();
    server.waitForFinished(5000);
    QVERIFY2(info.out.contains(u"PipeWire:Interface:Core"_s), qPrintable(info.out + info.err));
}

void TestSandbox::sceneProfileAllowsTheLibrariesNextToTheEngine()
{
    if (m_abi < 1)
        QSKIP("needs Landlock");
    // An engine installed outside the system dirs, with a library it finds through its RUNPATH.
    const QString cc = QStandardPaths::findExecutable(u"cc"_s);
    if (cc.isEmpty())
        QSKIP("needs a C compiler");
    FakeHome home;
    const QString libDir = home.dir.filePath(u"engine/lib"_s);
    testutil::writeFile(home.dir.filePath(u"lib.c"_s), "int answer(void) { return 42; }\n");
    testutil::writeFile(home.dir.filePath(u"main.c"_s), "#include <stdio.h>\nint answer(void);\nint main(void) { printf(\"%d\", answer()); return 0; }\n");
    QDir().mkpath(libDir);
    QCOMPARE(QProcess::execute(cc, {u"-shared"_s, u"-fPIC"_s, u"-o"_s, libDir + u"/libanswer.so"_s, home.dir.filePath(u"lib.c"_s)}), 0);
    const QString program = home.dir.filePath(u"engine/bin/engine"_s);
    QDir().mkpath(home.dir.filePath(u"engine/bin"_s));
    QCOMPARE(QProcess::execute(cc, {u"-o"_s, program, home.dir.filePath(u"main.c"_s), u"-L"_s + libDir, u"-lanswer"_s,
                                    u"-Wl,--enable-new-dtags,-rpath,$ORIGIN/../lib"_s}), 0);
    const Run run = sandboxed(sceneProfile(home.item()), program, {}, -1, home.environment());
    QCOMPARE(run.out, u"42"_s);
}

static bool bwrapUsable()
{
    const QString bwrap = QStandardPaths::findExecutable(u"bwrap"_s);
    if (bwrap.isEmpty())
        return false;
    QProcess probe;
    probe.start(bwrap, {u"--unshare-net"_s, u"--ro-bind"_s, u"/"_s, u"/"_s, u"--"_s, u"/usr/bin/true"_s});
    return probe.waitForFinished(10000) && probe.exitCode() == 0;
}

void TestSandbox::theMountNamespaceHidesTheSessionBus()
{
    const QString python = QStandardPaths::findExecutable(u"python3"_s);
    if (m_abi < 1 || python.isEmpty() || !bwrapUsable())
        QSKIP("needs Landlock, python3 and a working bwrap");
    FakeHome home;
    const QString bus = home.dir.filePath(u"run/bus"_s);
    QLocalServer server; // a stand-in for the session bus: a unix socket by path in the runtime directory
    QVERIFY(server.listen(bus));
    const QString connect = u"import socket\ns=socket.socket(socket.AF_UNIX)\ns.connect('%1')\nprint('connected')"_s.arg(bus);
    // Landlock alone can not refuse this (ABI 8)...
    const Run landlockOnly = sandboxed(sceneProfile(home.item()) + QStringList{u"--bwrap"_s, u"none"_s}, python, {u"-c"_s, connect}, -1,
                                       home.environment());
    if (m_abi < 9)
        QVERIFY2(landlockOnly.out.contains(u"connected"_s), qPrintable(landlockOnly.out + landlockOnly.err));
    // ...the mount namespace does: the socket is not there.
    const Run namespaced = sandboxed(sceneProfile(home.item()), python, {u"-c"_s, connect}, -1, home.environment());
    QVERIFY2(!namespaced.out.contains(u"connected"_s), qPrintable(namespaced.out + namespaced.err));
    QVERIFY2(namespaced.err.contains(u"No such file"_s) || namespaced.err.contains(u"FileNotFound"_s), qPrintable(namespaced.err));
}

void TestSandbox::theMountNamespaceKeepsTheBridgeDescriptorAndTheAudioSockets()
{
    if (m_abi < 1 || !bwrapUsable())
        QSKIP("needs Landlock and a working bwrap");
    FakeHome home;
    testutil::writeFile(home.dir.filePath(u"run/pipewire-0"_s), "stand-in for the socket"); // a file is enough to see the bind
    QDir().mkpath(home.dir.filePath(u"run/pulse"_s));
    testutil::writeFile(home.dir.filePath(u"run/pulse/native"_s), "x");
    testutil::writeFile(home.dir.filePath(u"run/bus"_s), "the bus");
    const int fd = ::open(home.secret().toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
    QVERIFY(fd >= 0);
    const QString run = home.dir.filePath(u"run"_s);
    const Run run1 = sandboxed(sceneProfile(home.item()), m_bash,
                               {u"-c"_s, u"%1 <&7; [ -e '%2/pipewire-0' ] && echo has-pipewire; [ -e '%2/pulse/native' ] && echo has-pulse; [ -e '%2/bus' ] && echo has-bus"_s.arg(m_cat, run)}, fd, home.environment());
    ::close(fd);
    QVERIFY2(run1.out.contains(u"private key"_s), qPrintable(run1.out + run1.err)); // the inherited descriptor works
    QVERIFY2(run1.out.contains(u"has-pipewire"_s) && run1.out.contains(u"has-pulse"_s), qPrintable(run1.out));
    QVERIFY2(!run1.out.contains(u"has-bus"_s), qPrintable(run1.out));
}

void TestSandbox::withoutBwrapLandlockAloneStillConfinesAndSaysSo()
{
    if (m_abi < 1)
        QSKIP("needs Landlock");
    FakeHome home;
    const Run run = sandboxed(sceneProfile(home.item()) + QStringList{u"--bwrap"_s, u"none"_s}, m_cat, {home.secret()}, -1,
                              home.environment());
    QVERIFY(run.exitCode != 0);
    QVERIFY2(!run.out.contains(u"private key"_s), qPrintable(run.out));
    QVERIFY2(run.err.contains(u"Landlock only"_s), qPrintable(run.err));
}

void TestSandbox::reportDescribesWhatTheSystemOffers()
{
    QProcess process;
    process.start(QStringLiteral(MURASAKI_SANDBOX_PATH), {u"--report"_s});
    QVERIFY(process.waitForFinished(15000));
    QCOMPARE(process.exitCode(), 0);
    const QJsonObject report = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
    QCOMPARE(report.value(u"landlockAbi"_s).toInt(), m_abi);
    QVERIFY(report.value(u"bwrap"_s).isBool());
    QCOMPARE(report.value(u"scopes"_s).toBool(), m_abi >= 6);
    QCOMPARE(report.value(u"tcpDeny"_s).toBool(), m_abi >= 4 || report.value(u"bwrap"_s).toBool());
}

void TestSandbox::requireFailsWhenTcpCannotBeRefused()
{
    if (m_abi < 1)
        QSKIP("needs Landlock");
    FakeHome home;
    // ABI 3 has no TCP rules; without a network namespace, --require must not pretend.
    const Run bare = sandboxed(sceneProfile(home.item()) + QStringList{u"--abi"_s, u"3"_s, u"--bwrap"_s, u"none"_s}, m_cat,
                               {home.item() + u"/project.json"_s}, -1, home.environment());
    QCOMPARE(bare.exitCode, 125);
    QVERIFY2(bare.err.contains(u"TCP"_s), qPrintable(bare.err));
    if (bwrapUsable()) { // the namespace has no network: that is a refusal too
        const Run namespaced = sandboxed(sceneProfile(home.item()) + QStringList{u"--abi"_s, u"3"_s}, m_cat,
                                         {home.item() + u"/project.json"_s}, -1, home.environment());
        QCOMPARE(namespaced.exitCode, 0);
    }
}

void TestSandbox::bwrapBindsPathsThatMerelyStartWithProc()
{
    // An item library under /procurement (or /process-data) is not procfs, which bwrap mounts itself.
    using murasaki::sandbox::isProcfsPath;
    QVERIFY(isProcfsPath("/proc"));
    QVERIFY(isProcfsPath("/proc/self"));
    QVERIFY(!isProcfsPath("/procurement/items/1"));
    QVERIFY(!isProcfsPath("/process-data"));

    // A PipeWire socket named by PIPEWIRE_REMOTE is bound next to the default one.
    murasaki::sandbox::BwrapInput input;
    input.bwrap = "bwrap";
    input.helper = "/usr/bin/true";
    input.runtimeDir = "/run/user/1000";
    const auto args = murasaki::sandbox::bwrapCommand({}, input, {{"PIPEWIRE_REMOTE", "pipewire-1"}});
    QVERIFY(std::find(args.begin(), args.end(), "/run/user/1000/pipewire-1") != args.end());
}

void TestSandbox::sceneProfileCoversFontconfigAndTheTimezone()
{
    murasaki::sandbox::ProfileEnvironment env;
    env.home = "/home/someone";
    env.xdgCacheHome = "/cache";
    env.xdgConfigHome = "/config";
    murasaki::sandbox::SceneProfileInput input;
    const murasaki::sandbox::Policy policy = murasaki::sandbox::sceneProfile(input, env);
    const auto has = [&policy](const char *path) {
        return std::find(policy.readOnly.begin(), policy.readOnly.end(), path) != policy.readOnly.end();
    };
    QVERIFY(has("/etc/fonts"));
    QVERIFY(has("/var/cache/fontconfig"));
    QVERIFY(has("/config/fontconfig"));
    QVERIFY(has("/cache/fontconfig"));
    QVERIFY(has("/home/someone/.local/share/fonts"));
    QVERIFY(has("/home/someone/.fonts"));
    QVERIFY(has("/etc/localtime"));
    QVERIFY(has("/etc/timezone"));
}

void TestSandbox::sceneProfileLetsTheEngineReadItsVideoPlugin()
{
    // an installed prefix: bin/engine and lib/murasaki/libmurasaki-scene-vk-video.so (what ~/.local, /usr and Flatpak's /app look like)
    QTemporaryDir prefix;
    QVERIFY(prefix.isValid());
    const QString engine = prefix.filePath(u"bin/engine"_s);
    QVERIFY(QDir().mkpath(prefix.filePath(u"bin"_s)));
    QVERIFY(QFile::copy(m_cat, engine)); // any ELF file will do
    testutil::writeFile(prefix.filePath(u"lib/murasaki/libmurasaki-scene-vk-video.so"_s), "plugin");
    murasaki::sandbox::ProfileEnvironment env;
    env.home = "/home/someone";
    murasaki::sandbox::SceneProfileInput input;
    input.program = engine.toStdString();
    const murasaki::sandbox::Policy policy = murasaki::sandbox::sceneProfile(input, env);
    const auto has = [&policy](const QString &path) {
        const std::string wanted = QFileInfo(path).canonicalFilePath().toStdString();
        return std::find(policy.readOnly.begin(), policy.readOnly.end(), wanted) != policy.readOnly.end();
    };
    QVERIFY(has(prefix.filePath(u"lib/murasaki/libmurasaki-scene-vk-video.so"_s)));
    QVERIFY(has(prefix.filePath(u"lib/murasaki"_s)));
    QVERIFY(has(prefix.filePath(u"lib"_s)));
    // an engine with no plugin gets none of that
    QTemporaryDir bare;
    QVERIFY(QDir().mkpath(bare.filePath(u"bin"_s)));
    QVERIFY(QFile::copy(m_cat, bare.filePath(u"bin/engine"_s)));
    input.program = bare.filePath(u"bin/engine"_s).toStdString();
    const murasaki::sandbox::Policy none = murasaki::sandbox::sceneProfile(input, env);
    const std::string lib = QFileInfo(bare.filePath(u"lib"_s)).absoluteFilePath().toStdString();
    QVERIFY(std::find(none.readOnly.begin(), none.readOnly.end(), lib) == none.readOnly.end());
}

void TestSandbox::theMountNamespaceFollowsASymlinkedItemDirectory()
{
    if (m_abi < 1 || !bwrapUsable())
        QSKIP("needs Landlock and a working bwrap");
    // The library the user points at is a symlink to another disk: the item must be readable through the link.
    FakeHome home;
    QTemporaryDir otherDisk;
    testutil::writeFile(otherDisk.filePath(u"item/project.json"_s), "{\"linked\":true}");
    const QString link = home.dir.filePath(u"steam/linked"_s);
    QVERIFY(QFile::link(otherDisk.filePath(u"item"_s), link));
    const Run run = sandboxed(sceneProfile(link), m_cat, {link + u"/project.json"_s}, -1, home.environment());
    QCOMPARE(run.out, u"{\"linked\":true}"_s);
}

void TestSandbox::theLocalTimezoneSurvivesTheMountNamespace()
{
    if (m_abi < 1 || !bwrapUsable())
        QSKIP("needs Landlock and a working bwrap");
    // A clock scene must show the local time: with TZ unset, /etc/localtime decides, inside the namespace as outside.
    const QString date = QStandardPaths::findExecutable(u"date"_s);
    if (date.isEmpty())
        QSKIP("needs date");
    FakeHome home;
    QProcessEnvironment env = home.environment();
    env.remove(u"TZ"_s);
    const Run inside = sandboxed(sceneProfile(home.item()), date, {u"+%z %Z"_s}, -1, env);
    QProcess outside;
    outside.setProcessEnvironment(env);
    outside.start(date, {u"+%z %Z"_s});
    QVERIFY(outside.waitForFinished(10000));
    QCOMPARE(inside.out.trimmed(), QString::fromUtf8(outside.readAllStandardOutput()).trimmed());
}

QTEST_GUILESS_MAIN(TestSandbox)
#include "tst_sandbox.moc"

// ---- The screen matching helper (murasaki-screenmatch) ----

void TestSandbox::screenmatchProfileTable()
{
    using namespace murasaki::sandbox;
    const Policy p = screenmatchProfile({"/usr/bin/murasaki-screenmatch"});
    QVERIFY(p.denyTcp);
    QVERIFY(p.readWrite.empty()); // it writes nothing, anywhere
    for (const char *path : {"/usr", "/lib", "/lib64", "/etc/ld.so.cache", "/etc/pipewire"})
        QVERIFY2(contains(p.readOnly, path), path);
    for (const std::string &path : p.readOnly) {
        QVERIFY2(path.rfind("/home", 0) != 0, path.c_str());      // no home
        QVERIFY2(path.rfind("/run", 0) != 0, path.c_str());       // no runtime directory
        QVERIFY2(path.rfind("/dev/dri", 0) != 0, path.c_str());   // no GPU
        QVERIFY2(path.rfind("/tmp", 0) != 0, path.c_str());
        QVERIFY2(path.rfind("/var", 0) != 0, path.c_str());
        QVERIFY2(path.rfind("/sys", 0) != 0, path.c_str());
    }
    // it holds the helper and nothing of an engine's tree: a helper in a build or user tree gets its own file, not the directory it sits in, a plugin directory or ~/.local/lib/murasaki
    QTemporaryDir tree;
    QVERIFY(QDir().mkpath(tree.filePath(u"build/bin"_s)));
    QVERIFY(QDir().mkpath(tree.filePath(u"build/lib/murasaki"_s)));
    const QString helper = tree.filePath(u"build/bin/murasaki-screenmatch"_s);
    for (const QString &file : {helper, tree.filePath(u"build/lib/murasaki/libmurasaki-scene-vk-video.so"_s)}) {
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
        f.close();
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
    const Policy built = screenmatchProfile({QFileInfo(helper).canonicalFilePath().toStdString()});
    QVERIFY(contains(built.readOnly, QFileInfo(helper).canonicalFilePath().toStdString()));
    for (const std::string &path : built.readOnly) {
        const QString q = QString::fromStdString(path);
        QVERIFY2(!q.startsWith(QFileInfo(tree.path()).canonicalFilePath() + u"/build/lib"_s), path.c_str());
        QVERIFY2(q != QFileInfo(tree.filePath(u"build/bin"_s)).canonicalFilePath(), path.c_str());
        QVERIFY2(!q.endsWith(u"/.local/lib/murasaki"_s), path.c_str());
        QVERIFY2(!q.contains(u"scene-vk"_s), path.c_str());
    }
    // the environment is its own and small
    const auto env = screenmatchProfileEnvironment();
    for (const auto &[name, value] : env) {
        QVERIFY2(name != "HOME" && name != "XDG_RUNTIME_DIR" && name != "DISPLAY" && name != "WAYLAND_DISPLAY" && name.rfind("DBUS", 0) != 0 && name != "LD_PRELOAD", name.c_str());
        Q_UNUSED(value);
    }
}

void TestSandbox::screenmatchSeccompFilterAnswersEachCase()
{
    const std::vector<sock_filter> program = murasaki::sandbox::buildScreenmatchFilter();
    QVERIFY(!program.empty());
#if defined(__x86_64__)
    const uint32_t arch = AUDIT_ARCH_X86_64;
#else
    const uint32_t arch = AUDIT_ARCH_AARCH64;
#endif
    const uint32_t eperm = SECCOMP_RET_ERRNO | EPERM;
    for (const long number : murasaki::sandbox::screenmatchDeniedSyscalls())
        QCOMPARE(runFilter(program, arch, uint32_t(number)), eperm);
    for (const long number : {long(SYS_socket), long(SYS_connect), long(SYS_bind), long(SYS_listen), long(SYS_accept), long(SYS_accept4), long(SYS_execve), long(SYS_execveat), long(SYS_ptrace),
                              long(SYS_io_uring_setup), long(SYS_unshare)})
        QCOMPARE(runFilter(program, arch, uint32_t(number)), eperm);
    // what PipeWire and the process loop need stays allowed: its descriptors, memfd mappings, polling
    for (const long number : {long(SYS_read), long(SYS_write), long(SYS_poll), long(SYS_epoll_wait), long(SYS_epoll_ctl), long(SYS_eventfd2), long(SYS_timerfd_create), long(SYS_sendmsg), long(SYS_recvmsg),
                              long(SYS_socketpair), long(SYS_mmap), long(SYS_munmap), long(SYS_ioctl), long(SYS_openat), long(SYS_futex), long(SYS_getpid), long(SYS_exit_group), long(SYS_clock_gettime),
                              long(SYS_fcntl), long(SYS_close), long(SYS_dup), long(SYS_memfd_create)})
        QCOMPARE(runFilter(program, arch, uint32_t(number)), uint32_t(SECCOMP_RET_ALLOW));
    // a puppet filter does not refuse these: the helper's list is the larger one
    const std::vector<sock_filter> puppet = murasaki::sandbox::buildPuppetFilter();
    QCOMPARE(runFilter(puppet, arch, uint32_t(SYS_socket)), uint32_t(SECCOMP_RET_ALLOW));
    QVERIFY(murasaki::sandbox::screenmatchDeniedSyscalls().size() > murasaki::sandbox::puppetDeniedSyscalls().size());
    // another ABI is refused whole
    QCOMPARE(runFilter(program, AUDIT_ARCH_I386, uint32_t(SYS_read)), eperm);
}

void TestSandbox::screenmatchHelperCannotOpenConnectOrExec()
{
    if (m_abi < 1)
        QSKIP("needs Landlock");
    const QString helper = QStringLiteral(MURASAKI_SCREENMATCH_PATH);
    for (const bool bwrap : {true, false}) {
        if (!bwrap && m_abi < 4)
            continue; // Landlock alone can not refuse TCP before ABI 4: with --require the wrapper refuses the profile (125)
        QStringList policy{u"--profile"_s, u"screenmatch"_s, u"--require"_s};
        if (!bwrap)
            policy << u"--no-bwrap"_s;
        const Run run = sandboxed(policy, helper, {u"--selftest"_s});
        QVERIFY2(run.exitCode == 0, qPrintable(run.out + run.err));
        QHash<QString, QPair<int, int>> got; // name -> result, errno
        for (const QString &line : run.out.split(u'\n', Qt::SkipEmptyParts)) {
            const QStringList f = line.split(u' ');
            QCOMPARE(f.size(), 3);
            got.insert(f[0], {f[1].toInt(), f[2].toInt()});
        }
        for (const char *name : {"socket", "connect", "bind", "listen", "accept4", "execve"}) {
            QVERIFY2(got.contains(QString::fromLatin1(name)), name);
            QCOMPARE(got.value(QString::fromLatin1(name)), (QPair<int, int>{-1, EPERM})); // the filter, not luck
        }
        QCOMPARE(got.value(u"core-limit"_s), (QPair<int, int>{0, 0})); // no core dump (the shared memory of the frames would be in it)
        QCOMPARE(got.value(u"dumpable"_s), (QPair<int, int>{0, 0}));   // and nobody of the same user can ptrace it
        QCOMPARE(got.value(u"socketpair"_s).first, 0); // its own channel stays possible
        QCOMPARE(got.value(u"getpid"_s).first, 0);
        QCOMPARE(got.value(u"open-etc"_s).first, -1);   // no file outside the system's libraries
        QCOMPARE(got.value(u"open-write"_s).first, -1); // nothing is writable
        QVERIFY(got.value(u"open-usr"_s).first >= 0);   // the libraries are readable
    }
}

void TestSandbox::screenmatchProfileRefusesLooseOptions()
{
    FakeHome home;
    const auto run = [&](const QStringList &args) { return sandboxed(args, m_cat, {u"/dev/null"_s}, -1, home.environment()); };
    const QStringList base{u"--profile"_s, u"screenmatch"_s};
    QVERIFY(run(base + QStringList{u"--ro"_s, u"/etc"_s}).exitCode == 125);                  // no paths of any kind
    QVERIFY(run(base + QStringList{u"--rw"_s, u"/tmp"_s}).exitCode == 125);
    QVERIFY(run(base + QStringList{u"--item"_s, home.item()}).exitCode == 125);
    QVERIFY(run(base + QStringList{u"--src"_s, u"/etc"_s}).exitCode == 125);
    QVERIFY(run(base + QStringList{u"--out"_s, u"/tmp"_s}).exitCode == 125);
    QVERIFY(run(base + QStringList{u"--core"_s, u"/x.so"_s}).exitCode == 125);
    QVERIFY(run(QStringList{u"--profile"_s, u"screenmatch"_s, u"--profile"_s, u"puppet"_s, u"--item"_s, home.item()}).exitCode == 125);
}

void TestSandbox::screenmatchHelperTakesNoFrameSourceOption()
{
    // The helper has two descriptors and a node list; no option names a file, a path or a fake source.
    const QString helper = QStringLiteral(MURASAKI_SCREENMATCH_PATH);
    for (const QStringList &args : {QStringList{}, QStringList{u"--source"_s, u"/etc/passwd"_s}, QStringList{u"--frames"_s, u"x.raw"_s}, QStringList{u"--fake"_s},
                                    QStringList{u"--chan-fd"_s, u"4"_s, u"--pw-fd"_s, u"4"_s, u"--nodes"_s, u"5"_s}, QStringList{u"--chan-fd"_s, u"4"_s, u"--pw-fd"_s, u"5"_s, u"--nodes"_s, u"0"_s},
                                    QStringList{u"--chan-fd"_s, u"4"_s, u"--pw-fd"_s, u"5"_s, u"--nodes"_s, u"1,2,3,4,5,6,7,8,9"_s}, QStringList{u"--chan-fd"_s, u"x"_s}, QStringList{u"--buffers"_s, u"dmabuf"_s}}) {
        QProcess p;
        p.start(helper, args);
        QVERIFY(p.waitForFinished(10000));
        QCOMPARE(p.exitCode(), 2);
    }
}
