// Which frames of a clip a screen trigger keeps, how one is scaled and written, and a region's frames file taken to PNGs in a job folder.

#include "npyfixtures.h"
#include "petimport.h"
#include "screenframes.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace murasaki::import;

namespace {

QString nameOf(std::string_view text)
{
    return QString::fromUtf8(text.data(), qsizetype(text.size()));
}

ScreenRefPlan anchor(int ref = 0, int start = 0, int end = -1)
{
    ScreenRefPlan p;
    p.refFrameIndex = ref;
    p.startIndex = start;
    p.endIndex = end;
    return p;
}

ScreenRefPlan sequence(int len, int step, int ref = 0, int start = 0, int end = -1)
{
    ScreenRefPlan p = anchor(ref, start, end);
    p.sequencer = true;
    p.seqLen = len;
    p.seqStep = step;
    return p;
}

} // namespace

class TstScreenFrames : public QObject
{
    Q_OBJECT

private slots:
    void theAnchorIsRelativeToTheClipStartAndClamped()
    {
        QCOMPARE(screenFramesUsed(anchor(), 10), (QList<int>{0}));
        QCOMPARE(screenFramesUsed(anchor(3), 10), (QList<int>{3}));
        QCOMPARE(screenFramesUsed(anchor(3, 2), 10), (QList<int>{5}));
        QCOMPARE(screenFramesUsed(anchor(99), 10), (QList<int>{9}));          // past the end: the last
        QCOMPARE(screenFramesUsed(anchor(99, 2, 6), 10), (QList<int>{6}));    // past the clip's end: the clip's last
        QCOMPARE(screenFramesUsed(anchor(0, 50), 10), (QList<int>{9}));       // a clip that starts past the array
        QCOMPARE(screenFramesUsed(anchor(0, 5, 3), 10), (QList<int>{5}));     // an end before the start
        QCOMPARE(screenFramesUsed(anchor(-4), 10), (QList<int>{0}));
        QCOMPARE(screenFramesUsed(anchor(), 1), (QList<int>{0}));
        QVERIFY(screenFramesUsed(anchor(), 0).isEmpty());
    }

    void aSequenceRunsFromTheAnchorAndStopsAtTheClipEnd()
    {
        bool shortened = true;
        QCOMPARE(screenFramesUsed(sequence(4, 2), 20, &shortened), (QList<int>{0, 2, 4, 6}));
        QVERIFY(!shortened);
        QCOMPARE(screenFramesUsed(sequence(4, 2, 5, 3), 20, &shortened), (QList<int>{8, 10, 12, 14}));
        QCOMPARE(screenFramesUsed(sequence(8, 3, 0, 0, 7), 20, &shortened), (QList<int>{0, 3, 6}));
        QVERIFY(shortened);
        QCOMPARE(screenFramesUsed(sequence(1, 5), 20, &shortened), (QList<int>{0}));
        QVERIFY(!shortened);
        QCOMPARE(screenFramesUsed(sequence(0, 0), 20), (QList<int>{0}));        // seq_len and seq_step under 1 are 1
        QCOMPARE(screenFramesUsed(sequence(3, 1, 99), 20), (QList<int>{19}));   // an anchor past the end leaves one frame
    }

    void aSequenceIsCappedAt64Frames()
    {
        bool shortened = false;
        const QList<int> used = screenFramesUsed(sequence(1000, 1), 1024, &shortened);
        QCOMPARE(used.size(), kMaxRefFramesPerRegion);
        QVERIFY(shortened);
        QCOMPARE(screenFramesUsed(sequence(1000000, 1000000), 1024), (QList<int>{0})); // no overflow
        QCOMPARE(screenFramesUsed(sequence(5, 2000000000, 0), 1024), (QList<int>{0}));
    }

    void thePlanComesFromThePackForm()
    {
        const QJsonObject pack{{"event", QJsonObject{{"startIndex", 4}, {"endIndex", 30}}},
                               {"match", QJsonObject{{"refMode", "sequencer"}, {"refFrameIndex", 2}, {"seqLen", 5}, {"seqStep", 3}}}};
        const ScreenRefPlan p = screenRefPlanFromPack(pack);
        QVERIFY(p.sequencer);
        QCOMPARE(p.refFrameIndex, 2);
        QCOMPARE(p.seqLen, 5);
        QCOMPARE(p.seqStep, 3);
        QCOMPARE(p.startIndex, 4);
        QCOMPARE(p.endIndex, 30);
        const ScreenRefPlan d = screenRefPlanFromPack({});
        QVERIFY(!d.sequencer);
        QCOMPARE(d.seqLen, 8); // the pack default
        QCOMPARE(d.seqStep, 1);
        QCOMPARE(d.endIndex, -1);
        QVERIFY(!screenRefPlanFromPack(QJsonObject{{"match", QJsonObject{{"refMode", "anchor"}}}}).sequencer);
    }

