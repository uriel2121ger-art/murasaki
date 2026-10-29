#include "animationinfo.h"
#include "animfixtures.h"
#include "petlimits.h"
#include "testutil.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>

using namespace Qt::StringLiterals;
using namespace murasaki;
using namespace animfixtures;

namespace {

std::optional<AnimationInfo> read(std::optional<AnimationInfo> (*reader)(QIODevice *), QByteArray data)
{
    QBuffer buffer(&data);
    buffer.open(QIODevice::ReadOnly);
    return reader(&buffer);
}

} // namespace

class TestAnimationInfo : public QObject
{
    Q_OBJECT

private slots:
    void apngSizeFramesAndFps();
    void apngAveragesMixedDelays();
    void apngZeroDenominatorMeansHundredths();
    void apngWithHiddenDefaultImage();
    void apngStillPngIsOneFrame();
    void apngTinyDelaysCountAsOneHundredMilliseconds();
    void webpTinyDurationsCountAsOneHundredMilliseconds();
    void chunkFloodsAreRejectedQuickly();
    void realWorldChunkCountsStayWithinTheCap();
    void manyFramesStayWithinTheCap();
    void webpStillLosslessNeedsOnlyFiveBytes();
    void apngRejectsMalformed_data();
    void apngRejectsMalformed();
    void apngLargeFileIsNotReadAsAWhole();
    void gifSizeFramesAndFps();
    void gifTreatsTinyDelaysAsTenCentiseconds();
    void theThreeFormatsApplyOneRuleToTinyDelays();
    void gifRejectsMalformed_data();
    void gifRejectsMalformed();
    void webpAnimatedFile();
    void webpStillFile();
    void webpRejectsMalformed_data();
    void webpRejectsMalformed();
    void readAnimationInfoSniffsTheFormat();
    void extensions();
    void aPngThatIsAnApngIsAnAnimationFile();
    void aGifWithASecondStreamAppendedCountsBothStreams();
    void aGifWithJunkWhereABlockShouldBeIsRejected();
    void aPngWithTooManyChunksOrAHugeLengthIsNoAnimation();
};

void TestAnimationInfo::aPngThatIsAnApngIsAnAnimationFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto put = [&](const QString &name, const QByteArray &bytes) {
        const QString path = dir.filePath(name);
        testutil::writeFile(path, bytes);
        return path;
    };
    // an APNG named .png (acTL before the first IDAT): an animation, in any case of the suffix
    QVERIFY(isAnimationFile(put(u"anim.png"_s, apng(4, 3, {{1, 10}, {1, 10}}))));
    QVERIFY(isAnimationFile(put(u"ANIM.PNG"_s, apng(4, 3, {{1, 10}, {1, 10}}))));
    QVERIFY(isAnimationFile(put(u"anim.apng"_s, apng(4, 3, {{1, 10}, {1, 10}}))));
    // a still picture is not
    QVERIFY(!isAnimationFile(put(u"still.png"_s, apng(4, 3, {}))));
    // an acTL after the first IDAT is ignored by the specification: still
    QByteArray late("\x89PNG\r\n\x1a\n", 8);
    late += pngChunk("IHDR", be32(4) + be32(3) + QByteArray("\x08\x06\x00\x00\x00", 5)) + pngChunk("IDAT", zlibZeros(1 + 16 * 3))
        + pngChunk("acTL", be32(1) + be32(0)) + pngChunk("IEND", {});
    QVERIFY(!isAnimationFile(put(u"late.png"_s, late)));
    // not a PNG at all, nothing there, other suffixes, a chunk that claims more than the file holds
    QVERIFY(!isAnimationFile(put(u"fake.png"_s, "GIF89a not a png")));
    QVERIFY(!isAnimationFile(dir.filePath(u"missing.png"_s)));
    QVERIFY(!isAnimationFile(put(u"photo.jpg"_s, apng(4, 3, {{1, 10}, {1, 10}}))));
    QByteArray cut = apng(4, 3, {{1, 10}, {1, 10}});
    cut.truncate(8 + 25 + 6);
    QVERIFY(!isAnimationFile(put(u"cut.png"_s, cut)));
    // a gif or webp is an animation by name alone (its header is read later)
    QVERIFY(isAnimationFile(dir.filePath(u"x.gif"_s)));
    QVERIFY(isAnimationFile(dir.filePath(u"x.webp"_s)));
}

