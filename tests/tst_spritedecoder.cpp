// SpriteDecoder: the whole animation decoded once, scaled, premultiplied, with its frame durations. The files are
// made at run time: an APNG written here, and a WebM and a GIF the ffmpeg command line makes out of it (those
// cases skip when ffmpeg or its VP9 encoder is missing).

#include "animationinfo.h"
#include "animfixtures.h"
#include "spritedecoder.h"
#include "testutil.h"

#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <cmath>
#include <numeric>
#include <thread>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

constexpr int kSize = 64;

// x 0..15 half transparent (alpha 128), x 16..31 opaque, the right half fully transparent. Straight pixels.
QImage halfImage(QColor color)
{
    QImage image(kSize, kSize, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize / 2; ++x) {
            QColor pixel = color;
            pixel.setAlpha(x < kSize / 4 ? 128 : 255);
            image.setPixelColor(x, y, pixel);
        }
    }
    return image;
}

QByteArray rawPixels(const QImage &image)
{
    QByteArray raw;
    for (int y = 0; y < image.height(); ++y) {
        raw.append('\0'); // filter type: none
        raw.append(reinterpret_cast<const char *>(image.constScanLine(y)), image.width() * 4);
    }
    return qCompress(raw, 9).mid(4); // a zlib stream: qCompress prefixes a 4-byte length
}

// Red, then green, then blue; each shown for `delayMs`, or for the delays given in milliseconds (one per frame, over 1000).
QByteArray alphaApng(int delayMs = 100, const QList<int> &delays = {}, int denominator = 1000)
{
    using namespace animfixtures;
    const QColor colors[] = {Qt::red, Qt::green, Qt::blue};
    QByteArray out("\x89PNG\r\n\x1a\n", 8);
    out += pngChunk("IHDR", be32(kSize) + be32(kSize) + QByteArray("\x08\x06\x00\x00\x00", 5));
    out += pngChunk("acTL", be32(3) + be32(0));
    quint32 sequence = 0;
    for (int i = 0; i < 3; ++i) {
        out += pngChunk("fcTL", be32(sequence++) + be32(kSize) + be32(kSize) + be32(0) + be32(0)
                                    + be16(delays.isEmpty() ? delayMs : delays.value(i)) + be16(denominator) + QByteArray("\x00\x00", 2));
        const QByteArray data = rawPixels(halfImage(colors[i]));
        out += i == 0 ? pngChunk("IDAT", data) : pngChunk("fdAT", be32(sequence++) + data);
    }
    out += pngChunk("IEND", {});
    return out;
}

// A GIF of 1x1 frames with the given delays in centiseconds (-1: no graphic control block). The picture data is the smallest
// valid one, so FFmpeg really decodes the file.
QByteArray tinyGif(const QList<int> &centiseconds)
{
    using namespace animfixtures;
    QByteArray out("GIF89a", 6);
    out += le16(1) + le16(1) + QByteArray("\x80\x00\x00", 3) + QByteArray("\xff\xff\xff\x00\x00\x00", 6);
    for (const int delay : centiseconds) {
        if (delay >= 0)
            out += QByteArray("\x21\xf9\x04\x00", 4) + le16(quint16(delay)) + QByteArray("\x00\x00", 2);
        out += QByteArray(",\x00\x00\x00\x00\x01\x00\x01\x00\x00", 10) + QByteArray("\x02\x02\x44\x01\x00", 5);
    }
    return out + ';';
}

// Sets the delay of every ANMF chunk of a WebP file the encoder made, in milliseconds.
QByteArray withWebpDelays(QByteArray webp, const QList<int> &milliseconds)
{
    int frame = 0;
    for (qsizetype pos = 12; pos + 8 <= webp.size();) {
        const quint32 length = qFromLittleEndian<quint32>(webp.constData() + pos + 4);
        if (webp.mid(pos, 4) == "ANMF" && frame < milliseconds.size()) {
            const int delay = milliseconds.at(frame++);
            for (int byte = 0; byte < 3; ++byte)
                webp[pos + 8 + 12 + byte] = char((delay >> (8 * byte)) & 0xff);
        }
        pos += 8 + length + (length & 1);
    }
    return webp;
}

} // namespace

