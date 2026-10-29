// The daemon's checker of puppet folders (src/daemon/puppetchecker.*): a fake `murasaki-puppet` script, no sandbox, no daemon.
#include "puppetchecker.h"
#include "testutil.h"

#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

// A fake helper: it counts its runs in <dir>/runs, then does what <dir>/mode says.
QString fakeHelper(const QTemporaryDir &dir)
{
    const QString path = dir.filePath(u"murasaki-puppet"_s);
    testutil::writeFile(path, R"(#!/bin/sh
# --check FOLDER
echo run >> "$(dirname "$0")/runs"
mode=$(cat "$(dirname "$0")/mode")
case "$mode" in
ok) echo '{"ok":true,"format":"open","canvas":[640,480],"reasons":[],"degraded":[]}' ;;
refuse) echo '{"ok":false,"reasons":["puppet: too large: kMaxVertices"],"degraded":[]}' ;;
junk) echo 'this is not json' ;;
silent) ;;
crash) kill -SEGV $$ ;;
status) echo '{"ok":true}'; exit 3 ;;
hang) exec sleep 30 ;;
flood) exec yes ;;
floodErr) exec yes >&2 ;;
esac
exit 0
)");
    QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::ExeOwner);
    return path;
}

int runs(const QTemporaryDir &dir)
{
    QFile f(dir.filePath(u"runs"_s));
    return f.open(QIODevice::ReadOnly) ? f.readAll().count('\n') : 0;
}

QString folderIn(const QTemporaryDir &dir, const QString &name = u"puppet"_s)
{
    const QString folder = dir.filePath(name);
    testutil::writeFile(folder + u"/model.inp"_s, "TRNSRTS2");
    return folder;
}

} // namespace

class TestPuppetChecker : public QObject
{
    Q_OBJECT
private slots:
    void theCommandLine();
    void anAnswerArrivesOnceAndIsKept();
    void whatCannotBeCheckedSaysWhy_data();
    void whatCannotBeCheckedSaysWhy();
    void aChangedFolderIsCheckedAgain();
    void theSandboxIsInFrontWhenThereIsOne();
    void aRequiredSandboxThatIsMissingRefuses();
    void checksRunOneAtATime();
    void aHelperThatCannotBeStartedEndsTheCheckAndTheQueueGoesOn();
    void anAnswerAboutAFolderThatChangedIsNotKept();
};

void TestPuppetChecker::theCommandLine()
{
    QCOMPARE(PuppetChecker::command(u"/bin/murasaki-puppet"_s, QString(), false, u"/m/haru"_s), (QStringList{u"/bin/murasaki-puppet"_s, u"--check"_s, u"/m/haru"_s}));
    QCOMPARE(PuppetChecker::command(u"/bin/murasaki-puppet"_s, u"/bin/murasaki-sandbox"_s, true, u"/m/haru"_s),
             (QStringList{u"/bin/murasaki-sandbox"_s, u"--profile"_s, u"puppet"_s, u"--item"_s, u"/m/haru"_s, u"--require"_s, u"--"_s, u"/bin/murasaki-puppet"_s, u"--check"_s, u"/m/haru"_s}));
    QVERIFY(!PuppetChecker::command(u"/h"_s, u"/s"_s, false, u"/m"_s).contains(u"--require"_s));
}

void TestPuppetChecker::anAnswerArrivesOnceAndIsKept()
{
    QTemporaryDir dir;
    testutil::writeFile(dir.filePath(u"mode"_s), "ok");
    const QString folder = folderIn(dir);
    PuppetChecker checker(fakeHelper(dir), QString());
    checker.setSandboxMode([] { return PuppetChecker::Sandbox::Off; });
    QSignalSpy changed(&checker, &PuppetChecker::changed);
    QVERIFY(checker.result(folder).isEmpty()); // none yet: a check is queued
    QVERIFY(QTest::qWaitFor([&] { return changed.count() == 1; }, 15000));
    QCOMPARE(changed.at(0).at(0).toString(), folder);
    const QJsonObject answer = checker.result(folder);
    QCOMPARE(answer.value(u"ok"_s).toBool(), true);
    QCOMPARE(answer.value(u"canvas"_s).toArray().at(0).toInt(), 640);
    // asking again does not run it again
    QCOMPARE(checker.result(folder), answer);
    for (int i = 0; i < 5; ++i)
        QCoreApplication::processEvents(); // (a check queued by that call would start on the next turn)
    QCOMPARE(runs(dir), 1);
    QCOMPARE(changed.count(), 1);
    // forgotten: checked again when asked
    checker.forget(folder);
    QVERIFY(checker.result(folder).isEmpty());
    QVERIFY(QTest::qWaitFor([&] { return changed.count() == 2; }, 15000));
    QCOMPARE(runs(dir), 2);
}