void TestAnimationInfo::aGifWithASecondStreamAppendedCountsBothStreams()
{
    // two GIFs joined: the first has no trailer, the second starts with its own header. FFmpeg and Pillow decode the frames of both
    QByteArray joined = gif(8, 8, {5, 5});
    joined.chop(1);
    joined += gif(8, 8, {5});
    const auto info = read(readGifInfo, joined);
    QVERIFY(info);
    QCOMPARE(info->frames, 3);
    QCOMPARE(info->width, 8);
    // the joined file still needs its (last) trailer
    QByteArray open = joined;
    open.chop(1);
    QVERIFY(!read(readGifInfo, open));
}

void TestAnimationInfo::aGifWithJunkWhereABlockShouldBeIsRejected()
{
    QByteArray junk = gif(8, 8, {5, 5});
    junk.chop(1);
    QVERIFY(!read(readGifInfo, junk + "Gxyz12345678901234"));              // a G that does not start a header
    QVERIFY(!read(readGifInfo, junk + "GIF89a" + le16(0) + le16(8) + QByteArray("\x00\x00\x00", 3) + "\x3b")); // a header of no size
    QByteArray many = gif(8, 8, {5});
    many.chop(1);
    QByteArray endless;
    for (int i = 0; i < 70; ++i)
        endless += many;
    QVERIFY(!read(readGifInfo, endless + QByteArray("\x3b", 1))); // more restarts than any file has
    // the limit itself: 64 headers after the first are read, 65 are not
    QByteArray atLimit;
    for (int i = 0; i < 65; ++i)
        atLimit += many;
    QVERIFY(read(readGifInfo, atLimit + QByteArray("\x3b", 1)));
    QCOMPARE(read(readGifInfo, atLimit + QByteArray("\x3b", 1))->frames, 65);
    QVERIFY(!read(readGifInfo, atLimit + many + QByteArray("\x3b", 1)));
}

void TestAnimationInfo::aPngWithTooManyChunksOrAHugeLengthIsNoAnimation()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray signature("\x89PNG\r\n\x1a\n", 8);
    const QByteArray ihdr = pngChunk("IHDR", be32(4) + be32(4) + QByteArray("\x08\x06\x00\x00\x00", 5));
    const QByteArray actl = pngChunk("acTL", be32(1) + be32(0));
    // acTL after more ancillary chunks than the walk allows (IHDR and 64 more): not looked for, whatever follows. A real APNG has it right
    // after the IHDR, so a hostile folder of such files costs 65 small reads each.
    const QByteArray tiny = pngChunk("tEXt", {});
    const auto withChunksBefore = [&](int count) {
        QByteArray out = signature + ihdr;
        for (int i = 0; i < count; ++i)
            out += tiny;
        return out + actl + pngChunk("IEND", {});
    };
    const QString found = dir.filePath(u"under.png"_s);
    testutil::writeFile(found, withChunksBefore(60));
    QVERIFY(isAnimationFile(found));
    // the budget is exact: the IHDR and 64 more chunks are read, so the acTL may be the 64th chunk after the IHDR but not the 65th
    const QString atBudget = dir.filePath(u"at.png"_s);
    testutil::writeFile(atBudget, withChunksBefore(63)); // IHDR, 63 tEXt, acTL: the 65th chunk
    QVERIFY(isAnimationFile(atBudget));
    const QString pastBudget = dir.filePath(u"past65.png"_s);
    testutil::writeFile(pastBudget, withChunksBefore(64)); // acTL is the 66th
    QVERIFY(!isAnimationFile(pastBudget));
    const QString capped = dir.filePath(u"capped.png"_s);
    testutil::writeFile(capped, withChunksBefore(70));
    QVERIFY(!isAnimationFile(capped));
    const QString wayOver = dir.filePath(u"wayover.png"_s);
    testutil::writeFile(wayOver, withChunksBefore(int(kMaxPetChunks) + 10));
    QVERIFY(!isAnimationFile(wayOver));
    // a chunk that claims more than 0x7fffffff bytes (or more than the file holds) before the acTL
    QByteArray huge = signature + ihdr + be32(0x80000000u) + QByteArray("tEXt", 4) + QByteArray(16, 0) + actl;
    const QString hugeFile = dir.filePath(u"huge.png"_s);
    testutil::writeFile(hugeFile, huge);
    QVERIFY(!isAnimationFile(hugeFile));
    QByteArray past = signature + ihdr + be32(5000) + QByteArray("tEXt", 4) + QByteArray(16, 0) + actl;
    const QString pastFile = dir.filePath(u"past.png"_s);
    testutil::writeFile(pastFile, past);
    QVERIFY(!isAnimationFile(pastFile));
}

