// murasaki-scene-vk picture decoding: libjpeg and libdeflate fast paths against what the pictures hold, the stb fallback, hostile
// files, the decoded-picture cache and the parallel loop. Pictures are written by Qt in the test.

#include "assets/imagedecode.h"
#include "assets/texcache.h"
#include "util/parallel.h"

#include <QBuffer>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <mutex>
#include <set>
#include <thread>

using namespace murasaki::scene;

namespace {

QImage pattern(int w, int h, QImage::Format format)
{
    QImage image(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // smooth gradients and noise-like detail, so every PNG filter gets used
            const int r = (x * 255 / std::max(1, w - 1));
            const int g = (y * 255 / std::max(1, h - 1));
            const int b = ((x * 7 + y * 13) ^ (x * y)) & 255;
            image.setPixel(x, y, qRgba(r, g, b, format == QImage::Format_ARGB32 ? 40 + ((x + y) % 200) : 255));
        }
    }
    return image.convertToFormat(format);
}

std::vector<uint8_t> encode(const QImage &image, const char *format, int quality = -1)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format, quality);
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

bool decodes(const std::vector<uint8_t> &bytes, DecodedImage &out, uint32_t maxDim = 32768, uint64_t maxBytes = 1ull << 28)
{
    std::string error;
    return decodeImage(bytes, maxDim, maxBytes, out, error);
}

} // namespace

class TstSceneVkImageDecode : public QObject
{
    Q_OBJECT

    QTemporaryDir m_cache;

private slots:
    void initTestCase()
    {
        QVERIFY(m_cache.isValid());
        qputenv("XDG_CACHE_HOME", m_cache.path().toUtf8());
        qunsetenv("MURASAKI_TEXTURE_CACHE");
    }

    void pngOfEveryColourTypeDecodesToTheSamePixels()
    {
        for (const QImage::Format format : {QImage::Format_RGB32, QImage::Format_ARGB32, QImage::Format_Grayscale8, QImage::Format_Indexed8, QImage::Format_RGBA8888}) {
            QImage source = pattern(97, 61, format);
            if (format == QImage::Format_Indexed8) {
                source = pattern(97, 61, QImage::Format_RGB32).convertToFormat(QImage::Format_Indexed8, Qt::ThresholdDither);
            }
            const auto png = encode(source, "PNG");
            DecodedImage image;
            QVERIFY(decodes(png, image));
            QCOMPARE(image.width, 97u);
            QCOMPARE(image.height, 61u);
            const QImage expected = source.convertToFormat(QImage::Format_RGBA8888);
            for (int y = 0; y < 61; ++y) {
                QVERIFY2(std::memcmp(image.rgba.data() + static_cast<size_t>(y) * 97 * 4, expected.constScanLine(y), 97 * 4) == 0,
                         qPrintable(QString("format %1 row %2").arg(format).arg(y)));
            }
        }
    }

    void sixteenBitAndTruncatedPngsAreHandledWithoutTrouble()
    {
        // 16 bits a channel is not the fast path's business: stb gets it and the sizes still agree
        QImage deep(40, 30, QImage::Format_RGBA64);
        deep.fill(QColor(10, 200, 90));
        DecodedImage image;
        QVERIFY(decodes(encode(deep, "PNG"), image));
        QCOMPARE(image.width, 40u);
        // a truncated PNG: stb either fails or fills what it has, never crashes
        auto png = encode(pattern(300, 200, QImage::Format_RGB32), "PNG");
        png.resize(png.size() / 2);
        DecodedImage cut;
        decodes(png, cut);
        // damage in the middle of the compressed data
        auto damaged = encode(pattern(300, 200, QImage::Format_RGB32), "PNG");
        for (size_t i = damaged.size() / 3; i < damaged.size() / 3 + 40; ++i)
            damaged[i] ^= 0x5A;
        DecodedImage broken;
        decodes(damaged, broken);
    }

