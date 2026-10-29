// Masks from the decoder's side: made while the frames are decoded, and by the low-resolution pass with its disk cache.

#include "animfixtures.h"
#include "spritedecoder.h"
#include "spritemask.h"
#include "testutil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <thread>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

constexpr int kW = 256;
constexpr int kH = 128;

// Three frames; frame i has an opaque 40 x 40 block at x = 20 + 80 i, y = 40, everything else transparent.
QImage frameImage(int i)
{
    QImage image(kW, kH, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.fillRect(20 + 80 * i, 40, 40, 40, QColor(200, 30, 30, 255));
    return image;
}

QByteArray rawPixels(const QImage &image)
{
    QByteArray raw;
    for (int y = 0; y < image.height(); ++y) {
        raw.append('\0');
        raw.append(reinterpret_cast<const char *>(image.constScanLine(y)), image.width() * 4);
    }
    return qCompress(raw, 9).mid(4);
}

QByteArray blocksApng(int frames = 3, int shift = 0)
{
    using namespace animfixtures;
    QByteArray out("\x89PNG\r\n\x1a\n", 8);
    out += pngChunk("IHDR", be32(kW) + be32(kH) + QByteArray("\x08\x06\x00\x00\x00", 5));
    out += pngChunk("acTL", be32(frames) + be32(0));
    quint32 sequence = 0;
    for (int i = 0; i < frames; ++i) {
        out += pngChunk("fcTL", be32(sequence++) + be32(kW) + be32(kH) + be32(0) + be32(0) + be16(100) + be16(1000) + QByteArray("\x00\x00", 2));
        const QByteArray data = rawPixels(frameImage((i + shift) % 3));
        out += i == 0 ? pngChunk("IDAT", data) : pngChunk("fdAT", be32(sequence++) + data);
    }
    out += pngChunk("IEND", {});
    return out;
}

struct Fixture {
    QTemporaryDir dir;
    QString file;
    explicit Fixture(const QByteArray &bytes = blocksApng())
    {
        file = dir.path() + u"/pet.apng"_s;
        testutil::writeFile(file, bytes);
    }
};

} // namespace

class TstSpriteMask : public QObject
{
    Q_OBJECT

private slots:
    void masksAreMadeFromTheDecodedFrames();
    void withoutARequestThereIsNoMask();
    void theFramesCanBeDropped();
    void aCancelledOrBrokenDecodeLeavesNoMask();
    void theBudgetIsTheRequests();
    void aStillWebpHasAMask();
    void maskPassSizes();
    void theMaskPassKeepsTheFullResolutionSilhouette();
    void aThinLineKeepsItsCell();
    void masksChangeNothingAboutTheDecodedFrames();
    void prefixVariantsDoNotDeleteEachOther();
    void unicodeNamesDoNotShareADirectory();
    void anEmptyCacheRootIsAnError();
    void theCacheIsWrittenReadAndInvalidatedWithTheFile();
    void aBadCacheFileIsReplaced();
    void cacheNamesCanNotEscapeTheCacheDirectory();
    void aMissingFileIsAnError();
};

void TstSpriteMask::masksAreMadeFromTheDecodedFrames()
{
    Fixture fx;
    const SpriteDecoder::MaskRequest request;
    const auto result = SpriteDecoder::decode(fx.file, {}, 1 << 28, nullptr, 0, &request);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.decodedFrames, 3);
    QCOMPARE(result.frames.images.size(), 3);
    QCOMPARE(int(result.masks.quality()), int(MaskQuality::Full));
    QCOMPARE(result.masks.frameCount(), 3);
    QCOMPARE(result.masks.silhouette().spaceWidth(), kW);
    QCOMPARE(result.masks.silhouette().spaceHeight(), kH);
    for (int i = 0; i < 3; ++i) {
        const AlphaMask &m = result.masks.maskFor(i);
        QVERIFY(m.contains(40 + 80 * i, 60));
        QVERIFY(!m.contains(40 + 80 * ((i + 1) % 3), 60));
        QVERIFY(!m.contains(5, 5));
        // Exactly the mask of the frame the decoder produced: no second decode, no other pixels.
        const QImage &image = result.frames.images.at(i);
        const AlphaMask direct = AlphaMask::fromAlpha(image.constBits() + 3, int(image.bytesPerLine()), 4, image.width(), image.height());
        QVERIFY(m == direct);
    }
    QVERIFY(result.masks.maskFor(0).isSubsetOf(result.masks.silhouette()));
}

