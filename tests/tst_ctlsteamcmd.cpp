#include "constants.h"
#include "fakesteamcmd.h"
#include "murasakiservice.h"
#include "steamcmdmanager.h"
#include "testutil.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

// `murasakictl steamcmd ...` against a daemon facade that owns the real service name (private bus only), with a fake
// steamcmd behind it.
class TestCtlSteamCmd : public QObject
{
    Q_OBJECT

private slots:
    void commands();
    void usageErrors();

private:
    struct Result {
        int code = -1;
        QString out;
        QString err;
    };
    // The service lives in this process, so the event loop must keep running while ctl waits; `input` is its stdin.
    static Result ctl(const QStringList &args, const QByteArray &input = {})
    {
        QProcess process;
        process.start(QStringLiteral(MURASAKICTL_PATH), args);
        if (!process.waitForStarted(5000))
            return {};
        if (!input.isEmpty()) {
            process.write(input);
            process.closeWriteChannel();
        }
        if (!QTest::qWaitFor([&] { return process.state() == QProcess::NotRunning; }, 30000)) {
            process.kill();
            process.waitForFinished(2000);
            return {-2, {}, u"timed out"_s};
        }
        return {process.exitCode(), QString::fromUtf8(process.readAllStandardOutput()),
                QString::fromUtf8(process.readAllStandardError())};
    }
};

void TestCtlSteamCmd::commands()
{
    if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
        QSKIP("needs a private bus; run through ctest (dbus-run-session)");
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        QSKIP("no D-Bus session bus");

    QTemporaryDir tmp;
    qputenv("FAKE_STEAMCMD_DIR", tmp.filePath(u"fake"_s).toUtf8());
    QDir().mkpath(tmp.filePath(u"fake"_s));
    qputenv("FAKE_GUARD", "1");
    Library library;
    AssignmentStore assignments(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    SteamCmdManager manager(tmp.filePath(u"install"_s), tmp.filePath(u"steamcmd.json"_s));
    manager.setProgram(fakesteamcmd::write(tmp.path()));
    MurasakiService service(&library, &assignments, &settings, u"ok"_s, {});
    service.attachSteamCmd(&manager);
    const auto cleanup = qScopeGuard([&] {
        bus.unregisterService(QString(kDBusService));
        bus.unregisterObject(QString(kDBusPath));
        qunsetenv("FAKE_GUARD");
    });
    QVERIFY(service.registerObject(bus));
    QVERIFY(bus.registerService(QString(kDBusService)));

    QCOMPARE(ctl({u"steamcmd"_s, u"status"_s}).out, u"logged-out\n"_s);
    QCOMPARE(ctl({u"steamcmd"_s, u"download"_s, u"1"_s}).code, 1); // signed out: the daemon says so
    // a wrong password: SteamCMD's reason, exit 1
    const Result wrong = ctl({u"steamcmd"_s, u"login"_s, u"bob"_s}, "nope\n");
    QCOMPARE(wrong.code, 1);
    QVERIFY(wrong.err.contains(u"Invalid Password"_s));
    // the password then the Steam Guard code, both from stdin
    const Result good = ctl({u"steamcmd"_s, u"login"_s, u"bob"_s}, "secret\n12345\n");
    QCOMPARE(good.code, 0);
    QVERIFY(good.out.contains(u"signed in as bob"_s));
    QVERIFY(!good.out.contains(u"secret"_s)); // nothing typed is echoed to stdout
    QCOMPARE(ctl({u"steamcmd"_s, u"status"_s}).out, u"logged-in\tbob\n"_s);

    QCOMPARE(ctl({u"steamcmd"_s, u"download"_s, u"11"_s, u"12"_s}).code, 0);
    QTRY_COMPARE_WITH_TIMEOUT(ctl({u"steamcmd"_s, u"status"_s}).out,
                              u"logged-in\tbob\n11\tdone\t100%\t\n12\tdone\t100%\t\n"_s, 20000);
    QCOMPARE(ctl({u"steamcmd"_s, u"download"_s, u"--app"_s, u"pets"_s, u"13"_s}).code, 0);
    QTRY_VERIFY_WITH_TIMEOUT(ctl({u"steamcmd"_s, u"status"_s}).out.contains(u"13\tdone"_s), 20000);
    QVERIFY(QFile::exists(SteamCmdManager::contentDir(manager.installDir(), u"3474900"_s) + u"/13/project.json"_s));
    QCOMPARE(ctl({u"steamcmd"_s, u"cancel"_s}).code, 0);
    QCOMPARE(ctl({u"steamcmd"_s, u"logout"_s}).code, 0);
    QVERIFY(ctl({u"steamcmd"_s, u"status"_s}).out.startsWith(u"logged-out\n"_s)); // the finished downloads stay listed
}

void TestCtlSteamCmd::usageErrors()
{
    // These never reach a daemon, so they are safe anywhere.
    QCOMPARE(ctl({u"steamcmd"_s}).code, 2);
    QCOMPARE(ctl({u"steamcmd"_s, u"bogus"_s}).code, 2);
    QCOMPARE(ctl({u"steamcmd"_s, u"login"_s}).code, 2);
    QCOMPARE(ctl({u"steamcmd"_s, u"download"_s}).code, 2);
    const Result app = ctl({u"steamcmd"_s, u"download"_s, u"--app"_s, u"730"_s, u"1"_s});
    QCOMPARE(app.code, 2);
    QVERIFY(app.err.contains(u"--app"_s));
}

QTEST_GUILESS_MAIN(TestCtlSteamCmd)
#include "tst_ctlsteamcmd.moc"
