#include "testutil.h"
#include "workshopwatcher.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestWorkshopWatcher : public QObject
{
    Q_OBJECT

private slots:
    void coalescesBurstIntoOneSignal();
    void keepsWatchingAtomicallyReplacedFile();
    void picksUpPathThatAppearsLater();
    void recoversFileRecreatedAfterDebounce();
    void maxWaitEmitsDuringContinuousStream();
};

void TestWorkshopWatcher::coalescesBurstIntoOneSignal()
{
    QTemporaryDir tmp;
    WorkshopWatcher watcher;
    watcher.setDebounceInterval(100);
    watcher.setPaths({tmp.path()});
    QSignalSpy spy(&watcher, &WorkshopWatcher::changed);

    for (int i = 0; i < 5; ++i)
        QVERIFY(QDir(tmp.path()).mkdir(QString::number(i)));

    QVERIFY(spy.wait(3000));
    QTest::qWait(300);
    QCOMPARE(spy.count(), 1);
}

void TestWorkshopWatcher::keepsWatchingAtomicallyReplacedFile()
{
    QTemporaryDir tmp;
    const QString manifest = tmp.filePath(u"appworkshop_431960.acf"_s);
    testutil::writeFile(manifest, "v1");
    WorkshopWatcher watcher;
    watcher.setDebounceInterval(50);
    watcher.setPaths({manifest});
    QSignalSpy spy(&watcher, &WorkshopWatcher::changed);

    // Steam-style atomic replace: write a temp file, then rename it over the original.
    testutil::writeFile(manifest + u".tmp"_s, "v2");
    QVERIFY(QFile::remove(manifest));
    QVERIFY(QFile::rename(manifest + u".tmp"_s, manifest));
    QVERIFY(spy.wait(3000));

    QFile file(manifest);
    QVERIFY(file.open(QIODevice::Append));
    file.write("v3");
    file.close();
    QVERIFY(spy.wait(3000));
    QCOMPARE(spy.count(), 2);
}

void TestWorkshopWatcher::picksUpPathThatAppearsLater()
{
    QTemporaryDir tmp;
    const QString later = tmp.filePath(u"workshop"_s);
    WorkshopWatcher watcher;
    watcher.setDebounceInterval(50);
    watcher.setRetryInterval(100);
    watcher.setPaths({later});
    QSignalSpy spy(&watcher, &WorkshopWatcher::changed);

    QVERIFY(QDir(tmp.path()).mkdir(u"workshop"_s));
    QVERIFY(spy.wait(5000));

    // The new dir must now be watched for real.
    const int before = spy.count();
    QVERIFY(QDir(later).mkdir(u"1"_s));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() > before, 5000);
}

void TestWorkshopWatcher::recoversFileRecreatedAfterDebounce()
{
    QTemporaryDir tmp;
    const QString manifest = tmp.filePath(u"appworkshop_431960.acf"_s);
    testutil::writeFile(manifest, "v1");
    WorkshopWatcher watcher;
    watcher.setDebounceInterval(50);
    watcher.setRetryInterval(100);
    watcher.setPaths({manifest});
    QSignalSpy spy(&watcher, &WorkshopWatcher::changed);

    QVERIFY(QFile::remove(manifest));
    QVERIFY(spy.wait(5000)); // debounce fires while the file is absent
    QTest::qWait(300); // stay absent for several debounce/retry intervals
    testutil::writeFile(manifest, "v2");
    const int afterRecreate = spy.count();
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() > afterRecreate, 5000);

    QTest::qWait(300);
    const int before = spy.count();
    QFile file(manifest);
    QVERIFY(file.open(QIODevice::Append));
    file.write("v3");
    file.close();
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() > before, 5000);
}

void TestWorkshopWatcher::maxWaitEmitsDuringContinuousStream()
{
    QTemporaryDir tmp;
    WorkshopWatcher watcher;
    watcher.setDebounceInterval(200);
    watcher.setMaxWait(400);
    watcher.setPaths({tmp.path()});
    QSignalSpy spy(&watcher, &WorkshopWatcher::changed);

    for (int i = 0; i < 20; ++i) {
        QVERIFY(QDir(tmp.path()).mkdir(QString::number(i)));
        QTest::qWait(50);
    }
    // The stream was still running when the cap should have fired.
    QVERIFY(spy.count() >= 1);
}

QTEST_GUILESS_MAIN(TestWorkshopWatcher)
#include "tst_workshopwatcher.moc"