class TestSpriteDecoder : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void apngKeepsFramesDurationsAndPremultipliedAlpha();
    void aProbeKeepsOnlyTheChosenFramesAndTheLast();
    void aProbeOfAnUnreadableFileSaysWhy();
    void aProbeRefusesWhatWouldNotFitItsBudget();
    void aGifWithASecondStreamAppendedDecodesAllItsFramesAndOurCountAgrees();
    void aProbeReadsGifAndWebpAndPicksTheMiddle();
    void aProbeRefusesABadSizeAndNeverPassesATruncatedFileAsWhole();
    void aProbeOfATruncatedWebpFailsInsteadOfCountingFewerFrames();
    void framesAreScaledToTheTarget();
    void headerEstimateIsFramesTimesPixels();
    void overBudgetFailsBeforeDecoding();
    void budgetIsAlsoEnforcedWhileDecoding();
    void aCancelledDecodeStops();
    void unreadableOrForeignFilesFail();
    void onlyLocalFilesAreOpened();
    void webmKeepsItsAlpha();
    void gifIsDecodedToo();
    void animatedWebpIsDecodedToo();
    void aFixedRateReplacesTheFilesDelays();
    void tinyDelaysPlayForOneHundredMillisecondsInEveryFormat();
    void hugeCanvasesAndFramesOverTheBudgetAreRefusedOnTheHeader();
    void aTruncatedFileFailsInsteadOfPlayingHalf();
    void aCancelledTaskDoesNotEvenOpenTheFile();
    void hdAndSdWebmColorsMatchTheirTags_data();
    void hdAndSdWebmColorsMatchTheirTags();

private:
    // Converts the APNG with the ffmpeg command line; empty when that is not possible.
    QString convert(const QString &name, const QStringList &args);
    QString makeWebm()
    {
        return convert(u"alpha.webm"_s, {u"-c:v"_s, u"libvpx-vp9"_s, u"-pix_fmt"_s, u"yuva420p"_s, u"-auto-alt-ref"_s,
                                         u"0"_s, u"-b:v"_s, u"0"_s, u"-crf"_s, u"10"_s});
    }

    QTemporaryDir m_dir;
    QString m_apng;
};

void TestSpriteDecoder::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_apng = m_dir.filePath(u"alpha.apng"_s);
    testutil::writeFile(m_apng, alphaApng());
}

QString TestSpriteDecoder::convert(const QString &name, const QStringList &args)
{
    const QString ffmpeg = QStandardPaths::findExecutable(u"ffmpeg"_s);
    if (ffmpeg.isEmpty())
        return {};
    const QString out = m_dir.filePath(name);
    QProcess process;
    process.start(ffmpeg, QStringList{u"-nostdin"_s, u"-v"_s, u"error"_s, u"-y"_s, u"-f"_s, u"apng"_s, u"-i"_s, m_apng}
                              + args + QStringList{out});
    if (!process.waitForFinished(60000) || process.exitCode() != 0)
        return {};
    return out;
}

void TestSpriteDecoder::apngKeepsFramesDurationsAndPremultipliedAlpha()
{
    const SpriteDecoder::Result result = SpriteDecoder::decode(m_apng, {}, 64 << 20);
    QVERIFY2(result.ok, qPrintable(result.error));
    const SpriteFrames &frames = result.frames;
    QCOMPARE(frames.images.size(), 3);
    QCOMPARE(frames.durationsMs, (QList<int>{100, 100, 100}));
    QCOMPARE(frames.size, QSize(kSize, kSize));
    QCOMPARE(frames.bytes, qint64(3 * kSize * kSize * 4));
    for (const QImage &image : frames.images) {
        QCOMPARE(image.format(), QImage::Format_RGBA8888_Premultiplied);
        QCOMPARE(image.size(), QSize(kSize, kSize));
    }
    // Frame 0 is red: half transparent on the left (premultiplied: 255 * 128 / 255), opaque in the middle,
    // nothing on the right.
    const QImage &red = frames.images.at(0);
    QCOMPARE(red.pixelColor(4, 10).alpha(), 128);
    const uchar *raw = red.constScanLine(10) + 4 * 4;
    QVERIFY2(qAbs(raw[0] - 128) <= 1 && raw[1] == 0 && raw[2] == 0 && raw[3] == 128, "premultiplied bytes");
    QCOMPARE(red.pixelColor(20, 10), QColor(255, 0, 0, 255));
    QCOMPARE(red.pixelColor(50, 10).alpha(), 0);
    QCOMPARE(frames.images.at(1).pixelColor(20, 10), QColor(0, 255, 0, 255));
    QCOMPARE(frames.images.at(2).pixelColor(20, 10), QColor(0, 0, 255, 255));
}

