#include "fakesteamcmd.h"
#include "steamcmdmanager.h"
#include "testutil.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <csignal>
#include <sys/types.h>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestSteamCmdManager : public QObject
{
    Q_OBJECT

    struct Rig {
        QTemporaryDir tmp;
        QString program;
        std::unique_ptr<SteamCmdManager> manager;

        explicit Rig(const QHash<QString, QString> &env = {})
        {
            qputenv("FAKE_STEAMCMD_DIR", tmp.filePath(u"fake"_s).toUtf8());
            QDir().mkpath(tmp.filePath(u"fake"_s));
            for (const char *name : {"FAKE_GUARD", "FAKE_MOBILE", "FAKE_PASSWORD", "FAKE_FAIL_ITEM", "FAKE_SLOW", "FAKE_EXPIRED",
                                     "FAKE_SILENT_ITEM", "FAKE_CHILD", "FAKE_CHATTER"})
                qunsetenv(name);
            for (auto it = env.cbegin(); it != env.cend(); ++it)
                qputenv(it.key().toUtf8().constData(), it.value().toUtf8());
            program = fakesteamcmd::write(tmp.path());
            makeManager();
        }
        void makeManager()
        {
            manager = std::make_unique<SteamCmdManager>(tmp.filePath(u"install"_s), tmp.filePath(u"state/steamcmd.json"_s));
            manager->setProgram(program);
        }
        QString token() const
        {
            QFile f(tmp.filePath(u"fake/token"_s));
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
        }
        QString calls() const
        {
            QFile f(tmp.filePath(u"fake/calls"_s));
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
        }
        QString typed() const
        {
            QFile f(tmp.filePath(u"fake/typed"_s));
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
        }
        SteamCmdJob job(const QString &item) const
        {
            for (const SteamCmdJob &j : manager->jobs()) {
                if (j.itemId == item)
                    return j;
            }
            return {};
        }
        // Signs in with the right password.
        void signIn(const QString &user = u"bob"_s)
        {
            QVERIFY(manager->login(user));
            QTRY_COMPARE(manager->prompt(), u"password"_s);
            QVERIFY(manager->submit(u"password"_s, u"secret"_s));
            QTRY_COMPARE(manager->state(), u"logged-in"_s);
        }
    };

private slots:
    void validatesWhatItIsGiven();
    void looksForTheProgram();
    void unavailableWithoutTheProgram();
    void signsInWithAPasswordAndKeepsOnlyTheName();
    void aWrongPasswordFailsWithSteamsReason();
    void steamGuardCodeIsAskedAndChecked();
    void aWrongGuardCodeFails();
    void mobileConfirmationIsShownThenAccepted();
    void aCachedTokenNeedsNoPassword();
    void theSecretsNeverReachTheCommandLine();
    void submitOnlyAnswersWhatIsAsked();
    void cancelLoginStopsIt();
    void aLoginThatTakesTooLongIsStopped();
    void downloadsReportProgressAndFinish();
    void downloadsNeedAnAccount();
    void downloadsRunOneAtATimeInOrder();
    void aFailedDownloadCarriesTheReason();
    void aSilentExitIsAFailure();
    void cancellingStopsTheRunningDownloadAndDropsTheQueued();
    void anExpiredSessionFailsTheJobsAndSignsOut();
    void aStalledDownloadIsStopped();
    void theSameItemIsNotQueuedTwice();
    void logoutForgetsTheAccountAndEndsTheSession();
    void theAccountSurvivesARestart();
    void historyIsBounded();
    void cancellingKillsTheWholeProcessGroup();
    void nothingStartsUntilThePreviousGroupIsGone();
    void logoutWaitsForTheGroupsItStopped();
    void theSplitterIsFlushedWhenAnAnswerIsSent();
};

