// murasaki-scene-vk video textures: the MP4 a .tex carries, decoded from memory with libav. The clip is a few hundred bytes; nothing
// here touches a file or the network.

#include "assets/tex.h"
#include "assets/texfile.h"
#include "assets/vfs.h"
#include "assets/videodecoder.h"
#include "core/context.h"
#include "render/texture.h"
#include "render/videoconvert.h"
#include "videofixtures.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <cstring>
#include <random>

using namespace murasaki::scene;

namespace {

std::vector<uint8_t> clip()
{
    return std::vector<uint8_t>(std::begin(videofix::kClip), std::end(videofix::kClip));
}

// A texture file: header, TEXB0003 container with one mip whose payload is `payload`, the video flag set.
std::vector<uint8_t> videoTex(const std::vector<uint8_t> &payload, uint32_t flags = TexFlags::Video | TexFlags::ClampUVs)
{
    std::vector<uint8_t> b;
    const auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i)
            b.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    const auto tag = [&](const char *t) {
        b.insert(b.end(), t, t + 8);
        b.push_back(0);
    };
    tag("TEXV0005");
    tag("TEXI0001");
    u32(0);      // format
    u32(flags);
    u32(64); u32(48); u32(64); u32(48);
    u32(0);      // unknown
    tag("TEXB0003");
    u32(1);                       // images
    u32(0xFFFFFFFFu);             // not an embedded picture
    u32(1);                       // mips
    u32(64); u32(48);
    u32(0);                       // not LZ4
    u32(static_cast<uint32_t>(payload.size()));
    u32(static_cast<uint32_t>(payload.size()));
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

std::vector<uint8_t> clip128()
{
    return std::vector<uint8_t>(std::begin(videofix::kClip128), std::end(videofix::kClip128));
}

bool nearColor(const uint8_t *rgba, int r, int g, int b)
{
    return std::abs(rgba[0] - r) < 48 && std::abs(rgba[1] - g) < 48 && std::abs(rgba[2] - b) < 48 && rgba[3] == 255;
}

// The texels of an RGBA8 texture, read back (a texture the GPU wrote into: the CPU path never needs this).
std::vector<uint8_t> readBack(const VkContext &ctx, GpuTexture &texture)
{
    const uint32_t width = texture.width(), height = texture.height();
    GpuBuffer buffer = ctx.createBuffer(static_cast<VkDeviceSize>(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    ctx.submitAndWait([&](VkCommandBuffer cb) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = texture.image();
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &dep);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cb, texture.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.handle(), 1, &region);
    });
    buffer.invalidate();
    const auto *data = static_cast<const uint8_t *>(buffer.mapped());
    return std::vector<uint8_t>(data, data + static_cast<size_t>(width) * height * 4);
}

} // namespace

class TstSceneVkVideo : public QObject
{
    Q_OBJECT
private slots:
    void aClipPlaysFromMemoryAndLoops()
    {
        if (!videoSupported())
            QSKIP("built without libav");
        std::string error;
        auto video = VideoDecoder::open(clip(), error);
        QVERIFY2(video, error.c_str());
        QCOMPARE(video->width(), 64u);
        QCOMPARE(video->height(), 48u);
        const size_t middle = (24 * 64 + 32) * 4;
        const uint8_t *first = video->frameAt(0.0);
        QVERIFY(first);
        QVERIFY2(nearColor(first + middle, 255, 0, 0), "first picture is red");
        // nothing new until the next picture's time comes
        QVERIFY(!video->frameAt(0.0));
        // the decoder runs ahead of the clock by a few pictures: ask for a time and wait until what it has is the picture of that time
        const auto showAt = [&](double seconds, int r, int g, int b) {
            for (int tries = 0; tries < 400; ++tries) {
                if (const uint8_t *f = video->frameAt(seconds); f && nearColor(f + middle, r, g, b))
                    return true;
                QTest::qWait(5);
            }
            return false;
        };
        QVERIFY2(showAt(0.85, 0, 0, 255), "the second half is blue");
        // and then it starts over: red again
        QVERIFY2(showAt(1.15, 255, 0, 0), "the clip loops");
    }

