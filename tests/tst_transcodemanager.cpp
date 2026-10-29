#include "animfixtures.h"
#include "testutil.h"
#include "transcodemanager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QtEndian>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <sys/resource.h>

using namespace Qt::StringLiterals;
using namespace murasaki;

// Runs against a fake "ffmpeg" shell script placed first in PATH; the real one is never started. The script
// writes its arguments to <tmp>/args.txt and its output (the last argument) as the test asks through FAKE_MODE.
class TestTranscodeManager : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void filterCapsLongestSideAndHalvesHighFps();
    void argumentsAreTheDocumentedCommand();
    void successfulJobBecomesDoneAndIsCached();
    void runsUnderNiceAndWritesOnlyInsideTheCache();
    void failureKeepsTheLastStderrLines();
    void failedOutputIsNotLeftBehindAndCanBeRetried();
    void automaticRequestsLeaveAFailedVariantAlone();
    void missingFfmpegFails();
    void jobsRunOneAtATime();
    void progressIsParsed();
    void silentFfmpegIsKilled();
    void changedSourceIsConvertedAgainAndStaleOutputRemoved();
    void unknownSourceIsRejected();
    void awkwardNamesStayInsideTheItemDir();
    void inputIsHardenedForUntrustedFiles();
    void similarVariantNamesDoNotShareFiles();
    void pruneRemovesStaleFilesAndKeepsLiveOnes();
    void filterUsesTheBucket();
    void bucketsHaveTheirOwnCacheFiles();
    void invalidBucketsAreRejected();
    void doneBucketsListsTheNewestFirstAndRemoveDoneDeletesOne();
    void aNewBucketLeavesTheOtherBucketsAlone();
    void filesFromBeforeBucketsAreCleanedUp();
    void pruneKeepsOnlyTheBucketsItIsTold();
    void failedBucketsAreIndependentAndNotRetriedAutomatically();
    void aLegacyCopyCountsAsTheBiggestBucketUntilReplaced();
    void queuedJobsCanBeCancelled();
    void aStagesCopyIsEncodedFasterThanAPetsAndWithinABudget();
    void smallCopiesGoBeforeBigOnes();
    void playbackRateRetimesEveryFrame();
    void rateIsPartOfTheCacheName();
    void copiesOfEveryRateAreListed();
    void realFfmpegKeepsEveryFrameAtTheRate();
    void realFfmpegAppliesTheTinyDelayRuleToAnApng();
    void copiesFromBeforeTheRateCountAsOriginalTiming();

private:
    TranscodeSource source(const QString &name = u"idle"_s, double fps = 30, int frames = 30)
    {
        const QString file = m_tmp->filePath(u"src/"_s + name + u".apng"_s);
        if (!QFile::exists(file))
            testutil::writeFile(file, "source " + name.toUtf8());
        return {u"anima:1"_s, name, file, fps, frames};
    }
    void installFfmpeg(const QString &body)
    {
        const QString path = m_tmp->filePath(u"bin/ffmpeg"_s);
        testutil::writeFile(path, "#!/bin/sh\n" + body.toUtf8());
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    }
    bool waitDone(TranscodeManager &m, const TranscodeSource &s, int ms = 15000)
    {
        return QTest::qWaitFor([&] {
            const auto st = m.status(s).state;
            return st == TranscodeStatus::Done || st == TranscodeStatus::Failed;
        }, ms);
    }

    QTemporaryDir *m_tmp = nullptr;
    QByteArray m_oldPath;
};

void TestTranscodeManager::init()
{
    m_tmp = new QTemporaryDir;
    m_oldPath = qgetenv("PATH");
    qputenv("PATH", (m_tmp->filePath(u"bin"_s).toUtf8() + ":" + m_oldPath));
    // The default fake: log the arguments, report progress, write the output.
    installFfmpeg(u"for last; do :; done\n"
                  "printf '%s\\n' \"$@\" > '"_s + m_tmp->filePath(u"args.txt"_s) + u"'\n"
                  "nice > '"_s + m_tmp->filePath(u"nice.txt"_s) + u"'\n"
                  "echo out_time_us=500000\necho progress=end\necho fake > \"$last\"\n"_s);
}

void TestTranscodeManager::cleanup()
{
    qputenv("PATH", m_oldPath);
    delete m_tmp;
    m_tmp = nullptr;
}

void TestTranscodeManager::filterCapsLongestSideAndHalvesHighFps()
{
    QCOMPARE(TranscodeManager::filterFor(30),
             u"scale='if(gt(iw,ih),min(650,iw),-2)':'if(gt(iw,ih),-2,min(650,ih))'"_s);
    QVERIFY(TranscodeManager::filterFor(49.9).indexOf(u"fps="_s) < 0);
    QVERIFY(TranscodeManager::filterFor(50).endsWith(u",fps=30"_s));
    QVERIFY(TranscodeManager::filterFor(60).endsWith(u",fps=30"_s));
}

void TestTranscodeManager::aStagesCopyIsEncodedFasterThanAPetsAndWithinABudget()
{
    TranscodeSource stage = source(u"a"_s, 60);
    stage.bucket = 1920;
    const QString joined = TranscodeManager::ffmpegArguments(stage, u"/cache/x.webm.part"_s).join(u' ');
    QVERIFY(joined.contains(u"-deadline good -cpu-used 4 -tile-columns 2 -lag-in-frames 0"_s));
    QVERIFY(joined.contains(u"-threads 4"_s) && !joined.contains(u"-threads 2"_s));
    QVERIFY(joined.contains(u"-b:v 0 -crf 32 -an"_s)); // (the quality is the pet's)
    // a pet's copy keeps its command
    const QString pet = TranscodeManager::ffmpegArguments(source(u"a"_s, 60), u"/cache/x.webm.part"_s).join(u' ');
    QVERIFY(!pet.contains(u"-cpu-used"_s) && !pet.contains(u"-deadline"_s) && pet.contains(u"-row-mt 1 -threads 2 -b:v 0"_s));
    // an encode that goes on past its budget is stopped and fails (the original plays), and the next job starts
    installFfmpeg(u"for l; do :; done\nexec sleep 20\n"_s);
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    manager.setStageBudgetMs(300);
    QVERIFY(manager.request(stage));
    QVERIFY(waitDone(manager, stage, 8000));
    QCOMPARE(manager.status(stage).state, TranscodeStatus::Failed);
    QVERIFY2(manager.status(stage).error.contains(u"too long"_s), qPrintable(manager.status(stage).error));
}