    void aLargeFrameIsScaledToThreeHundredSixtyAndKeptAsAPng()
    {
        const QByteArray rgb = npyfixtures::framePixels(1, 1080, 1920);
        QString error;
        const QByteArray png = scaledFramePng(reinterpret_cast<const uchar *>(rgb.constData()), 1920, 1080, &error);
        QVERIFY2(!png.isEmpty(), qPrintable(error));
        QImage image;
        QVERIFY(image.loadFromData(png));
        QCOMPARE(image.width(), 360);
        QCOMPARE(image.height(), 203);
        QCOMPARE(image.pixelColor(10, 100).red(), 50);   // the bright half: 10 + 40
        QCOMPARE(image.pixelColor(350, 100).red(), 10);
        QCOMPARE(image.pixelColor(10, 100).green(), 100);
        QCOMPARE(image.pixelColor(10, 100).blue(), 200);
        QCOMPARE(image.pixelColor(10, 100).alpha(), 255);
    }

    void aSmallFrameKeepsItsSize()
    {
        const QByteArray rgb = npyfixtures::framePixels(1, 30, 40);
        const QByteArray png = scaledFramePng(reinterpret_cast<const uchar *>(rgb.constData()), 40, 30, nullptr);
        QImage image;
        QVERIFY(image.loadFromData(png));
        QCOMPARE(image.size(), QSize(40, 30));
        // tall, thin and tiny
        const QByteArray tall = npyfixtures::framePixels(1, 1000, 10);
        QVERIFY(image.loadFromData(scaledFramePng(reinterpret_cast<const uchar *>(tall.constData()), 10, 1000, nullptr)));
        QCOMPARE(image.height(), 360);
        QCOMPARE(image.width(), 4);
        const QByteArray one(3, char(77));
        QVERIFY(image.loadFromData(scaledFramePng(reinterpret_cast<const uchar *>(one.constData()), 1, 1, nullptr)));
        QCOMPARE(image.size(), QSize(1, 1));
        QString error;
        QVERIFY(scaledFramePng(nullptr, 10, 10, &error).isEmpty());
        QVERIFY(scaledFramePng(reinterpret_cast<const uchar *>(one.constData()), 0, 1, &error).isEmpty());
    }

    void aRegionKeepsOnlyTheFramesTheTriggerUses()
    {
        QTemporaryDir dir;
        long long left = 1 << 20;
        QBuffer npz;
        npz.setData(npyfixtures::npzFile(10, 30, 40));
        npz.open(QIODevice::ReadOnly);
        const ScreenRefRegion r = extractRegionRefs(&npz, 2, anchor(3, 1), dir.path(), &left);
        QVERIFY2(r.ready(), qPrintable(r.status));
        QCOMPARE(r.sourceFrames, 10);
        QCOMPARE(r.frames.size(), 1);
        QCOMPARE(r.frames.at(0).clipIndex, 4);
        QCOMPARE(r.frames.at(0).file, QString("screenrefs/region2/step0.png"));
        QCOMPARE(QDir(dir.filePath("screenrefs/region2")).entryList(QDir::Files).size(), 1);
        QVERIFY(left < (1 << 20));
        QImage image(dir.filePath(r.frames.at(0).file));
        QCOMPARE(image.pixelColor(2, 5).red(), 4 * 20 + 10 + 40); // frame 4 is the one kept
    }

    void aSequenceKeepsItsRunInOrder()
    {
        QTemporaryDir dir;
        long long left = 1 << 20;
        QBuffer npz;
        npz.setData(npyfixtures::npzFile(10, 20, 20, false));
        npz.open(QIODevice::ReadOnly);
        const ScreenRefRegion r = extractRegionRefs(&npz, 0, sequence(4, 3, 1), dir.path(), &left);
        QVERIFY2(r.ready(), qPrintable(r.status));
        QCOMPARE(r.frames.size(), 3); // 1, 4, 7 then 10 is past the end
        QCOMPARE(r.frames.at(0).clipIndex, 1);
        QCOMPARE(r.frames.at(2).clipIndex, 7);
        QCOMPARE(r.frames.at(1).file, QString("screenrefs/region0/step1.png"));
        QVERIFY(r.notes.join(' ').contains(nameOf(kScreenRefsShortened)));
        QCOMPARE(QImage(dir.filePath(r.frames.at(2).file)).pixelColor(1, 1).red(), 7 * 20 + 10 + 40);
    }