void TestAnimationInfo::apngSizeFramesAndFps()
{
    const auto info = read(readApngInfo, apng(500, 666, {{1, 60}, {1, 60}, {1, 60}, {1, 60}, {1, 60}, {1, 60}}));
    QVERIFY(info);
    QCOMPARE(info->format, AnimationFormat::Apng);
    QCOMPARE(info->width, 500);
    QCOMPARE(info->height, 666);
    QCOMPARE(info->frames, 6);
    QCOMPARE(info->fps, 60.0);
}

void TestAnimationInfo::apngAveragesMixedDelays()
{
    // 0.1 s + 0.1 s + 0.2 s over 3 frames = 0.4 s -> 7.5 fps
    const auto info = read(readApngInfo, apng(8, 8, {{1, 10}, {1, 10}, {2, 10}}));
    QVERIFY(info);
    QCOMPARE(info->frames, 3);
    QCOMPARE(info->fps, 7.5);
}

void TestAnimationInfo::apngZeroDenominatorMeansHundredths()
{
    const auto info = read(readApngInfo, apng(8, 8, {{10, 0}, {10, 0}}));
    QVERIFY(info);
    QCOMPARE(info->fps, 10.0);
}

void TestAnimationInfo::apngWithHiddenDefaultImage()
{
    const auto info = read(readApngInfo, apng(16, 9, {{1, 25}, {1, 25}}, /*defaultIsFrame=*/false));
    QVERIFY(info);
    QCOMPARE(info->frames, 2);
    QCOMPARE(info->fps, 25.0);
}

void TestAnimationInfo::apngStillPngIsOneFrame()
{
    const auto info = read(readApngInfo, apng(4, 3, {}));
    QVERIFY(info);
    QCOMPARE(info->frames, 1);
    QCOMPARE(info->fps, 0.0);
    QCOMPARE(info->width, 4);
    QCOMPARE(info->height, 3);
}

void TestAnimationInfo::apngTinyDelaysCountAsOneHundredMilliseconds()
{
    // 0, 1 ms, 2 ms and 10 ms itself are all "as fast as you can": 100 ms (petFrameDelayMs); 20 ms stays 50 fps.
    for (const Delay delay : {Delay{0, 1}, Delay{1, 1000}, Delay{1, 500}, Delay{1, 100}, Delay{0, 0}}) {
        const auto info = read(readApngInfo, apng(4, 4, {delay, delay}));
        QVERIFY(info);
        QCOMPARE(info->frames, 2);
        QCOMPARE(info->fps, 10.0);
    }
    QCOMPARE(read(readApngInfo, apng(4, 4, {{1, 50}, {1, 50}}))->fps, 50.0);
    QCOMPARE(read(readApngInfo, apng(4, 4, {{11, 1000}, {11, 1000}}))->fps, 90.91);
}

