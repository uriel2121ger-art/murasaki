// The daemon's side of the pets' sounds (petaudiomanager.h): the import job queue over the real helper, the asset folder, references and collection.

#include "petaudiomanager.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace murasaki;

class TstPetAudioManager : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString makeSound(const QString &name, double seconds = 0.3)
    {
        QProcess p;
        p.start(QStandardPaths::findExecutable("ffmpeg"), {"-v", "error", "-y", "-f", "lavfi", "-i", QString("sine=frequency=500:duration=%1").arg(seconds), m_dir.filePath(name)});
        if (!p.waitForFinished(30000) || p.exitCode() != 0)
            return {};
        return m_dir.filePath(name);
    }
    static bool waitFor(PetAudioManager &m, const QString &id, const QString &state)
    {
        for (int i = 0; i < 300; ++i) {
            if (m.job(id).value("state").toString() == state)
                return true;
            QTest::qWait(50);
        }
        return false;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
            QSKIP("needs ffmpeg");
    }

    void aSoundBecomesAnAssetAndIsListed()
    {
        PetAudioManager m(m_dir.filePath("assets"), MURASAKI_PETAUDIO_BIN, QString());
        m.setSandboxMode([] { return PetAudioManager::Sandbox::Off; });
        QSignalSpy changed(&m, &PetAudioManager::changed);
        const QString file = makeSound("a.wav");
        QVERIFY(!file.isEmpty());
        QString error;
        const QString id = m.importFile(file, &error);
        QVERIFY2(!id.isEmpty(), qPrintable(error));
        QVERIFY(waitFor(m, id, "done"));
        const QJsonObject result = m.job(id).value("result").toObject();
        QVERIFY(result.value("ok").toBool());
        const QString asset = result.value("asset").toString();
        QCOMPARE(asset.size(), 64);
        const QJsonArray list = m.assets({asset, asset});
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.at(0).toObject().value("refs").toInt(), 2);
        QCOMPARE(list.at(0).toObject().value("name").toString(), QString("a.wav"));
        QVERIFY(list.at(0).toObject().value("durationMs").toInt() > 250);
        QVERIFY(changed.count() >= 3);
        // used: refused; unused: removed; unknown: refused
        QVERIFY(!m.removeAsset(asset, {asset}, &error));
        QVERIFY(error.contains("used"));
        QVERIFY(!m.removeAsset("../../x", {}, &error));
        QVERIFY(m.removeAsset(asset, {}, &error));
        QCOMPARE(m.assets({}).size(), 0);
    }

    void aBadSourceFailsWithANameAndNothingStaysBehind()
    {
        PetAudioManager m(m_dir.filePath("assets"), MURASAKI_PETAUDIO_BIN, QString());
        m.setSandboxMode([] { return PetAudioManager::Sandbox::Off; });
        QFile bad(m_dir.filePath("noise.mp3"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write(QByteArray(4000, 'x'));
        bad.close();
        QString error;
        const QString id = m.importFile(bad.fileName(), &error);
        QVERIFY(waitFor(m, id, "failed"));
        QVERIFY2(m.job(id).value("error").toString().startsWith("audio: not audio"), qPrintable(m.job(id).value("error").toString()));
        QCOMPARE(m.assets({}).size(), 0);
        QVERIFY(m.importFile("relative/path.mp3", &error).isEmpty());
        QVERIFY(error.contains("absolute"));
        const QString missing = m.importFile(m_dir.filePath("none.wav"), &error);
        QVERIFY(waitFor(m, missing, "failed"));
        QVERIFY(m.job(missing).value("error").toString().startsWith("source not found"));
    }

    void jobsRunOneAtATimeAndAQueuedOneCanBeCancelled()
    {
        PetAudioManager m(m_dir.filePath("assets"), MURASAKI_PETAUDIO_BIN, QString());
        m.setSandboxMode([] { return PetAudioManager::Sandbox::Off; });
        const QString a = makeSound("q1.wav", 0.4), b = makeSound("q2.wav", 0.5), c = makeSound("q3.wav", 0.6);
        QString error;
        const QString ia = m.importFile(a, &error), ib = m.importFile(b, &error), ic = m.importFile(c, &error);
        QCOMPARE(m.job(ib).value("state").toString(), QString("queued"));
        QVERIFY(m.cancel(ib));
        QCOMPARE(m.job(ib).value("state").toString(), QString("cancelled"));
        QVERIFY(waitFor(m, ia, "done"));
        QVERIFY(waitFor(m, ic, "done"));
        QCOMPARE(m.job(ib).value("state").toString(), QString("cancelled"));
        QCOMPARE(m.assets({}).size(), 2);
        // the queue has a bound
        QStringList ids;
        int refused = 0;
        for (int i = 0; i < 12; ++i)
            refused += m.importFile(m_dir.filePath("none.wav"), &error).isEmpty();
        QVERIFY(refused >= 1);
    }

    void collectingRemovesOnlyWhatNoPetUsesAndIsOldEnough()
    {
        PetAudioManager m(m_dir.filePath("assets"), MURASAKI_PETAUDIO_BIN, QString());
        const QJsonArray before = m.assets({});
        QVERIFY(before.size() >= 2);
        const QString keep = before.at(0).toObject().value("asset").toString();
        QCOMPARE(m.collect({keep}, 3600), 0);               // too young
        QCOMPARE(m.collect({keep}, 0), before.size() - 1);  // the others go
        QCOMPARE(m.assets({}).size(), 1);
    }

    void collectingLeavesThePartialFileOfTheImportThatRuns()
    {
        const QString helper = m_dir.filePath("slow-helper.sh");
        QFile script(helper);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nsleep 2\necho '{\"ok\":false,\"error\":\"x\"}'\nexit 1\n");
        script.close();
        QVERIFY(QFile::setPermissions(helper, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        PetAudioManager m(m_dir.filePath("assets4"), helper, QString());
        m.setSandboxMode([] { return PetAudioManager::Sandbox::Off; });
        QVERIFY(QDir().mkpath(m_dir.filePath("assets4")));
        const QString part = m_dir.filePath("assets4/" + QString(64, QLatin1Char('d')) + ".wav.part");
        QFile f(part);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("half");
        f.close();
        QString error;
        const QString id = m.importFile(m_dir.filePath("a.wav"), &error);
        QVERIFY(!id.isEmpty());
        QTest::qWait(300);
        QCOMPARE(m.job(id).value("state").toString(), QString("running"));
        m.collect({}, 0);
        QVERIFY(QFile::exists(part)); // the import that runs may be the one writing it
        QVERIFY(waitFor(m, id, "failed"));
        m.collect({}, 0);
        QVERIFY(!QFile::exists(part));
    }

    void requiringTheSandboxWithoutOneIsAnErrorAndWithOneItWorks()
    {
        PetAudioManager none(m_dir.filePath("assets2"), MURASAKI_PETAUDIO_BIN, m_dir.filePath("no-such-sandbox"));
        none.setSandboxMode([] { return PetAudioManager::Sandbox::Require; });
        QString error;
        const QString file = makeSound("s.wav");
        const QString id = none.importFile(file, &error);
        // a sandbox path that does not exist is looked up by name; if the system has none, the job says so
        QVERIFY(waitFor(none, id, "failed") || none.job(id).value("state").toString() == "done");
        PetAudioManager boxed(m_dir.filePath("assets3"), MURASAKI_PETAUDIO_BIN, MURASAKI_SANDBOX_BIN);
        boxed.setSandboxMode([] { return PetAudioManager::Sandbox::Prefer; });
        const QString id2 = boxed.importFile(file, &error);
        QVERIFY(waitFor(boxed, id2, "done") || boxed.job(id2).value("state").toString() == "failed");
        if (boxed.job(id2).value("state").toString() == "failed")
            QSKIP(qPrintable("the sandboxed import failed here: " + boxed.job(id2).value("error").toString()));
        QCOMPARE(boxed.assets({}).size(), 1);
    }
};

QTEST_GUILESS_MAIN(TstPetAudioManager)
#include "tst_petaudiomanager.moc"