    void aScriptCanSeekAndStopTheLoop()
    {
        if (!videoSupported())
            QSKIP("built without libav");
        std::string error;
        VideoDecoder::Options options;
        options.hardware = false;
        auto video = VideoDecoder::open(clip(), error, options);
        QVERIFY2(video, error.c_str());
        const size_t middle = (24 * 64 + 32) * 4;
        const auto showAt = [&](double seconds, int r, int g, int b) {
            for (int tries = 0; tries < 400; ++tries) {
                if (const uint8_t *f = video->frameAt(seconds); f && nearColor(f + middle, r, g, b))
                    return true;
                QTest::qWait(5);
            }
            return false;
        };
        QVERIFY(video->frameAt(0.0));
        QVERIFY(!video->ended()); // it loops
        // into the second half: the clock restarts at the target, the picture is the blue one, the position says where
        video->seek(0.7);
        QVERIFY2(showAt(0.7, 0, 0, 255), "a seek into the second half shows blue");
        QVERIFY2(video->position() >= 0.5 && video->position() <= 0.7, "the picture on show is from the second half");
        // back to the start
        video->seek(0.0);
        QVERIFY2(showAt(0.0, 255, 0, 0), "a seek to the start shows red");
        QVERIFY(video->position() < 0.5);
        // a stream that does not loop stops on its last picture and says so
        video->setLoop(false);
        video->seek(0.55);
        QVERIFY2(showAt(0.55, 0, 0, 255), "blue");
        bool ended = false;
        for (int tries = 0; tries < 400 && !ended; ++tries) {
            video->frameAt(3.0);
            ended = video->ended();
            QTest::qWait(5);
        }
        QVERIFY2(ended, "the last picture is on show and the stream is spent");
        QVERIFY(!video->frameAt(6.0));
        QVERIFY(video->position() > 0.7);
        // a seek wakes it again, and so does looping
        video->seek(0.0);
        QVERIFY2(showAt(0.0, 255, 0, 0), "red again after the end");
        QVERIFY(!video->ended());
        video->setLoop(true);
        video->seek(0.9);
        QVERIFY2(showAt(0.9, 0, 0, 255), "blue");
        QVERIFY2(showAt(1.15, 255, 0, 0), "and it loops once more");
    }

    // A seek to the end (setCurrentTime(duration)) of a clip whose last picture is long lands past that picture's start: it is shown all
    // the same, the stream says it is spent, and the decoder is still there for the next seek (it used to end its thread: frozen for good).
    void aSeekToTheEndShowsTheLastPicture()
    {
        if (!videoSupported())
            QSKIP("built without libav");
        std::string error;
        VideoDecoder::Options options;
        options.hardware = false;
        auto video = VideoDecoder::open(std::vector<uint8_t>(std::begin(videofix::kClipLongTail), std::end(videofix::kClipLongTail)), error, options);
        QVERIFY2(video, error.c_str());
        QVERIFY(video->duration() > 0.9);
        const size_t middle = (24 * 64 + 32) * 4;
        const auto showAt = [&](double seconds, int r, int g, int b) {
            for (int tries = 0; tries < 400; ++tries) {
                if (const uint8_t *f = video->frameAt(seconds); f && nearColor(f + middle, r, g, b))
                    return true;
                QTest::qWait(5);
            }
            return false;
        };
        QVERIFY(video->frameAt(0.0));
        video->setLoop(false);
        video->seek(video->duration());
        QVERIFY2(showAt(video->duration(), 0, 0, 255), "the last picture (blue) is on show");
        bool ended = false;
        for (int tries = 0; tries < 400 && !ended; ++tries) {
            video->frameAt(video->duration() + 1.0);
            ended = video->ended();
            QTest::qWait(5);
        }
        QVERIFY2(ended, "and the stream is spent");
        video->seek(0.0);
        QVERIFY2(showAt(0.0, 255, 0, 0), "the decoder still plays after it");
        // looping (the even clip: this one's loop offset is counted from its last picture's start), the same seek goes round to the start
        video = VideoDecoder::open(clip(), error, options);
        QVERIFY2(video, error.c_str());
        QVERIFY(video->frameAt(0.0));
        video->seek(video->duration());
        QVERIFY2(showAt(video->duration() + 0.2, 255, 0, 0), "a looping video goes on from the start");
    }