void TestTranscodeManager::smallCopiesGoBeforeBigOnes()
{
    const QString order = m_tmp->filePath(u"order.txt"_s);
    installFfmpeg(u"for l; do :; done\necho \"$l\" >> '"_s + order + u"'\nsleep 0.25\necho x > \"$l\"\n"_s);
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    TranscodeSource first = source(u"first"_s);
    TranscodeSource big = source(u"big"_s);
    big.bucket = 1920;
    TranscodeSource huge = source(u"huge"_s);
    huge.bucket = 3840;
    TranscodeSource small = source(u"small"_s);
    small.bucket = 384;
    TranscodeSource small2 = source(u"small2"_s);
    small2.bucket = 384;
    manager.request(first);
    QVERIFY(QTest::qWaitFor([&] { return manager.status(first).state == TranscodeStatus::Running; }, 5000));
    manager.request(huge);
    manager.request(big);
    manager.request(small);
    manager.request(small2);
    QVERIFY(waitDone(manager, huge));
    QFile log(order);
    QVERIFY(log.open(QIODevice::ReadOnly));
    const QStringList lines = QString::fromUtf8(log.readAll()).split(u'\n', Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 5);
    const auto bucketOf = [](const QString &path) { return path.contains(u"-3840-"_s) ? 3840 : path.contains(u"-1920-"_s) ? 1920 : path.contains(u"-384-"_s) ? 384 : 650; };
    QList<int> buckets;
    for (const QString &line : lines)
        buckets.append(bucketOf(line));
    QCOMPARE(buckets, (QList<int>{650, 384, 384, 1920, 3840})); // (the one running first, then smallest first; the same size in the order asked)
    QVERIFY(lines.at(1).contains(u"small-"_s) && lines.at(2).contains(u"small2-"_s));
}

void TestTranscodeManager::argumentsAreTheDocumentedCommand()
{
    const QStringList args = TranscodeManager::ffmpegArguments(source(u"a"_s, 60), u"/cache/x.webm.part"_s);
    const QString joined = args.join(u' ');
    QVERIFY(joined.contains(u"-c:v libvpx-vp9 -pix_fmt yuva420p"_s));
    QVERIFY(joined.contains(u"-b:v 0 -crf 32 -an -progress pipe:1"_s));
    QVERIFY(joined.contains(u"-auto-alt-ref 0"_s)); // without it VP9 drops the alpha plane
    QVERIFY(joined.contains(u"-row-mt 1 -threads 2"_s)); // two threads at most: the desktop stays responsive
    QVERIFY(joined.contains(u",fps=30"_s));
    QCOMPARE(args.last(), u"/cache/x.webm.part"_s);
    QVERIFY(args.contains(u"-y"_s));
    QCOMPARE(args.at(args.lastIndexOf(u"-f"_s) + 1), u"webm"_s);
}

void TestTranscodeManager::successfulJobBecomesDoneAndIsCached()
{
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    const TranscodeSource src = source();
    QCOMPARE(manager.status(src).state, TranscodeStatus::None);
    QSignalSpy spy(&manager, &TranscodeManager::changed);

    QVERIFY(manager.request(src));
    QCOMPARE(manager.status(src).state, TranscodeStatus::Queued);
    QVERIFY(waitDone(manager, src));

    const TranscodeStatus done = manager.status(src);
    QCOMPARE(done.state, TranscodeStatus::Done);
    QVERIFY(done.file.startsWith(m_tmp->filePath(u"cache/pets/anima_1/idle-"_s)));
    QVERIFY(done.file.endsWith(u".webm"_s));
    QVERIFY(QFile::exists(done.file));
    QVERIFY(!QFile::exists(done.file + u".part"_s));
    QVERIFY(spy.count() >= 2);
    QCOMPARE(spy.at(0).at(0).toString(), u"anima:1"_s);

    // A new manager (a daemon restart) sees the cached file; requesting again starts nothing.
    TranscodeManager again(m_tmp->filePath(u"cache/pets"_s));
    QCOMPARE(again.status(src).state, TranscodeStatus::Done);
    QVERIFY(again.request(src));
    QCOMPARE(again.status(src).state, TranscodeStatus::Done);
}

void TestTranscodeManager::runsUnderNiceAndWritesOnlyInsideTheCache()
{
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    const TranscodeSource src = source();
    manager.request(src);
    QVERIFY(waitDone(manager, src));

    QFile nice(m_tmp->filePath(u"nice.txt"_s));
    QVERIFY(nice.open(QIODevice::ReadOnly));
    const int expected = qMin(19, getpriority(PRIO_PROCESS, 0) + 10);
    QCOMPARE(nice.readAll().trimmed().toInt(), expected);

    QFile args(m_tmp->filePath(u"args.txt"_s));
    QVERIFY(args.open(QIODevice::ReadOnly));
    const QStringList list = QString::fromUtf8(args.readAll()).split(u'\n', Qt::SkipEmptyParts);
    QCOMPARE(list.at(list.indexOf(u"-i"_s) + 1), u"file:"_s + src.file);
    QVERIFY(list.last().startsWith(m_tmp->filePath(u"cache/pets/"_s)));
    QVERIFY(list.last().endsWith(u".webm.part"_s));
    // The source is untouched and nothing else was created next to it.
    QCOMPARE(QDir(m_tmp->filePath(u"src"_s)).entryList(QDir::Files).size(), 1);
}

void TestTranscodeManager::failureKeepsTheLastStderrLines()
{
    installFfmpeg(u"for l; do :; done\n"
                  "for i in 1 2 3 4 5 6 7 8 9 10 11 12; do echo \"error line $i\" >&2; done\n"
                  "echo partial > \"$l\"\nexit 1\n"_s);
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    const TranscodeSource src = source();
    manager.request(src);
    QVERIFY(waitDone(manager, src));

    const TranscodeStatus failed = manager.status(src);
    QCOMPARE(failed.state, TranscodeStatus::Failed);
    QVERIFY(failed.error.contains(u"error line 12"_s));
    QVERIFY(failed.error.contains(u"error line 5"_s));
    QVERIFY(!failed.error.contains(u"error line 4\n"_s)); // only the last 8 lines
    QCOMPARE(manager.status(src).toJson().value(u"state"_s).toString(), u"failed"_s);
}