void TstSpriteMask::withoutARequestThereIsNoMask()
{
    Fixture fx;
    const auto result = SpriteDecoder::decode(fx.file, {}, 1 << 28);
    QVERIFY(result.ok);
    QVERIFY(result.masks.isNull());
    QCOMPARE(result.decodedFrames, 3);
}

void TstSpriteMask::theFramesCanBeDropped()
{
    Fixture fx;
    const SpriteDecoder::MaskRequest request{kMaskBudgetBytes, false};
    // maxBytes 0: the frame cache budget does not apply when the frames are not kept.
    const auto result = SpriteDecoder::decode(fx.file, {}, 0, nullptr, 0, &request);
    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(result.frames.images.isEmpty());
    QCOMPARE(result.frames.bytes, qint64(0));
    QCOMPARE(result.decodedFrames, 3);
    QCOMPARE(result.masks.frameCount(), 3);
    // The same file, frames kept, over a budget of 0, fails as before.
    const SpriteDecoder::MaskRequest keep;
    QVERIFY(!SpriteDecoder::decode(fx.file, {}, 0, nullptr, 0, &keep).ok);
}

void TstSpriteMask::aCancelledOrBrokenDecodeLeavesNoMask()
{
    Fixture fx;
    const SpriteDecoder::MaskRequest request;
    std::atomic_bool cancel{true};
    const auto cancelled = SpriteDecoder::decode(fx.file, {}, 1 << 28, &cancel, 0, &request);
    QVERIFY(cancelled.cancelled);
    QVERIFY(!cancelled.ok);
    QVERIFY(cancelled.masks.isNull());
    // A truncated file: no half animation, no half masks.
    Fixture broken(blocksApng().left(blocksApng().size() / 2));
    const auto result = SpriteDecoder::decode(broken.file, {}, 1 << 28, nullptr, 0, &request);
    QVERIFY(!result.ok);
    QVERIFY(result.masks.isNull());
    const auto low = SpriteDecoder::decodeMasks(broken.file);
    QVERIFY(!low.result.ok);
    QVERIFY(low.result.masks.isNull());
}

void TstSpriteMask::theBudgetIsTheRequests()
{
    Fixture fx;
    SpriteDecoder::MaskRequest request;
    request.budgetBytes = 10; // less than one mask
    const auto tiny = SpriteDecoder::decode(fx.file, {}, 1 << 28, nullptr, 0, &request);
    QVERIFY(tiny.ok);
    QCOMPARE(int(tiny.masks.quality()), int(MaskQuality::Silhouette));
    QVERIFY(tiny.masks.silhouette().contains(40, 60) && tiny.masks.silhouette().contains(200, 60));
    request.budgetBytes = 3 * 1000 + 1000; // fits 3 frames at the 64 grid but not the 128 (2 KiB each)
    const auto coarse = SpriteDecoder::decode(fx.file, {}, 1 << 28, nullptr, 0, &request);
    QVERIFY(coarse.ok);
    QCOMPARE(int(coarse.masks.quality()), int(MaskQuality::Coarse));
    QCOMPARE(coarse.masks.frameCount(), 3);
    QVERIFY(coarse.masks.bytes() <= request.budgetBytes);
}

void TstSpriteMask::aStillWebpHasAMask()
{
    QTemporaryDir dir;
    const QString file = dir.path() + u"/still.webp"_s;
    if (!frameImage(1).save(file, "webp"))
        QSKIP("Qt's WebP plugin can not write");
    const SpriteDecoder::MaskRequest request;
    const auto result = SpriteDecoder::decode(file, {}, 1 << 28, nullptr, 0, &request);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.masks.frameCount(), 1);
    QVERIFY(result.masks.maskFor(0).contains(120, 60));
    QVERIFY(!result.masks.maskFor(0).contains(40, 60));
    const auto low = SpriteDecoder::decodeMasks(file);
    QVERIFY2(low.result.ok, qPrintable(low.result.error));
    QCOMPARE(low.size, QSize(kW, kH)); // the file's own size
}