void TestSteamCmdManager::validatesWhatItIsGiven()
{
    QVERIFY(SteamCmdManager::isValidUser(u"bob_42"_s));
    QVERIFY(SteamCmdManager::isValidUser(u"a.b-c@example.com"_s));
    for (const QString &bad : {QString(), u"+quit"_s, u"-x"_s, u".hidden"_s, u"_a"_s, u"@a"_s, u"a b"_s, u"x\ny"_s, QString(65, u'a'), u"a;b"_s})
        QVERIFY2(!SteamCmdManager::isValidUser(bad), qPrintable(bad));
    QVERIFY(SteamCmdManager::isValidItemId(u"123456"_s));
    for (const QString &bad : {QString(), u"12a"_s, u"-1"_s, u"1 2"_s, QString(21, u'1'), u"+quit"_s})
        QVERIFY2(!SteamCmdManager::isValidItemId(bad), qPrintable(bad));
    QVERIFY(SteamCmdManager::isValidAppId(u"431960"_s));
    QVERIFY(SteamCmdManager::isValidAppId(u"3474900"_s));
    QVERIFY(!SteamCmdManager::isValidAppId(u"730"_s));
    QCOMPARE(SteamCmdManager::contentDir(u"/d"_s, u"431960"_s), u"/d/steamapps/workshop/content/431960"_s);
}

void TestSteamCmdManager::looksForTheProgram()
{
    QTemporaryDir tmp;
    const QString fake = fakesteamcmd::write(tmp.path());
    qputenv("MURASAKI_STEAMCMD", fake.toUtf8());
    QCOMPARE(SteamCmdManager::findProgram(), fake);
    qputenv("MURASAKI_STEAMCMD", "/nonexistent/steamcmd");
    QVERIFY(SteamCmdManager::findProgram().isEmpty()); // an explicit choice that is wrong is not silently replaced
    qunsetenv("MURASAKI_STEAMCMD");
}

void TestSteamCmdManager::unavailableWithoutTheProgram()
{
    Rig rig;
    rig.manager->setProgram(QString());
    QCOMPARE(rig.manager->state(), u"unavailable"_s);
    QVERIFY(!rig.manager->toJson().value(u"available"_s).toBool());
    QString error;
    QVERIFY(!rig.manager->login(u"bob"_s, &error));
    QVERIFY(error.contains(u"not installed"_s));
    QVERIFY(!rig.manager->download(u"431960"_s, u"1"_s, &error));
}

void TestSteamCmdManager::signsInWithAPasswordAndKeepsOnlyTheName()
{
    Rig rig;
    QCOMPARE(rig.manager->state(), u"logged-out"_s);
    QSignalSpy changed(rig.manager.get(), &SteamCmdManager::changed);
    QVERIFY(rig.manager->login(u"bob"_s));
    QCOMPARE(rig.manager->state(), u"logging-in"_s);
    QTRY_COMPARE(rig.manager->prompt(), u"password"_s);
    QVERIFY(rig.manager->submit(u"password"_s, u"secret"_s));
    QCOMPARE(rig.manager->prompt(), QString()); // answered
    QTRY_COMPARE(rig.manager->state(), u"logged-in"_s);
    QCOMPARE(rig.manager->user(), u"bob"_s);
    QTRY_VERIFY(changed.count() >= 1);
    QCOMPARE(rig.token(), u"bob"_s);
    QCOMPARE(rig.typed().trimmed(), u"secret"_s);
    // the state file holds the name and nothing secret
    QFile f(rig.tmp.filePath(u"state/steamcmd.json"_s));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray stored = f.readAll();
    QVERIFY(stored.contains("bob"));
    QVERIFY(!stored.contains("secret"));
    // the status JSON neither
    const QByteArray json = QJsonDocument(rig.manager->toJson()).toJson();
    QVERIFY(!json.contains("secret"));
    QCOMPARE(rig.manager->toJson().value(u"state"_s).toString(), u"logged-in"_s);
    // the process is gone once the login is done
    QTRY_VERIFY(rig.calls().contains(u"+login bob"_s));
}

void TestSteamCmdManager::aWrongPasswordFailsWithSteamsReason()
{
    Rig rig;
    QVERIFY(rig.manager->login(u"bob"_s));
    QTRY_COMPARE(rig.manager->prompt(), u"password"_s);
    QVERIFY(rig.manager->submit(u"password"_s, u"nope"_s));
    QTRY_COMPARE(rig.manager->state(), u"logged-out"_s);
    QCOMPARE(rig.manager->message(), u"Invalid Password"_s);
    QCOMPARE(rig.manager->user(), u"bob"_s); // kept to prefill the dialog
    QVERIFY(!rig.manager->download(u"431960"_s, u"1"_s));
    // and a new attempt starts clean
    QVERIFY(rig.manager->login(u"bob"_s));
    QCOMPARE(rig.manager->message(), QString());
    QTRY_COMPARE(rig.manager->prompt(), u"password"_s);
}