void TestSpriteDecoder::aProbeKeepsOnlyTheChosenFramesAndTheLast()
{
    // the sweep's streaming read: the count and the span of the times come from the decode, the pictures are the ones asked for
    const SpriteDecoder::Probe probe = SpriteDecoder::probe(m_apng, {1, 99});
    QVERIFY2(probe.ok, qPrintable(probe.error));
    QCOMPARE(probe.frames, qint64(3));
    QCOMPARE(probe.size, QSize(kSize, kSize));
    QVERIFY2(qAbs(probe.durationMs - 300.0) < 1.0, qPrintable(QString::number(probe.durationMs)));
    QCOMPARE(probe.images.keys(), (QList<qint64>{1, 2})); // frame 1 and the last; 99 does not exist and is not made up; frame 0 was not asked for
    const SpriteDecoder::Result whole = SpriteDecoder::decode(m_apng, {}, 64 << 20);
    QVERIFY(whole.ok);
    QCOMPARE(probe.images.value(1), whole.frames.images.at(1)); // the same pictures as the overlay's own decode
    QCOMPARE(probe.images.value(2), whole.frames.images.at(2));
    QCOMPARE(SpriteDecoder::promisedFrames(m_apng), qint64(0)); // FFmpeg's APNG demuxer does not say; the chunk walk of animationinfo does (petprobe asks it)
    QCOMPARE(readAnimationInfo(m_apng)->frames, 3);
}

void TestSpriteDecoder::aProbeOfAnUnreadableFileSaysWhy()
{
    const QString junk = m_dir.filePath(u"junk.apng"_s);
    testutil::writeFile(junk, "not an image at all");
    const SpriteDecoder::Probe probe = SpriteDecoder::probe(junk, {0});
    QVERIFY(!probe.ok);
    QVERIFY(!probe.error.isEmpty());
    QVERIFY(probe.images.isEmpty());
    QString error;
    QCOMPARE(SpriteDecoder::promisedFrames(m_dir.filePath(u"missing.apng"_s), &error), qint64(0));
    QVERIFY(!error.isEmpty());
}

void TestSpriteDecoder::aProbeReadsGifAndWebpAndPicksTheMiddle()
{
    const QString gif = convert(u"probe.gif"_s, {u"-filter_complex"_s, u"split[a][b];[a]palettegen=reserve_transparent=1[p];[b][p]paletteuse"_s});
    const QString webp = convert(u"probe.webp"_s, {u"-c:v"_s, u"libwebp_anim"_s, u"-lossless"_s, u"1"_s, u"-loop"_s, u"0"_s});
    if (gif.isEmpty() || webp.isEmpty())
        QSKIP("ffmpeg cannot make the test GIF and WebP");
    for (const QString &file : {gif, webp}) {
        const SpriteDecoder::Probe probe = SpriteDecoder::probe(file, {1}); // the middle of three frames
        QVERIFY2(probe.ok, qPrintable(file + u": "_s + probe.error));
        QCOMPARE(probe.frames, qint64(3));
        QCOMPARE(probe.size, QSize(kSize, kSize));
        QVERIFY2(qAbs(probe.durationMs - 300.0) < 1.0, qPrintable(QString::number(probe.durationMs)));
        QCOMPARE(probe.images.keys(), (QList<qint64>{1, 2}));
        QCOMPARE(probe.resizedFrames, qint64(0));
        QCOMPARE(probe.images.value(1).pixelColor(20, 10), QColor(0, 255, 0, 255)); // frame 1 is the green one
        const SpriteDecoder::Result whole = SpriteDecoder::decode(file, {}, 64 << 20);
        QVERIFY(whole.ok);
        QCOMPARE(probe.images.value(2), whole.frames.images.at(2));
    }
    // what the container header promises: FFmpeg's GIF demuxer knows the count from its stream info, Qt's reader the WebP's
    QCOMPARE(SpriteDecoder::promisedFrames(webp), qint64(3));
    QCOMPARE(SpriteDecoder::promisedFrames(gif), qint64(3));
}

void TestSpriteDecoder::aProbeRefusesABadSizeAndNeverPassesATruncatedFileAsWhole()
{
    const QString huge = m_dir.filePath(u"probehuge.apng"_s);
    testutil::writeFile(huge, animfixtures::apng(9000, 10, {{1, 10}, {1, 10}}));
    const SpriteDecoder::Probe refused = SpriteDecoder::probe(huge, {0});
    QVERIFY(!refused.ok);
    QVERIFY2(refused.error.contains(u"bad size"_s), qPrintable(refused.error));
    // a file cut short: either the damage is reported, or the animation ends where the data does, with fewer frames than the whole
    const QString cut = m_dir.filePath(u"probecut.apng"_s);
    testutil::writeFile(cut, alphaApng().left(alphaApng().size() * 2 / 3));
    const SpriteDecoder::Probe probe = SpriteDecoder::probe(cut, {0});
    if (probe.ok)
        QVERIFY(probe.frames < 3);
    else
        QVERIFY(!probe.error.isEmpty());
}