void TestAnimationInfo::webpTinyDurationsCountAsOneHundredMilliseconds()
{
    QCOMPARE(read(readWebpInfo, webpAnimated(8, 8, {0, 1, 5}))->fps, 10.0);
    QCOMPARE(read(readWebpInfo, webpAnimated(8, 8, {10, 10}))->fps, 10.0);
    QCOMPARE(read(readWebpInfo, webpAnimated(8, 8, {20, 20}))->fps, 50.0);
}

void TestAnimationInfo::chunkFloodsAreRejectedQuickly()
{
    // 600k empty chunks, more than a 2 GiB file in 4 KiB pieces has: the walk must stop at its cap instead of crawling the whole file.
    QByteArray apngFlood = QByteArray("\x89PNG\r\n\x1a\n", 8)
        + pngChunk("IHDR", be32(8) + be32(8) + QByteArray("\x08\x06\x00\x00\x00", 5));
    const QByteArray empty = pngChunk("tEXt", {});
    for (int i = 0; i < 600000; ++i)
        apngFlood += empty;
    apngFlood += pngChunk("IDAT", zlibZeros(8 * 33)) + pngChunk("IEND", {});

    QByteArray webpChunks = riffChunk("VP8X", QByteArray("\x12\x00\x00\x00", 4) + le24(7) + le24(7));
    const QByteArray emptyRiff = riffChunk("JUNK", {});
    for (int i = 0; i < 600000; ++i)
        webpChunks += emptyRiff;
    const QByteArray webpFlood = riffWebp(webpChunks);

    QElapsedTimer timer;
    timer.start();
    QVERIFY(!read(readApngInfo, apngFlood));
    QVERIFY(!read(readWebpInfo, webpFlood));
    QVERIFY2(timer.elapsed() < 2000, qPrintable(u"took %1 ms"_s.arg(timer.elapsed())));

    // The same size within the cap is still fine.
    QByteArray ok = apngFlood.left(8 + 25);
    for (int i = 0; i < 1000; ++i)
        ok += empty;
    ok += pngChunk("IDAT", zlibZeros(8 * 33)) + pngChunk("IEND", {});
    QVERIFY(read(readApngInfo, ok));
}

void TestAnimationInfo::realWorldChunkCountsStayWithinTheCap()
{
    // A 300 MB Workshop APNG has about 75k chunks (868 frames in ~4 KB fdAT pieces).
    QByteArray data = QByteArray("\x89PNG\r\n\x1a\n", 8)
        + pngChunk("IHDR", be32(2) + be32(2) + QByteArray("\x08\x06\x00\x00\x00", 5))
        + pngChunk("acTL", be32(1) + be32(0))
        + pngChunk("fcTL", be32(0) + be32(2) + be32(2) + be32(0) + be32(0) + be16(1) + be16(60) + QByteArray(2, 0))
        + pngChunk("IDAT", zlibZeros(18));
    const QByteArray piece = pngChunk("fdAT", be32(1) + QByteArray(4, 0));
    for (int i = 0; i < 80000; ++i)
        data += piece;
    data += pngChunk("IEND", {});
    QVERIFY(read(readApngInfo, data));

    // The cap is sized for the largest file a pet may be (2 GiB) written in 4 KiB pieces.
    QByteArray big = data.left(data.size() - 12 - 80000 * piece.size());
    for (qint64 i = 0; i < kMaxPetChunks - 100; ++i)
        big += piece;
    big += pngChunk("IEND", {});
    QVERIFY(read(readApngInfo, big));
}