void TstSpriteMask::maskPassSizes()
{
    QCOMPARE(SpriteDecoder::maskPassSize({256, 128}), QSize(256, 128)); // its own size
    QCOMPARE(SpriteDecoder::maskPassSize({4000, 3000}), QSize(4000, 3000));
    QCOMPARE(SpriteDecoder::maskPassSize({8192, 8192}).width(), 4096);  // 16 Mpixel at most, the aspect kept
    const QSize big = SpriteDecoder::maskPassSize({8192, 4096});
    QVERIFY(qint64(big.width()) * big.height() <= SpriteDecoder::kMaskPassMaxPixels);
    QCOMPARE(big.width(), big.height() * 2);
    QCOMPARE(SpriteDecoder::maskPassSize({100000, 1}, 1000), QSize(1000, 1)); // never an empty side
    QCOMPARE(SpriteDecoder::maskPassSize({0, 5}), QSize());
    QCOMPARE(SpriteDecoder::maskPassSize({10, 10}, 0), QSize());
    Fixture fx;
    QCOMPARE(SpriteDecoder::nativeSize(fx.file), QSize(kW, kH));
    QCOMPARE(SpriteDecoder::nativeSize(fx.dir.path() + u"/nothing.apng"_s), QSize());
}

void TstSpriteMask::theMaskPassKeepsTheFullResolutionSilhouette()
{
    Fixture fx;
    const auto low = SpriteDecoder::decodeMasks(fx.file);
    QVERIFY2(low.result.ok, qPrintable(low.result.error));
    QCOMPARE(low.size, QSize(kW, kH));
    QVERIFY(low.result.frames.images.isEmpty()); // streamed: nothing accumulates
    QCOMPARE(low.result.decodedFrames, 3);
    QCOMPARE(int(low.result.masks.quality()), int(MaskQuality::Full));
    // The same masks the frame cache path makes: the alpha of the file, the same threshold.
    const SpriteDecoder::MaskRequest request;
    const auto cached = SpriteDecoder::decode(fx.file, {}, 1 << 28, nullptr, 0, &request);
    QVERIFY(cached.ok);
    for (int i = 0; i < 3; ++i) {
        QVERIFY(low.result.masks.maskFor(i) == cached.masks.maskFor(i));
        QVERIFY(low.result.masks.maskFor(i).contains(40 + 80 * i, 60));
        QVERIFY(!low.result.masks.maskFor(i).contains(40 + 80 * ((i + 1) % 3), 60));
    }
    // Cancelled before it starts.
    std::atomic_bool cancel{true};
    QVERIFY(SpriteDecoder::decodeMasks(fx.file, &cancel).result.cancelled);
}

