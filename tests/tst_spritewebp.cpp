// Animated WebP pets: the composited frames must be the same every time, and must be what the container says. Qt's WebP plugin leaves the
// part of the canvas that the first frame does not cover uninitialised, so two decodes of one file in one process differed; the pixels now
// come from libwebp's animation decoder. The fixtures are embedded (tests/webpfixtures.h): no encoder is needed to run this.

#include "spritedecoder.h"
#include "testutil.h"
#include "webpfixtures.h"

#include <QImage>
#include <QImageWriter>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cstring>
#include <vector>
#include <atomic>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

constexpr int kW = 256;
constexpr int kH = 128;

// What the fixtures composite to, premultiplied (alpha 255 or 0, so the same as straight).
QImage expectedFrame(int i)
{
    QImage image(kW, kH, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    for (int y = 40; y < 80; ++y) {
        for (int x = 20 + 80 * i; x < 60 + 80 * i; ++x)
            image.setPixelColor(x, y, QColor(200, 30, 30, 255));
    }
    return image;
}

template<size_t N>
QString write(const QTemporaryDir &dir, const char *name, const unsigned char (&bytes)[N])
{
    const QString path = dir.path() + u'/' + QLatin1String(name);
    testutil::writeFile(path, QByteArray(reinterpret_cast<const char *>(bytes), qsizetype(N)));
    return path;
}

} // namespace

class TstSpriteWebp : public QObject
{
    Q_OBJECT

private slots:
    void aLosslessAnimationCompositesToTheContainersFrames();
    void decodingIsDeterministicInOneProcess();
    void aLossyAnimationKeepsItsAlphaExactly();
    void aStillWebpIsOneFrame();
    void frameCountAndDurationsComeFromTheContainer();
    void aTruncatedOrBrokenFileIsRefused();
    void aHugeClaimedCanvasIsRefused();
    void theUncoveredCanvasIsTransparentWhateverTheHeapHolds();
    void theHeaderNumbersAgreeEverywhere();
    void hostileDurationsAreClamped();
    void aFileOverTheSizeCapIsRefusedWithoutReadingIt();
    void theProbeSeesTheSameFrames();
    void theProbeStopsOnCancel();
    void masksFollowTheCompositedFrames();
};

void TstSpriteWebp::aLosslessAnimationCompositesToTheContainersFrames()
{
    QTemporaryDir dir;
    const QString file = write(dir, "blocks.webp", webpfixtures::kBlocksLossless);
    const auto result = SpriteDecoder::decode(file, {}, 1 << 28);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.frames.size, QSize(kW, kH));
    QCOMPARE(result.frames.images.size(), 3);
    for (int i = 0; i < 3; ++i) {
        QCOMPARE(result.frames.images.at(i).format(), QImage::Format_RGBA8888_Premultiplied);
        QVERIFY2(result.frames.images.at(i) == expectedFrame(i), qPrintable(QStringLiteral("frame %1").arg(i)));
    }
}

void TstSpriteWebp::decodingIsDeterministicInOneProcess()
{
    QTemporaryDir dir;
    for (const auto *fixture : {"lossless", "lossy"}) {
        const QString file = std::strcmp(fixture, "lossless") == 0 ? write(dir, "a.webp", webpfixtures::kBlocksLossless)
                                                                   : write(dir, "b.webp", webpfixtures::kBlocksLossy);
        const auto first = SpriteDecoder::decode(file, {}, 1 << 28);
        QVERIFY2(first.ok, qPrintable(first.error));
        for (int run = 0; run < 12; ++run) {
            // Dirty, freed memory of the size of a canvas is what the next allocation gets back.
            for (int k = 0; k < 8; ++k) {
                std::vector<unsigned char> junk(size_t(kW) * kH * 4 + size_t(k) * 4096, 0xA5);
                QVERIFY(junk.front() == 0xA5);
            }
            const auto again = SpriteDecoder::decode(file, {}, 1 << 28);
            QVERIFY2(again.ok, qPrintable(again.error));
            QCOMPARE(again.frames.images.size(), first.frames.images.size());
            for (qsizetype i = 0; i < first.frames.images.size(); ++i)
                QVERIFY2(again.frames.images.at(i) == first.frames.images.at(i), qPrintable(QStringLiteral("%1 run %2 frame %3").arg(QLatin1String(fixture)).arg(run).arg(i)));
            QCOMPARE(again.frames.durationsMs, first.frames.durationsMs);
        }
    }
}