void TestSpriteDecoder::aGifWithASecondStreamAppendedDecodesAllItsFramesAndOurCountAgrees()
{
    // two GIFs joined without a trailer between (what 3431910992's `cx.gif.gif` is): FFmpeg decodes the frames of both streams, but its own count
    // before decoding stops at the second header, so the file's chunk walk is the count to trust (and it now reads such a file)
    QByteArray joined = animfixtures::gif(kSize, kSize, {10, 10});
    joined.chop(1);
    joined += animfixtures::gif(kSize, kSize, {10});
    const QString path = m_dir.filePath(u"joined.gif"_s);
    testutil::writeFile(path, joined);
    const SpriteDecoder::Probe probe = SpriteDecoder::probe(path, {1});
    QVERIFY2(probe.ok, qPrintable(probe.error));
    const auto info = readAnimationInfo(path);
    QVERIFY(info);
    QCOMPARE(info->frames, 3);
    QCOMPARE(probe.frames, qint64(info->frames));
}

void TestSpriteDecoder::aProbeRefusesWhatWouldNotFitItsBudget()
{
    // 64 x 64 x 4 bytes a canvas, for the frame asked for, the last and the one being made: 48 KiB, so 40 KiB is too little and says so
    const SpriteDecoder::Probe tight = SpriteDecoder::probe(m_apng, {1}, 40 * 1024);
    QVERIFY(!tight.ok);
    QVERIFY2(tight.error.contains(u"memory budget"_s), qPrintable(tight.error));
    QVERIFY(tight.images.isEmpty());
    const SpriteDecoder::Probe fits = SpriteDecoder::probe(m_apng, {1}, 64 * 1024);
    QVERIFY2(fits.ok, qPrintable(fits.error));
    QCOMPARE(fits.resizedFrames, qint64(0)); // every frame has the stream's size: nothing to report
    // the picture was converted in place and still reads as premultiplied RGBA, not a corrupted one
    QCOMPARE(fits.images.value(1).pixelColor(20, 10), QColor(0, 255, 0, 255));
    QCOMPARE(fits.images.value(2).pixelColor(4, 10).alpha(), 128);
    // the same budget holds for a WebP
    const QString webp = convert(u"budget.webp"_s, {u"-c:v"_s, u"libwebp_anim"_s, u"-lossless"_s, u"1"_s, u"-loop"_s, u"0"_s});
    if (!webp.isEmpty()) {
        QVERIFY(!SpriteDecoder::probe(webp, {1}, 40 * 1024).ok);
        QVERIFY(SpriteDecoder::probe(webp, {1}, 64 * 1024).ok);
    }
}

void TestSpriteDecoder::aProbeOfATruncatedWebpFailsInsteadOfCountingFewerFrames()
{
    const QString webp = convert(u"whole.webp"_s, {u"-c:v"_s, u"libwebp_anim"_s, u"-lossless"_s, u"1"_s, u"-loop"_s, u"0"_s});
    if (webp.isEmpty())
        QSKIP("ffmpeg cannot make the test WebP");
    const SpriteDecoder::Probe whole = SpriteDecoder::probe(webp, {0});
    QVERIFY2(whole.ok, qPrintable(whole.error));
    QCOMPARE(whole.frames, qint64(3));
    QFile in(webp);
    QVERIFY(in.open(QIODevice::ReadOnly));
    const QByteArray bytes = in.readAll();
    // cut at three quarters, then at a third: a file that loses its last frames must not be reported as a shorter animation
    for (const int cut : {int(bytes.size() * 3 / 4), int(bytes.size() / 3)}) {
        const QString path = m_dir.filePath(u"cut%1.webp"_s.arg(cut));
        testutil::writeFile(path, bytes.left(cut));
        const SpriteDecoder::Probe probe = SpriteDecoder::probe(path, {0});
        QVERIFY2(!probe.ok, qPrintable(u"cut at %1 still reads %2 frames"_s.arg(cut).arg(probe.frames)));
        QVERIFY(!probe.error.isEmpty());
    }
}

void TestSpriteDecoder::framesAreScaledToTheTarget()
{
    const SpriteDecoder::Result result = SpriteDecoder::decode(m_apng, QSize(32, 16), 64 << 20);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.frames.size, QSize(32, 16));
    QCOMPARE(result.frames.images.at(0).size(), QSize(32, 16));
    QCOMPARE(result.frames.bytes, qint64(3 * 32 * 16 * 4));
    // The opaque middle stays opaque, the empty right stays empty.
    QCOMPARE(result.frames.images.at(0).pixelColor(12, 8).alpha(), 255);
    QCOMPARE(result.frames.images.at(0).pixelColor(28, 8).alpha(), 0);
}