void TestAnimationInfo::manyFramesStayWithinTheCap()
{
    // A 5000-frame animation (fcTL + fdAT each) is far below the cap.
    QByteArray data = QByteArray("\x89PNG\r\n\x1a\n", 8)
        + pngChunk("IHDR", be32(2) + be32(2) + QByteArray("\x08\x06\x00\x00\x00", 5))
        + pngChunk("acTL", be32(5000) + be32(0));
    const QByteArray pixels = zlibZeros(2 * 9);
    quint32 sequence = 0;
    for (int i = 0; i < 5000; ++i) {
        data += pngChunk("fcTL", be32(sequence++) + be32(2) + be32(2) + be32(0) + be32(0) + be16(1) + be16(60)
                                     + QByteArray(2, 0));
        data += i == 0 ? pngChunk("IDAT", pixels) : pngChunk("fdAT", be32(sequence++) + pixels);
    }
    data += pngChunk("IEND", {});
    const auto info = read(readApngInfo, data);
    QVERIFY(info);
    QCOMPARE(info->frames, 5000);
    QCOMPARE(info->fps, 60.0);
}

void TestAnimationInfo::webpStillLosslessNeedsOnlyFiveBytes()
{
    // A VP8L chunk of exactly 5 bytes (signature + packed size) still yields the canvas.
    const quint32 bits = quint32(9) | (quint32(4) << 14);
    const QByteArray data = riffWebp(riffChunk("VP8L", QByteArray("\x2f", 1) + le32(bits)));
    const auto info = read(readWebpInfo, data);
    QVERIFY(info);
    QCOMPARE(info->width, 10);
    QCOMPARE(info->height, 5);
    QVERIFY(!read(readWebpInfo, riffWebp(riffChunk("VP8L", QByteArray("\x2f\x00", 2)))));
}

void TestAnimationInfo::apngRejectsMalformed_data()
{
    QTest::addColumn<QByteArray>("data");
    const QByteArray good = apng(8, 8, {{1, 10}, {1, 10}});
    QTest::newRow("empty") << QByteArray();
    QTest::newRow("not a png") << QByteArray(200, 'x');
    QTest::newRow("signature only") << good.left(8);
    QTest::newRow("truncated before IEND") << good.left(good.size() - 12);
    QTest::newRow("truncated mid chunk") << good.left(60);
    QTest::newRow("frame count mismatch")
        << (QByteArray("\x89PNG\r\n\x1a\n", 8)
            + pngChunk("IHDR", be32(8) + be32(8) + QByteArray("\x08\x06\x00\x00\x00", 5))
            + pngChunk("acTL", be32(3) + be32(0))
            + pngChunk("fcTL", be32(0) + be32(8) + be32(8) + be32(0) + be32(0) + be16(1) + be16(10) + QByteArray(2, 0))
            + pngChunk("IDAT", zlibZeros(8 * 33)) + pngChunk("IEND", {}));
    QTest::newRow("zero width") << apng(0, 8, {{1, 10}});
    QTest::newRow("absurd width") << apng(70000, 8, {{1, 10}});
    QTest::newRow("no pixel data")
        << (QByteArray("\x89PNG\r\n\x1a\n", 8)
            + pngChunk("IHDR", be32(8) + be32(8) + QByteArray("\x08\x06\x00\x00\x00", 5)) + pngChunk("IEND", {})
            + QByteArray(20, 0));
    QByteArray hugeLength = good;
    hugeLength.replace(8, 4, be32(0x7fffffffu));
    QTest::newRow("chunk longer than the file") << hugeLength;
    QByteArray wrongFirst = good;
    wrongFirst.replace(12, 4, "tEXt");
    QTest::newRow("IHDR not first") << wrongFirst;
}

void TestAnimationInfo::apngRejectsMalformed()
{
    QFETCH(QByteArray, data);
    QVERIFY(!read(readApngInfo, data));
}