void TestSteamCmdManager::steamGuardCodeIsAskedAndChecked()
{
    Rig rig({{u"FAKE_GUARD"_s, u"1"_s}});
    QVERIFY(rig.manager->login(u"bob"_s));
    QTRY_COMPARE(rig.manager->prompt(), u"password"_s);
    QVERIFY(rig.manager->submit(u"password"_s, u"secret"_s));
    QTRY_COMPARE(rig.manager->prompt(), u"guard"_s);
    QCOMPARE(rig.manager->state(), u"logging-in"_s);
    QVERIFY(rig.manager->submit(u"guard"_s, u"12345"_s));
    QTRY_COMPARE(rig.manager->state(), u"logged-in"_s);
}

void TestSteamCmdManager::aWrongGuardCodeFails()
{
    Rig rig({{u"FAKE_GUARD"_s, u"1"_s}});
    QVERIFY(rig.manager->login(u"bob"_s));
    QTRY_COMPARE(rig.manager->prompt(), u"password"_s);
    QVERIFY(rig.manager->submit(u"password"_s, u"secret"_s));
    QTRY_COMPARE(rig.manager->prompt(), u"guard"_s);
    QVERIFY(rig.manager->submit(u"guard"_s, u"00000"_s));
    QTRY_COMPARE(rig.manager->state(), u"logged-out"_s);
    QCOMPARE(rig.manager->message(), u"Two-factor code mismatch"_s);
}

void TestSteamCmdManager::mobileConfirmationIsShownThenAccepted()
{
    Rig rig({{u"FAKE_MOBILE"_s, u"1"_s}});
    QVERIFY(rig.manager->login(u"bob"_s));
    QTRY_COMPARE(rig.manager->prompt(), u"password"_s);
    QVERIFY(rig.manager->submit(u"password"_s, u"secret"_s));
    QTRY_COMPARE(rig.manager->prompt(), u"confirm"_s);
    QString error;
    QVERIFY(!rig.manager->submit(u"guard"_s, u"1"_s, &error)); // nothing to type: confirm on the phone
    QTRY_COMPARE(rig.manager->state(), u"logged-in"_s);
    QCOMPARE(rig.manager->prompt(), QString());
}

void TestSteamCmdManager::aCachedTokenNeedsNoPassword()
{
    Rig rig;
    rig.signIn();
    rig.manager->logout();
    QTRY_COMPARE(rig.manager->state(), u"logged-out"_s);
    QTRY_VERIFY(rig.token().isEmpty()); // SteamCMD's +logout has run
    // sign in once more, then restart: SteamCMD's token is enough
    rig.signIn();
    testutil::writeFile(rig.tmp.filePath(u"fake/typed"_s), "");
    QVERIFY(rig.manager->login(u"bob"_s));
    QTRY_COMPARE(rig.manager->state(), u"logged-in"_s);
    QCOMPARE(rig.manager->message(), QString());
    QVERIFY(rig.typed().isEmpty()); // no password was asked or sent
}

void TestSteamCmdManager::theSecretsNeverReachTheCommandLine()
{
    Rig rig({{u"FAKE_GUARD"_s, u"1"_s}});
    QVERIFY(rig.manager->login(u"bob"_s));
    QTRY_COMPARE(rig.manager->prompt(), u"password"_s);
    QVERIFY(rig.manager->submit(u"password"_s, u"secret"_s));
    QTRY_COMPARE(rig.manager->prompt(), u"guard"_s);
    QVERIFY(rig.manager->submit(u"guard"_s, u"12345"_s));
    QTRY_COMPARE(rig.manager->state(), u"logged-in"_s);
    const QString calls = rig.calls();
    QVERIFY(!calls.contains(u"secret"_s));
    QVERIFY(!calls.contains(u"12345"_s));
    QVERIFY(calls.contains(u"+login bob"_s));
}