void TestPuppetChecker::whatCannotBeCheckedSaysWhy_data()
{
    QTest::addColumn<QByteArray>("mode");
    QTest::addColumn<QString>("reason");
    QTest::newRow("a refusal is its own answer") << QByteArray("refuse") << u"puppet: too large: kMaxVertices"_s;
    QTest::newRow("not json") << QByteArray("junk") << u"puppet: check failed: the checker exited with status 0"_s;
    QTest::newRow("no output") << QByteArray("silent") << u"puppet: check failed: the checker exited with status 0"_s;
    QTest::newRow("crash") << QByteArray("crash") << u"puppet: check failed: the checker crashed"_s;
    QTest::newRow("a status that is not 0") << QByteArray("status") << u"puppet: check failed: the checker exited with status 3"_s;
    QTest::newRow("too slow") << QByteArray("hang") << u"puppet: check failed: it took too long"_s;
    QTest::newRow("endless output") << QByteArray("flood") << u"puppet: check failed: its output is too long"_s;
    QTest::newRow("endless error output") << QByteArray("floodErr") << u"puppet: check failed: it took too long"_s;
}

void TestPuppetChecker::whatCannotBeCheckedSaysWhy()
{
    QFETCH(QByteArray, mode);
    QFETCH(QString, reason);
    QTemporaryDir dir;
    testutil::writeFile(dir.filePath(u"mode"_s), mode);
    const QString folder = folderIn(dir);
    PuppetChecker checker(fakeHelper(dir), QString());
    checker.setSandboxMode([] { return PuppetChecker::Sandbox::Off; });
    checker.setDeadlineMs(mode == "hang" || mode == "floodErr" ? 500 : 20000); // (a crash can take a while to be reaped on a loaded machine: only the hang may time out)
    QSignalSpy changed(&checker, &PuppetChecker::changed);
    checker.result(folder);
    QVERIFY(QTest::qWaitFor([&] { return changed.count() == 1; }, 15000));
    const QJsonObject answer = checker.result(folder);
    QCOMPARE(answer.value(u"ok"_s).toBool(), false);
    QCOMPARE(answer.value(u"reasons"_s).toArray().at(0).toString(), reason);
}

void TestPuppetChecker::aChangedFolderIsCheckedAgain()
{
    QTemporaryDir dir;
    testutil::writeFile(dir.filePath(u"mode"_s), "ok");
    const QString folder = folderIn(dir);
    PuppetChecker checker(fakeHelper(dir), QString());
    checker.setSandboxMode([] { return PuppetChecker::Sandbox::Off; });
    QSignalSpy changed(&checker, &PuppetChecker::changed);
    checker.result(folder);
    QVERIFY(QTest::qWaitFor([&] { return changed.count() == 1; }, 15000));
    // the user replaces the model: another size, so another signature
    testutil::writeFile(folder + u"/model.inp"_s, "TRNSRTS2 and a longer one");
    QVERIFY(checker.result(folder).isEmpty()); // (the old answer is not about this folder)
    QVERIFY(QTest::qWaitFor([&] { return changed.count() == 2; }, 15000));
    QCOMPARE(runs(dir), 2);
    QVERIFY(PuppetChecker::signatureOf(folder) != QString());
}

void TestPuppetChecker::theSandboxIsInFrontWhenThereIsOne()
{
    // a fake sandbox that records its arguments and runs what follows "--"
    QTemporaryDir dir;
    testutil::writeFile(dir.filePath(u"mode"_s), "ok");
    const QString sandbox = dir.filePath(u"murasaki-sandbox"_s);
    testutil::writeFile(sandbox, "#!/bin/sh\necho \"$@\" > \"$(dirname \"$0\")/sandbox-args\"\nwhile [ \"$1\" != -- ]; do shift; done\nshift\nexec \"$@\"\n");
    QFile::setPermissions(sandbox, QFile::permissions(sandbox) | QFileDevice::ExeOwner);
    const QString folder = folderIn(dir);
    PuppetChecker checker(fakeHelper(dir), sandbox);
    checker.setSandboxMode([] { return PuppetChecker::Sandbox::Require; });
    QSignalSpy changed(&checker, &PuppetChecker::changed);
    checker.result(folder);
    QVERIFY(QTest::qWaitFor([&] { return changed.count() == 1; }, 15000));
    QCOMPARE(checker.result(folder).value(u"ok"_s).toBool(), true);
    QFile args(dir.filePath(u"sandbox-args"_s));
    QVERIFY(args.open(QIODevice::ReadOnly));
    const QString line = QString::fromUtf8(args.readAll()).trimmed();
    QVERIFY2(line.startsWith(u"--profile puppet --item "_s + folder + u" --require -- "_s), qPrintable(line));
}