void TestAnimationInfo::apngLargeFileIsNotReadAsAWhole()
{
    // One 64 MB IDAT of zeros (sparse on disk): the reader must seek over it, not read it.
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"big.apng"_s);
    const QByteArray head = QByteArray("\x89PNG\r\n\x1a\n", 8)
        + pngChunk("IHDR", be32(100) + be32(100) + QByteArray("\x08\x06\x00\x00\x00", 5))
        + pngChunk("acTL", be32(1) + be32(0))
        + pngChunk("fcTL", be32(0) + be32(100) + be32(100) + be32(0) + be32(0) + be16(1) + be16(30) + QByteArray(2, 0));
    constexpr qint64 idatSize = 64 * 1024 * 1024;
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(head);
    file.write(be32(quint32(idatSize)) + QByteArray("IDAT", 4));
    QVERIFY(file.resize(file.pos() + idatSize + 4)); // data and CRC: zeros
    file.seek(file.size());
    file.write(pngChunk("IEND", {}));
    file.close();

    const auto info = readAnimationInfo(path);
    QVERIFY(info);
    QCOMPARE(info->width, 100);
    QCOMPARE(info->frames, 1);
}

void TestAnimationInfo::gifSizeFramesAndFps()
{
    const auto info = read(readGifInfo, gif(320, 200, {4, 4, 4, 4, 4}));
    QVERIFY(info);
    QCOMPARE(info->format, AnimationFormat::Gif);
    QCOMPARE(info->width, 320);
    QCOMPARE(info->height, 200);
    QCOMPARE(info->frames, 5);
    QCOMPARE(info->fps, 25.0);
}

void TestAnimationInfo::theThreeFormatsApplyOneRuleToTinyDelays()
{
    // the same animation of four frames, delays 0, 10, 20 and 100 ms, in each format: 100 + 100 + 20 + 100 = 320 ms
    const auto gifInfo = read(readGifInfo, gif(8, 8, {0, 1, 2, 10}));
    const auto apngInfo = read(readApngInfo, apng(4, 4, {{0, 1}, {1, 100}, {1, 50}, {1, 10}}));
    const auto webpInfo = read(readWebpInfo, webpAnimated(8, 8, {0, 10, 20, 100}));
    QVERIFY(gifInfo && apngInfo && webpInfo);
    QCOMPARE(gifInfo->fps, 12.5);
    QCOMPARE(apngInfo->fps, gifInfo->fps);
    QCOMPARE(webpInfo->fps, gifInfo->fps);
    // and the rule itself
    QCOMPARE(petFrameDelayMs(0), 100.0);
    QCOMPARE(petFrameDelayMs(-5), 100.0);
    QCOMPARE(petFrameDelayMs(std::nan("")), 100.0);
    QCOMPARE(petFrameDelayMs(10), 100.0);
    QCOMPARE(petFrameDelayMs(10.0 + 2e-15), 100.0); // a timestamp's rounding: still 10 ms
    QCOMPARE(petFrameDelayMs(10.5), 10.5);
    QCOMPARE(petFrameDelayMs(20), 20.0);
}

void TestAnimationInfo::gifTreatsTinyDelaysAsTenCentiseconds()
{
    // 0, 1 and a missing GCE all count as 10 cs; 20 cs stays: (10 + 10 + 10 + 20) cs over 4 frames = 8 fps
    const auto info = read(readGifInfo, gif(8, 8, {0, 1, -1, 20}));
    QVERIFY(info);
    QCOMPARE(info->frames, 4);
    QCOMPARE(info->fps, 8.0);
}

void TestAnimationInfo::gifRejectsMalformed_data()
{
    QTest::addColumn<QByteArray>("data");
    const QByteArray good = gif(8, 8, {5, 5});
    QTest::newRow("empty") << QByteArray();
    QTest::newRow("bad magic") << (QByteArray("GIF90a") + good.mid(6));
    QTest::newRow("header only") << good.left(13);
    QTest::newRow("no trailer") << good.left(good.size() - 1);
    QTest::newRow("truncated in image data") << good.left(good.size() - 4);
    QTest::newRow("zero size") << gif(0, 8, {5});
    QTest::newRow("no frames") << gif(8, 8, {});
    QByteArray garbage = good;
    garbage.insert(garbage.size() - 1, '\x99');
    QTest::newRow("unknown block") << garbage;
}