void TestSteamCmdManager::submitOnlyAnswersWhatIsAsked()
{
    Rig rig;
    QString error;
    QVERIFY(!rig.manager->submit(u"password"_s, u"x"_s, &error)); // no login running
    QVERIFY(rig.manager->login(u"bob"_s));
    QVERIFY(!rig.manager->login(u"alice"_s, &error)); // already logging in
    QTRY_COMPARE(rig.manager->prompt(), u"password"_s);
    QVERIFY(!rig.manager->submit(u"guard"_s, u"1"_s, &error)); // it asked for the password
    QVERIFY(!rig.manager->submit(u"bogus"_s, u"1"_s, &error));
    QVERIFY(!rig.manager->submit(u"password"_s, QString(), &error));
    QVERIFY(!rig.manager->submit(u"password"_s, u"two\nlines"_s, &error));
    QVERIFY(!rig.manager->submit(u"password"_s, QString(300, u'x'), &error));
    QCOMPARE(rig.manager->prompt(), u"password"_s); // none of those consumed the prompt
    rig.manager->cancelLogin();
}

void TestSteamCmdManager::cancelLoginStopsIt()
{
    Rig rig;
    QVERIFY(rig.manager->login(u"bob"_s));
    QTRY_COMPARE(rig.manager->prompt(), u"password"_s);
    rig.manager->cancelLogin();
    QCOMPARE(rig.manager->state(), u"logged-out"_s);
    QCOMPARE(rig.manager->prompt(), QString());
    QCOMPARE(rig.manager->message(), QString());
    bool started = false; // free to try again once the old SteamCMD is gone (login has a side effect: not QTRY_VERIFY)
    for (int i = 0; i < 100 && !started; ++i, QTest::qWait(50))
        started = rig.manager->login(u"bob"_s);
    QVERIFY(started);
    rig.manager->cancelLogin();
}

void TestSteamCmdManager::aLoginThatTakesTooLongIsStopped()
{
    Rig rig;
    rig.manager->setLoginTimeoutMs(300);
    QVERIFY(rig.manager->login(u"bob"_s));
    QTRY_COMPARE(rig.manager->state(), u"logged-out"_s);
    QVERIFY(rig.manager->message().contains(u"too long"_s));
}

void TestSteamCmdManager::downloadsReportProgressAndFinish()
{
    Rig rig({{u"FAKE_SLOW"_s, u"1"_s}});
    rig.signIn();
    QVERIFY(rig.manager->download(u"431960"_s, u"111"_s));
    QCOMPARE(rig.job(u"111"_s).state, SteamCmdJob::Running);
    double seen = 0;
    QTRY_VERIFY((seen = rig.job(u"111"_s).progress) > 0.05 && seen < 1);
    QTRY_COMPARE(rig.job(u"111"_s).state, SteamCmdJob::Done);
    QCOMPARE(rig.job(u"111"_s).progress, 1.0);
    QVERIFY(QFile::exists(SteamCmdManager::contentDir(rig.manager->installDir(), u"431960"_s) + u"/111/project.json"_s));
    // the arguments name the install dir, the account and the app
    const QString calls = rig.calls();
    QVERIFY(calls.contains(u"+force_install_dir %1"_s.arg(rig.manager->installDir())));
    QVERIFY(calls.contains(u"+workshop_download_item 431960 111 +quit"_s));
    const QJsonObject json = rig.manager->toJson();
    QCOMPARE(json.value(u"jobs"_s).toArray().at(0).toObject().value(u"state"_s).toString(), u"done"_s);
}

void TestSteamCmdManager::downloadsNeedAnAccount()
{
    Rig rig;
    QString error;
    QVERIFY(!rig.manager->download(u"431960"_s, u"111"_s, &error));
    QVERIFY(error.contains(u"Sign in"_s));
    rig.signIn();
    QVERIFY(!rig.manager->download(u"730"_s, u"111"_s, &error));      // not a Workshop app of ours
    QVERIFY(!rig.manager->download(u"431960"_s, u"1; rm"_s, &error)); // not an id
    QVERIFY(!rig.manager->download(u"431960"_s, u"+quit"_s, &error));
    QVERIFY(rig.manager->jobs().isEmpty());
}

