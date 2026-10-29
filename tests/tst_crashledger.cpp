// CrashLedger: markers left by a dead process become persistent crash counts of their file.

#include "crashledger.h"
#include "testutil.h"

#include <QDir>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TstCrashLedger : public QObject
{
    Q_OBJECT

private slots:
    void aMarkerIsNotACrashWhileTheProcessLives();
    void leftoverMarkersCountAndPersist();
    void aChangedFileStartsOver();
    void forgetAndPlayingClearTheCount();
    void garbageStateIsIgnored();
    void severalMarkersChargeNobody();
    void theRecordIsCapped();
    void aRecordedCrashPersistsAndBlocks();
    void crashesFarApartAreForgotten();
    void aCrashAfterALongRunStartsTheCountAgain();
    void aModelThatDiesEverySixtyOneSecondsIsBlocked();

private:
    static QStringList markers(const QString &dir) { return QDir(dir).entryList({u"opening-*.json"_s}, QDir::Files); }
};

void TstCrashLedger::aRecordedCrashPersistsAndBlocks()
{
    QTemporaryDir tmp;
    const QString file = tmp.filePath(u"p.inp"_s);
    testutil::writeFile(file, "model");
    const QString state = tmp.filePath(u"state"_s);
    {
        CrashLedger ledger(state);
        ledger.load();
        for (int i = 1; i < CrashLedger::kMaxCrashes; ++i) {
            ledger.recordCrash(file);
            QCOMPARE(ledger.crashes(file), i);
            QVERIFY(!ledger.blocked(file));
        }
        ledger.recordCrash(file);
        QVERIFY(ledger.blocked(file));
    }
    CrashLedger again(state); // (a new run: the count was written)
    again.load();
    QVERIFY(again.blocked(file));
    // another file is not charged; replacing the file starts over; forgetting clears
    const QString other = tmp.filePath(u"q.inp"_s);
    testutil::writeFile(other, "other");
    QCOMPARE(again.crashes(other), 0);
    testutil::writeFile(file, "a model of another size");
    QCOMPARE(again.crashes(file), 0);
    again.recordCrash(file);
    QCOMPARE(again.crashes(file), 1);
    again.forget(file);
    QCOMPARE(again.crashes(file), 0);
    // a file that does not exist can be charged too (its signature is "missing")
    again.recordCrash(tmp.filePath(u"gone.inp"_s));
    QCOMPARE(again.crashes(tmp.filePath(u"gone.inp"_s)), 1);
}

void TstCrashLedger::crashesFarApartAreForgotten()
{
    // an injected clock: three crashes in a week do not add up to a block, three in an hour do
    QTemporaryDir tmp;
    const QString file = tmp.filePath(u"p.inp"_s);
    testutil::writeFile(file, "model");
    const QString state = tmp.filePath(u"state"_s);
    const qint64 day = 24 * 60 * 60;
    qint64 now = 1'800'000'000;
    {
        CrashLedger ledger(state);
        ledger.load();
        ledger.recordCrash(file, false, now);
        ledger.recordCrash(file, false, now + 3600);        // an hour later: counts
        QCOMPARE(ledger.crashes(file), 2);
        ledger.recordCrash(file, false, now + 3600 + day);  // exactly a day after the last: still counts (the rule is "over a day")
        QCOMPARE(ledger.crashes(file), 3);
        QVERIFY(ledger.blocked(file));
    }
    CrashLedger spread(tmp.filePath(u"state2"_s));
    spread.load();
    spread.recordCrash(file, false, now);
    spread.recordCrash(file, false, now + day + 1);          // over a day since the last: the count starts again
    QCOMPARE(spread.crashes(file), 1);
    spread.recordCrash(file, false, now + 2 * day + 2);
    QCOMPARE(spread.crashes(file), 1);
    spread.recordCrash(file, false, now + 2 * day + 60);    // close to the last: counts
    QCOMPARE(spread.crashes(file), 2);
    QVERIFY(!spread.blocked(file));
    // the forgetting is kept across runs: a crash a week after a persisted one is the first again
    CrashLedger next(tmp.filePath(u"state2"_s));
    next.load();
    QCOMPARE(next.crashes(file), 2);
    next.recordCrash(file, false, now + 10 * day);
    QCOMPARE(next.crashes(file), 1);
}

void TstCrashLedger::aCrashAfterALongRunStartsTheCountAgain()
{
    QTemporaryDir tmp;
    const QString file = tmp.filePath(u"p.inp"_s);
    testutil::writeFile(file, "model");
    CrashLedger ledger(tmp.filePath(u"state"_s));
    ledger.load();
    const qint64 now = 1'800'000'000;
    ledger.recordCrash(file, false, now);
    ledger.recordCrash(file, false, now + 10);
    QCOMPARE(ledger.crashes(file), 2);
    ledger.recordCrash(file, /*afterLongRun=*/true, now + 20); // it had run for minutes: not the model breaking the engine
    QCOMPARE(ledger.crashes(file), 1);
    ledger.recordCrash(file, false, now + 30);
    ledger.recordCrash(file, false, now + 40);
    QVERIFY(ledger.blocked(file)); // three quick ones after it still block
    ledger.recordCrash(file, true, now + 50);
    QCOMPARE(ledger.crashes(file), 1);
    QVERIFY(!ledger.blocked(file));
}