    void aMissingGpuFallsBackToTheCpu()
    {
        if (!videoSupported())
            QSKIP("built without libav");
        std::string error;
        VideoDecoder::Options options;
        options.renderNode = "/dev/null/none"; // no such render node: VAAPI cannot open it
        auto video = VideoDecoder::open(clip(), error, options);
        QVERIFY2(video, error.c_str());
        QVERIFY(!video->hardware());
        const uint8_t *first = nullptr;
        for (int tries = 0; tries < 200 && !first; ++tries) {
            first = video->frameAt(0.0);
            if (!first)
                QTest::qWait(5);
        }
        QVERIFY(first);
        // and the machine's own choice plays the clip too, on the GPU or not
        auto any = VideoDecoder::open(clip(), error);
        QVERIFY2(any, error.c_str());
        QVERIFY(any->frameAt(0.0));
        qInfo() << "VAAPI decoding on this machine:" << any->hardware();
    }

    // A decoder that keeps the GPU's surfaces still gives RGBA through frameAt (it converts on the spot): the fallback of the fallbacks.
    void aSurfaceDecoderStillGivesRgbaThroughFrameAt()
    {
        if (!videoSupported())
            QSKIP("built without libav");
        std::string error;
        VideoDecoder::Options options;
        options.zeroCopy = true;
        auto video = VideoDecoder::open(clip128(), error, options);
        QVERIFY2(video, error.c_str());
        if (!video->hardware())
            QSKIP("no VAAPI on this machine: nothing keeps surfaces");
        const uint8_t *first = nullptr;
        for (int tries = 0; tries < 200 && !first; ++tries) {
            first = video->frameAt(0.0);
            if (!first)
                QTest::qWait(5);
        }
        QVERIFY(first);
        QVERIFY2(nearColor(first + (48 * 128 + 64) * 4, 255, 0, 0), "the first picture is red");
    }

    // Software decoding has no surface to export: the status says so and the pictures come through frameAt.
    void aSoftwareDecoderExportsNothing()
    {
        if (!videoSupported())
            QSKIP("built without libav");
        std::string error;
        VideoDecoder::Options options;
        options.hardware = false;
        options.zeroCopy = true;
        auto video = VideoDecoder::open(clip128(), error, options);
        QVERIFY2(video, error.c_str());
        QVERIFY(!video->hardware());
        const MurasakiVideoExport e = video->exportFrame(0.0);
        QCOMPARE(e.status, -1);
        QCOMPARE(e.fd, -1);
        const uint8_t *first = nullptr;
        for (int tries = 0; tries < 200 && !first; ++tries) {
            first = video->frameAt(0.0);
            if (!first)
                QTest::qWait(5);
        }
        QVERIFY(first);
    }

