// The store of reference pictures (screenrefstore.h): where it lives (XDG_DATA_HOME), what it accepts, atomic writes, ids that cannot name a path, deletion with the trigger, and
// taking the pictures of an import.

#include "screenrefstore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <unistd.h>

using namespace murasaki;

namespace {

// A PNG signature and IHDR for a picture of w x h (the store reads the header only), then padding bytes.
QByteArray fakePng(int w, int h, int extra = 20)
{
    QByteArray out("\x89PNG\r\n\x1a\n", 8);
    QByteArray len(4, 0), width(4, 0), height(4, 0);
    qToBigEndian<quint32>(13, len.data());
    qToBigEndian<quint32>(quint32(w), width.data());
    qToBigEndian<quint32>(quint32(h), height.data());
    out += len + "IHDR" + width + height + QByteArray("\x08\x06\x00\x00\x00", 5) + QByteArray(4, 0);
    return out + QByteArray(extra, 'x');
}

} // namespace

class TstScreenRefStore : public QObject
{
    Q_OBJECT

private slots:
    void theDefaultRootFollowsXdgDataHome()
    {
        QTemporaryDir dir;
        qputenv("XDG_DATA_HOME", dir.path().toUtf8());
        QCOMPARE(ScreenRefStore::defaultRoot(), dir.path() + "/murasaki/triggers");
        ScreenRefStore store;
        QCOMPARE(store.root(), dir.path() + "/murasaki/triggers");
        QVERIFY(store.putFrame("t1", 0, 0, fakePng(10, 10)));
        QVERIFY(QFile::exists(dir.path() + "/murasaki/triggers/t1/region0/step0.png"));
        qunsetenv("XDG_DATA_HOME");
    }