void TstSpriteWebp::aLossyAnimationKeepsItsAlphaExactly()
{
    QTemporaryDir dir;
    const QString file = write(dir, "blocks-lossy.webp", webpfixtures::kBlocksLossy);
    const auto result = SpriteDecoder::decode(file, {}, 1 << 28);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.frames.images.size(), 3);
    for (int i = 0; i < 3; ++i) {
        const QImage want = expectedFrame(i);
        const QImage &got = result.frames.images.at(i);
        int worst = 0;
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                const QColor a = want.pixelColor(x, y), b = got.pixelColor(x, y);
                QCOMPARE(b.alpha(), a.alpha());
                if (a.alpha() == 255)
                    worst = std::max({worst, std::abs(a.red() - b.red()), std::abs(a.green() - b.green()), std::abs(a.blue() - b.blue())});
                else
                    QCOMPARE(b.red() + b.green() + b.blue(), 0); // transparent pixels are 0,0,0,0 once premultiplied
            }
        }
        QVERIFY2(worst <= 24, qPrintable(QString::number(worst))); // lossy colour
    }
}

void TstSpriteWebp::aStillWebpIsOneFrame()
{
    QTemporaryDir dir;
    const QString file = dir.path() + u"/still.webp"_s;
    QImageWriter writer(file, "webp");
    writer.setQuality(100); // lossless
    if (!writer.write(expectedFrame(1).convertToFormat(QImage::Format_RGBA8888)))
        QSKIP("Qt's WebP plugin can not write");
    const auto result = SpriteDecoder::decode(file, {}, 1 << 28);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.frames.images.size(), 1);
    QVERIFY(result.frames.images.at(0) == expectedFrame(1));
    // Scaled to a target as the frame cache does.
    const auto scaled = SpriteDecoder::decode(file, {128, 64}, 1 << 28);
    QVERIFY(scaled.ok);
    QCOMPARE(scaled.frames.images.at(0).size(), QSize(128, 64));
    QCOMPARE(scaled.frames.size, QSize(128, 64));
}

void TstSpriteWebp::frameCountAndDurationsComeFromTheContainer()
{
    QTemporaryDir dir;
    const QString file = write(dir, "blocks.webp", webpfixtures::kBlocksLossless);
    QCOMPARE(SpriteDecoder::promisedFrames(file), qint64(3));
    QCOMPARE(SpriteDecoder::nativeSize(file), QSize(kW, kH));
    const auto result = SpriteDecoder::decode(file, {}, 1 << 28);
    QVERIFY(result.ok);
    QCOMPARE(result.frames.durationsMs, (QList<int>{100, 100, 100}));
    const auto fixed = SpriteDecoder::decode(file, {}, 1 << 28, nullptr, 24);
    QCOMPARE(fixed.frames.durationsMs, SpriteDecoder::fixedDurations(3, 24));
    // Over the budget: refused before any frame.
    QVERIFY(!SpriteDecoder::decode(file, {}, kW * kH * 4 * 2).ok);
    std::atomic_bool cancel{true};
    QVERIFY(SpriteDecoder::decode(file, {}, 1 << 28, &cancel).cancelled);
}

void TstSpriteWebp::aTruncatedOrBrokenFileIsRefused()
{
    QTemporaryDir dir;
    QByteArray good(reinterpret_cast<const char *>(webpfixtures::kBlocksLossless), sizeof webpfixtures::kBlocksLossless);
    for (const int keep : {0, 4, 12, 30, 100, int(good.size()) - 1}) {
        const QString file = dir.path() + u"/cut.webp"_s;
        testutil::writeFile(file, good.left(keep));
        const auto result = SpriteDecoder::decode(file, {}, 1 << 28);
        QVERIFY2(!result.ok, qPrintable(QString::number(keep)));
        QVERIFY(result.frames.images.isEmpty());
    }
    // A flipped byte anywhere: no crash, and either refused or whole frames.
    for (int at = 40; at < good.size(); at += 7) {
        QByteArray bad = good;
        bad[at] = char(bad[at] ^ 0x5a);
        const QString file = dir.path() + u"/bad.webp"_s;
        testutil::writeFile(file, bad);
        const auto result = SpriteDecoder::decode(file, {}, 1 << 28);
        if (result.ok) { // a damaged chunk header can hide the last frame from the demuxer; whatever is returned is whole frames
            QVERIFY(result.frames.images.size() >= 1 && result.frames.images.size() <= 3);
            QCOMPARE(result.frames.images.at(0).size(), QSize(kW, kH));
        }
    }
    testutil::writeFile(dir.path() + u"/junk.webp"_s, QByteArray(500, 'q'));
    QVERIFY(!SpriteDecoder::decode(dir.path() + u"/junk.webp"_s, {}, 1 << 28).ok);
    QVERIFY(!SpriteDecoder::decode(dir.path() + u"/missing.webp"_s, {}, 1 << 28).ok);
}