void TestSteamCmdManager::downloadsRunOneAtATimeInOrder()
{
    Rig rig({{u"FAKE_SLOW"_s, u"1"_s}});
    rig.signIn();
    QVERIFY(rig.manager->download(u"431960"_s, u"1"_s));
    QVERIFY(rig.manager->download(u"3474900"_s, u"2"_s));
    QVERIFY(rig.manager->download(u"431960"_s, u"3"_s));
    QCOMPARE(rig.job(u"1"_s).state, SteamCmdJob::Running);
    QCOMPARE(rig.job(u"2"_s).state, SteamCmdJob::Queued);
    QCOMPARE(rig.job(u"3"_s).state, SteamCmdJob::Queued);
    QTRY_COMPARE_WITH_TIMEOUT(rig.job(u"3"_s).state, SteamCmdJob::Done, 20000);
    QCOMPARE(rig.job(u"1"_s).state, SteamCmdJob::Done);
    QCOMPARE(rig.job(u"2"_s).state, SteamCmdJob::Done);
    QVERIFY(QFile::exists(SteamCmdManager::contentDir(rig.manager->installDir(), u"3474900"_s) + u"/2/project.json"_s));
    // never two at once: each download call line appears alone, in order
    const QStringList lines = rig.calls().split(u'\n', Qt::SkipEmptyParts).filter(u"workshop_download_item"_s);
    QCOMPARE(lines.size(), 3);
    QVERIFY(lines.at(0).contains(u" 1 +quit"_s));
    QVERIFY(lines.at(1).contains(u"3474900 2"_s));
}

void TestSteamCmdManager::aFailedDownloadCarriesTheReason()
{
    Rig rig({{u"FAKE_FAIL_ITEM"_s, u"666"_s}});
    rig.signIn();
    QVERIFY(rig.manager->download(u"431960"_s, u"666"_s));
    QVERIFY(rig.manager->download(u"431960"_s, u"7"_s));
    QTRY_COMPARE(rig.job(u"666"_s).state, SteamCmdJob::Failed);
    QCOMPARE(rig.job(u"666"_s).error, u"Access Denied"_s);
    QTRY_COMPARE(rig.job(u"7"_s).state, SteamCmdJob::Done); // the next one still runs
    QCOMPARE(rig.manager->state(), u"logged-in"_s);
}

void TestSteamCmdManager::aSilentExitIsAFailure()
{
    Rig rig({{u"FAKE_SILENT_ITEM"_s, u"9"_s}});
    rig.signIn();
    QVERIFY(rig.manager->download(u"431960"_s, u"9"_s));
    QTRY_COMPARE(rig.job(u"9"_s).state, SteamCmdJob::Failed);
    QVERIFY(rig.job(u"9"_s).error.contains(u"without downloading"_s));
}

void TestSteamCmdManager::cancellingStopsTheRunningDownloadAndDropsTheQueued()
{
    Rig rig({{u"FAKE_SLOW"_s, u"1"_s}});
    rig.signIn();
    QVERIFY(rig.manager->download(u"431960"_s, u"1"_s));
    QVERIFY(rig.manager->download(u"431960"_s, u"2"_s));
    QVERIFY(rig.manager->download(u"431960"_s, u"3"_s));
    QTRY_VERIFY(rig.job(u"1"_s).progress > 0);
    rig.manager->cancel(u"2"_s); // a queued one only
    QCOMPARE(rig.job(u"2"_s).state, SteamCmdJob::Failed);
    QCOMPARE(rig.job(u"2"_s).error, u"Cancelled."_s);
    QCOMPARE(rig.job(u"1"_s).state, SteamCmdJob::Running);
    rig.manager->cancel(u"1"_s); // the running one
    QCOMPARE(rig.job(u"1"_s).state, SteamCmdJob::Failed);
    QCOMPARE(rig.job(u"1"_s).error, u"Cancelled."_s);
    QTRY_COMPARE(rig.job(u"3"_s).state, SteamCmdJob::Running); // the queue goes on
    rig.manager->cancel({});
    QCOMPARE(rig.job(u"3"_s).state, SteamCmdJob::Failed);
    QCOMPARE(rig.manager->state(), u"logged-in"_s); // cancelling downloads does not sign out
    QVERIFY(!QFile::exists(SteamCmdManager::contentDir(rig.manager->installDir(), u"431960"_s) + u"/1/project.json"_s));
}