    void jpegDecodesCloseToWhatWasEncoded()
    {
        const QImage source = pattern(160, 90, QImage::Format_RGB32);
        const auto jpeg = encode(source, "JPEG", 95);
        DecodedImage image;
        QVERIFY(decodes(jpeg, image));
        QCOMPARE(image.width, 160u);
        QCOMPARE(image.height, 90u);
        const QImage expected = source.convertToFormat(QImage::Format_RGBA8888);
        long error = 0;
        for (size_t i = 0; i < image.rgba.size(); ++i) {
            error += std::abs(static_cast<int>(image.rgba[i]) - static_cast<int>(expected.constBits()[i]));
        }
        QVERIFY2(error / static_cast<long>(image.rgba.size()) < 12, qPrintable(QString::number(error / static_cast<long>(image.rgba.size()))));
        for (size_t i = 3; i < image.rgba.size(); i += 4)
            QCOMPARE(image.rgba[i], uint8_t{255});
        // a grey JPEG
        DecodedImage grey;
        QVERIFY(decodes(encode(pattern(64, 64, QImage::Format_Grayscale8), "JPEG"), grey));
        QCOMPARE(grey.rgba[0], grey.rgba[1]);
    }

    void hostileFilesAreRefusedOrFailAndNeverAllocateByTheirClaims()
    {
        DecodedImage image;
        QVERIFY(!decodes({}, image));
        uint32_t w = 0, h = 0;
        QVERIFY(!imageSize(std::span<const uint8_t>{}, w, h)); // a null buffer must not reach stb's memcpy
        QVERIFY(!decodes({0x89, 'P', 'N', 'G'}, image));
        QVERIFY(!decodes({0xFF, 0xD8, 0xFF, 0xE0, 0, 0}, image));
        std::vector<uint8_t> noise(5000);
        uint32_t x = 99;
        for (auto &b : noise) {
            x = x * 1664525u + 1013904223u;
            b = static_cast<uint8_t>(x >> 24);
        }
        QVERIFY(!decodes(noise, image));
        // a PNG whose header claims a gigantic picture: refused by the limits before anything is allocated
        auto png = encode(pattern(8, 8, QImage::Format_RGB32), "PNG");
        const auto claim = [&](uint32_t w, uint32_t h) {
            auto copy = png;
            if (copy.size() < 24)
                return copy;
            for (int i = 0; i < 4; ++i) {
                copy[16 + i] = static_cast<uint8_t>(w >> (24 - 8 * i));
                copy[20 + i] = static_cast<uint8_t>(h >> (24 - 8 * i));
            }
            return copy;
        };
        std::string error;
        QVERIFY(!decodeImage(claim(60000, 60000), 32768, 1ull << 28, image, error));
        QVERIFY(!error.empty());
        QVERIFY(!decodeImage(claim(30000, 30000), 32768, 1ull << 28, image, error)); // within the side limit, over the byte limit
        QVERIFY(!decodeImage(claim(0, 8), 32768, 1ull << 28, image, error));
        // a smaller claim than the data holds: not a crash (the decoder's own checks decide)
        decodes(claim(4, 4), image);
        // a JPEG that claims a big size (patched SOF0 dimensions) against the byte limit
        auto jpeg = encode(pattern(16, 16, QImage::Format_RGB32), "JPEG");
        for (size_t i = 0; i + 8 < jpeg.size(); ++i) {
            if (jpeg[i] == 0xFF && jpeg[i + 1] == 0xC0) { // SOF0: length(2) precision(1) height(2) width(2)
                jpeg[i + 5] = 0xFF; jpeg[i + 6] = 0xF0; jpeg[i + 7] = 0xFF; jpeg[i + 8] = 0xF0;
                break;
            }
        }
        QVERIFY(!decodeImage(jpeg, 32768, 1ull << 20, image, error));
    }