void TestSpriteDecoder::headerEstimateIsFramesTimesPixels()
{
    QString error;
    // The WebM header has the duration; an APNG's does not say (the limit is then enforced while decoding).
    QCOMPARE(SpriteDecoder::estimateBytes(m_apng, QSize(32, 32), &error), qint64(0));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QString webm = makeWebm();
    if (webm.isEmpty())
        QSKIP("ffmpeg with libvpx-vp9 is not available");
    QCOMPARE(SpriteDecoder::estimateBytes(webm, QSize(32, 32), &error), qint64(3 * 32 * 32 * 4));
    QCOMPARE(SpriteDecoder::estimateBytes(webm, {}, &error), qint64(3 * kSize * kSize * 4));
    // So a WebM over the budget fails before a single frame is decoded.
    const SpriteDecoder::Result refused = SpriteDecoder::decode(webm, {}, 2 * kSize * kSize * 4);
    QVERIFY(!refused.ok);
    QVERIFY(refused.error.contains(u"budget"_s));
    QCOMPARE(SpriteDecoder::estimateBytes(m_dir.filePath(u"missing.apng"_s), {}, &error), qint64(0));
    QVERIFY(!error.isEmpty());
}

void TestSpriteDecoder::overBudgetFailsBeforeDecoding()
{
    // Three frames of 16 KiB need 48 KiB: one frame of room is not enough.
    const SpriteDecoder::Result result = SpriteDecoder::decode(m_apng, {}, 20000);
    QVERIFY(!result.ok);
    QVERIFY(!result.cancelled);
    QVERIFY2(result.error.contains(u"budget"_s), qPrintable(result.error));
    QVERIFY(result.frames.isEmpty()); // nothing is kept
    QVERIFY(SpriteDecoder::decode(m_apng, {}, 3 * kSize * kSize * 4).ok); // exactly enough
}

void TestSpriteDecoder::budgetIsAlsoEnforcedWhileDecoding()
{
    // A GIF of unknown length gets no up-front check from a header without a frame count; the limit still holds.
    const QString gif = convert(u"long.gif"_s, {u"-vf"_s, u"loop=loop=50:size=3:start=0"_s});
    if (gif.isEmpty())
        QSKIP("ffmpeg cannot make the test GIF");
    const SpriteDecoder::Result result = SpriteDecoder::decode(gif, {}, 5 * kSize * kSize * 4);
    QVERIFY(!result.ok);
    QVERIFY2(result.error.contains(u"budget"_s), qPrintable(result.error));
}

void TestSpriteDecoder::aCancelledDecodeStops()
{
    std::atomic_bool cancel(true);
    const SpriteDecoder::Result result = SpriteDecoder::decode(m_apng, {}, 64 << 20, &cancel);
    QVERIFY(!result.ok);
    QVERIFY(result.cancelled);
    QVERIFY(result.frames.isEmpty());

    // Cancelling in the middle: the flag turns on once the first frame is done (it is polled per frame).
    const QString gif = convert(u"cancel.gif"_s, {u"-vf"_s, u"loop=loop=200:size=3:start=0"_s});
    if (gif.isEmpty())
        QSKIP("ffmpeg cannot make the test GIF");
    std::atomic_bool later(false);
    std::thread flipper([&later] {
        QThread::msleep(5);
        later = true;
    });
    const SpriteDecoder::Result middle = SpriteDecoder::decode(gif, {}, qint64(1) << 30, &later);
    flipper.join();
    QVERIFY(middle.cancelled || middle.ok); // on a very fast machine it may have finished first
}

void TestSpriteDecoder::unreadableOrForeignFilesFail()
{
    QVERIFY(!SpriteDecoder::decode(m_dir.filePath(u"missing.apng"_s), {}, 1 << 20).ok);
    const QString text = m_dir.filePath(u"notes.txt"_s);
    testutil::writeFile(text, "hello");
    const SpriteDecoder::Result foreign = SpriteDecoder::decode(text, {}, 1 << 20);
    QVERIFY(!foreign.ok);
    QVERIFY(foreign.error.contains(u"unsupported"_s));
    const QString garbage = m_dir.filePath(u"garbage.apng"_s);
    testutil::writeFile(garbage, "this is not a png at all");
    QVERIFY(!SpriteDecoder::decode(garbage, {}, 1 << 20).ok);
    const QString empty = m_dir.filePath(u"empty.webm"_s);
    testutil::writeFile(empty, "");
    QVERIFY(!SpriteDecoder::decode(empty, {}, 1 << 20).ok);
}

void TestSpriteDecoder::onlyLocalFilesAreOpened()
{
    // The path is always handed over as "file:<path>": a URL in its place is a (missing) file name, not a request.
    const SpriteDecoder::Result result = SpriteDecoder::decode(u"http://127.0.0.1:9/a.apng"_s, {}, 1 << 20);
    QVERIFY(!result.ok);
    QVERIFY2(result.error.contains(u"cannot open"_s), qPrintable(result.error));
}