    // The same clip through the GPU conversion and through the copy path: the same pictures, within the rounding of two colour conversions.
    void theGpuConversionMatchesTheCopyPath()
    {
        if (!videoSupported())
            QSKIP("built without libav");
        std::string error;
        auto ctx = VkContext::create(contextOptionsFromEnvironment(), error);
        if (!ctx)
            QSKIP(("no Vulkan device: " + error).c_str());
        auto converter = VideoConverter::create(*ctx, error);
        if (!converter)
            QSKIP(("no zero-copy on this device: " + error).c_str());
        VideoDecoder::Options options;
        options.renderNode = ctx->renderNode();
        options.zeroCopy = true;
        auto video = VideoDecoder::open(clip128(), error, options);
        QVERIFY2(video, error.c_str());
        if (!video->hardware())
            QSKIP("no VAAPI on this machine");
        auto reference = VideoDecoder::open(clip128(), error, [&] { VideoDecoder::Options o; o.renderNode = ctx->renderNode(); return o; }());
        QVERIFY2(reference, error.c_str());
        auto texture = GpuTexture::createDynamic(*ctx, video->width(), video->height(), false, true, error);
        QVERIFY2(texture, error.c_str());

        QString lastSeen;
        const auto compareAt = [&](double seconds, int r, int g, int b) {
            for (int tries = 0; tries < 400; ++tries) {
                MurasakiVideoExport e = video->exportFrame(seconds);
                if (e.status == 1) {
                    std::string why;
                    QVERIFY2(converter->convert(e, *texture, why), why.c_str());
                    const std::vector<uint8_t> gpu = readBack(*ctx, *texture);
                    const uint8_t *px = gpu.data() + (48 * 128 + 64) * 4;
                    if (!nearColor(px, r, g, b)) {
                        lastSeen = QStringLiteral("%1 %2 %3 %4").arg(px[0]).arg(px[1]).arg(px[2]).arg(px[3]);
                        QTest::qWait(5); // the decoder runs a few pictures ahead of the clock: the picture of this time is on its way
                        continue;
                    }
                    // the reference decoder is asked for the same time, as often as it takes to reach it
                    const uint8_t *copy = nullptr;
                    for (int again = 0; again < 400; ++again) {
                        if (const uint8_t *f = reference->frameAt(seconds); f && nearColor(f + (48 * 128 + 64) * 4, r, g, b)) {
                            copy = f;
                            break;
                        }
                        QTest::qWait(5);
                    }
                    QVERIFY2(copy, "the copy path shows the same picture");
                    long total = 0;
                    for (size_t i = 0; i < gpu.size(); ++i)
                        if ((i & 3) != 3)
                            total += std::abs(int(gpu[i]) - int(copy[i]));
                    const double mean = double(total) / (gpu.size() / 4 * 3);
                    QVERIFY2(mean < 6.0, qPrintable(QStringLiteral("mean difference %1 per channel").arg(mean)));
                    return;
                }
                if (e.status == -1)
                    QSKIP("this machine's decoder gives no exportable surface (the picture comes through the copy path)");
                QTest::qWait(5);
            }
            QFAIL(qPrintable(QStringLiteral("no converted picture had the colour of the clip (last: %1)").arg(lastSeen)));
        };
        compareAt(0.0, 255, 0, 0);
        compareAt(0.85, 0, 0, 255);
        // a buffer is imported once and kept: the clip's few surfaces, not one import per picture
        QVERIFY(converter->importedCount() <= 48);
    }

    // A video texture of hundreds of megabytes is not held in memory: the parser says where the stream starts, and the decoder reads it from
    // where it lies in the file.
    void aBigVideoTextureIsPlayedFromItsFile()
    {
        if (!videoSupported())
            QSKIP("built without libav");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // a .tex whose stream is the clip followed by padding up to over 16 MiB, so that the file counts as big
        std::vector<uint8_t> payload = clip();
        const size_t clipSize = payload.size();
        payload.resize(kBigTexBytes + 4096, 0);
        const std::vector<uint8_t> file = videoTex(payload);
        QDir(dir.path()).mkpath(QStringLiteral("materials"));
        QFile out(dir.filePath(QStringLiteral("materials/movie.tex")));
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(reinterpret_cast<const char *>(file.data()), static_cast<qint64>(file.size()));
        out.close();
        Vfs vfs;
        vfs.addDirectory(dir.path().toStdString());

        // the header says where the stream starts; the bytes can end before it does
        TexReadOptions peek;
        peek.videoInPlace = true;
        const auto head = parseTex(std::span<const uint8_t>(file.data(), 256), peek);
        QVERIFY(head);
        QCOMPARE(head->videoSize, uint64_t(payload.size()));
        QCOMPARE(head->videoOffset, file.size() - payload.size());
        QVERIFY(head->video.empty());

        // through the Vfs: located on disk, nothing read
        const auto tex = peekVideoTexFile(vfs, "materials/movie.tex");
        QVERIFY(tex);
        QVERIFY(tex->video.empty());
        QCOMPARE(tex->videoSize, uint64_t(payload.size()));
        QCOMPARE(QString::fromStdString(tex->videoFile), dir.filePath(QStringLiteral("materials/movie.tex")));
        QCOMPARE(tex->videoFileOffset, file.size() - payload.size());
        QVERIFY(!peekVideoTexFile(vfs, "materials/missing.tex"));

        // and the decoder plays the clip from that place in the file
        std::string error;
        auto video = VideoDecoder::openFile(tex->videoFile, tex->videoFileOffset, clipSize, error, {});
        QVERIFY2(video, error.c_str());
        QCOMPARE(video->width(), 64u);
        const uint8_t *first = nullptr;
        for (int tries = 0; tries < 200 && !first; ++tries) {
            first = video->frameAt(0.0);
            if (!first)
                QTest::qWait(5);
        }
        QVERIFY(first);
        QVERIFY2(nearColor(first + (24 * 64 + 32) * 4, 255, 0, 0), "the first picture is red");
        // a file region that is not there is refused, not a crash
        QVERIFY(!VideoDecoder::openFile("/nonexistent/none.tex", 0, 100, error, {}));
        QVERIFY(!VideoDecoder::openFile(tex->videoFile, 0, 0, error, {}));
    }