void TstCrashLedger::aModelThatDiesEverySixtyOneSecondsIsBlocked()
{
    // surviving a minute is not a long run (the host reports a long run only after kEngineLongRunSeconds, 30 minutes): three such deaths block
    QTemporaryDir tmp;
    const QString file = tmp.filePath(u"p.inp"_s);
    testutil::writeFile(file, "model");
    CrashLedger ledger(tmp.filePath(u"state"_s));
    ledger.load();
    qint64 now = 1'800'000'000;
    for (int i = 0; i < CrashLedger::kMaxCrashes; ++i) {
        QVERIFY(!ledger.blocked(file));
        now += 61 + 1; // 61 s alive and the 250 ms restart
        ledger.recordCrash(file, /*afterLongRun=*/false, now);
    }
    QVERIFY(ledger.blocked(file));
}

void TstCrashLedger::aMarkerIsNotACrashWhileTheProcessLives()
{
    QTemporaryDir tmp;
    const QString file = tmp.filePath(u"a.apng"_s);
    testutil::writeFile(file, "x");
    CrashLedger ledger(tmp.filePath(u"state"_s));
    ledger.load();
    ledger.markOpening(u"p1"_s, file);
    QCOMPARE(markers(tmp.filePath(u"state"_s)).size(), 1);
    QCOMPARE(ledger.crashes(file), 0);
    ledger.clearOpening(u"p1"_s);
    QCOMPARE(markers(tmp.filePath(u"state"_s)).size(), 0);
}

void TstCrashLedger::leftoverMarkersCountAndPersist()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"state"_s);
    const QString file = tmp.filePath(u"a.apng"_s);
    testutil::writeFile(file, "x");
    for (int run = 1; run <= CrashLedger::kMaxCrashes; ++run) {
        CrashLedger ledger(dir);
        ledger.load(); // the marker of the previous run, if any
        QCOMPARE(ledger.crashes(file), run - 1);
        QCOMPARE(ledger.blocked(file), false);
        ledger.markOpening(u"p1"_s, file); // ...and the process dies here
    }
    CrashLedger after(dir);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"ended while opening"_s));
    after.load();
    QCOMPARE(after.crashes(file), CrashLedger::kMaxCrashes);
    QVERIFY(after.blocked(file));
    QCOMPARE(markers(dir).size(), 0);
    CrashLedger again(dir); // the count is on disk, not only in memory
    again.load();
    QVERIFY(again.blocked(file));
}

void TstCrashLedger::aChangedFileStartsOver()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"state"_s);
    const QString file = tmp.filePath(u"a.apng"_s);
    testutil::writeFile(file, "x");
    for (int run = 0; run < CrashLedger::kMaxCrashes + 1; ++run) {
        CrashLedger ledger(dir);
        ledger.load();
        ledger.markOpening(u"p1"_s, file);
    }
    CrashLedger blockedLedger(dir);
    blockedLedger.load();
    QVERIFY(blockedLedger.blocked(file));
    testutil::writeFile(file, "a longer replacement");
    QVERIFY(!blockedLedger.blocked(file));
    QCOMPARE(blockedLedger.crashes(file), 0);
}

void TstCrashLedger::forgetAndPlayingClearTheCount()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"state"_s);
    const QString file = tmp.filePath(u"a.apng"_s);
    testutil::writeFile(file, "x");
    {
        CrashLedger ledger(dir);
        ledger.load();
        ledger.markOpening(u"p1"_s, file);
    }
    CrashLedger ledger(dir);
    ledger.load();
    QCOMPARE(ledger.crashes(file), 1);
    ledger.forget(file);
    QCOMPARE(ledger.crashes(file), 0);
    CrashLedger reloaded(dir);
    reloaded.load();
    QCOMPARE(reloaded.crashes(file), 0);
}

void TstCrashLedger::garbageStateIsIgnored()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"state"_s);
    testutil::writeFile(dir + u"/crashes.json"_s, "not json");
    testutil::writeFile(dir + u"/opening-bad.json"_s, "{\"nofile\": 1}");
    CrashLedger ledger(dir);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"crash record"_s));
    ledger.load();
    QCOMPARE(ledger.crashes(u"/x"_s), 0);
    QCOMPARE(markers(dir).size(), 0);
}

void TstCrashLedger::severalMarkersChargeNobody()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"state"_s);
    const QString a = tmp.filePath(u"a.apng"_s);
    const QString b = tmp.filePath(u"b.apng"_s);
    testutil::writeFile(a, "x");
    testutil::writeFile(b, "y");
    {
        CrashLedger ledger(dir);
        ledger.load();
        ledger.markOpening(u"p1"_s, a); // two windows were opening when the process died
        ledger.markOpening(u"p2"_s, b);
    }
    CrashLedger ledger(dir);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"not blaming"_s));
    ledger.load();
    QCOMPARE(ledger.crashes(a), 0);
    QCOMPARE(ledger.crashes(b), 0);
    QCOMPARE(markers(dir).size(), 0);
}

void TstCrashLedger::theRecordIsCapped()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"state"_s);
    const int count = CrashLedger::kMaxFiles + 10;
    QStringList files;
    for (int i = 0; i < count; ++i) {
        files << tmp.filePath(u"f%1.apng"_s.arg(i));
        testutil::writeFile(files.last(), "x");
    }
    // One crash per run (a run only ever has one file opening), each run charging the previous one's file.
    for (int i = 0; i < count; ++i) {
        CrashLedger run(dir);
        run.load();
        run.markOpening(u"pet"_s, files.at(i));
    }
    CrashLedger last(dir);
    last.load();
    CrashLedger reloaded(dir);
    reloaded.load();
    int known = 0;
    for (const QString &file : files)
        known += reloaded.crashes(file);
    QCOMPARE(known, CrashLedger::kMaxFiles);
}

QTEST_GUILESS_MAIN(TstCrashLedger)
#include "tst_crashledger.moc"