void TestSpriteDecoder::webmKeepsItsAlpha()
{
    const QString webm = makeWebm();
    if (webm.isEmpty())
        QSKIP("ffmpeg with libvpx-vp9 is not available");
    const SpriteDecoder::Result result = SpriteDecoder::decode(webm, {}, 64 << 20);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.frames.images.size(), 3);
    QCOMPARE(result.frames.durationsMs.size(), 3);
    for (const int ms : result.frames.durationsMs)
        QVERIFY2(ms >= 90 && ms <= 110, qPrintable(QString::number(ms)));
    const QImage &first = result.frames.images.at(0);
    QCOMPARE(first.format(), QImage::Format_RGBA8888_Premultiplied);
    // Lossy, so with a tolerance: the transparent right half is still transparent, the opaque middle opaque, the
    // left strip about half transparent and premultiplied.
    QVERIFY2(first.pixelColor(50, 10).alpha() <= 3, "the transparent side stays transparent");
    QVERIFY2(first.pixelColor(24, 10).alpha() >= 250, "the opaque side stays opaque");
    const int alpha = first.pixelColor(4, 10).alpha();
    QVERIFY2(qAbs(alpha - 128) <= 12, qPrintable(QString::number(alpha)));
    const uchar *raw = first.constScanLine(10) + 4 * 4;
    QVERIFY2(raw[0] <= alpha + 3, "premultiplied: the red channel can not exceed alpha"); // 255 would be straight

    // And scaled to the size the pet is shown at.
    const SpriteDecoder::Result small = SpriteDecoder::decode(webm, QSize(32, 32), 64 << 20);
    QVERIFY2(small.ok, qPrintable(small.error));
    QCOMPARE(small.frames.images.at(0).size(), QSize(32, 32));
}

void TestSpriteDecoder::gifIsDecodedToo()
{
    const QString gif = convert(u"alpha.gif"_s, {u"-filter_complex"_s,
                                                  u"split[a][b];[a]palettegen=reserve_transparent=1[p];[b][p]paletteuse"_s});
    if (gif.isEmpty())
        QSKIP("ffmpeg cannot make the test GIF");
    const SpriteDecoder::Result result = SpriteDecoder::decode(gif, {}, 64 << 20);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.frames.images.size(), 3);
    QCOMPARE(result.frames.durationsMs, (QList<int>{100, 100, 100}));
    QCOMPARE(result.frames.images.at(0).pixelColor(50, 10).alpha(), 0);
}

void TestSpriteDecoder::animatedWebpIsDecodedToo()
{
    const QString webp = convert(u"alpha.webp"_s, {u"-c:v"_s, u"libwebp_anim"_s, u"-lossless"_s, u"1"_s, u"-loop"_s, u"0"_s});
    if (webp.isEmpty())
        QSKIP("ffmpeg cannot make the test WebP");
    const SpriteDecoder::Result result = SpriteDecoder::decode(webp, {}, 64 << 20);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.frames.images.size(), 3);
    QCOMPARE(result.frames.durationsMs, (QList<int>{100, 100, 100}));
    QCOMPARE(result.frames.images.at(1).pixelColor(20, 10), QColor(0, 255, 0, 255));
    QCOMPARE(result.frames.images.at(0).pixelColor(50, 10).alpha(), 0);
}

