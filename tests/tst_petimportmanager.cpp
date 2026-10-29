// The daemon's import queue (petimportmanager.h): the real helper with and without the sandbox, and fake helpers for the failure paths.

#include "petimportmanager.h"
#include "petimport.h"

#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace murasaki;
using namespace murasaki::import;

namespace {

QString nameOf(std::string_view text)
{
    return QString::fromUtf8(text.data(), qsizetype(text.size()));
}

QJsonObject firstJob(const PetImportManager &m)
{
    return m.jobs().first().toObject();
}

} // namespace

class TstPetImportManager : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString path(const QString &name) const { return m_dir.filePath(name); }
    QString m_sheet;
    QByteArray spec() const { return QByteArray(R"j({"kind":"sheet","name":"S","frameWidth":4,"frameHeight":4,"frameCount":4,"sources":[")j") + m_sheet.toUtf8() + "\"]}"; }

    QString script(const QString &name, const QString &body)
    {
        const QString file = path(name);
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly))
            return {};
        f.write("#!/bin/sh\n" + body.toUtf8());
        f.close();
        QFile::setPermissions(file, QFile::permissions(file) | QFileDevice::ExeOwner);
        return file;
    }
    static bool waitFor(PetImportManager &m, const QString &state)
    {
        return QTest::qWaitFor([&] { return !m.jobs().isEmpty() && m.jobs().last().toObject().value("state").toString() == state; }, 20000);
    }
    bool sandboxWorks() const
    {
        QProcess p;
        p.start(QStringLiteral(MURASAKI_SANDBOX_BIN), {"--report"});
        if (!p.waitForFinished(5000))
            return false;
        // the import profile under --require also needs a mount namespace (bwrap), which some systems refuse
        const QJsonObject report = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
        return report.value("landlockAbi").toInt() > 0 && report.value("unixSocketsHidden").toBool();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QImage sheet(16, 4, QImage::Format_RGBA8888);
        sheet.fill(qRgb(10, 200, 30));
        m_sheet = path("sheet.png");
        QVERIFY(sheet.save(m_sheet, "PNG"));
        QVERIFY(QDir().mkpath(path("out")));
    }

    void theRealHelperImportsWithoutTheSandbox()
    {
        PetImportManager m(path("out"));
        m.setHelper(QStringLiteral(MURASAKI_PETIMPORT_BIN), {}, false);
        QSignalSpy done(&m, &PetImportManager::done);
        QString error;
        const QString id = m.submit(spec(), &error);
        QVERIFY2(!id.isEmpty(), qPrintable(error));
        QVERIFY(waitFor(m, "done"));
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.first().at(0).toString(), id);
        QVERIFY(QFileInfo::exists(done.first().at(1).toString()));
        const QJsonObject job = firstJob(m);
        QCOMPARE(job.value("frames").toInt(), 4);
        QCOMPARE(job.value("progress").toDouble(), 1.0);
        QVERIFY(!QFileInfo::exists(path("out/.specs/") + id + ".json"));
        m.setItemId(id, "local:abc");
        QCOMPARE(firstJob(m).value("itemId").toString(), QStringLiteral("local:abc"));
    }
    void theRealHelperImportsBehindTheSandbox()
    {
        if (!sandboxWorks())
            QSKIP("Landlock or a mount namespace (bwrap) is not available here");
        PetImportManager m(path("out2"));
        QVERIFY(QDir().mkpath(path("out2")));
        m.setHelper(QStringLiteral(MURASAKI_PETIMPORT_BIN), QStringLiteral(MURASAKI_SANDBOX_BIN), true);
        QString error;
        QVERIFY2(!m.submit(spec(), &error).isEmpty(), qPrintable(error));
        QVERIFY(waitFor(m, "done"));
        QVERIFY2(firstJob(m).value("error").toString().isEmpty(), qPrintable(firstJob(m).value("error").toString()));
    }
    void badSpecsAreRefusedAtSubmit()
    {
        PetImportManager m(path("out"));
        QString error;
        QVERIFY(m.submit("not json", &error).isEmpty());
        QVERIFY(error.startsWith(nameOf(kSpecRefused)));
        QVERIFY(m.jobs().isEmpty());
        QString previewError;
        QVERIFY(m.preview("{}", &previewError).isEmpty());
        QVERIFY(previewError.startsWith(nameOf(kSpecRefused)));
    }
    void aMissingHelperFailsTheJobByName()
    {
        PetImportManager m(path("out"));
        m.setHelper(path("nope"), {}, false);
        QString error;
        m.submit(spec(), &error);
        QVERIFY(waitFor(m, "failed"));
        QVERIFY(firstJob(m).value("error").toString().startsWith(nameOf(kHelperFailed)));
        PetImportManager strict(path("out"));
        strict.setHelper(QStringLiteral(MURASAKI_PETIMPORT_BIN), {}, true);
        strict.submit(spec(), &error);
        QVERIFY(waitFor(strict, "failed"));
        QVERIFY(firstJob(strict).value("error").toString().startsWith(nameOf(kSandboxUnavailable)));
    }
    void jobsRunOneAtATimeInOrderAndCanBeCancelled()
    {
        const QString slow = script("slow.sh", "echo '{\"progress\":0.1,\"frames\":1}'\ntrap 'echo \"{\\\"error\\\":\\\"cancelled\\\"}\"; exit 1' TERM\nsleep 5 &\nwait\necho '{\"done\":true,\"file\":\"/nonexistent\",\"frames\":1}'\n");
        PetImportManager m(path("out"));
        m.setHelper(slow, {}, false);
        QString error;
        const QString a = m.submit(spec(), &error), b = m.submit(spec(), &error), c = m.submit(spec(), &error);
        QVERIFY(!a.isEmpty() && !b.isEmpty() && !c.isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return m.job(a).value("state").toString() == "running"; }, 5000));
        QCOMPARE(m.job(b).value("state").toString(), QStringLiteral("queued"));
        QVERIFY(m.cancel(c)); // queued: dropped at once
        QCOMPARE(m.job(c).value("state").toString(), QStringLiteral("cancelled"));
        QVERIFY(m.cancel(a)); // running: SIGTERM
        QVERIFY(QTest::qWaitFor([&] { return m.job(a).value("state").toString() == "cancelled"; }, 10000));
        QVERIFY(QTest::qWaitFor([&] { return m.job(b).value("state").toString() == "running"; }, 5000));
        QVERIFY(m.cancel(b));
        QVERIFY(QTest::qWaitFor([&] { return m.job(b).value("state").toString() == "cancelled"; }, 10000));
        QVERIFY(!m.cancel("imp-unknown"));
    }
    void aHelperThatDiesIsAFailureNotAHang()
    {
        const QString dies = script("dies.sh", "echo '{\"progress\":0.5,\"frames\":2}'\nkill -9 $$\n");
        PetImportManager m(path("out"));
        m.setHelper(dies, {}, false);
        QString error;
        m.submit(spec(), &error);
        QVERIFY(waitFor(m, "failed"));
        QVERIFY(firstJob(m).value("error").toString().startsWith(nameOf(kHelperFailed)));
    }
    void aWallClockLimitStopsAHelper()
    {
        const QString hang = script("hang.sh", "sleep 30\n");
        PetImportManager m(path("out"));
        m.setHelper(hang, {}, false);
        m.setWallClockMs(300);
        QString error;
        m.submit(spec(), &error);
        QVERIFY(QTest::qWaitFor([&] { return m.jobs().last().toObject().value("state").toString() == "failed"; }, 15000));
    }
    void progressIsThrottled()
    {
        QString lines;
        for (int i = 1; i <= 200; ++i)
            lines += QStringLiteral("echo '{\"progress\":%1,\"frames\":%2}'\n").arg(i / 200.0).arg(i);
        const QString chatty = script("chatty.sh", lines + "echo '{\"done\":true,\"file\":\"/none\",\"frames\":200}'\n");
        PetImportManager m(path("out"));
        m.setHelper(chatty, {}, false);
        QSignalSpy changed(&m, &PetImportManager::changed);
        QString error;
        m.submit(spec(), &error);
        QVERIFY(waitFor(m, "done"));
        QVERIFY2(changed.count() < 12, qPrintable(QString::number(changed.count())));
    }
    void aQueueFullOfWaitingJobsRefusesMore()
    {
        const QString slow = script("slow2.sh", "sleep 5\n");
        PetImportManager m(path("out"));
        m.setHelper(slow, {}, false);
        QString error;
        for (int i = 0; i < PetImportManager::kMaxQueued; ++i)
            QVERIFY(!m.submit(spec(), &error).isEmpty());
        QVERIFY(m.submit(spec(), &error).isEmpty());
        QVERIFY(error.startsWith(nameOf(kSpecRefused)));
    }
    void previewAsksTheHelperForHeadersOnly()
    {
        PetImportManager m(path("out"));
        m.setHelper(QStringLiteral(MURASAKI_PETIMPORT_BIN), {}, false);
        QString error;
        const QJsonObject ok = QJsonDocument::fromJson(m.preview(spec(), &error).toUtf8()).object();
        QVERIFY(ok.value("ok").toBool());
        QCOMPARE(ok.value("preview").toObject().value("frames").toInt(), 4);
        const QByteArray bad = QByteArray(R"j({"kind":"sheet","sources":["/nonexistent/x.png"]})j");
        const QJsonObject refused = QJsonDocument::fromJson(m.preview(bad, &error).toUtf8()).object();
        QVERIFY(!refused.value("ok").toBool());
        QVERIFY(refused.value("error").toString().startsWith(nameOf(kSourceNotFound)));
        QVERIFY(m.jobs().isEmpty()); // a preview is not a job
    }
    void previewsRunOneAtATimeShareAnswersAndAreBounded()
    {
        const QString slow = script("slowpreview.sh", "sleep 0.4\necho '{\"preview\":{\"frames\":2,\"width\":1,\"height\":1,\"totalDelayMs\":10,\"estimatedBytes\":8,\"degraded\":[]}}'\n");
        PetImportManager m(path("out"));
        m.setHelper(slow, {}, false);
        QStringList answers;
        QString error;
        const auto ask = [&](const QString &source) {
            return m.previewAsync(QByteArray("{\"kind\":\"gif\",\"sources\":[\"") + source.toUtf8() + "\"]}", [&](const QString &a) { answers.append(a); }, &error);
        };
        QVERIFY(ask("/a.gif"));
        QVERIFY(ask("/a.gif")); // shares the first
        QVERIFY(ask("/b.gif"));
        QCOMPARE(answers.size(), 0); // nothing blocked, nothing answered yet
        QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 3, 8000);
        QVERIFY(answers.at(0).contains("\"ok\":true"));
        // beyond the bound the answer is an immediate "busy"
        answers.clear();
        for (int i = 0; i < PetImportManager::kMaxPendingPreviews; ++i)
            QVERIFY(ask(QStringLiteral("/p%1.gif").arg(i)));
        QVERIFY(ask("/one-too-many.gif"));
        QCOMPARE(answers.size(), 1);
        QVERIFY(answers.first().contains("busy"));
        QVERIFY(!m.previewAsync("{}", [](const QString &) {}, &error));
        QVERIFY(error.startsWith(nameOf(kSpecRefused)));
        QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 1 + PetImportManager::kMaxPendingPreviews, 20000);
    }
    void aJobRunWithoutTheSandboxSaysSoAndTheWallClockHasAName()
    {
        const QString ok = script("ok.sh", "echo '{\"done\":true,\"file\":\"/none\",\"frames\":1}'\n");
        PetImportManager m(path("out"));
        m.setHelper(ok, {}, false); // no sandbox helper at all, and not required
        QString error;
        m.submit(spec(), &error);
        QVERIFY(waitFor(m, "done"));
        QVERIFY(firstJob(m).value("degraded").toArray().contains(QJsonValue(nameOf(kNotSandboxed))));
        // a sandbox that runs the program unconfined (the Landlock fallback) says so on stderr
        const QString fake = script("fake-sandbox.sh", "echo 'murasaki-sandbox: running x unconfined: no landlock' >&2\nwhile [ \"$1\" != -- ]; do shift; done\nshift\nexec \"$@\"\n");
        PetImportManager n(path("out"));
        n.setHelper(ok, fake, false);
        n.submit(spec(), &error);
        QVERIFY(waitFor(n, "done"));
        QVERIFY(firstJob(n).value("degraded").toArray().contains(QJsonValue(nameOf(kNotSandboxed))));
        // required: the sandbox's own refusal (status 125) is "sandbox unavailable"
        const QString refuses = script("refusing-sandbox.sh", "echo 'murasaki-sandbox: cannot sandbox: no landlock' >&2\nexit 125\n");
        PetImportManager r(path("out"));
        r.setHelper(ok, refuses, true);
        r.submit(spec(), &error);
        QVERIFY(waitFor(r, "failed"));
        QVERIFY(firstJob(r).value("error").toString().startsWith(nameOf(kSandboxUnavailable)));
        // the wall clock
        const QString hang = script("hang2.sh", "sleep 30\n");
        PetImportManager w(path("out"));
        w.setHelper(hang, {}, false);
        w.setWallClockMs(300);
        w.submit(spec(), &error);
        QVERIFY(waitFor(w, "failed"));
        QVERIFY(firstJob(w).value("error").toString().startsWith(nameOf(kTimeBudget)));
    }
    void aFinishedJobCanBeCancelledWithoutFuss()
    {
        const QString ok = script("ok2.sh", "echo '{\"done\":true,\"file\":\"/none\",\"frames\":1}'\n");
        PetImportManager m(path("out"));
        m.setHelper(ok, {}, false);
        QString error;
        const QString id = m.submit(spec(), &error);
        QVERIFY(waitFor(m, "done"));
        QVERIFY(m.cancel(id));
        QVERIFY(!m.cancel("imp-unknown"));
        m.markFailed(id, "helper failed: test");
        QCOMPARE(m.job(id).value("state").toString(), QStringLiteral("failed"));
    }
    void staleFoldersAreCleanedAtStart()
    {
        QVERIFY(QDir().mkpath(path("out/dead.part")));
        QVERIFY(QDir().mkpath(path("out/.specs")));
        QFile f(path("out/.specs/old.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        QFile lock(path("out/.deadbeef.lock"));
        QVERIFY(lock.open(QIODevice::WriteOnly));
        lock.close();
        PetImportManager m(path("out"));
        m.cleanStale();
        QVERIFY(!QFileInfo::exists(path("out/.deadbeef.lock")));
        QVERIFY(!QFileInfo::exists(path("out/dead.part")));
        QVERIFY(!QFileInfo::exists(path("out/.specs/old.json")));
    }
};

QTEST_GUILESS_MAIN(TstPetImportManager)
#include "tst_petimportmanager.moc"