    void thePictureCacheRoundTripsAndRejectsDamage()
    {
        const auto source = encode(pattern(600, 520, QImage::Format_RGB32), "PNG");
        DecodedImage decoded;
        QVERIFY(decodes(source, decoded));
        DecodedImage cached;
        QVERIFY(!loadCachedImage(source, 32768, 1ull << 28, cached)); // not stored yet
        storeCachedImage(source, decoded);
        flushCachedImageStores();
        QVERIFY(loadCachedImage(source, 32768, 1ull << 28, cached));
        QCOMPARE(cached.width, decoded.width);
        QCOMPARE(cached.height, decoded.height);
        QVERIFY(cached.rgba == decoded.rgba);
        // another picture, another entry
        auto other = source;
        other[other.size() / 2] ^= 1;
        QVERIFY(!loadCachedImage(other, 32768, 1ull << 28, cached));
        // limits apply to what is read back too
        QVERIFY(!loadCachedImage(source, 100, 1ull << 28, cached));
        QVERIFY(!loadCachedImage(source, 32768, 1000, cached));
        // small pictures are not worth a file
        const auto tiny = encode(pattern(8, 8, QImage::Format_RGB32), "PNG");
        DecodedImage tinyImage;
        QVERIFY(decodes(tiny, tinyImage));
        storeCachedImage(tiny, tinyImage);
        flushCachedImageStores();
        QVERIFY(!loadCachedImage(tiny, 32768, 1ull << 28, cached));
        // damaged files: truncated, wrong magic, wrong lengths, garbage
        const QDir dir(m_cache.filePath("murasaki/texture-cache"));
        const QStringList files = dir.entryList({"*.bin"}, QDir::Files);
        QCOMPARE(files.size(), 1);
        const QString path = dir.filePath(files.first());
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray good = f.readAll();
        f.close();
        const auto write = [&](const QByteArray &bytes) {
            QFile w(path);
            QVERIFY(w.open(QIODevice::WriteOnly | QIODevice::Truncate));
            w.write(bytes);
        };
        write(good.left(good.size() / 2));
        QVERIFY(!loadCachedImage(source, 32768, 1ull << 28, cached));
        write(QByteArray("XXXX") + good.mid(4));
        QVERIFY(!loadCachedImage(source, 32768, 1ull << 28, cached));
        write(good + "extra");
        QVERIFY(!loadCachedImage(source, 32768, 1ull << 28, cached));
        QByteArray lie = good;
        lie[12] = static_cast<char>(0xFF);
        lie[13] = static_cast<char>(0xFF);
        lie[14] = static_cast<char>(0xFF);
        lie[15] = 0x7F; // a compressed size far beyond the data
        write(lie);
        QVERIFY(!loadCachedImage(source, 32768, 1ull << 28, cached));
        QByteArray wrongDims = good;
        wrongDims[4] = static_cast<char>(wrongDims[4] + 1); // width no longer matches the decompressed length
        write(wrongDims);
        QVERIFY(!loadCachedImage(source, 32768, 1ull << 28, cached));
        write(good);
        QVERIFY(loadCachedImage(source, 32768, 1ull << 28, cached)); // and the intact file works again
        // switched off
        qputenv("MURASAKI_TEXTURE_CACHE", "0");
        QVERIFY(!loadCachedImage(source, 32768, 1ull << 28, cached));
        qunsetenv("MURASAKI_TEXTURE_CACHE");
    }

    void parallelForRunsEveryIndexOnceAndNestedLoopsStayBounded()
    {
        std::vector<std::atomic<int>> hits(200);
        parallelFor(hits.size(), [&](size_t i) { hits[i].fetch_add(1); });
        for (auto &h : hits)
            QCOMPARE(h.load(), 1);
        // nested: the helpers over all loops never pass the limit
        std::atomic<int> running{0}, peak{0};
        std::set<std::thread::id> threads;
        std::mutex mutex;
        parallelFor(8, [&](size_t) {
            parallelFor(8, [&](size_t) {
                const int now = running.fetch_add(1) + 1;
                int p = peak.load();
                while (now > p && !peak.compare_exchange_weak(p, now)) {
                }
                {
                    std::lock_guard lock(mutex);
                    threads.insert(std::this_thread::get_id());
                }
                QTest::qSleep(2);
                running.fetch_sub(1);
            });
        });
        QVERIFY2(peak.load() <= maxHelperThreads() + 1, qPrintable(QString::number(peak.load())));
        QCOMPARE(activeHelpers().load(), 0);
        parallelFor(0, [](size_t) { QFAIL("no index"); });
    }
};

QTEST_APPLESS_MAIN(TstSceneVkImageDecode)
#include "tst_scenevk_imagedecode.moc"