void TestSpriteDecoder::tinyDelaysPlayForOneHundredMillisecondsInEveryFormat()
{
    // The rule is petFrameDelayMs: 10 ms or less is 100 ms, anything longer stays.
    const QString apng = m_dir.filePath(u"tiny.apng"_s);
    testutil::writeFile(apng, alphaApng(100, {0, 1, 10}));
    const SpriteDecoder::Result fromApng = SpriteDecoder::decode(apng, {}, 64 << 20);
    QVERIFY2(fromApng.ok, qPrintable(fromApng.error));
    QCOMPARE(fromApng.frames.durationsMs, (QList<int>{100, 100, 100})); // 0 (FFmpeg would say 66), 1 and 10 ms
    const QString mixed = m_dir.filePath(u"mixed.apng"_s);
    testutil::writeFile(mixed, alphaApng(100, {11, 20, 0}));
    QCOMPARE(SpriteDecoder::decode(mixed, {}, 64 << 20).frames.durationsMs, (QList<int>{11, 20, 100}));

    const QString gif = m_dir.filePath(u"tiny.gif"_s);
    testutil::writeFile(gif, tinyGif({-1, 0, 1, 2, 10})); // (a frame with no control block after others repeats their delay in FFmpeg: not tested)
    const SpriteDecoder::Result fromGif = SpriteDecoder::decode(gif, {}, 64 << 20);
    QVERIFY2(fromGif.ok, qPrintable(fromGif.error));
    QCOMPARE(fromGif.frames.durationsMs, (QList<int>{100, 100, 100, 20, 100}));

    const QString made = convert(u"base.webp"_s, {u"-c:v"_s, u"libwebp_anim"_s, u"-lossless"_s, u"1"_s, u"-loop"_s, u"0"_s});
    if (!made.isEmpty()) {
        QFile source(made);
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QString webp = m_dir.filePath(u"tiny.webp"_s);
        testutil::writeFile(webp, withWebpDelays(source.readAll(), {0, 10, 30}));
        const SpriteDecoder::Result fromWebp = SpriteDecoder::decode(webp, {}, 64 << 20);
        QVERIFY2(fromWebp.ok, qPrintable(fromWebp.error));
        QCOMPARE(fromWebp.frames.durationsMs, (QList<int>{100, 100, 30}));
        const SpriteDecoder::Probe probeWebp = SpriteDecoder::probe(webp, {});
        QVERIFY(probeWebp.ok);
        QCOMPARE(probeWebp.durationMs, 230.0);
    }

    // Many frames of 1 centisecond (the ordinary "fast" GIF): timestamps are not exact to the last digit, and not one of them may slip
    // past the rule as a delay of 10.000000000000002 ms.
    const QString fast = m_dir.filePath(u"fast.gif"_s);
    testutil::writeFile(fast, tinyGif(QList<int>(120, 1)));
    const SpriteDecoder::Result fromFast = SpriteDecoder::decode(fast, {}, 64 << 20);
    QVERIFY2(fromFast.ok, qPrintable(fromFast.error));
    QCOMPARE(fromFast.frames.durationsMs, QList<int>(120, 100));
    const SpriteDecoder::Probe probeFast = SpriteDecoder::probe(fast, {});
    QVERIFY2(probeFast.ok, qPrintable(probeFast.error));
    QCOMPARE(probeFast.durationMs, 12000.0);

    // The probe counts the same way: its duration is the played one.
    const SpriteDecoder::Probe probeApng = SpriteDecoder::probe(apng, {});
    QVERIFY2(probeApng.ok, qPrintable(probeApng.error));
    QCOMPARE(probeApng.durationMs, 300.0);
    const SpriteDecoder::Probe probeGif = SpriteDecoder::probe(gif, {});
    QVERIFY2(probeGif.ok, qPrintable(probeGif.error));
    QCOMPARE(probeGif.durationMs, 420.0);
    // and the header readers agree with the decoder on all of them
    for (const QString &file : {apng, gif}) {
        const auto info = readAnimationInfo(file);
        QVERIFY(info);
        const SpriteDecoder::Probe probe = SpriteDecoder::probe(file, {});
        QCOMPARE(info->fps, std::round(info->frames / (probe.durationMs / 1000.0) * 100.0) / 100.0);
    }
}

void TestSpriteDecoder::aFixedRateReplacesTheFilesDelays()
{
    QCOMPARE(SpriteDecoder::fixedDurations(3, 30), (QList<int>{33, 34, 33}));
    QCOMPARE(SpriteDecoder::fixedDurations(6, 60), (QList<int>{17, 16, 17, 17, 16, 17}));
    QCOMPARE(SpriteDecoder::fixedDurations(2, 15), (QList<int>{67, 66}));
    QCOMPARE(SpriteDecoder::fixedDurations(0, 30), QList<int>());
    QCOMPARE(SpriteDecoder::fixedDurations(2, 0), QList<int>());
    // 30 frames at 30 fps last one second whatever the rounding, and 60 fps never drops under the minimum.
    const QList<int> second = SpriteDecoder::fixedDurations(30, 30);
    QCOMPARE(std::accumulate(second.begin(), second.end(), 0), 1000);
    QVERIFY(SpriteDecoder::fixedDurations(100, 60).first() >= SpriteDecoder::kMinFrameMs);

    // The APNG says 100 ms a frame: 24 fps makes it 42, 41, 42.
    const SpriteDecoder::Result result = SpriteDecoder::decode(m_apng, {}, 64 << 20, nullptr, 24);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.frames.durationsMs, (QList<int>{42, 41, 42}));
    QCOMPARE(result.frames.images.size(), 3);
    // 0 keeps the file's own.
    QCOMPARE(SpriteDecoder::decode(m_apng, {}, 64 << 20, nullptr, 0).frames.durationsMs, (QList<int>{100, 100, 100}));
}