void TstSpriteWebp::aHugeClaimedCanvasIsRefused()
{
    QTemporaryDir dir;
    QByteArray bytes(reinterpret_cast<const char *>(webpfixtures::kBlocksLossless), sizeof webpfixtures::kBlocksLossless);
    // VP8X canvas width and height minus one are 24-bit little endian at offsets 24 and 27.
    bytes[24] = char(0xff);
    bytes[25] = char(0xff);
    bytes[26] = char(0xff);
    bytes[27] = char(0xff);
    bytes[28] = char(0xff);
    bytes[29] = char(0xff);
    const QString file = dir.path() + u"/huge.webp"_s;
    testutil::writeFile(file, bytes);
    const auto result = SpriteDecoder::decode(file, {}, qint64(1) << 40);
    QVERIFY(!result.ok);
}

namespace {

struct Anmf {
    int x, y, w, h, duration;
    size_t offset; // of the chunk payload
};

// The ANMF chunks of a WebP file, read from the RIFF structure (what the test needs to know about the fixtures, independent of any decoder).
std::vector<Anmf> anmfChunks(const QByteArray &file)
{
    std::vector<Anmf> out;
    const auto u24 = [&](qsizetype at) { return int(uchar(file[at])) | int(uchar(file[at + 1])) << 8 | int(uchar(file[at + 2])) << 16; };
    qsizetype pos = 12;
    while (pos + 8 <= file.size()) {
        const quint32 size = quint32(uchar(file[pos + 4])) | quint32(uchar(file[pos + 5])) << 8 | quint32(uchar(file[pos + 6])) << 16
            | quint32(uchar(file[pos + 7])) << 24;
        if (file.mid(pos, 4) == "ANMF")
            out.push_back({u24(pos + 8) * 2, u24(pos + 11) * 2, u24(pos + 14) + 1, u24(pos + 17) + 1, u24(pos + 20), size_t(pos + 8)});
        pos += 8 + qsizetype(size) + qsizetype(size & 1);
    }
    return out;
}

} // namespace

void TstSpriteWebp::theUncoveredCanvasIsTransparentWhateverTheHeapHolds()
{
    // The root cause of the bug this file guards: the first frame's rectangle is smaller than the canvas, so a decoder has to supply
    // the rest. Qt's plugin did not (it showed the heap); the container says it is transparent.
    const QByteArray bytes(reinterpret_cast<const char *>(webpfixtures::kBlocksLossless), sizeof webpfixtures::kBlocksLossless);
    const auto frames = anmfChunks(bytes);
    QCOMPARE(int(frames.size()), 3);
    QVERIFY(frames[0].w < kW && frames[0].h < kH);
    QCOMPARE(frames[0].x, 20);
    QCOMPARE(frames[0].y, 40);
    QTemporaryDir dir;
    const QString file = write(dir, "blocks.webp", webpfixtures::kBlocksLossless);
    for (int run = 0; run < 20; ++run) {
        for (int k = 0; k < 8; ++k) {
            std::vector<unsigned char> junk(size_t(kW) * kH * 4 + size_t(k) * 997, 0xA5);
            QVERIFY(junk.back() == 0xA5);
        }
        const auto result = SpriteDecoder::decode(file, {}, 1 << 28);
        QVERIFY(result.ok);
        const QImage &first = result.frames.images.at(0);
        for (int y = 0; y < kH; ++y) {
            const uchar *row = first.constScanLine(y);
            for (int x = 0; x < kW; ++x) {
                const bool covered = x >= frames[0].x && x < frames[0].x + frames[0].w && y >= frames[0].y && y < frames[0].y + frames[0].h;
                if (!covered)
                    QVERIFY2(row[4 * x] == 0 && row[4 * x + 1] == 0 && row[4 * x + 2] == 0 && row[4 * x + 3] == 0,
                             qPrintable(QStringLiteral("run %1 pixel %2,%3").arg(run).arg(x).arg(y)));
            }
        }
    }
}