void TestTranscodeManager::failedOutputIsNotLeftBehindAndCanBeRetried()
{
    installFfmpeg(u"for l; do :; done\necho partial > \"$l\"\nexit 3\n"_s);
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    const TranscodeSource src = source();
    manager.request(src);
    QVERIFY(waitDone(manager, src));
    QCOMPARE(manager.status(src).state, TranscodeStatus::Failed);
    QVERIFY(manager.status(src).error.contains(u"status 3"_s));
    QCOMPARE(QDir(m_tmp->filePath(u"cache/pets/anima_1"_s)).entryList(QDir::Files).size(), 0);

    installFfmpeg(u"for l; do :; done\necho ok > \"$l\"\n"_s);
    QVERIFY(manager.request(src)); // retry
    QCOMPARE(manager.status(src).state, TranscodeStatus::Queued);
    QVERIFY(waitDone(manager, src));
    QCOMPARE(manager.status(src).state, TranscodeStatus::Done);
}

void TestTranscodeManager::automaticRequestsLeaveAFailedVariantAlone()
{
    installFfmpeg(u"exit 3\n"_s);
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    QSignalSpy changed(&manager, &TranscodeManager::changed);
    const TranscodeSource src = source();
    manager.request(src, false);
    QVERIFY(waitDone(manager, src));
    QCOMPARE(manager.status(src).state, TranscodeStatus::Failed);

    // An edit of the pet asks again on its own: still failed, nothing queued, nothing announced.
    changed.clear();
    QVERIFY(manager.request(src, false));
    QTest::qWait(100);
    QCOMPARE(manager.status(src).state, TranscodeStatus::Failed);
    QCOMPARE(changed.count(), 0);

    // "Try again" is an explicit request: it clears the failure and queues the variant.
    installFfmpeg(u"for l; do :; done\necho ok > \"$l\"\n"_s);
    QVERIFY(manager.request(src));
    QCOMPARE(manager.status(src).state, TranscodeStatus::Queued);
    QVERIFY(waitDone(manager, src));
    QCOMPARE(manager.status(src).state, TranscodeStatus::Done);
}

void TestTranscodeManager::missingFfmpegFails()
{
    qputenv("PATH", m_tmp->filePath(u"empty"_s).toUtf8());
    QVERIFY(QDir().mkpath(m_tmp->filePath(u"empty"_s)));
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    const TranscodeSource src = source();
    manager.request(src);
    QVERIFY(waitDone(manager, src));
    QCOMPARE(manager.status(src).state, TranscodeStatus::Failed);
    QCOMPARE(manager.status(src).error, u"ffmpeg not found"_s);
}

void TestTranscodeManager::jobsRunOneAtATime()
{
    const QString lock = m_tmp->filePath(u"lock"_s);
    const QString overlap = m_tmp->filePath(u"overlap"_s);
    installFfmpeg(u"for l; do :; done\nif [ -e '"_s + lock + u"' ]; then touch '"_s + overlap + u"'; fi\ntouch '"_s + lock
                  + u"'\nsleep 0.3\nrm '"_s + lock + u"'\necho x > \"$l\"\n"_s);
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    const TranscodeSource a = source(u"a"_s);
    const TranscodeSource b = source(u"b"_s);
    const TranscodeSource c = source(u"c"_s);
    manager.request(a);
    manager.request(b);
    manager.request(c);
    QVERIFY(QTest::qWaitFor([&] { return manager.status(a).state == TranscodeStatus::Running; }, 5000));
    QCOMPARE(manager.status(b).state, TranscodeStatus::Queued);
    QCOMPARE(manager.status(c).state, TranscodeStatus::Queued);
    QVERIFY(waitDone(manager, c));
    QCOMPARE(manager.status(a).state, TranscodeStatus::Done);
    QCOMPARE(manager.status(b).state, TranscodeStatus::Done);
    QVERIFY(!QFile::exists(overlap));
}

void TestTranscodeManager::progressIsParsed()
{
    installFfmpeg(u"for l; do :; done\necho out_time_us=500000\nsleep 1\necho out_time_us=1000000\necho progress=end\n"
                  "echo x > \"$l\"\n"_s);
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    const TranscodeSource src = source(u"idle"_s, 10, 10); // 1 second long
    manager.request(src);
    QVERIFY(QTest::qWaitFor([&] { return manager.status(src).progress > 0.4; }, 5000));
    const TranscodeStatus running = manager.status(src);
    QCOMPARE(running.state, TranscodeStatus::Running);
    QVERIFY(running.progress >= 0.49 && running.progress <= 0.51);
    QCOMPARE(running.toJson().value(u"progress"_s).toDouble(), running.progress);
    QVERIFY(waitDone(manager, src));
    QCOMPARE(manager.status(src).state, TranscodeStatus::Done);
}

void TestTranscodeManager::silentFfmpegIsKilled()
{
    installFfmpeg(u"exec sleep 30\n"_s);
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    manager.setStallTimeoutMs(300);
    const TranscodeSource src = source();
    manager.request(src);
    QVERIFY(waitDone(manager, src, 10000));
    QCOMPARE(manager.status(src).state, TranscodeStatus::Failed);
    QVERIFY(manager.status(src).error.contains(u"stopped responding"_s));
}

void TestTranscodeManager::changedSourceIsConvertedAgainAndStaleOutputRemoved()
{
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    TranscodeSource src = source();
    manager.request(src);
    QVERIFY(waitDone(manager, src));
    const QString first = manager.status(src).file;

    // Same path, new size and mtime.
    testutil::writeFile(src.file, "a different and longer source");
    QCOMPARE(manager.status(src).state, TranscodeStatus::None);
    manager.request(src);
    QVERIFY(waitDone(manager, src));
    const QString second = manager.status(src).file;
    QVERIFY(first != second);
    QVERIFY(!QFile::exists(first));
    QVERIFY(QFile::exists(second));
}

