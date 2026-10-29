#include "constants.h"
#include "murasakiservice.h"
#include "playlist.h"
#include "playlistmanager.h"
#include "testutil.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

// `murasakictl playlist ...` against a daemon facade that owns the real service name. That is only safe on a private
// bus, so the test runs under dbus-run-session with MURASAKI_TEST_PRIVATE_BUS=1 and skips otherwise.
class TestCtlPlaylists : public QObject
{
    Q_OBJECT

private slots:
    void playlistCommands();
    void usageErrors();

private:
    struct Result {
        int code = -1;
        QString out;
        QString err;
    };
    // The service lives in this process, so the event loop must keep running while ctl waits for its reply.
    static Result ctl(const QStringList &args)
    {
        QProcess process;
        process.start(QStringLiteral(MURASAKICTL_PATH), args);
        if (!process.waitForStarted(5000))
            return {};
        if (!QTest::qWaitFor([&] { return process.state() == QProcess::NotRunning; }, 15000)) {
            process.kill();
            process.waitForFinished(2000);
            return {-2, {}, u"timed out"_s};
        }
        return {process.exitCode(), QString::fromUtf8(process.readAllStandardOutput()),
                QString::fromUtf8(process.readAllStandardError())};
    }
};

void TestCtlPlaylists::playlistCommands()
{
    if (qEnvironmentVariableIsEmpty("MURASAKI_TEST_PRIVATE_BUS"))
        QSKIP("needs a private bus; run through ctest (dbus-run-session)");
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        QSKIP("no D-Bus session bus");

    QTemporaryDir tmp;
    for (const QString &id : {u"1"_s, u"2"_s, u"3"_s})
        testutil::writeVideoItem(tmp.filePath(u"we"_s), id, u"Item "_s + id);
    Library library;
    library.setWorkshopDirs({tmp.filePath(u"we"_s)});
    library.rescan();
    AssignmentStore assignments(tmp.filePath(u"assignments.json"_s));
    SettingsStore settings(tmp.filePath(u"settings.json"_s));
    PlaylistStore playlists(tmp.filePath(u"playlists.json"_s));
    PlaylistManager manager(&playlists, &assignments, &library, tmp.filePath(u"state/playlist-state.json"_s));
    MurasakiService service(&library, &assignments, &settings, u"ok"_s, {});
    service.attachPlaylists(&playlists, &manager);
    const auto cleanup = qScopeGuard([&] {
        bus.unregisterService(QString(kDBusService));
        bus.unregisterObject(QString(kDBusPath));
    });
    QVERIFY(service.registerObject(bus));
    QVERIFY(bus.registerService(QString(kDBusService)));

    QCOMPARE(ctl({u"playlist"_s, u"list"_s}).out, QString());
    const Result created = ctl({u"playlist"_s, u"create"_s, u"Calm"_s});
    QCOMPARE(created.code, 0);
    const QString id = created.out.trimmed();
    QCOMPARE(playlists.playlists().size(), 1);
    QCOMPARE(ctl({u"playlist"_s, u"list"_s}).out, id + u"\tCalm\tsequential\t15\t0\n"_s);

    // by name or by id
    QCOMPARE(ctl({u"playlist"_s, u"add"_s, u"calm"_s, u"we:1"_s, u"we:2"_s, u"we:3"_s}).code, 0);
    QCOMPARE(ctl({u"playlist"_s, u"remove"_s, id, u"we:2"_s}).code, 0);
    QCOMPARE(ctl({u"playlist"_s, u"show"_s, u"Calm"_s}).out, u"we:1\nwe:3\n"_s);
    QCOMPARE(ctl({u"playlist"_s, u"set"_s, u"Calm"_s, u"order=shuffle"_s, u"interval=30"_s, u"changeOnLogin=on"_s,
                  u"name=Calmer"_s}).code, 0);
    const Playlist stored = *playlists.playlist(id);
    QCOMPARE(stored.name, u"Calmer"_s);
    QCOMPARE(stored.order, PlaylistOrder::Shuffle);
    QCOMPARE(stored.intervalMinutes, 30);
    QVERIFY(stored.changeOnLogin);
    QCOMPARE(ctl({u"playlist"_s, u"set"_s, u"Calmer"_s, u"order=sequential"_s}).code, 0);

    const Result unknown = ctl({u"playlist"_s, u"add"_s, id, u"we:404"_s});
    QCOMPARE(unknown.code, 1);
    QVERIFY(unknown.err.contains(u"Unknown item"_s));
    QCOMPARE(ctl({u"playlist"_s, u"show"_s, u"nope"_s}).code, 1);
    QCOMPARE(ctl({u"playlist"_s, u"set"_s, id, u"interval=0"_s}).code, 1); // the daemon checks the range

    QCOMPARE(ctl({u"playlist"_s, u"next"_s}).code, 1); // nothing plays yet
    QCOMPARE(ctl({u"playlist"_s, u"apply"_s, u"Calmer"_s, u"--output"_s, u"DP-1"_s, u"--fill"_s, u"fit"_s}).code, 0);
    const auto stillAssigned = assignments.resolve(u"DP-1"_s, u"x"_s);
    QVERIFY(stillAssigned);
    QCOMPARE(stillAssigned->itemId, playlistItemId(id));
    QCOMPARE(stillAssigned->fill, u"fit"_s);
    QCOMPARE(manager.current(u"DP-1"_s, u"x"_s)->itemId, u"we:1"_s);
    QCOMPARE(ctl({u"playlist"_s, u"next"_s}).code, 0);
    QCOMPARE(manager.current(u"DP-1"_s, u"x"_s)->itemId, u"we:3"_s);
    QCOMPARE(ctl({u"playlist"_s, u"prev"_s, u"--output"_s, u"DP-1"_s}).code, 0);
    QCOMPARE(manager.current(u"DP-1"_s, u"x"_s)->itemId, u"we:1"_s);
    QCOMPARE(ctl({u"playlist"_s, u"next"_s, u"--output"_s, u"DP-2"_s}).code, 1); // nothing plays on DP-2

    // spanning: all screens only
    QCOMPARE(ctl({u"playlist"_s, u"apply"_s, id, u"--output"_s, u"DP-3"_s, u"--span"_s}).code, 1);
    QVERIFY(!assignments.resolve(u"DP-3"_s, u"x"_s)); // nothing was stored for it
    QCOMPARE(ctl({u"playlist"_s, u"apply"_s, id, u"--span"_s}).code, 0);
    QVERIFY(assignments.resolve(u"HDMI-9"_s, u"x"_s)->span);
    QCOMPARE(ctl({u"set"_s, u"we:2"_s, u"--span"_s, u"--fill"_s, u"fit"_s}).code, 0);
    QVERIFY(assignments.resolve(u"HDMI-9"_s, u"x"_s)->span);
    QCOMPARE(assignments.resolve(u"HDMI-9"_s, u"x"_s)->itemId, u"we:2"_s);
    QCOMPARE(ctl({u"set"_s, u"we:2"_s}).code, 0); // without it, spanning is off again
    QVERIFY(!assignments.resolve(u"HDMI-9"_s, u"x"_s)->span);
    QCOMPARE(ctl({u"unset"_s}).code, 0);
    QCOMPARE(ctl({u"playlist"_s, u"delete"_s, id}).code, 1); // still played by DP-1
    QCOMPARE(ctl({u"unset"_s, u"--output"_s, u"DP-1"_s}).code, 0);
    QCOMPARE(ctl({u"playlist"_s, u"delete"_s, id}).code, 0);
    QVERIFY(playlists.playlists().isEmpty());
}

void TestCtlPlaylists::usageErrors()
{
    // These never reach a daemon, so they are safe anywhere.
    QCOMPARE(ctl({u"playlist"_s}).code, 2);
    QCOMPARE(ctl({u"playlist"_s, u"bogus"_s}).code, 2);
    QCOMPARE(ctl({u"playlist"_s, u"create"_s}).code, 2);
    QCOMPARE(ctl({u"playlist"_s, u"add"_s, u"x"_s}).code, 2);
    const Result badSet = ctl({u"playlist"_s, u"set"_s, u"x"_s, u"colour=red"_s});
    QCOMPARE(badSet.code, 2);
    QVERIFY(badSet.err.contains(u"colour"_s));
    QCOMPARE(ctl({u"playlist"_s, u"set"_s, u"x"_s}).code, 2);
}

QTEST_GUILESS_MAIN(TestCtlPlaylists)
#include "tst_ctlplaylists.moc"