    void thePicturesAreNoMoreThanTheSceneShows()
    {
        if (!videoSupported())
            QSKIP("built without libav");
        const auto shown = [&](double maxFps) {
            std::string error;
            auto video = VideoDecoder::open(clip(), error, [] { VideoDecoder::Options o; o.hardware = false; return o; }());
            if (!video)
                return -1;
            video->setMaxFps(maxFps);
            QTest::qWait(150); // the first pictures are decoded ahead before the cap is read; let them go by
            int count = 0;
            (void)video->frameAt(0.45);
            for (double t = 0.5; t < 0.95; t += 0.1) {
                for (int tries = 0; tries < 100; ++tries) {
                    if (video->frameAt(t)) {
                        ++count;
                        break;
                    }
                    QTest::qWait(5);
                }
            }
            return count;
        };
        const int all = shown(0.0);
        const int capped = shown(5.0);
        QVERIFY2(all >= 4, qPrintable(QString::number(all)));
        QVERIFY2(capped >= 1 && capped <= 3, qPrintable(QString::number(capped)));
    }

    void aStreamThatIsNotAClipIsRefused()
    {
        if (!videoSupported())
            QSKIP("built without libav");
        std::string error;
        QVERIFY(!VideoDecoder::open({}, error));
        QVERIFY(!error.empty());
        std::vector<uint8_t> noise(4096);
        std::mt19937 rng(7);
        for (auto &v : noise)
            v = static_cast<uint8_t>(rng());
        QVERIFY(!VideoDecoder::open(noise, error));
        // the first half of a clip: the demuxer may or may not find a picture in it, but it must not crash or hang
        auto half = clip();
        half.resize(half.size() / 2);
        (void)VideoDecoder::open(half, error);
    }

    void aDamagedClipNeverCrashesTheDecoder()
    {
        if (!videoSupported())
            QSKIP("built without libav");
        std::mt19937 rng(11);
        for (int round = 0; round < 40; ++round) {
            auto bytes = clip();
            for (int i = 0; i < 1 + round % 6; ++i)
                bytes[rng() % bytes.size()] = static_cast<uint8_t>(rng());
            std::string error;
            if (auto video = VideoDecoder::open(std::move(bytes), error)) {
                for (double t = 0.0; t < 2.0; t += 0.1)
                    (void)video->frameAt(t);
            }
        }
    }

    void aVideoTextureCarriesItsStream()
    {
        const auto payload = clip();
        const auto bytes = videoTex(payload);
        std::string error;
        const auto tex = parseTex(bytes, {}, &error);
        QVERIFY2(tex, error.c_str());
        QVERIFY(tex->hasVideo());
        QCOMPARE(tex->video.size(), payload.size());
        QCOMPARE(tex->videoSize, uint64_t{payload.size()});
        QVERIFY(std::memcmp(tex->video.data(), payload.data(), payload.size()) == 0);
        // a header read knows the size and leaves the bytes
        const auto header = parseTex(bytes, TexReadOptions{false}, &error);
        QVERIFY2(header, error.c_str());
        QVERIFY(header->hasVideo());
        QCOMPARE(header->videoSize, uint64_t{payload.size()});
        QVERIFY(header->video.empty());
    }
};

QTEST_GUILESS_MAIN(TstSceneVkVideo)
#include "tst_scenevk_video.moc"