void TestTranscodeManager::unknownSourceIsRejected()
{
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    TranscodeSource src = source();
    src.file = m_tmp->filePath(u"nope.apng"_s);
    QVERIFY(!manager.request(src));
    QCOMPARE(manager.status(src).state, TranscodeStatus::None);
}

void TestTranscodeManager::awkwardNamesStayInsideTheItemDir()
{
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    TranscodeSource src = source(u"x"_s);
    src.itemId = u"../../evil"_s;
    src.variant = u"../v a/r"_s;
    const QString out = manager.outputPath(src);
    QVERIFY(out.startsWith(m_tmp->filePath(u"cache/pets/"_s)));
    QVERIFY(!out.contains(u".."_s + u"/"_s));
    QCOMPARE(QFileInfo(out).absolutePath().count(u'/'), m_tmp->filePath(u"cache/pets/x"_s).count(u'/'));
    manager.request(src);
    QVERIFY(waitDone(manager, src));
    QCOMPARE(manager.status(src).state, TranscodeStatus::Done);
}

void TestTranscodeManager::inputIsHardenedForUntrustedFiles()
{
    const auto argsFor = [&](const QString &ext) {
        TranscodeSource src = source(u"v"_s + ext);
        const QString file = m_tmp->filePath(u"src/clip."_s + ext);
        testutil::writeFile(file, "x");
        src.file = file;
        return TranscodeManager::ffmpegArguments(src, u"/cache/o.part"_s);
    };
    for (const auto &[ext, demuxer] : {std::pair{u"apng"_s, u"apng"_s}, {u"GIF"_s, u"gif"_s}, {u"webp"_s, u"webp_pipe"_s}}) {
        const QStringList a = argsFor(ext);
        const qsizetype in = a.indexOf(u"-i"_s);
        QVERIFY(in > 0);
        // -protocol_whitelist file, then the forced demuxer, then the file: input, all before -i's neighbours.
        QCOMPARE(a.at(in - 4), u"-protocol_whitelist"_s);
        QCOMPARE(a.at(in - 3), u"file"_s);
        QCOMPARE(a.at(in - 2), u"-f"_s);
        QCOMPARE(a.at(in - 1), demuxer);
        QVERIFY(a.at(in + 1).startsWith(u"file:/"_s));
    }
    // A name that looks like an option or a URL stays a plain file path.
    TranscodeSource odd = source();
    odd.file = m_tmp->filePath(u"src/-i http:x.apng"_s);
    testutil::writeFile(odd.file, "x");
    const QStringList a = TranscodeManager::ffmpegArguments(odd, u"/cache/o.part"_s);
    QCOMPARE(a.at(a.indexOf(u"-i"_s) + 1), u"file:"_s + odd.file);
}

void TestTranscodeManager::similarVariantNamesDoNotShareFiles()
{
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    TranscodeSource a = source(u"a b"_s);
    TranscodeSource b = a;
    b.variant = u"a_b"_s; // same safe name, same source file
    QVERIFY(manager.outputPath(a) != manager.outputPath(b));
    manager.request(a);
    manager.request(b);
    QVERIFY(waitDone(manager, a));
    QVERIFY(waitDone(manager, b));
    QCOMPARE(manager.status(a).state, TranscodeStatus::Done);
    QCOMPARE(manager.status(b).state, TranscodeStatus::Done);
    QVERIFY(QFile::exists(manager.outputPath(a)));
    QVERIFY(QFile::exists(manager.outputPath(b))); // the second job did not clean the first one up
}

void TestTranscodeManager::pruneRemovesStaleFilesAndKeepsLiveOnes()
{
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    const TranscodeSource live = source(u"live"_s);
    manager.request(live);
    QVERIFY(waitDone(manager, live));
    const QString liveFile = manager.outputPath(live);
    const QString dir = QFileInfo(liveFile).absolutePath();
    testutil::writeFile(dir + u"/live-deadbeef-0123456789ab.webm"_s, "old key");
    testutil::writeFile(dir + u"/live-deadbeef-0123456789ab.webm.part"_s, "crash leftover");
    testutil::writeFile(dir + u"/notes.txt"_s, "not ours");
    // A vanished item: fresh files stay (its drive may be unplugged), old ones go.
    const QString gone = m_tmp->filePath(u"cache/pets/anima_2"_s);
    testutil::writeFile(gone + u"/x-00000000-0123456789ab.webm"_s, "fresh");
    testutil::writeFile(m_tmp->filePath(u"cache/pets/anima_3/y-00000000-0123456789ab.webm"_s), "old");
    const QString oldFile = m_tmp->filePath(u"cache/pets/anima_3/y-00000000-0123456789ab.webm"_s);
    QFile old(oldFile);
    QVERIFY(old.open(QIODevice::ReadWrite));
    QVERIFY(old.setFileTime(QDateTime::currentDateTime().addDays(-30), QFileDevice::FileModificationTime));
    old.close();

    manager.pruneCache({live});

    QVERIFY(QFile::exists(liveFile));
    QVERIFY(!QFile::exists(dir + u"/live-deadbeef-0123456789ab.webm"_s));
    QVERIFY(!QFile::exists(dir + u"/live-deadbeef-0123456789ab.webm.part"_s));
    QVERIFY(QFile::exists(dir + u"/notes.txt"_s));
    QVERIFY(QFile::exists(gone + u"/x-00000000-0123456789ab.webm"_s));
    QVERIFY(!QFile::exists(oldFile));
    QVERIFY(!QDir(m_tmp->filePath(u"cache/pets/anima_3"_s)).exists()); // emptied dirs go too
}

void TestTranscodeManager::filterUsesTheBucket()
{
    QCOMPARE(TranscodeManager::filterFor(30, 384),
             u"scale='if(gt(iw,ih),min(384,iw),-2)':'if(gt(iw,ih),-2,min(384,ih))'"_s);
    QVERIFY(TranscodeManager::filterFor(60, 256).startsWith(u"scale='if(gt(iw,ih),min(256,iw)"_s));
    QVERIFY(TranscodeManager::filterFor(60, 256).endsWith(u",fps=30"_s));
    TranscodeSource src = source(u"a"_s);
    src.bucket = 512;
    const QStringList args = TranscodeManager::ffmpegArguments(src, u"/cache/x.webm.part"_s);
    QCOMPARE(args.at(args.indexOf(u"-vf"_s) + 1), TranscodeManager::filterFor(src.fps, 512));
}