void TstSpriteWebp::theHeaderNumbersAgreeEverywhere()
{
    QTemporaryDir dir;
    const QString file = write(dir, "blocks.webp", webpfixtures::kBlocksLossless);
    const auto decoded = SpriteDecoder::decode(file, {}, 1 << 28);
    QVERIFY(decoded.ok);
    QCOMPARE(SpriteDecoder::nativeSize(file), decoded.frames.size);
    QCOMPARE(SpriteDecoder::promisedFrames(file), qint64(decoded.frames.images.size()));
    QCOMPARE(SpriteDecoder::estimateBytes(file, {}), decoded.frames.bytes);
    QCOMPARE(SpriteDecoder::estimateBytes(file, {64, 32}), qint64(3) * 64 * 32 * 4);
    const auto pass = SpriteDecoder::decodeMasks(file);
    QVERIFY(pass.result.ok);
    QCOMPARE(pass.size, decoded.frames.size);
    QCOMPARE(pass.result.decodedFrames, qint64(3));
    QString error;
    QCOMPARE(SpriteDecoder::promisedFrames(dir.path() + u"/none.webp"_s, &error), qint64(0));
    QVERIFY(!error.isEmpty());
    QCOMPARE(SpriteDecoder::nativeSize(dir.path() + u"/none.webp"_s), QSize());
    testutil::writeFile(dir.path() + u"/junk.webp"_s, QByteArray(300, 'j'));
    QCOMPARE(SpriteDecoder::nativeSize(dir.path() + u"/junk.webp"_s), QSize());
    QCOMPARE(SpriteDecoder::promisedFrames(dir.path() + u"/junk.webp"_s), qint64(0));
}

void TstSpriteWebp::hostileDurationsAreClamped()
{
    QByteArray bytes(reinterpret_cast<const char *>(webpfixtures::kBlocksLossless), sizeof webpfixtures::kBlocksLossless);
    for (const auto &frame : anmfChunks(bytes)) {
        bytes[qsizetype(frame.offset) + 12] = char(0xff); // 24-bit duration 16 777 215 ms
        bytes[qsizetype(frame.offset) + 13] = char(0xff);
        bytes[qsizetype(frame.offset) + 14] = char(0xff);
    }
    QTemporaryDir dir;
    const QString file = dir.path() + u"/long.webp"_s;
    testutil::writeFile(file, bytes);
    const auto result = SpriteDecoder::decode(file, {}, 1 << 28);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.frames.images.size(), 3);
    for (const int ms : result.frames.durationsMs) {
        QVERIFY(ms > 0);
        QVERIFY(ms <= 3600 * 1000); // an hour at most
    }
}

void TstSpriteWebp::aFileOverTheSizeCapIsRefusedWithoutReadingIt()
{
    QTemporaryDir dir;
    QFile big(dir.path() + u"/big.webp"_s);
    QVERIFY(big.open(QIODevice::WriteOnly));
    QVERIFY(big.write(reinterpret_cast<const char *>(webpfixtures::kBlocksLossless), sizeof webpfixtures::kBlocksLossless) > 0);
    QVERIFY(big.resize((qint64(512) << 20) + 1)); // sparse
    big.close();
    const auto result = SpriteDecoder::decode(big.fileName(), {}, qint64(1) << 40);
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(u"too large"_s));
}

void TstSpriteWebp::theProbeStopsOnCancel()
{
    QTemporaryDir dir;
    const QString file = write(dir, "blocks.webp", webpfixtures::kBlocksLossless);
    std::atomic_bool cancel{true};
    const auto probe = SpriteDecoder::probe(file, {0}, SpriteDecoder::kProbeMaxBytes, &cancel);
    QVERIFY(!probe.ok);
    QVERIFY(probe.images.isEmpty());
}

void TstSpriteWebp::theProbeSeesTheSameFrames()
{
    QTemporaryDir dir;
    const QString file = write(dir, "blocks.webp", webpfixtures::kBlocksLossless);
    const auto probe = SpriteDecoder::probe(file, {0, 1});
    QVERIFY2(probe.ok, qPrintable(probe.error));
    QCOMPARE(probe.frames, qint64(3));
    QCOMPARE(probe.size, QSize(kW, kH));
    QVERIFY(qAbs(probe.durationMs - 300) < 1);
    QVERIFY(probe.images.value(0) == expectedFrame(0));
    QVERIFY(probe.images.value(1) == expectedFrame(1));
    QVERIFY(probe.images.value(2) == expectedFrame(2)); // the last
}

void TstSpriteWebp::masksFollowTheCompositedFrames()
{
    QTemporaryDir dir;
    const QString file = write(dir, "blocks.webp", webpfixtures::kBlocksLossless);
    const SpriteDecoder::MaskRequest request;
    const auto result = SpriteDecoder::decode(file, {}, 1 << 28, nullptr, 0, &request);
    QVERIFY(result.ok);
    QCOMPARE(result.masks.frameCount(), 3);
    for (int i = 0; i < 3; ++i) {
        QVERIFY(result.masks.maskFor(i).contains(40 + 80 * i, 60));
        QVERIFY(!result.masks.maskFor(i).contains(40 + 80 * ((i + 1) % 3), 60));
        QVERIFY(!result.masks.maskFor(i).contains(5, 5)); // outside the first frame's rectangle: transparent, every time
    }
}

QTEST_APPLESS_MAIN(TstSpriteWebp)
#include "tst_spritewebp.moc"