void TstSpriteMask::aThinLineKeepsItsCell()
{
    // A one-pixel horizontal line across a 1200 x 600 frame: with area averaging down to 128 px its alpha would fall under 16/255.
    QImage image(1200, 600, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    for (int x = 0; x < 1200; ++x)
        image.setPixelColor(x, 301, QColor(255, 255, 255, 255));
    for (int y = 0; y < 600; ++y)
        image.setPixelColor(77, y, QColor(255, 255, 255, 255)); // and a vertical one
    using namespace animfixtures;
    QByteArray out("\x89PNG\r\n\x1a\n", 8);
    out += pngChunk("IHDR", be32(1200) + be32(600) + QByteArray("\x08\x06\x00\x00\x00", 5));
    out += pngChunk("acTL", be32(2) + be32(0));
    quint32 sequence = 0;
    for (int i = 0; i < 2; ++i) {
        out += pngChunk("fcTL", be32(sequence++) + be32(1200) + be32(600) + be32(0) + be32(0) + be16(100) + be16(1000) + QByteArray("\x00\x00", 2));
        const QByteArray data = rawPixels(image);
        out += i == 0 ? pngChunk("IDAT", data) : pngChunk("fdAT", be32(sequence++) + data);
    }
    out += pngChunk("IEND", {});
    Fixture fx(out);
    const auto low = SpriteDecoder::decodeMasks(fx.file);
    QVERIFY2(low.result.ok, qPrintable(low.result.error));
    QCOMPARE(low.size, QSize(1200, 600));
    const AlphaMask &m = low.result.masks.maskFor(0);
    for (int x = 0; x < 1200; x += 13)
        QVERIFY2(m.contains(x, 301), qPrintable(QString::number(x)));
    for (int y = 0; y < 600; y += 13)
        QVERIFY2(m.contains(77, y), qPrintable(QString::number(y)));
    QVERIFY(!m.contains(600, 100));
    // Cell for cell the mask of the full-resolution alpha.
    QVERIFY(m == AlphaMask::fromAlpha(image.constBits() + 3, int(image.bytesPerLine()), 4, 1200, 600));
}

void TstSpriteMask::masksChangeNothingAboutTheDecodedFrames()
{
    const auto same = [](const SpriteDecoder::Result &a, const SpriteDecoder::Result &b, const char *what = "") {
        QVERIFY(a.ok && b.ok);
        QCOMPARE(a.frames.images.size(), b.frames.images.size());
        for (qsizetype i = 0; i < a.frames.images.size(); ++i)
            QVERIFY2(a.frames.images.at(i) == b.frames.images.at(i),
                     qPrintable(QStringLiteral("%1 frame %2").arg(QLatin1String(what)).arg(i)));
        QCOMPARE(a.frames.durationsMs, b.frames.durationsMs);
        QCOMPARE(a.frames.bytes, b.frames.bytes);
        QCOMPARE(a.frames.size, b.frames.size);
        QCOMPARE(a.decodedFrames, b.decodedFrames);
    };
    const SpriteDecoder::MaskRequest request;
    Fixture fx;
    for (const int fps : {0, 24}) {
        const auto plain = SpriteDecoder::decode(fx.file, {}, 1 << 28, nullptr, fps);
        const auto masked = SpriteDecoder::decode(fx.file, {}, 1 << 28, nullptr, fps, &request);
        same(plain, masked, fps ? "apng fixed fps" : "apng");
        QVERIFY(plain.masks.isNull() && !masked.masks.isNull());
    }
    // Scaled to a target as the frame cache does.
    same(SpriteDecoder::decode(fx.file, {100, 50}, 1 << 28), SpriteDecoder::decode(fx.file, {100, 50}, 1 << 28, nullptr, 0, &request), "apng scaled");

    // WebP: a still, and an animated one when ffmpeg can make it.
    QTemporaryDir dir;
    const QString still = dir.path() + u"/still.webp"_s;
    if (frameImage(1).save(still, "webp")) {
        same(SpriteDecoder::decode(still, {}, 1 << 28), SpriteDecoder::decode(still, {}, 1 << 28, nullptr, 0, &request), "still webp");
        const SpriteDecoder::MaskRequest drop{kMaskBudgetBytes, false};
        const auto dropped = SpriteDecoder::decode(still, {}, 1 << 28, nullptr, 0, &drop);
        QVERIFY(dropped.ok);
        QVERIFY(dropped.frames.images.isEmpty());
        QCOMPARE(dropped.decodedFrames, 1);
        QCOMPARE(dropped.masks.frameCount(), 1);
    } else {
        qInfo("Qt's WebP plugin can not write: the still WebP cases are skipped");
    }
#ifndef MURASAKI_HAVE_LIBWEBP
    qInfo("no libwebp: Qt's animated WebP reader is not deterministic (tst_spritewebp), the animated cases are skipped");
    return;
#endif
    const QString animated = dir.path() + u"/anim.webp"_s;
    QProcess ffmpeg;
    ffmpeg.start(u"ffmpeg"_s, {u"-v"_s, u"error"_s, u"-y"_s, u"-i"_s, fx.file, u"-c:v"_s, u"libwebp_anim"_s, u"-loop"_s, u"0"_s, u"-lossless"_s, u"1"_s, animated});
    if (!ffmpeg.waitForFinished(60000) || ffmpeg.exitCode() != 0 || !QFileInfo::exists(animated)) {
        qInfo("ffmpeg can not make an animated WebP: the animated cases are skipped");
        return;
    }
    const auto plain = SpriteDecoder::decode(animated, {}, 1 << 28);
    if (!plain.ok || plain.frames.images.size() < 2) {
        qInfo("Qt's WebP plugin does not read the animated file: the animated cases are skipped");
        return;
    }
    // Animated WebP pixels are composited by libwebp now (they were not deterministic through Qt's plugin: tst_spritewebp), so the
    // comparison is exact like the others.
    same(plain, SpriteDecoder::decode(animated, {}, 1 << 28), "animated webp, twice without masks");
    same(plain, SpriteDecoder::decode(animated, {}, 1 << 28, nullptr, 0, &request), "animated webp");
    const auto masked = SpriteDecoder::decode(animated, {}, 1 << 28, nullptr, 0, &request);
    QCOMPARE(masked.masks.frameCount(), int(plain.decodedFrames));
    const SpriteDecoder::MaskRequest drop{kMaskBudgetBytes, false};
    const auto dropped = SpriteDecoder::decode(animated, {}, 0, nullptr, 0, &drop); // maxBytes 0: no frame cache budget
    QVERIFY2(dropped.ok, qPrintable(dropped.error));
    QVERIFY(dropped.frames.images.isEmpty());
    QCOMPARE(dropped.decodedFrames, plain.decodedFrames);
    QCOMPARE(dropped.masks.frameCount(), int(plain.decodedFrames));
    const auto pass = SpriteDecoder::decodeMasks(animated);
    QVERIFY2(pass.result.ok, qPrintable(pass.result.error));
    QCOMPARE(pass.result.decodedFrames, plain.decodedFrames);
}

void TstSpriteMask::prefixVariantsDoNotDeleteEachOther()
{
    Fixture fx;
    QTemporaryDir cache;
    const QString root = cache.path();
    QVERIFY(loadMaskPass(root, u"item"_s, u"idle"_s, fx.file).ok);
    QVERIFY(loadMaskPass(root, u"item"_s, u"idle-left"_s, fx.file).ok);
    const QString key = maskCacheKey(fx.file);
    const QString idle = maskCachePath(root, u"item"_s, u"idle"_s, key);
    const QString idleLeft = maskCachePath(root, u"item"_s, u"idle-left"_s, key);
    QVERIFY(QFile::exists(idle) && QFile::exists(idleLeft));
    // The file changes; loading `idle` removes only its own old file.
    testutil::writeFile(fx.file, blocksApng(3, 1));
    {
        QFile touched(fx.file);
        QVERIFY(touched.open(QIODevice::ReadWrite));
        QVERIFY(touched.setFileTime(QFileInfo(fx.file).lastModified().addSecs(5), QFileDevice::FileModificationTime));
    }
    QVERIFY(loadMaskPass(root, u"item"_s, u"idle"_s, fx.file).ok);
    QVERIFY(!QFile::exists(idle));
    QVERIFY(QFile::exists(idleLeft)); // not touched
    QVERIFY(QFile::exists(maskCachePath(root, u"item"_s, u"idle"_s, maskCacheKey(fx.file))));
    // And a stray file that only looks like it is left alone too.
    const QString stray = QFileInfo(idle).absolutePath() + u"/" + maskCacheSafeName(u"idle"_s) + u"-" + maskCacheHash(u"idle"_s) + u"-notahexkey.mask"_s;
    testutil::writeFile(stray, "x");
    testutil::writeFile(fx.file, blocksApng(3, 2));
    {
        QFile touched(fx.file);
        QVERIFY(touched.open(QIODevice::ReadWrite));
        QVERIFY(touched.setFileTime(QFileInfo(fx.file).lastModified().addSecs(10), QFileDevice::FileModificationTime));
    }
    QVERIFY(loadMaskPass(root, u"item"_s, u"idle"_s, fx.file).ok);
    QVERIFY(QFile::exists(stray));
}

void TstSpriteMask::unicodeNamesDoNotShareADirectory()
{
    Fixture fx;
    QTemporaryDir cache;
    const QString root = cache.path();
    const QString a = u"日本"_s, b = u"中国"_s;
    QCOMPARE(maskCacheSafeName(a), maskCacheSafeName(b)); // the sanitizer alone can not tell them apart
    QVERIFY(maskCacheHash(a) != maskCacheHash(b));
    const QString key = maskCacheKey(fx.file);
    QVERIFY(maskCachePath(root, a, u"v"_s, key) != maskCachePath(root, b, u"v"_s, key));
    QVERIFY(QFileInfo(maskCachePath(root, a, u"v"_s, key)).absolutePath() != QFileInfo(maskCachePath(root, b, u"v"_s, key)).absolutePath());
    QVERIFY(maskCachePath(root, u"anima:1"_s, u"v"_s, key) != maskCachePath(root, u"anima_1"_s, u"v"_s, key));
    // Variant names collide the same way in the file name.
    QVERIFY(maskCachePath(root, u"i"_s, a, key) != maskCachePath(root, u"i"_s, b, key));
    // Two items, one variant name, one file: each keeps its own cache file across loads.
    QVERIFY(loadMaskPass(root, a, u"v"_s, fx.file).ok);
    QVERIFY(loadMaskPass(root, b, u"v"_s, fx.file).ok);
    QVERIFY(QFile::exists(maskCachePath(root, a, u"v"_s, key)));
    QVERIFY(QFile::exists(maskCachePath(root, b, u"v"_s, key)));
    QVERIFY(loadMaskPass(root, a, u"v"_s, fx.file).fromCache);
    QVERIFY(loadMaskPass(root, b, u"v"_s, fx.file).fromCache);
}

void TstSpriteMask::anEmptyCacheRootIsAnError()
{
    Fixture fx;
    const auto load = loadMaskPass(QString(), u"i"_s, u"v"_s, fx.file);
    QVERIFY(!load.ok);
    QVERIFY(!load.error.isEmpty());
}

void TstSpriteMask::theCacheIsWrittenReadAndInvalidatedWithTheFile()
{
    Fixture fx;
    QTemporaryDir cache;
    const QString root = cache.path();
    const auto first = loadMaskPass(root, u"anima:9001"_s, u"disc"_s, fx.file);
    QVERIFY2(first.ok, qPrintable(first.error));
    QVERIFY(!first.fromCache);
    QCOMPARE(first.frames, 3);
    const QString key = maskCacheKey(fx.file);
    QVERIFY(!key.isEmpty());
    const QString path = maskCachePath(root, u"anima:9001"_s, u"disc"_s, key);
    QVERIFY(path.startsWith(root + u"/murasaki/pets/anima_9001-"_s + maskCacheHash(u"anima:9001"_s) + u"/disc-"_s + maskCacheHash(u"disc"_s) + u'-'));
    QVERIFY(path.endsWith(u".mask"_s));
    QVERIFY(QFile::exists(path));
    // The next time it is read, not decoded: a decode of a deleted file would fail, and the cache does not need it... but the
    // key does, so the file stays; what proves the read is fromCache and the identical masks.
    const auto second = loadMaskPass(root, u"anima:9001"_s, u"disc"_s, fx.file);
    QVERIFY2(second.ok, qPrintable(second.error));
    QVERIFY(second.fromCache);
    QCOMPARE(second.masks.frameCount(), 3);
    for (int i = 0; i < 3; ++i)
        QVERIFY(second.masks.maskFor(i) == first.masks.maskFor(i));
    QCOMPARE(second.size, first.size);

    // The file changes (another order of frames, same size): another key, the old cache file is removed, new masks.
    testutil::writeFile(fx.file, blocksApng(3, 1));
    {
        QFile touched(fx.file);
        QVERIFY(touched.open(QIODevice::ReadWrite));
        QVERIFY(touched.setFileTime(QFileInfo(fx.file).lastModified().addSecs(5), QFileDevice::FileModificationTime));
    }
    const QString newKey = maskCacheKey(fx.file);
    QVERIFY(newKey != key);
    const auto third = loadMaskPass(root, u"anima:9001"_s, u"disc"_s, fx.file);
    QVERIFY2(third.ok, qPrintable(third.error));
    QVERIFY(!third.fromCache);
    QVERIFY(!QFile::exists(path));
    QVERIFY(QFile::exists(maskCachePath(root, u"anima:9001"_s, u"disc"_s, newKey)));
    QVERIFY(third.masks.maskFor(0).contains(20 + 80 + 20, 60)); // frame 0 is now the block of old frame 1
    // Another variant of the same item is not touched.
    const auto other = loadMaskPass(root, u"anima:9001"_s, u"other"_s, fx.file);
    QVERIFY(other.ok);
    const auto again = loadMaskPass(root, u"anima:9001"_s, u"disc"_s, fx.file);
    QVERIFY(again.fromCache);
}

void TstSpriteMask::aBadCacheFileIsReplaced()
{
    Fixture fx;
    QTemporaryDir cache;
    const QString root = cache.path();
    const QString path = maskCachePath(root, u"item"_s, u"v"_s, maskCacheKey(fx.file));
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    // Garbage under the right name.
    testutil::writeFile(path, QByteArray(300, 'x'));
    auto load = loadMaskPass(root, u"item"_s, u"v"_s, fx.file);
    QVERIFY2(load.ok, qPrintable(load.error));
    QVERIFY(!load.fromCache);
    QVERIFY(loadMaskPass(root, u"item"_s, u"v"_s, fx.file).fromCache); // and now it is a good one
    // A valid cache of another picture (written for a different size) is not used for this file.
    MaskBuilder builder(64, 64, 3);
    QImage other(64, 64, QImage::Format_RGBA8888);
    other.fill(QColor(0, 0, 0, 255));
    for (int i = 0; i < 3; ++i)
        builder.addFrame(other.constBits() + 3, int(other.bytesPerLine()), 4, 64, 64);
    QVERIFY(writeMaskFile(path.toStdString(), builder.finish()));
    load = loadMaskPass(root, u"item"_s, u"v"_s, fx.file);
    QVERIFY(load.ok);
    QVERIFY(!load.fromCache);
    QCOMPARE(load.size, QSize(kW, kH));
}

void TstSpriteMask::cacheNamesCanNotEscapeTheCacheDirectory()
{
    Fixture fx;
    QTemporaryDir cache;
    const QString root = cache.path() + u"/root"_s;
    QVERIFY(QDir().mkpath(root));
    for (const QString &name : {u"../../evil"_s, u"/etc/passwd"_s, u".."_s, u"."_s, u""_s, u"a/b/../../../c"_s, u"x\0y"_s, u"..\\..\\w"_s}) {
        const QString path = maskCachePath(root, name, name, u"k"_s);
        QVERIFY2(QDir::cleanPath(path).startsWith(root + u"/murasaki/pets/"_s), qPrintable(path));
        const QString item = path.mid((root + u"/murasaki/pets/"_s).size());
        QCOMPARE(item.count(u'/'), 1); // <item>-<hash>/<variant>-<hash>-<key>.mask: no extra component
        QVERIFY(!item.startsWith(u'.') && !item.startsWith(u'/'));
        const auto load = loadMaskPass(root, name, name, fx.file);
        QVERIFY(load.ok);
    }
    QVERIFY(!QFile::exists(cache.path() + u"/evil"_s));
    QVERIFY(!QFile::exists(cache.path() + u"/c"_s));
    QCOMPARE(maskCacheSafeName(QString(500, u'a')).size(), 80);
    QCOMPARE(maskCacheSafeName(u"anima:123"_s), u"anima_123"_s);
}

void TstSpriteMask::aMissingFileIsAnError()
{
    QTemporaryDir dir, cache;
    const auto load = loadMaskPass(cache.path(), u"i"_s, u"v"_s, dir.path() + u"/none.apng"_s);
    QVERIFY(!load.ok);
    QVERIFY(!load.cancelled);
    QVERIFY(!load.error.isEmpty());
    // An unreadable (not an animation) file.
    const QString junk = dir.path() + u"/junk.apng"_s;
    testutil::writeFile(junk, QByteArray(1000, 'z'));
    const auto bad = loadMaskPass(cache.path(), u"i"_s, u"v"_s, junk);
    QVERIFY(!bad.ok);
    QVERIFY(QDir(cache.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()); // and nothing was cached for it
    std::atomic_bool cancel{true};
    QVERIFY(loadMaskPass(cache.path(), u"i"_s, u"v"_s, junk, &cancel).cancelled);
}

QTEST_APPLESS_MAIN(TstSpriteMask)
#include "tst_spritemask.moc"