void TestTranscodeManager::bucketsHaveTheirOwnCacheFiles()
{
    installFfmpeg(u"for l; do :; done\nprintf '%s\\n' \"$@\" >> '"_s + m_tmp->filePath(u"args.txt"_s) + u"'\n"
                  "echo fake > \"$l\"\n"_s);
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    TranscodeSource small = source();
    small.bucket = 256;
    TranscodeSource big = small;
    big.bucket = 650;
    QVERIFY(manager.outputPath(small) != manager.outputPath(big));
    const QString name = QFileInfo(manager.outputPath(small)).fileName();
    QVERIFY2(name.startsWith(u"idle-"_s) && name.contains(u"-256-"_s) && name.endsWith(u".webm"_s), qPrintable(name));
    QVERIFY(QFileInfo(manager.outputPath(big)).fileName().contains(u"-650-"_s));

    QCOMPARE(manager.status(small).bucket, 256);
    QCOMPARE(manager.status(small).toJson().value(u"bucket"_s).toInt(), 256);
    QVERIFY(manager.request(small));
    QVERIFY(manager.request(big));
    QCOMPARE(manager.status(big).state, TranscodeStatus::Queued);
    QVERIFY(waitDone(manager, big));
    QCOMPARE(manager.status(small).state, TranscodeStatus::Done);
    QCOMPARE(manager.status(big).state, TranscodeStatus::Done);
    QVERIFY(QFile::exists(manager.outputPath(small)));
    QVERIFY(QFile::exists(manager.outputPath(big)));

    QFile args(m_tmp->filePath(u"args.txt"_s));
    QVERIFY(args.open(QIODevice::ReadOnly));
    const QString logged = QString::fromUtf8(args.readAll());
    QVERIFY(logged.contains(u"min(256,iw)"_s));
    QVERIFY(logged.contains(u"min(650,iw)"_s));
}

void TestTranscodeManager::invalidBucketsAreRejected()
{
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    TranscodeSource src = source();
    src.bucket = 300;
    QVERIFY(!manager.request(src));
    src.bucket = 0;
    QVERIFY(!manager.request(src));
    QCOMPARE(manager.status(src).state, TranscodeStatus::None);
}

void TestTranscodeManager::doneBucketsListsTheNewestFirstAndRemoveDoneDeletesOne()
{
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    TranscodeSource src = source();
    QVERIFY(manager.doneBuckets(src).isEmpty());
    const QList<int> order = {384, 650, 256};
    for (const int bucket : order) {
        src.bucket = bucket;
        manager.request(src);
        QVERIFY(waitDone(manager, src));
        // Distinct modification times, oldest first, whatever the file system's resolution.
        QFile file(manager.outputPath(src));
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(QDateTime::currentDateTime().addSecs(-100 + 10 * order.indexOf(bucket)),
                                 QFileDevice::FileModificationTime));
    }
    QCOMPARE(manager.doneBuckets(src), (QList<int>{256, 650, 384}));

    QSignalSpy changed(&manager, &TranscodeManager::changed);
    src.bucket = 650;
    QVERIFY(manager.removeDone(src));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(manager.status(src).state, TranscodeStatus::None);
    QCOMPARE(manager.doneBuckets(src), (QList<int>{256, 384}));
    QVERIFY(!manager.removeDone(src)); // already gone

    // A copy of an older state of the source does not count.
    testutil::writeFile(src.file, "a different and longer source");
    QVERIFY(manager.doneBuckets(src).isEmpty());
}

void TestTranscodeManager::aNewBucketLeavesTheOtherBucketsAlone()
{
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    TranscodeSource small = source();
    small.bucket = 256;
    manager.request(small);
    QVERIFY(waitDone(manager, small));
    TranscodeSource big = small;
    big.bucket = 650;
    manager.request(big);
    QVERIFY(waitDone(manager, big));
    QVERIFY(QFile::exists(manager.outputPath(small))); // starting the second job did not clean the first copy up
    QVERIFY(QFile::exists(manager.outputPath(big)));
}

void TestTranscodeManager::filesFromBeforeBucketsAreCleanedUp()
{
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    TranscodeSource src = source();
    src.bucket = 384;
    const QString dir = QFileInfo(manager.outputPath(src)).absolutePath();
    const QString prefix = QFileInfo(manager.outputPath(src)).fileName().section(u'-', 0, 1) + u'-'; // "idle-<hash>-"
    const QString legacy = dir + u'/' + prefix + u"0123456789ab.webm"_s;
    testutil::writeFile(legacy, "old naming");
    // Another bucket of an older source state and a .part of another bucket are leftovers too.
    testutil::writeFile(dir + u'/' + prefix + u"650-0123456789ab.webm"_s, "old source");
    testutil::writeFile(dir + u'/' + prefix + u"256-0123456789ab.webm.part"_s, "crash");
    manager.request(src);
    QVERIFY(waitDone(manager, src));
    QVERIFY(!QFile::exists(legacy));
    QVERIFY(!QFile::exists(dir + u'/' + prefix + u"650-0123456789ab.webm"_s));
    QVERIFY(!QFile::exists(dir + u'/' + prefix + u"256-0123456789ab.webm.part"_s));
    QVERIFY(QFile::exists(manager.outputPath(src)));
}

void TestTranscodeManager::pruneKeepsOnlyTheBucketsItIsTold()
{
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    TranscodeSource a = source();
    a.bucket = 256;
    TranscodeSource b = a;
    b.bucket = 384;
    TranscodeSource c = a;
    c.bucket = 650;
    for (const TranscodeSource &src : {a, b, c}) {
        manager.request(src);
        QVERIFY(waitDone(manager, src));
    }
    const QString dir = QFileInfo(manager.outputPath(a)).absolutePath();
    const QString legacy = dir + u"/idle-00000000-0123456789ab.webm"_s;
    testutil::writeFile(legacy, "old naming");

    manager.pruneCache({a, c}, {u"anima:1"_s});
    QVERIFY(QFile::exists(manager.outputPath(a)));
    QVERIFY(!QFile::exists(manager.outputPath(b)));
    QVERIFY(QFile::exists(manager.outputPath(c)));
    QVERIFY(!QFile::exists(legacy));

    // An item the library knows but keeps nothing of is emptied, however fresh its files are.
    manager.pruneCache({}, {u"anima:1"_s});
    QVERIFY(!QFile::exists(manager.outputPath(a)));
    QVERIFY(!QFile::exists(manager.outputPath(c)));
}