void TestSpriteDecoder::hugeCanvasesAndFramesOverTheBudgetAreRefusedOnTheHeader()
{
    // A header that claims 9000 px is refused whatever the budget; the zeros it holds are never decoded.
    const QString huge = m_dir.filePath(u"huge.apng"_s);
    testutil::writeFile(huge, animfixtures::apng(9000, 10, {{1, 10}, {1, 10}}));
    const SpriteDecoder::Result refused = SpriteDecoder::decode(huge, {}, qint64(1) << 40);
    QVERIFY(!refused.ok);
    QVERIFY2(refused.error.contains(u"bad size"_s), qPrintable(refused.error));
    QVERIFY(!SpriteDecoder::decode(m_apng, QSize(9000, 10), qint64(1) << 40).ok); // nor frames made that wide
    // One frame already over the budget: refused before anything is allocated or decoded.
    const SpriteDecoder::Result tight = SpriteDecoder::decode(m_apng, {}, kSize * kSize * 4 - 1);
    QVERIFY(!tight.ok);
    QVERIFY2(tight.error.contains(u"budget"_s), qPrintable(tight.error));
}

void TestSpriteDecoder::aTruncatedFileFailsInsteadOfPlayingHalf()
{
    QByteArray bytes = alphaApng();
    const QString cut = m_dir.filePath(u"cut.apng"_s);
    testutil::writeFile(cut, bytes.left(bytes.size() * 2 / 3));
    const SpriteDecoder::Result result = SpriteDecoder::decode(cut, {}, 64 << 20);
    // Either FFmpeg reports the damage and nothing plays, or it ends the file cleanly where the data ends; it must
    // never hand over a partial animation as a good one with the full length.
    if (result.ok)
        QVERIFY(result.frames.images.size() < 3);
    else
        QVERIFY(!result.error.isEmpty());
}

void TestSpriteDecoder::aCancelledTaskDoesNotEvenOpenTheFile()
{
    std::atomic_bool cancel(true);
    const SpriteDecoder::Result result = SpriteDecoder::decode(m_dir.filePath(u"does-not-exist.apng"_s), {}, 1 << 20, &cancel);
    QVERIFY(result.cancelled); // not "cannot open": it never got that far
    QVERIFY(result.error.isEmpty());
}

void TestSpriteDecoder::hdAndSdWebmColorsMatchTheirTags_data()
{
    QTest::addColumn<QString>("matrix");
    QTest::addColumn<QSize>("size");
    QTest::newRow("BT.709 tagged, HD") << u"bt709"_s << QSize(1280, 720);
    QTest::newRow("BT.709 tagged, small") << u"bt709"_s << QSize(64, 64);
    QTest::newRow("BT.601 tagged, SD") << u"smpte170m"_s << QSize(64, 64);
}

void TestSpriteDecoder::hdAndSdWebmColorsMatchTheirTags()
{
    QFETCH(QString, matrix);
    QFETCH(QSize, size);
    // swscale assumes BT.601 for every YUV frame; a stream tagged BT.709 must still decode to the colors that were
    // encoded (mpv honors the tag). Green and red are the colors where the two matrices differ most.
    const QString out = m_dir.filePath(u"tagged-%1-%2.webm"_s.arg(matrix).arg(size.width()));
    const QString filter = u"scale=%1:%2:out_color_matrix=%3:out_range=tv,format=yuva420p"_s.arg(size.width())
                               .arg(size.height()).arg(matrix == u"bt709"_s ? u"bt709"_s : u"bt601"_s);
    const QString converted = convert(QFileInfo(out).fileName(),
                                      {u"-vf"_s, filter, u"-c:v"_s, u"libvpx-vp9"_s, u"-pix_fmt"_s, u"yuva420p"_s,
                                       u"-auto-alt-ref"_s, u"0"_s, u"-b:v"_s, u"0"_s, u"-crf"_s, u"4"_s, u"-colorspace"_s,
                                       matrix, u"-color_range"_s, u"tv"_s});
    if (converted.isEmpty())
        QSKIP("ffmpeg with libvpx-vp9 is not available");
    const SpriteDecoder::Result result = SpriteDecoder::decode(converted, QSize(kSize, kSize), 64 << 20);
    QVERIFY2(result.ok, qPrintable(result.error));
    const auto close = [](QColor got, QColor want) {
        return qAbs(got.red() - want.red()) <= 12 && qAbs(got.green() - want.green()) <= 12
            && qAbs(got.blue() - want.blue()) <= 12;
    };
    const QColor red = result.frames.images.at(0).pixelColor(24, 10);
    const QColor green = result.frames.images.at(1).pixelColor(24, 10);
    QVERIFY2(close(red, QColor(255, 0, 0)), qPrintable(red.name()));
    QVERIFY2(close(green, QColor(0, 255, 0)), qPrintable(green.name()));
}

QTEST_GUILESS_MAIN(TestSpriteDecoder)
#include "tst_spritedecoder.moc"