void TestPuppetChecker::aRequiredSandboxThatIsMissingRefuses()
{
    QTemporaryDir dir;
    testutil::writeFile(dir.filePath(u"mode"_s), "ok");
    const QString folder = folderIn(dir);
    qputenv("MURASAKI_SANDBOX", dir.filePath(u"no-such-sandbox"_s).toUtf8());
    PuppetChecker checker(fakeHelper(dir), QString());
    checker.setSandboxMode([] { return PuppetChecker::Sandbox::Require; });
    QSignalSpy changed(&checker, &PuppetChecker::changed);
    checker.result(folder);
    QVERIFY(QTest::qWaitFor([&] { return changed.count() == 1; }, 15000));
    qunsetenv("MURASAKI_SANDBOX");
    QCOMPARE(checker.result(folder).value(u"reasons"_s).toArray().at(0).toString(), u"puppet: check failed: sandbox unavailable"_s);
    QCOMPARE(runs(dir), 0); // the helper never ran unconfined
}

void TestPuppetChecker::checksRunOneAtATime()
{
    QTemporaryDir dir;
    testutil::writeFile(dir.filePath(u"mode"_s), "ok");
    PuppetChecker checker(fakeHelper(dir), QString());
    checker.setSandboxMode([] { return PuppetChecker::Sandbox::Off; });
    QSignalSpy changed(&checker, &PuppetChecker::changed);
    QStringList folders;
    for (int i = 0; i < 4; ++i)
        folders << folderIn(dir, u"p%1"_s.arg(i));
    for (const QString &f : folders)
        checker.result(f);
    QVERIFY(QTest::qWaitFor([&] { return changed.count() == 4; }, 20000));
    QCOMPARE(runs(dir), 4);
    for (const QString &f : folders)
        QCOMPARE(checker.result(f).value(u"ok"_s).toBool(), true);
}

void TestPuppetChecker::aHelperThatCannotBeStartedEndsTheCheckAndTheQueueGoesOn()
{
    // Qt emits no `finished` for a process that does not start: the check ends with the error, and the next one is not left waiting for ever
    QTemporaryDir dir;
    testutil::writeFile(dir.filePath(u"mode"_s), "ok");
    PuppetChecker checker(dir.filePath(u"not-there/murasaki-puppet"_s), QString());
    checker.setSandboxMode([] { return PuppetChecker::Sandbox::Off; });
    QSignalSpy changed(&checker, &PuppetChecker::changed);
    const QStringList folders{folderIn(dir, u"a"_s), folderIn(dir, u"b"_s), folderIn(dir, u"c"_s)};
    for (const QString &f : folders)
        checker.result(f);
    QVERIFY(QTest::qWaitFor([&] { return changed.count() == 3; }, 15000));
    for (const QString &f : folders)
        QCOMPARE(checker.result(f).value(u"reasons"_s).toArray().at(0).toString(), u"puppet: check failed: the checker cannot be started"_s);
    // a file without the execute bit is the same
    const QString plain = dir.filePath(u"plain"_s);
    testutil::writeFile(plain, "#!/bin/sh\n");
    PuppetChecker other(plain, QString());
    other.setSandboxMode([] { return PuppetChecker::Sandbox::Off; });
    QSignalSpy otherChanged(&other, &PuppetChecker::changed);
    other.result(folders.at(0));
    QVERIFY(QTest::qWaitFor([&] { return otherChanged.count() == 1; }, 15000));
    QCOMPARE(other.result(folders.at(0)).value(u"ok"_s).toBool(), false);
}

void TestPuppetChecker::anAnswerAboutAFolderThatChangedIsNotKept()
{
    QTemporaryDir dir;
    testutil::writeFile(dir.filePath(u"mode"_s), "ok");
    const QString folder = folderIn(dir);
    PuppetChecker checker(fakeHelper(dir), QString());
    checker.setSandboxMode([] { return PuppetChecker::Sandbox::Off; });
    QSignalSpy changed(&checker, &PuppetChecker::changed);
    checker.result(folder);
    QVERIFY(QTest::qWaitFor([&] { return runs(dir) >= 1; }, 15000)); // the check is running
    testutil::writeFile(folder + u"/model.inp"_s, "TRNSRTS2 and then the user replaced it");
    QVERIFY(checker.result(folder).isEmpty()); // (the signature moved on while the old check was running)
    QVERIFY(QTest::qWaitFor([&] { return changed.count() >= 1; }, 15000));
    // the old answer was dropped, not kept for the new folder; a check of the new one follows
    QVERIFY(QTest::qWaitFor([&] { return !checker.result(folder).isEmpty(); }, 15000));
    QVERIFY(runs(dir) >= 2);
}

QTEST_GUILESS_MAIN(TestPuppetChecker)
#include "tst_puppetchecker.moc"