void TestTranscodeManager::failedBucketsAreIndependentAndNotRetriedAutomatically()
{
    installFfmpeg(u"exit 3\n"_s);
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    TranscodeSource a = source();
    a.bucket = 384;
    TranscodeSource b = a;
    b.bucket = 512;
    manager.request(a, false);
    QVERIFY(waitDone(manager, a));
    QCOMPARE(manager.status(a).state, TranscodeStatus::Failed);
    QCOMPARE(manager.status(b).state, TranscodeStatus::None); // a failed 384 says nothing about 512
    manager.request(a, false);
    QTest::qWait(100);
    QCOMPARE(manager.status(a).state, TranscodeStatus::Failed);
    installFfmpeg(u"for l; do :; done\necho ok > \"$l\"\n"_s);
    manager.request(b, false); // a different bucket is tried
    QVERIFY(waitDone(manager, b));
    QCOMPARE(manager.status(b).state, TranscodeStatus::Done);
    QCOMPARE(manager.status(a).state, TranscodeStatus::Failed);
}

void TestTranscodeManager::aLegacyCopyCountsAsTheBiggestBucketUntilReplaced()
{
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    TranscodeSource big = source();
    big.bucket = 650;
    manager.request(big);
    QVERIFY(waitDone(manager, big));
    QString legacy = manager.outputPath(big);
    legacy.replace(u"650-0-"_s, QString()); // <variant>-<key>.webm, from before buckets
    QVERIFY(QFile::rename(manager.outputPath(big), legacy));

    QCOMPARE(manager.status(big).state, TranscodeStatus::Done);
    QCOMPARE(manager.status(big).file, legacy);
    QCOMPARE(manager.doneBuckets(big), QList<int>{650});
    QSignalSpy changed(&manager, &TranscodeManager::changed);
    QVERIFY(manager.request(big)); // already done: nothing is converted again
    QCOMPARE(changed.count(), 0);

    // Another bucket's conversion and a prune that keeps 650 leave it alone.
    TranscodeSource small = big;
    small.bucket = 384;
    manager.request(small);
    QVERIFY(waitDone(manager, small));
    QVERIFY(QFile::exists(legacy));
    manager.pruneCache({big, small}, {u"anima:1"_s});
    QVERIFY(QFile::exists(legacy));
    QCOMPARE(manager.doneBuckets(big).size(), 2);

    // A prune that does not keep 650 removes it, and removeDone does too.
    manager.pruneCache({small}, {u"anima:1"_s});
    QVERIFY(!QFile::exists(legacy));
    QCOMPARE(manager.status(big).state, TranscodeStatus::None);
    QVERIFY(QFile::rename(manager.outputPath(small), legacy));
    QVERIFY(manager.removeDone(big));
    QVERIFY(!QFile::exists(legacy));

    // The copy of an older state of the source is no copy: it is cleaned when a conversion starts.
    testutil::writeFile(legacy, "old");
    testutil::writeFile(big.file, "a different and longer source");
    QCOMPARE(manager.status(big).state, TranscodeStatus::None);
    manager.request(big);
    QVERIFY(waitDone(manager, big));
    QVERIFY(!QFile::exists(legacy));
    QVERIFY(QFile::exists(manager.outputPath(big)));
}

void TestTranscodeManager::queuedJobsCanBeCancelled()
{
    installFfmpeg(u"for l; do :; done\nsleep 1\necho x > \"$l\"\n"_s);
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    TranscodeSource a = source();
    a.bucket = 256;
    TranscodeSource b = a;
    b.bucket = 384;
    manager.request(a);
    manager.request(b);
    QVERIFY(QTest::qWaitFor([&] { return manager.status(a).state == TranscodeStatus::Running; }, 5000));
    QCOMPARE(manager.queued().size(), 1);
    QCOMPARE(manager.queued().at(0).bucket, 384);
    QSignalSpy changed(&manager, &TranscodeManager::changed);
    QVERIFY(!manager.cancelQueued(a)); // running: left alone
    QVERIFY(manager.cancelQueued(b));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(manager.status(b).state, TranscodeStatus::None);
    QVERIFY(!manager.cancelQueued(b));
    QVERIFY(waitDone(manager, a));
    QTest::qWait(200);
    QCOMPARE(manager.status(b).state, TranscodeStatus::None);
}

void TestTranscodeManager::playbackRateRetimesEveryFrame()
{
    // Without a rate: as before (halve 50+ fps sources to 30).
    QCOMPARE(TranscodeManager::filterFor(60, 384), TranscodeManager::filterFor(60, 384, 0));
    QVERIFY(TranscodeManager::filterFor(60, 384).endsWith(u",fps=30"_s));
    // With one: frame N at N/rate, scaled, written at the rate. Nothing is dropped, whatever the source rate.
    QCOMPARE(TranscodeManager::filterFor(60, 384, 30),
             u"setpts=N/(30*TB),scale='if(gt(iw,ih),min(384,iw),-2)':'if(gt(iw,ih),-2,min(384,ih))',fps=30"_s);
    QCOMPARE(TranscodeManager::filterFor(10, 256, 60),
             u"setpts=N/(60*TB),scale='if(gt(iw,ih),min(256,iw),-2)':'if(gt(iw,ih),-2,min(256,ih))',fps=60"_s);
    TranscodeSource src = source(u"a"_s, 60);
    src.playFps = 24;
    const QStringList args = TranscodeManager::ffmpegArguments(src, u"/cache/x.webm.part"_s);
    QCOMPARE(args.at(args.indexOf(u"-vf"_s) + 1), TranscodeManager::filterFor(60, kTranscodeMaxSide, 24));
    // The progress is read on the copy's timeline: frames / rate.
    installFfmpeg(u"for l; do :; done\necho out_time_us=500000\nsleep 1\necho out_time_us=1000000\necho x > \"$l\"\n"_s);
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    TranscodeSource slow = source(u"slow"_s, 60, 120); // 2 s at its own 60 fps
    slow.playFps = 30;                                  // 4 s at 30 fps: half a second is 12.5 %
    manager.request(slow);
    QVERIFY(QTest::qWaitFor([&] { return manager.status(slow).progress > 0.05; }, 5000));
    QVERIFY2(manager.status(slow).progress < 0.2, qPrintable(QString::number(manager.status(slow).progress)));
    QVERIFY(waitDone(manager, slow));
    // An out-of-range rate is refused.
    slow.playFps = kMaxPetFps + 1;
    QVERIFY(!manager.request(slow));
    slow.playFps = -1;
    QVERIFY(!manager.request(slow));
}