void TestSteamCmdManager::anExpiredSessionFailsTheJobsAndSignsOut()
{
    Rig rig;
    rig.signIn();
    qputenv("FAKE_EXPIRED", "1"); // the token is no longer good
    QVERIFY(rig.manager->download(u"431960"_s, u"1"_s));
    QVERIFY(rig.manager->download(u"431960"_s, u"2"_s));
    QTRY_COMPARE(rig.job(u"1"_s).state, SteamCmdJob::Failed);
    QVERIFY(rig.job(u"1"_s).error.contains(u"Sign in again"_s));
    QCOMPARE(rig.job(u"2"_s).state, SteamCmdJob::Failed); // nothing can run without an account
    QCOMPARE(rig.manager->state(), u"logged-out"_s);
    QVERIFY(rig.manager->message().contains(u"expired"_s));
    QCOMPARE(rig.manager->user(), u"bob"_s);
    qunsetenv("FAKE_EXPIRED");
}

void TestSteamCmdManager::aStalledDownloadIsStopped()
{
    Rig rig({{u"FAKE_SLOW"_s, u"1"_s}});
    rig.signIn();
    rig.manager->setStallTimeoutMs(300); // the slow fake is silent for a second between lines
    QVERIFY(rig.manager->download(u"431960"_s, u"1"_s));
    QTRY_COMPARE(rig.job(u"1"_s).state, SteamCmdJob::Failed);
    QVERIFY(rig.job(u"1"_s).error.contains(u"stopped answering"_s));
}

void TestSteamCmdManager::theSameItemIsNotQueuedTwice()
{
    Rig rig({{u"FAKE_SLOW"_s, u"1"_s}});
    rig.signIn();
    QVERIFY(rig.manager->download(u"431960"_s, u"1"_s));
    QVERIFY(rig.manager->download(u"431960"_s, u"1"_s));
    QCOMPARE(rig.manager->jobs().size(), 1);
    rig.manager->cancel({});
    QVERIFY(rig.manager->download(u"431960"_s, u"1"_s)); // a finished (cancelled) entry makes way for a new one
    QCOMPARE(rig.manager->jobs().size(), 1);
    QTRY_COMPARE(rig.job(u"1"_s).state, SteamCmdJob::Running); // after the stopped one has gone
    rig.manager->cancel({});
}

void TestSteamCmdManager::logoutForgetsTheAccountAndEndsTheSession()
{
    Rig rig;
    rig.signIn();
    QCOMPARE(rig.token(), u"bob"_s);
    rig.manager->logout();
    QCOMPARE(rig.manager->state(), u"logged-out"_s);
    QVERIFY(rig.manager->user().isEmpty());
    QVERIFY(!QFile::exists(rig.tmp.filePath(u"state/steamcmd.json"_s)));
    QTRY_VERIFY(rig.token().isEmpty()); // SteamCMD's own +logout ran and dropped its session
    QVERIFY(rig.calls().contains(u"+logout"_s));
    // logging out when nothing is signed in starts nothing
    const QString before = rig.calls();
    rig.manager->logout();
    QTest::qWait(100);
    QCOMPARE(rig.calls(), before);
}

void TestSteamCmdManager::theAccountSurvivesARestart()
{
    Rig rig;
    rig.signIn();
    rig.makeManager();
    QCOMPARE(rig.manager->state(), u"logged-in"_s);
    QCOMPARE(rig.manager->user(), u"bob"_s);
    QVERIFY(rig.manager->download(u"431960"_s, u"5"_s));
    QTRY_COMPARE(rig.job(u"5"_s).state, SteamCmdJob::Done);
    // after a refused login only the name is left
    rig.manager->logout();
    QTRY_VERIFY(rig.token().isEmpty());
    QVERIFY(rig.manager->login(u"alice"_s));
    QTRY_COMPARE(rig.manager->prompt(), u"password"_s);
    QVERIFY(rig.manager->submit(u"password"_s, u"wrong"_s));
    QTRY_COMPARE(rig.manager->state(), u"logged-out"_s);
    rig.makeManager();
    QCOMPARE(rig.manager->state(), u"logged-out"_s);
    QCOMPARE(rig.manager->user(), u"alice"_s);
}