    void theTotalCapCutsTheRunHard()
    {
        QTemporaryDir dir;
        long long left = 1 << 20;
        QBuffer npz;
        npz.setData(npyfixtures::npzFile(30, 8, 8));
        npz.open(QIODevice::ReadOnly);
        const ScreenRefRegion r = extractRegionRefs(&npz, 0, sequence(20, 1), dir.path(), &left, 5);
        QVERIFY2(r.ready(), qPrintable(r.status));
        QCOMPARE(r.frames.size(), 5);
        QVERIFY(r.notes.join(' ').contains(nameOf(kScreenRefsOverLimit)));
        QCOMPARE(QDir(dir.filePath("screenrefs/region0")).entryList(QDir::Files).size(), 5);
    }

    void aClipThatStartsPastTheDataKeepsNothing()
    {
        QTemporaryDir dir;
        long long left = 1 << 20;
        QBuffer npz;
        npz.setData(npyfixtures::npzFile(10, 8, 8));
        npz.open(QIODevice::ReadOnly);
        const ScreenRefRegion r = extractRegionRefs(&npz, 0, anchor(0, 50), dir.path(), &left);
        QVERIFY2(r.status.startsWith(nameOf(kScreenRefsNoFrames)), qPrintable(r.status));
        QVERIFY(r.frames.isEmpty());
        QVERIFY(!QDir(dir.filePath("screenrefs/region0")).exists());
    }

    void aReadThatIsNotAliveStopsInsideTheRegion()
    {
        QTemporaryDir dir;
        long long left = 1 << 20;
        QBuffer npz;
        npz.setData(npyfixtures::npzFile(10, 8, 8));
        npz.open(QIODevice::ReadOnly);
        int asked = 0;
        const ScreenRefRegion r = extractRegionRefs(&npz, 0, sequence(10, 1), dir.path(), &left, 64, [&] { return ++asked < 2; });
        QVERIFY2(r.status.startsWith(nameOf(kCancelled)), qPrintable(r.status));
        QVERIFY(r.frames.isEmpty());
        QCOMPARE(left, 1LL << 20);
    }

    void aBrokenFramesFileLeavesNothingBehind()
    {
        QTemporaryDir dir;
        long long left = 1 << 20;
        QByteArray file = npyfixtures::npzFile(6, 20, 20, false);
        QBuffer good;
        good.setData(file);
        good.open(QIODevice::ReadOnly);
        QVERIFY(extractRegionRefs(&good, 0, sequence(4, 1), dir.path(), &left).ready());
        const long long afterGood = left;

        // A pickle where the array should be, a truncated array, and a file that is not a zip.
        const QByteArray bad[] = {importfixtures::buildZip({npyfixtures::zipMember("frames.npy", QByteArray("\x80\x04 cos\nsystem\n"), false, false, false, 0)}),
                                  importfixtures::buildZip({npyfixtures::zipMember("frames.npy", npyfixtures::npyFile(6, 20, 20).chopped(100), true, false, false, 0)}), QByteArray(300, 'x')};
        const QString reasons[] = {nameOf(kNpyNotNpy), nameOf(kNpyTruncated), nameOf(kNpzNotAZip)};
        for (int i = 0; i < 3; ++i) {
            QBuffer b;
            b.setData(bad[i]);
            b.open(QIODevice::ReadOnly);
            const ScreenRefRegion r = extractRegionRefs(&b, 5 + i, anchor(), dir.path(), &left);
            QVERIFY2(r.status.startsWith(reasons[i]), qPrintable(r.status));
            QVERIFY(r.frames.isEmpty());
            QVERIFY(!QDir(dir.filePath(QString("screenrefs/region%1").arg(5 + i))).exists());
            QCOMPARE(left, afterGood);
        }
    }

    void theBudgetStopsARegionAndGivesBackWhatItWrote()
    {
        QTemporaryDir dir;
        long long left = 700; // room for about one small picture
        QBuffer npz;
        npz.setData(npyfixtures::npzFile(8, 60, 60));
        npz.open(QIODevice::ReadOnly);
        const long long before = left;
        const ScreenRefRegion r = extractRegionRefs(&npz, 1, sequence(8, 1), dir.path(), &left);
        QVERIFY2(r.status.startsWith(nameOf(kOutputOverBudget)), qPrintable(r.status));
        QVERIFY(r.frames.isEmpty());
        QVERIFY(!QDir(dir.filePath("screenrefs/region1")).exists());
        QCOMPARE(left, before); // what was written before the stop is removed, and its bytes are given back
    }
};

QTEST_GUILESS_MAIN(TstScreenFrames)
#include "tst_screenframes.moc"