void TestTranscodeManager::rateIsPartOfTheCacheName()
{
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    TranscodeSource original = source();
    original.bucket = 384;
    TranscodeSource thirty = original;
    thirty.playFps = 30;
    TranscodeSource fifteen = original;
    fifteen.playFps = 15;
    const QString name = QFileInfo(manager.outputPath(thirty)).fileName();
    QVERIFY2(name.contains(u"-384-30-"_s), qPrintable(name));
    QVERIFY(QFileInfo(manager.outputPath(original)).fileName().contains(u"-384-0-"_s));
    QCOMPARE(QSet<QString>({manager.outputPath(original), manager.outputPath(thirty), manager.outputPath(fifteen)}).size(), 3);
    QCOMPARE(manager.status(thirty).playFps, 30);
    QCOMPARE(manager.status(thirty).toJson().value(u"fps"_s).toInt(), 30);
    for (const TranscodeSource &src : {thirty, fifteen}) {
        manager.request(src);
        QVERIFY(waitDone(manager, src));
    }
    // Each rate has its own copy, and converting one leaves the other (same source state) alone.
    QCOMPARE(manager.status(thirty).state, TranscodeStatus::Done);
    QCOMPARE(manager.status(fifteen).state, TranscodeStatus::Done);
    QCOMPARE(manager.status(original).state, TranscodeStatus::None);
    QVERIFY(QFile::exists(manager.outputPath(thirty)));
    QVERIFY(QFile::exists(manager.outputPath(fifteen)));
}

void TestTranscodeManager::copiesOfEveryRateAreListed()
{
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    TranscodeSource src = source();
    QList<TranscodeSource> made;
    for (const auto &[bucket, fps] : {std::pair{256, 30}, {650, 30}, {256, 15}}) {
        TranscodeSource one = src;
        one.bucket = bucket;
        one.playFps = fps;
        manager.request(one);
        QVERIFY(waitDone(manager, one));
        made.append(one);
    }
    src.playFps = 30;
    const QList<int> thirty = manager.doneBuckets(src);
    QCOMPARE(QSet<int>(thirty.begin(), thirty.end()), (QSet<int>{256, 650}));
    src.playFps = 15;
    QCOMPARE(manager.doneBuckets(src), QList<int>{256});
    src.playFps = 0;
    QVERIFY(manager.doneBuckets(src).isEmpty());
    QCOMPARE(manager.doneCopies(src).size(), 3);

    // statuses() lists them all, and what is running, queued and failed too.
    installFfmpeg(u"for l; do :; done\nsleep 1\necho x > \"$l\"\n"_s);
    TranscodeSource running = src;
    running.bucket = 384;
    running.playFps = 24;
    TranscodeSource queued = running;
    queued.bucket = 512;
    manager.request(running);
    manager.request(queued);
    QVERIFY(QTest::qWaitFor([&] { return manager.status(running).state == TranscodeStatus::Running; }, 5000));
    const QList<TranscodeStatus> list = manager.statuses(src);
    QCOMPARE(list.size(), 5);
    const auto find = [&](int bucket, int fps) {
        for (const TranscodeStatus &s : list) {
            if (s.bucket == bucket && s.playFps == fps)
                return s.state;
        }
        return TranscodeStatus::None;
    };
    QCOMPARE(find(384, 24), TranscodeStatus::Running);
    QCOMPARE(find(512, 24), TranscodeStatus::Queued);
    QCOMPARE(find(256, 30), TranscodeStatus::Done);
    QCOMPARE(find(650, 30), TranscodeStatus::Done);
    QCOMPARE(find(256, 15), TranscodeStatus::Done);
    QVERIFY(waitDone(manager, queued));
    installFfmpeg(u"exit 3\n"_s);
    TranscodeSource bad = src;
    bad.bucket = 256;
    bad.playFps = 60;
    manager.request(bad);
    QVERIFY(waitDone(manager, bad));
    bool failed = false;
    for (const TranscodeStatus &s : manager.statuses(src))
        failed = failed || (s.state == TranscodeStatus::Failed && s.bucket == 256 && s.playFps == 60 && !s.error.isEmpty());
    QVERIFY(failed);
}