void TestSteamCmdManager::historyIsBounded()
{
    Rig rig;
    rig.signIn();
    for (int i = 1; i <= kMaxSteamCmdJobHistory + 5; ++i) {
        QVERIFY(rig.manager->download(u"431960"_s, QString::number(i)));
        QTRY_VERIFY(rig.job(QString::number(i)).state == SteamCmdJob::Done);
    }
    QVERIFY(rig.manager->download(u"431960"_s, u"900"_s));
    QTRY_COMPARE(rig.job(u"900"_s).state, SteamCmdJob::Done);
    QCOMPARE(rig.manager->jobs().size(), kMaxSteamCmdJobHistory);
    QCOMPARE(rig.manager->jobs().last().itemId, u"900"_s);
}

namespace {
bool alive(qint64 pid)
{
    return pid > 0 && ::kill(pid_t(pid), 0) == 0;
}
} // namespace

void TestSteamCmdManager::cancellingKillsTheWholeProcessGroup()
{
    Rig rig({{u"FAKE_SLOW"_s, u"1"_s}, {u"FAKE_CHILD"_s, u"1"_s}});
    rig.signIn();
    QVERIFY(rig.manager->download(u"431960"_s, u"1"_s));
    QTRY_VERIFY(QFile::exists(rig.tmp.filePath(u"fake/childpid"_s)));
    QFile f(rig.tmp.filePath(u"fake/childpid"_s));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const qint64 child = f.readAll().trimmed().toLongLong();
    QVERIFY(alive(child)); // a helper of steamcmd, as the real one has many
    rig.manager->cancel(u"1"_s);
    QTRY_VERIFY_WITH_TIMEOUT(!alive(child), 8000);
}

void TestSteamCmdManager::nothingStartsUntilThePreviousGroupIsGone()
{
    Rig rig({{u"FAKE_SLOW"_s, u"1"_s}, {u"FAKE_CHILD"_s, u"1"_s}});
    rig.signIn();
    QVERIFY(rig.manager->download(u"431960"_s, u"1"_s));
    QVERIFY(rig.manager->download(u"431960"_s, u"2"_s));
    QTRY_VERIFY(QFile::exists(rig.tmp.filePath(u"fake/childpid"_s)));
    QFile f(rig.tmp.filePath(u"fake/childpid"_s));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const qint64 child = f.readAll().trimmed().toLongLong();
    QFile::remove(rig.tmp.filePath(u"fake/childpid"_s));
    rig.manager->cancel(u"1"_s);
    // the second download starts only once the first one's group (its child included) is gone
    QTRY_COMPARE_WITH_TIMEOUT(rig.job(u"2"_s).state, SteamCmdJob::Running, 8000);
    QVERIFY(!alive(child));
    rig.manager->cancel({});
}

void TestSteamCmdManager::logoutWaitsForTheGroupsItStopped()
{
    Rig rig({{u"FAKE_SLOW"_s, u"1"_s}, {u"FAKE_CHILD"_s, u"1"_s}});
    rig.signIn();
    QVERIFY(rig.manager->download(u"431960"_s, u"1"_s));
    QTRY_VERIFY(QFile::exists(rig.tmp.filePath(u"fake/childpid"_s)));
    QFile f(rig.tmp.filePath(u"fake/childpid"_s));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const qint64 child = f.readAll().trimmed().toLongLong();
    rig.manager->logout(); // stops the download and its group, then runs +logout
    QCOMPARE(rig.manager->state(), u"logged-out"_s);
    QTRY_VERIFY_WITH_TIMEOUT(rig.token().isEmpty(), 10000); // SteamCMD's own +logout ran after the stop
    QVERIFY(!alive(child));
    QVERIFY(rig.calls().contains(u"+logout"_s));
}

void TestSteamCmdManager::theSplitterIsFlushedWhenAnAnswerIsSent()
{
    // After the password SteamCMD prints a little without a newline while it works; the spent "password:" text must not
    // read as a new prompt.
    Rig rig({{u"FAKE_CHATTER"_s, u"1"_s}});
    QVERIFY(rig.manager->login(u"bob"_s));
    QTRY_COMPARE(rig.manager->prompt(), u"password"_s);
    QVERIFY(rig.manager->submit(u"password"_s, u"secret"_s));
    for (int i = 0; i < 8; ++i) {
        QTest::qWait(50);
        QCOMPARE(rig.manager->prompt(), QString());
    }
    QTRY_COMPARE(rig.manager->state(), u"logged-in"_s);
}

QTEST_GUILESS_MAIN(TestSteamCmdManager)
#include "tst_steamcmdmanager.moc"