void TestAnimationInfo::gifRejectsMalformed()
{
    QFETCH(QByteArray, data);
    QVERIFY(!read(readGifInfo, data));
}

void TestAnimationInfo::webpAnimatedFile()
{
    const auto info = read(readWebpInfo, webpAnimated(640, 480, {40, 40, 40}));
    QVERIFY(info);
    QCOMPARE(info->format, AnimationFormat::Webp);
    QCOMPARE(info->width, 640);
    QCOMPARE(info->height, 480);
    QCOMPARE(info->frames, 3);
    QCOMPARE(info->fps, 25.0);
}

void TestAnimationInfo::webpStillFile()
{
    const auto info = read(readWebpInfo, webpStillLossless(123, 77));
    QVERIFY(info);
    QCOMPARE(info->width, 123);
    QCOMPARE(info->height, 77);
    QCOMPARE(info->frames, 1);
    QCOMPARE(info->fps, 0.0);
}

void TestAnimationInfo::webpRejectsMalformed_data()
{
    QTest::addColumn<QByteArray>("data");
    const QByteArray good = webpAnimated(32, 32, {50, 50});
    QTest::newRow("empty") << QByteArray();
    QTest::newRow("not riff") << (QByteArray("RIFX") + good.mid(4));
    QTest::newRow("not webp") << (good.left(8) + QByteArray("WEBX") + good.mid(12));
    QTest::newRow("truncated") << good.left(good.size() - 5);
    QTest::newRow("header only") << good.left(12);
    QTest::newRow("animated without frames") << webpAnimated(32, 32, {});
    QTest::newRow("absurd canvas") << webpAnimated(40000, 32, {50});
    QTest::newRow("chunk past the end") << (good.left(12) + QByteArray("VP8X") + le32(0x10000000));
}

void TestAnimationInfo::webpRejectsMalformed()
{
    QFETCH(QByteArray, data);
    QVERIFY(!read(readWebpInfo, data));
}

void TestAnimationInfo::readAnimationInfoSniffsTheFormat()
{
    QTemporaryDir tmp;
    // The extension lies on purpose: the content decides.
    testutil::writeFile(tmp.filePath(u"a.gif"_s), apng(10, 20, {{1, 20}, {1, 20}}));
    testutil::writeFile(tmp.filePath(u"b.png"_s), gif(30, 40, {10, 10}));
    testutil::writeFile(tmp.filePath(u"c.apng"_s), webpAnimated(50, 60, {100, 100}));
    testutil::writeFile(tmp.filePath(u"d.apng"_s), "not an image at all");

    const auto a = readAnimationInfo(tmp.filePath(u"a.gif"_s));
    QVERIFY(a);
    QCOMPARE(a->format, AnimationFormat::Apng);
    QCOMPARE(a->width, 10);
    const auto b = readAnimationInfo(tmp.filePath(u"b.png"_s));
    QVERIFY(b);
    QCOMPARE(b->format, AnimationFormat::Gif);
    QCOMPARE(b->height, 40);
    const auto c = readAnimationInfo(tmp.filePath(u"c.apng"_s));
    QVERIFY(c);
    QCOMPARE(c->format, AnimationFormat::Webp);
    QCOMPARE(c->fps, 10.0);
    QVERIFY(!readAnimationInfo(tmp.filePath(u"d.apng"_s)));
    QVERIFY(!readAnimationInfo(tmp.filePath(u"missing.apng"_s)));
}

void TestAnimationInfo::extensions()
{
    QVERIFY(isAnimationExtension(u"apng"_s));
    QVERIFY(isAnimationExtension(u"GIF"_s));
    QVERIFY(isAnimationExtension(u"WebP"_s));
    QVERIFY(!isAnimationExtension(u"png"_s));
    QVERIFY(!isAnimationExtension(u"webm"_s));
    QVERIFY(!isAnimationExtension(QString()));
}

QTEST_APPLESS_MAIN(TestAnimationInfo)
#include "tst_animationinfo.moc"