    void framesAreStoredReadBackAndListed()
    {
        QTemporaryDir dir;
        ScreenRefStore store(dir.path() + "/triggers");
        const QByteArray a = fakePng(360, 203), b = fakePng(40, 30, 99);
        QVERIFY(store.putFrame("trigger-1", 0, 0, a));
        QVERIFY(store.putFrame("trigger-1", 0, 2, b));
        QVERIFY(store.putFrame("trigger-1", 3, 0, b));
        QCOMPARE(store.frame("trigger-1", 0, 0), a);
        QCOMPARE(store.frame("trigger-1", 0, 2), b);
        QVERIFY(store.frame("trigger-1", 0, 1).isEmpty());
        QCOMPARE(store.steps("trigger-1", 0), (QList<int>{0, 2}));
        QCOMPARE(store.regions("trigger-1"), (QList<int>{0, 3}));
        QCOMPARE(store.triggerIds(), (QStringList{"trigger-1"}));
        QVERIFY(store.putFrame("trigger-1", 0, 0, b)); // replaced
        QCOMPARE(store.frame("trigger-1", 0, 0), b);
        QCOMPARE(QDir(dir.path() + "/triggers/trigger-1/region0").entryList(QDir::Files).size(), 2); // no temporary file left
        QFile::Permissions p = QFileInfo(dir.path() + "/triggers/trigger-1").permissions();
        QVERIFY(!(p & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther)));
    }

    void idsThatCouldNameAPathAreRefused_data()
    {
        QTest::addColumn<QString>("id");
        for (const char *id : {"", ".", "..", "../x", "a/b", "/abs", "a\\b", "a b", "a.b", "..%2f", "x\n", "é", "-ok/../.."})
            QTest::newRow(id[0] ? id : "empty") << QString::fromUtf8(id);
        QTest::newRow("65 chars") << QString(65, 'a');
        QTest::newRow("nul") << QString(QStringLiteral("a") + QChar(0) + QStringLiteral("b"));
    }
    void idsThatCouldNameAPathAreRefused()
    {
        QFETCH(QString, id);
        QTemporaryDir dir;
        ScreenRefStore store(dir.path() + "/triggers");
        QVERIFY(!ScreenRefStore::validTriggerId(id));
        QString error;
        QVERIFY(!store.putFrame(id, 0, 0, fakePng(5, 5), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!store.putInfo(id, {}));
        QVERIFY(store.frame(id, 0, 0).isEmpty());
        QVERIFY(store.triggerFolder(id).isEmpty());
        QVERIFY(!store.removeTrigger(id));
        QVERIFY(!QDir(dir.path() + "/triggers").exists()); // nothing was created, nothing outside either
        QVERIFY(QDir(dir.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
    }

    void validIdsAreAccepted()
    {
        for (const char *id : {"a", "0", "A-b_9", "3f2a9c1e-77aa-4b10-9d0e-0123456789ab", "_", "-"})
            QVERIFY(ScreenRefStore::validTriggerId(QString::fromLatin1(id)));
        QVERIFY(ScreenRefStore::validTriggerId(QString(64, 'a')));
    }

    void picturesAndPositionsAreChecked()
    {
        QTemporaryDir dir;
        ScreenRefStore store(dir.path() + "/triggers");
        QString error;
        QVERIFY(!store.putFrame("t", -1, 0, fakePng(5, 5), &error));
        QVERIFY(!store.putFrame("t", 64, 0, fakePng(5, 5), &error));
        QVERIFY(!store.putFrame("t", 0, -1, fakePng(5, 5), &error));
        QVERIFY(!store.putFrame("t", 0, 64, fakePng(5, 5), &error));
        QVERIFY(store.putFrame("t", 63, 63, fakePng(5, 5)));
        QVERIFY(!store.putFrame("t", 0, 0, QByteArray(), &error));
        QVERIFY(!store.putFrame("t", 0, 0, QByteArray("GIF89a plenty of bytes to pass the length check....."), &error));
        QVERIFY(!store.putFrame("t", 0, 0, fakePng(361, 10), &error));   // over 360 on a side
        QVERIFY(!store.putFrame("t", 0, 0, fakePng(10, 361), &error));
        QVERIFY(store.putFrame("t", 0, 0, fakePng(360, 360)));
        QVERIFY(!store.putFrame("t", 0, 0, fakePng(0, 10), &error));
        QVERIFY(!store.putFrame("t", 0, 0, fakePng(10, 10, 5 << 20), &error)); // over 4 MiB
        QVERIFY(!store.putFrame("t", 0, 0, fakePng(1 << 30, 1 << 30), &error));
        QByteArray wrongChunk = fakePng(10, 10);
        wrongChunk.replace(12, 4, "IDAT");
        QVERIFY(!store.putFrame("t", 0, 0, wrongChunk, &error));
        QVERIFY(store.frame("t", 64, 0).isEmpty());
        QVERIFY(store.steps("t", 99).isEmpty());
    }

    void aFailedReplacementKeepsTheOldPicture()
    {
        QTemporaryDir dir;
        ScreenRefStore store(dir.path() + "/triggers");
        const QByteArray good = fakePng(30, 30);
        QVERIFY(store.putFrame("t", 0, 0, good));
        QVERIFY(!store.putFrame("t", 0, 0, QByteArray("junk")));
        QCOMPARE(store.frame("t", 0, 0), good);
    }

    void removingATriggerRemovesItsFolderAndOnlyThat()
    {
        QTemporaryDir dir;
        ScreenRefStore store(dir.path() + "/triggers");
        QVERIFY(store.putFrame("keep", 0, 0, fakePng(5, 5)));
        QVERIFY(store.putFrame("drop", 0, 0, fakePng(5, 5)));
        QVERIFY(store.putFrame("drop", 2, 1, fakePng(5, 5)));
        QVERIFY(store.putInfo("drop", QJsonObject{{"x", 1}}));
        QCOMPARE(store.info("drop").value("x").toInt(), 1);
        QVERIFY(store.removeTrigger("drop"));
        QVERIFY(!QDir(dir.path() + "/triggers/drop").exists());
        QVERIFY(store.frame("drop", 0, 0).isEmpty());
        QVERIFY(store.info("drop").isEmpty());
        QCOMPARE(store.triggerIds(), (QStringList{"keep"}));
        QVERIFY(!store.frame("keep", 0, 0).isEmpty());
        QVERIFY(store.removeTrigger("drop"));      // already gone
        QVERIFY(store.removeTrigger("never-was"));
    }

    void aLinkInsideTheStoreIsNeverFollowed()
    {
        QTemporaryDir dir;
        QDir().mkpath(dir.path() + "/outside");
        QFile secret(dir.path() + "/outside/secret.txt");
        QVERIFY(secret.open(QIODevice::WriteOnly));
        secret.write("keep me");
        secret.close();
        ScreenRefStore store(dir.path() + "/triggers");
        QVERIFY(store.putFrame("real", 0, 0, fakePng(5, 5)));
        // the trigger folder is a link to a folder outside the store
        QVERIFY(QFile::link(dir.path() + "/outside", dir.path() + "/triggers/linked"));
        QVERIFY(!store.putFrame("linked", 0, 0, fakePng(5, 5)));
        QVERIFY(store.frame("linked", 0, 0).isEmpty());
        QVERIFY(store.removeTrigger("linked")); // removes the link
        QVERIFY(QFile::exists(dir.path() + "/outside/secret.txt"));
        QVERIFY(!QFileInfo(dir.path() + "/triggers/linked").isSymLink());
        // a picture that is a link out
        QVERIFY(QFile::link(dir.path() + "/outside/secret.txt", dir.path() + "/triggers/real/region0/step1.png"));
        QVERIFY(store.frame("real", 0, 1).isEmpty());
        QCOMPARE(store.steps("real", 0), (QList<int>{0}));
    }

    void aWriteNeverGoesThroughALink()
    {
        QTemporaryDir dir;
        const QString secret = dir.path() + "/outside/secret.txt";
        QDir().mkpath(dir.path() + "/outside");
        const auto plant = [&] {
            QFile f(secret);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("keep me");
        };
        plant();
        ScreenRefStore store(dir.path() + "/triggers");
        QVERIFY(store.putFrame("t", 0, 0, fakePng(5, 5)));
        QVERIFY(store.putInfo("t", QJsonObject{{"a", 1}}));
        // step and info are links to a file outside; putting replaces the link, the file stays as it was
        QFile::remove(dir.path() + "/triggers/t/region0/step1.png");
        QVERIFY(QFile::link(secret, dir.path() + "/triggers/t/region0/step1.png"));
        QVERIFY(QFile::remove(dir.path() + "/triggers/t/info.json"));
        QVERIFY(QFile::link(secret, dir.path() + "/triggers/t/info.json"));
        QVERIFY(store.putFrame("t", 0, 1, fakePng(6, 6)));
        QVERIFY(store.putInfo("t", QJsonObject{{"b", 2}}));
        QFile check(secret);
        QVERIFY(check.open(QIODevice::ReadOnly));
        QCOMPARE(check.readAll(), QByteArray("keep me"));
        QVERIFY(!QFileInfo(dir.path() + "/triggers/t/region0/step1.png").isSymLink());
        QVERIFY(!QFileInfo(dir.path() + "/triggers/t/info.json").isSymLink());
        QCOMPARE(store.frame("t", 0, 1), fakePng(6, 6));
        QCOMPARE(store.info("t").value("b").toInt(), 2);
        // a dangling link too
        QFile::remove(dir.path() + "/triggers/t/region0/step2.png");
        QVERIFY(QFile::link(dir.path() + "/outside/not-there.txt", dir.path() + "/triggers/t/region0/step2.png"));
        QVERIFY(store.putFrame("t", 0, 2, fakePng(7, 7)));
        QVERIFY(!QFileInfo::exists(dir.path() + "/outside/not-there.txt"));
        QCOMPARE(store.frame("t", 0, 2), fakePng(7, 7));
        // a hard link: the other name keeps its bytes
        QFile::remove(dir.path() + "/triggers/t/region0/step3.png");
        QCOMPARE(::link(QFile::encodeName(secret).constData(), QFile::encodeName(dir.path() + "/triggers/t/region0/step3.png").constData()), 0);
        QVERIFY(store.putFrame("t", 0, 3, fakePng(8, 8)));
        check.close();
        QVERIFY(check.open(QIODevice::ReadOnly));
        QCOMPARE(check.readAll(), QByteArray("keep me"));
        QCOMPARE(store.frame("t", 0, 3), fakePng(8, 8));
        // the region folder is a link: refused
        QVERIFY(QFile::link(dir.path() + "/outside", dir.path() + "/triggers/t/region5"));
        QVERIFY(!store.putFrame("t", 5, 0, fakePng(5, 5)));
        QVERIFY(!QFile::exists(dir.path() + "/outside/step0.png"));
        // no temporary file is left behind
        for (const QString &name : QDir(dir.path() + "/triggers/t/region0").entryList(QDir::Files | QDir::Hidden))
            QVERIFY2(!name.startsWith(".tmp"), qPrintable(name));
    }

    void adoptingOverAnExistingTriggerIsAllOrNothing()
    {
        QTemporaryDir dir;
        const QString job = dir.path() + "/job";
        QDir().mkpath(job + "/screenrefs/region0");
        const auto write = [&](const QString &rel, const QByteArray &bytes) {
            QFile f(job + "/" + rel);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(bytes);
        };
        write("screenrefs/region0/step0.png", fakePng(30, 30, 3));
        const auto record = [](const QStringList &files) {
            QJsonArray frames;
            for (const QString &f : files)
                frames.append(QJsonObject{{"file", f}, {"clipIndex", 1}});
            return QJsonObject{{"regions", QJsonArray{QJsonObject{{"region", 0}, {"status", "ready"}, {"frames", frames}}}}};
        };
        ScreenRefStore store(dir.path() + "/triggers");
        // an old trigger with a step and a region the new import does not have
        QVERIFY(store.putFrame("t", 0, 0, fakePng(10, 10, 1)));
        QVERIFY(store.putFrame("t", 0, 7, fakePng(10, 10, 2)));
        QVERIFY(store.putFrame("t", 4, 0, fakePng(10, 10, 3)));
        QString error;
        // a refusal in the middle leaves every old picture as it was
        QCOMPARE(store.adoptImported("t", job, record({"screenrefs/region0/step0.png", "screenrefs/region0/missing.png"}), &error), 0);
        QCOMPARE(store.frame("t", 0, 0), fakePng(10, 10, 1));
        QCOMPARE(store.steps("t", 0), (QList<int>{0, 7}));
        QCOMPARE(store.regions("t"), (QList<int>{0, 4}));
        // a success replaces the lot: no stale step 7, no stale region 4
        QCOMPARE(store.adoptImported("t", job, record({"screenrefs/region0/step0.png"}), &error), 1);
        QCOMPARE(store.frame("t", 0, 0), fakePng(30, 30, 3));
        QCOMPARE(store.steps("t", 0), (QList<int>{0}));
        QCOMPARE(store.regions("t"), (QList<int>{0}));
        QCOMPARE(store.triggerIds(), (QStringList{"t"}));
        // no staging or old folder is left in the store
        QCOMPARE(QDir(dir.path() + "/triggers").entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden), (QStringList{"t"}));
    }

    void anImportIsAdoptedOrNotAtAll()
    {
        QTemporaryDir dir;
        const QString job = dir.path() + "/job";
        QDir().mkpath(job + "/screenrefs/region0");
        QDir().mkpath(job + "/screenrefs/region1");
        const auto write = [&](const QString &rel, const QByteArray &bytes) {
            QFile f(job + "/" + rel);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(bytes);
        };
        write("screenrefs/region0/step0.png", fakePng(100, 50));
        write("screenrefs/region0/step1.png", fakePng(100, 50, 40));
        write("screenrefs/region1/step0.png", fakePng(20, 20));
        const auto record = [](const QStringList &files) {
            QJsonArray frames;
            int clip = 0;
            for (const QString &f : files)
                frames.append(QJsonObject{{"file", f}, {"clipIndex", clip++}});
            return QJsonObject{{"regions", QJsonArray{QJsonObject{{"region", 0}, {"status", "ready"}, {"frames", frames}}}}};
        };
        ScreenRefStore store(dir.path() + "/triggers");
        QString error;
        QCOMPARE(store.adoptImported("t1", job, record({"screenrefs/region0/step0.png", "screenrefs/region0/step1.png"}), &error), 2);
        QCOMPARE(store.steps("t1", 0), (QList<int>{0, 1}));
        QCOMPARE(store.frame("t1", 0, 1), fakePng(100, 50, 40));
        QCOMPARE(store.info("t1").value("regions").toArray().at(0).toObject().value("frames").toArray().size(), 2);

        // a region that is not ready is skipped; a name that is not ours refuses the whole import and leaves no folder
        QJsonObject withBad = record({"screenrefs/region0/step0.png", "../../etc/passwd"});
        QCOMPARE(store.adoptImported("t2", job, withBad, &error), 0);
        QVERIFY(!error.isEmpty());
        QVERIFY(!QDir(dir.path() + "/triggers/t2").exists());
        for (const char *bad : {"screenrefs/region0/step0.png/../step1.png", "/etc/passwd", "screenrefs/region0/step00.png", "screenrefs/region99/step0.png", "screenrefs//region0/step0.png",
                                "screenrefs/region0/step0.png.txt", "screenrefs/region0/missing.png"}) {
            QCOMPARE(store.adoptImported("t3", job, record({bad}), &error), 0);
            QVERIFY2(!QDir(dir.path() + "/triggers/t3").exists(), bad);
        }
        // a file that is not a PNG, and one that is a link out
        write("screenrefs/region0/step5.png", QByteArray("not a png at all, just bytes............"));
        QCOMPARE(store.adoptImported("t4", job, record({"screenrefs/region0/step0.png", "screenrefs/region0/step5.png"}), &error), 0);
        QVERIFY(!QDir(dir.path() + "/triggers/t4").exists());
        QVERIFY(QFile::link(dir.path() + "/triggers/t1/region0/step0.png", job + "/screenrefs/region0/step6.png"));
        QCOMPARE(store.adoptImported("t5", job, record({"screenrefs/region0/step6.png"}), &error), 0);
        QVERIFY(!QDir(dir.path() + "/triggers/t5").exists());
        // an existing trigger is not removed by a refused adoption
        QCOMPARE(store.adoptImported("t1", job, withBad, &error), 0);
        QVERIFY(QDir(dir.path() + "/triggers/t1").exists());
        QVERIFY(!store.adoptImported("../x", job, record({}), &error));
        // not ready: nothing to keep, nothing created
        QJsonObject notReady{{"regions", QJsonArray{QJsonObject{{"region", 0}, {"status", "npy: truncated"}, {"frames", QJsonArray()}}}}};
        QCOMPARE(store.adoptImported("t6", job, notReady, &error), 0);
    }
};

QTEST_GUILESS_MAIN(TstScreenRefStore)
#include "tst_screenrefstore.moc"