void TestTranscodeManager::realFfmpegKeepsEveryFrameAtTheRate()
{
    // The one test that runs the real ffmpeg: a 60 fps APNG of 30 frames, played at 30, 15 and 60 fps.
    const QStringList dirs = QString::fromLocal8Bit(m_oldPath).split(u':', Qt::SkipEmptyParts);
    const QString ffmpeg = QStandardPaths::findExecutable(u"ffmpeg"_s, dirs);
    const QString ffprobe = QStandardPaths::findExecutable(u"ffprobe"_s, dirs);
    if (ffmpeg.isEmpty() || ffprobe.isEmpty())
        QSKIP("ffmpeg and ffprobe are not available");
    const QString apng = m_tmp->filePath(u"src/real.apng"_s);
    QVERIFY(QDir().mkpath(m_tmp->filePath(u"src"_s)));
    QProcess make;
    make.start(ffmpeg, {u"-v"_s, u"error"_s, u"-y"_s, u"-f"_s, u"lavfi"_s, u"-i"_s,
                        u"testsrc=s=200x100:r=60:d=0.5,format=rgba"_s, u"-f"_s, u"apng"_s, apng});
    if (!make.waitForFinished(30000) || make.exitCode() != 0)
        QSKIP("ffmpeg cannot make the test APNG");
    for (const int fps : {30, 15, 60}) {
        TranscodeSource src{u"anima:1"_s, u"real"_s, apng, 60, 30, 384, fps};
        const QString out = m_tmp->filePath(u"real%1.webm"_s.arg(fps));
        QStringList args = TranscodeManager::ffmpegArguments(src, out);
        QProcess run;
        run.start(ffmpeg, args);
        if (!run.waitForFinished(60000) || run.exitCode() != 0)
            QSKIP("ffmpeg cannot write VP9 with alpha");
        QProcess probe;
        probe.start(ffprobe, {u"-v"_s, u"error"_s, u"-count_frames"_s, u"-show_entries"_s,
                              u"stream=nb_read_frames:format=duration"_s, u"-of"_s, u"default=nw=1"_s, out});
        QVERIFY(probe.waitForFinished(30000));
        const QString text = QString::fromUtf8(probe.readAllStandardOutput());
        QVERIFY2(text.contains(u"nb_read_frames=30"_s), qPrintable(text)); // every frame is kept
        const double duration = text.mid(text.indexOf(u"duration="_s) + 9).trimmed().toDouble();
        QVERIFY2(qAbs(duration - 30.0 / fps) < 0.02, qPrintable(u"%1 fps: %2 s"_s.arg(fps).arg(duration)));
    }
}

void TestTranscodeManager::realFfmpegAppliesTheTinyDelayRuleToAnApng()
{
    // 20 frames that say 1 ms each (the pets' rule makes them 100 ms: a copy of 2 s, as the frame cache plays the file), and the same
    // 20 frames that say 0.
    const QStringList dirs = QString::fromLocal8Bit(m_oldPath).split(u':', Qt::SkipEmptyParts);
    const QString ffmpeg = QStandardPaths::findExecutable(u"ffmpeg"_s, dirs);
    const QString ffprobe = QStandardPaths::findExecutable(u"ffprobe"_s, dirs);
    if (ffmpeg.isEmpty() || ffprobe.isEmpty())
        QSKIP("ffmpeg and ffprobe are not available");
    QVERIFY(QDir().mkpath(m_tmp->filePath(u"src"_s)));
    const QString apng = m_tmp->filePath(u"src/tiny.apng"_s);
    QProcess make;
    make.start(ffmpeg, {u"-v"_s, u"error"_s, u"-y"_s, u"-f"_s, u"lavfi"_s, u"-i"_s,
                        u"testsrc=s=64x64:r=1000:d=0.02,format=rgba"_s, u"-f"_s, u"apng"_s, apng});
    if (!make.waitForFinished(30000) || make.exitCode() != 0)
        QSKIP("ffmpeg cannot make the test APNG");
    QByteArray zero;
    {
        QFile file(apng);
        QVERIFY(file.open(QIODevice::ReadOnly));
        zero = file.readAll();
    }
    // the same file with every delay numerator 0 (CRCs recomputed)
    for (qsizetype pos = 8; pos + 12 <= zero.size();) {
        const quint32 length = qFromBigEndian<quint32>(zero.constData() + pos);
        if (zero.mid(pos + 4, 4) == "fcTL" && length == 26) {
            zero[pos + 8 + 20] = 0;
            zero[pos + 8 + 21] = 0;
            qToBigEndian(animfixtures::crc32(zero.mid(pos + 4, 4 + length)), zero.data() + pos + 8 + length);
        }
        pos += 12 + length;
    }
    const QString zeroApng = m_tmp->filePath(u"src/zero.apng"_s);
    testutil::writeFile(zeroApng, zero);
    for (const QString &file : {apng, zeroApng}) {
        TranscodeSource src{u"anima:1"_s, u"tiny"_s, file, 10, 20, 384, 0};
        const QString out = m_tmp->filePath(QFileInfo(file).baseName() + u".webm"_s);
        QProcess run;
        run.start(ffmpeg, TranscodeManager::ffmpegArguments(src, out));
        if (!run.waitForFinished(60000) || run.exitCode() != 0)
            QSKIP("ffmpeg cannot write VP9 with alpha");
        QProcess probe;
        probe.start(ffprobe, {u"-v"_s, u"error"_s, u"-show_entries"_s, u"format=duration"_s, u"-of"_s, u"default=nw=1"_s, out});
        QVERIFY(probe.waitForFinished(30000));
        const QString text = QString::fromUtf8(probe.readAllStandardOutput());
        const double duration = text.mid(text.indexOf(u"duration="_s) + 9).trimmed().toDouble();
        QVERIFY2(qAbs(duration - 2.0) < 0.2, qPrintable(file + u": "_s + text));
    }
}

void TestTranscodeManager::copiesFromBeforeTheRateCountAsOriginalTiming()
{
    TranscodeManager manager(m_tmp->filePath(u"cache/pets"_s));
    TranscodeSource src = source();
    src.bucket = 384;
    manager.request(src);
    QVERIFY(waitDone(manager, src));
    // The name from before the rate existed: <variant>-<bucket>-<key>.webm.
    QString older = manager.outputPath(src);
    older.replace(u"384-0-"_s, u"384-"_s);
    QVERIFY(QFile::rename(manager.outputPath(src), older));
    QCOMPARE(manager.status(src).state, TranscodeStatus::Done);
    QCOMPARE(manager.status(src).file, older);
    QCOMPARE(manager.doneBuckets(src), QList<int>{384});
    TranscodeSource thirty = src;
    thirty.playFps = 30;
    QCOMPARE(manager.status(thirty).state, TranscodeStatus::None); // it is not a copy at 30 fps
    QVERIFY(manager.doneBuckets(thirty).isEmpty());
    manager.request(thirty);
    QVERIFY(waitDone(manager, thirty));
    QVERIFY(QFile::exists(older)); // the conversion at another rate did not clean it up
    manager.pruneCache({thirty}, {u"anima:1"_s});
    QVERIFY(!QFile::exists(older)); // not wanted any more
    QVERIFY(QFile::exists(manager.outputPath(thirty)));
}

QTEST_GUILESS_MAIN(TestTranscodeManager)
#include "tst_transcodemanager.moc"
