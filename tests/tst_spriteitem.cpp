// SpriteItem: an animation decoded once, uploaded as textures and drawn without the video path. The item is drawn
// by Qt Quick's software backend on the offscreen platform and grabbed, so no GPU, display or desktop session is
// needed (and none is ever touched): this runs everywhere.

#include "spriteitem.h"
#include "testutil.h"
#include "animfixtures.h"

#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>

#include <time.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

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
        raw.append('\0');
        raw.append(reinterpret_cast<const char *>(image.constScanLine(y)), image.width() * 4);
    }
    return qCompress(raw, 9).mid(4);
}

// Three frames of `delayMs` each, in the given colors.
QByteArray alphaApng(QList<QColor> colors, int delayMs)
{
    using namespace animfixtures;
    QByteArray out("\x89PNG\r\n\x1a\n", 8);
    out += pngChunk("IHDR", be32(kSize) + be32(kSize) + QByteArray("\x08\x06\x00\x00\x00", 5));
    out += pngChunk("acTL", be32(colors.size()) + be32(0));
    quint32 sequence = 0;
    for (int i = 0; i < colors.size(); ++i) {
        out += pngChunk("fcTL", be32(sequence++) + be32(kSize) + be32(kSize) + be32(0) + be32(0) + be16(delayMs)
                                    + be16(1000) + QByteArray("\x00\x00", 2));
        const QByteArray data = rawPixels(halfImage(colors[i]));
        out += i == 0 ? pngChunk("IDAT", data) : pngChunk("fdAT", be32(sequence++) + data);
    }
    out += pngChunk("IEND", {});
    return out;
}

} // namespace

class TstSpriteItem : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void transparentPixelsStayTransparentAndPremultiplied();
    void croppedFramesDrawTheSamePicture();
    void cuttingHappensInTheDecodeAndTheBudgetCountsWhatIsKept();
    void framesWithPlacesThatDoNotMatchAreRefused();
    void framesAdvanceAtTheirOwnPaceAndLoop();
    void pausedHoldsTheFrame();
    void theWrapsAreAnnouncedAheadAtTheirExactTimeAndCancelledByAPause();
    void aChangeOfTimingIsLiveAndDecodesNothing();
    void rotationAndFlipAreTransformsOfTheSceneAndDecodeNothingAgain();
    void theHandoverFromTheVideoDoesNotSoundTheFirstLoopTwice_data();
    void theHandoverFromTheVideoDoesNotSoundTheFirstLoopTwice();
    void aPreloadedPictureIsSwappedInWithoutDecodingAndTheOldSetIsDropped();
    void aRestartPlaysFromTheFirstFrameWithoutDecoding();
    void theCapLimitsHowOftenTheFrameChanges();
    void aLateTimerLowersTheCapAndAnEasedOneRaisesIt();
    void newFramesReplaceTheOldOnesWithoutAGap();
    void aFailedRequestDropsTheFrames();
    void clearDropsTheFrames();
    void theCpuCopiesAreFreedOnceUploaded();
    void destroyingTheItemDoesNotWaitForTheDecoder();
    void aCancelledRequestNeverRuns();
    void aRequestForAFixedRateUsesIt();

private:
    struct Scene {
        std::unique_ptr<QQuickWindow> window;
        SpriteItem *item = nullptr;
    };
    void setUp(Scene &scene);
    QImage frame(Scene &scene);
    bool renderUntil(Scene &scene, const std::function<bool()> &done, int ms = 10000);

    QTemporaryDir m_dir;
};

void TstSpriteItem::initTestCase()
{
    QVERIFY(m_dir.isValid());
    testutil::writeFile(m_dir.filePath(u"rgb.apng"_s), alphaApng({Qt::red, Qt::green, Qt::blue}, 100));
    testutil::writeFile(m_dir.filePath(u"still.apng"_s), alphaApng({Qt::yellow}, 100));
}

void TstSpriteItem::setUp(Scene &scene)
{
    scene.window = std::make_unique<QQuickWindow>();
    scene.window->setColor(Qt::transparent);
    scene.window->resize(kSize, kSize);
    scene.item = new SpriteItem;
    scene.item->setParentItem(scene.window->contentItem());
    scene.item->setSize(QSizeF(kSize, kSize));
    scene.window->show();
    QVERIFY(QTest::qWaitForWindowExposed(scene.window.get()));
}

QImage TstSpriteItem::frame(Scene &scene)
{
    return scene.window->grabWindow().convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

bool TstSpriteItem::renderUntil(Scene &scene, const std::function<bool()> &done, int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QTest::qWait(20);
        frame(scene);
        if (done())
            return true;
    }
    return false;
}

// The color of the opaque strip (x 16..31) of an image.
static QRgb strip(const QImage &image)
{
    return image.pixel(kSize / 2 - 8, kSize / 2);
}

void TstSpriteItem::transparentPixelsStayTransparentAndPremultiplied()
{
    Scene scene;
    setUp(scene);
    QSignalSpy loaded(scene.item, &SpriteItem::loaded);
    QVERIFY(!scene.item->hasFrames());
    scene.item->request(m_dir.filePath(u"rgb.apng"_s), kSize, kSize, 1 << 24);
    QVERIFY2(renderUntil(scene, [&] { return scene.item->hasFrames() && loaded.count() == 1; }), "never loaded");
    // (the right half of each frame is clear: the half that is drawn and a pixel of margin are kept - SpriteFrames::cropTransparentMargins)
    const qint64 kept = qint64(3) * (kSize / 2 + 1) * kSize * 4;
    QCOMPARE(scene.item->cacheBytes(), kept);
    QCOMPARE(loaded.at(0).at(0).toDouble(), double(kept));

    const QImage image = frame(scene);
    // Whichever frame shows, the transparent half is clear and carries no color, and the soft strip is
    // premultiplied: a color of alpha 128 reads back as at most 128 per channel.
    const QRgb clear = image.pixel(kSize * 3 / 4, kSize / 2);
    QCOMPARE(qAlpha(clear), 0);
    QCOMPARE(qRed(clear) + qGreen(clear) + qBlue(clear), 0);
    const QRgb half = image.pixel(kSize / 8, kSize / 2);
    QVERIFY2(qAbs(qAlpha(half) - 128) <= 6, qPrintable(u"alpha %1"_s.arg(qAlpha(half))));
    QVERIFY2(qRed(half) <= qAlpha(half) + 6 && qGreen(half) <= qAlpha(half) + 6 && qBlue(half) <= qAlpha(half) + 6,
             "not premultiplied");
    QCOMPARE(qAlpha(strip(image)), 255);
}

void TstSpriteItem::croppedFramesDrawTheSamePicture()
{
    // A small shape off the middle that moves between two frames, so that the two crop rectangles differ: shown whole, and shown cut down to its rectangle, at several sizes, it is the same picture.
    const auto make = [](int shift) {
        QImage image(kSize, kSize, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::transparent);
        for (int y = 10; y < 25; ++y)
            for (int x = 7 + shift; x < 30 + shift; ++x)
                image.setPixelColor(x, y + shift / 2, QColor(200, 40, 40, (x + y) % 3 ? 255 : 128).toRgb());
        return image;
    };
    // `first`: how long frame 0 shows; with 20 ms the clock goes on to frame 1 and stays there
    const auto frames = [&](int first) {
        SpriteFrames whole;
        whole.images = {make(0), make(20)};
        whole.durationsMs = {first, 100000};
        whole.size = QSize(kSize, kSize);
        whole.bytes = 2 * kSize * kSize * 4;
        return whole;
    };
    SpriteFrames sample = frames(100000);
    sample.cropTransparentMargins();
    QVERIFY(sample.bytes < 2 * kSize * kSize * 4 / 4);
    QCOMPARE(sample.offsets.size(), 2);
    QVERIFY(sample.offsets.at(0) != sample.offsets.at(1)); // the frames are cut at different places
    for (const int first : {100000, 20}) {
        for (const double scale : {1.0, 2.0, 0.5, 1.5, 0.37}) {
            QImage shown[2];
            for (int variant = 0; variant < 2; ++variant) {
                Scene scene;
                setUp(scene);
                const int side = int(std::ceil(kSize * scale));
                scene.window->resize(side, side);
                scene.item->setSize(QSizeF(kSize * scale, kSize * scale));
                SpriteFrames set = frames(first);
                if (variant)
                    set.cropTransparentMargins();
                scene.item->showFrames(std::move(set));
                QVERIFY(renderUntil(scene, [&] { return scene.item->hasFrames() && scene.item->pendingCpuFrames() == 0 && scene.item->currentFrame() == (first == 20 ? 1 : 0); }));
                shown[variant] = frame(scene);
            }
            int drawn = 0;
            for (int y = 0; y < shown[0].height(); ++y)
                for (int x = 0; x < shown[0].width(); ++x)
                    drawn += qAlpha(shown[0].pixel(x, y)) > 0;
            QVERIFY2(drawn > 0, qPrintable(u"nothing was drawn (scale %1, frame %2)"_s.arg(scale).arg(first == 20)));
            QVERIFY2(shown[0] == shown[1], qPrintable(u"scale %1, frame %2"_s.arg(scale).arg(first == 20)));
        }
    }
    // a frame with nothing in it, and a frame that is all drawn
    SpriteFrames edge;
    edge.images = {QImage(kSize, kSize, QImage::Format_RGBA8888_Premultiplied), QImage(kSize, kSize, QImage::Format_RGBA8888_Premultiplied)};
    edge.images[0].fill(Qt::transparent);
    edge.images[1].fill(QColor(1, 2, 3).rgba());
    edge.size = QSize(kSize, kSize);
    edge.cropTransparentMargins();
    QCOMPARE(edge.images[0].size(), QSize(1, 1));
    QCOMPARE(edge.images[1].size(), QSize(kSize, kSize));
    QCOMPARE(edge.offsets[1], QPoint());
}

void TstSpriteItem::cuttingHappensInTheDecodeAndTheBudgetCountsWhatIsKept()
{
    // rgb.apng: three frames of kSize x kSize with the right half clear: 49152 bytes whole, 25344 cut
    Scene scene;
    setUp(scene);
    QSignalSpy loaded(scene.item, &SpriteItem::loaded);
    QSignalSpy failed(scene.item, &SpriteItem::failed);
    scene.item->request(m_dir.filePath(u"rgb.apng"_s), kSize, kSize, 30000);
    QVERIFY2(renderUntil(scene, [&] { return loaded.count() == 1 || failed.count() == 1; }), "no answer");
    QCOMPARE(failed.count(), 0);
    QCOMPARE(scene.item->cacheBytes(), qint64(3) * (kSize / 2 + 1) * kSize * 4);
    // the decoder without the cut still refuses by the whole frames
    const SpriteDecoder::Result whole = SpriteDecoder::decode(m_dir.filePath(u"rgb.apng"_s), QSize(kSize, kSize), 30000);
    QVERIFY(!whole.ok);
    const SpriteDecoder::Result cut = SpriteDecoder::decode(m_dir.filePath(u"rgb.apng"_s), QSize(kSize, kSize), 30000, nullptr, 0, nullptr, true);
    QVERIFY2(cut.ok, qPrintable(cut.error));
    QVERIFY(cut.frames.consistent() && cut.frames.offsets.size() == 3);
    QCOMPARE(cut.frames.bytes, qint64(3) * (kSize / 2 + 1) * kSize * 4);
    // and a limit under the cut size refuses
    QVERIFY(!SpriteDecoder::decode(m_dir.filePath(u"rgb.apng"_s), QSize(kSize, kSize), 20000, nullptr, 0, nullptr, true).ok);
}

void TstSpriteItem::framesWithPlacesThatDoNotMatchAreRefused()
{
    Scene scene;
    setUp(scene);
    SpriteFrames frames;
    frames.images = {QImage(8, 8, QImage::Format_RGBA8888_Premultiplied), QImage(8, 8, QImage::Format_RGBA8888_Premultiplied)};
    frames.images[0].fill(Qt::red);
    frames.images[1].fill(Qt::red);
    frames.durationsMs = {100, 100};
    frames.size = QSize(kSize, kSize);
    frames.offsets = {QPoint(1, 1)}; // one place for two images
    QVERIFY(!frames.consistent());
    scene.item->showFrames(frames);
    QTest::qWait(200);
    frame(scene);
    QVERIFY(!scene.item->hasFrames());
    frames.offsets = {QPoint(1, 1), QPoint(2, 2)};
    QVERIFY(frames.consistent());
}

void TstSpriteItem::framesAdvanceAtTheirOwnPaceAndLoop()
{
    Scene scene;
    setUp(scene);
    scene.item->request(m_dir.filePath(u"rgb.apng"_s), kSize, kSize, 1 << 24);
    QVERIFY(renderUntil(scene, [&] { return scene.item->hasFrames(); }));
    QSet<QRgb> seen;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 1500) {
        QTest::qWait(10);
        seen.insert(strip(frame(scene)));
    }
    // 100 ms per frame over 1.5 s: all three colors, looping.
    QVERIFY2(seen.contains(qRgba(255, 0, 0, 255)) && seen.contains(qRgba(0, 255, 0, 255))
                 && seen.contains(qRgba(0, 0, 255, 255)),
             qPrintable(u"%1 colors seen"_s.arg(seen.size())));
}

static qint64 monotonicNow()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return qint64(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

void TstSpriteItem::theWrapsAreAnnouncedAheadAtTheirExactTimeAndCancelledByAPause()
{
    Scene scene;
    setUp(scene);
    QSignalSpy wraps(scene.item, &SpriteItem::wrapped);
    QSignalSpy cancelled(scene.item, &SpriteItem::wrapCancelled);
    const auto heardAtPtr = std::make_shared<QList<qint64>>(); // when each announcement arrived (outlives the item's signals)
    const QList<qint64> &heardAt = *heardAtPtr;
    connect(scene.item, &SpriteItem::wrapped, scene.item, [heardAtPtr] { *heardAtPtr << monotonicNow(); });
    scene.item->request(m_dir.filePath(u"rgb.apng"_s), kSize, kSize, 1 << 24);
    QVERIFY(renderUntil(scene, [&] { return scene.item->hasFrames(); }));
    QVERIFY(renderUntil(scene, [&] { return wraps.count() >= 5; }, 4000));
    // loop 0 is the start; every later wrap is one loop (3 x 100 ms) after the one before, to the millisecond, and was announced before it happened
    QCOMPARE(wraps.at(0).at(1).toLongLong(), 0LL);
    for (int i = 1; i < wraps.count(); ++i) {
        QCOMPARE(wraps.at(i).at(1).toLongLong(), qint64(i));
        const qint64 gap = wraps.at(i).at(0).toLongLong() - wraps.at(i - 1).at(0).toLongLong();
        QVERIFY2(std::llabs(gap - 300'000'000) < 1'500'000, qPrintable(QString::number(gap)));
        if (i >= 2)
            QVERIFY2(wraps.at(i).at(0).toLongLong() >= heardAt.at(i) - 1'000'000, "announced after it happened");
    }
    // a pause withdraws an announcement that has not come true; the clock running again announces the same wrap at its new time
    QVERIFY(renderUntil(scene, [&] { return wraps.count() > 5 && wraps.last().at(0).toLongLong() > monotonicNow() + 20'000'000; }, 4000));
    const qint64 lastLoop = wraps.last().at(1).toLongLong();
    const qint64 lastTime = wraps.last().at(0).toLongLong();
    cancelled.clear();
    scene.item->setPaused(true);
    QCOMPARE(cancelled.count(), 1);
    QTest::qWait(200);
    const int before = wraps.count();
    scene.item->setPaused(false);
    QVERIFY(renderUntil(scene, [&] { return wraps.count() > before; }, 3000));
    QCOMPARE(wraps.at(before).at(1).toLongLong(), lastLoop);
    QVERIFY(wraps.at(before).at(0).toLongLong() > lastTime + 150'000'000); // later by about the time it was paused
    // a change of timing moves it: 2x speed halves the loop
    cancelled.clear();
    scene.item->setTimingSpeed(2.0);
    QVERIFY(cancelled.count() <= 1);
    const int atChange = wraps.count();
    QVERIFY(renderUntil(scene, [&] { return wraps.count() >= atChange + 3; }, 4000));
    const qint64 gap = wraps.at(wraps.count() - 1).at(0).toLongLong() - wraps.at(wraps.count() - 2).at(0).toLongLong();
    QVERIFY2(std::llabs(gap - 150'000'000) < 1'500'000, qPrintable(QString::number(gap)));
}

void TstSpriteItem::pausedHoldsTheFrame()
{
    Scene scene;
    setUp(scene);
    scene.item->request(m_dir.filePath(u"rgb.apng"_s), kSize, kSize, 1 << 24);
    QVERIFY(renderUntil(scene, [&] { return scene.item->hasFrames(); }));
    scene.item->setPaused(true);
    const int held = scene.item->currentFrame();
    const QRgb color = strip(frame(scene));
    QTest::qWait(450);
    QCOMPARE(scene.item->currentFrame(), held);
    QCOMPARE(strip(frame(scene)), color);
    scene.item->setPaused(false);
    QVERIFY(renderUntil(scene, [&] { return scene.item->currentFrame() != held; }, 2000));
}

void TstSpriteItem::rotationAndFlipAreTransformsOfTheSceneAndDecodeNothingAgain()
{
    // The real PetItem.qml, loaded from the source tree, with a stand-in for the Murasaki.Render module (the libmpv item is not needed here: it is only
    // loaded while no frames are on the GPU). The pictures are the ones of the other tests: the colored half is the left one (x 0 to 31), opaque from
    // x 16, the right half is clear.
    static const bool registered = (qmlRegisterType<SpriteItem>("Murasaki.Sprite", 1, 0, "SpriteItem"), true);
    Q_UNUSED(registered);
    const QString stub = m_dir.filePath(u"qml"_s);
    QVERIFY(QDir().mkpath(stub + u"/Murasaki/Render"_s));
    testutil::writeFile(stub + u"/Murasaki/Render/qmldir"_s, "module Murasaki.Render\nVideoItem 1.0 VideoItem.qml\n");
    // (the puppet's picture is not needed either: a stand-in with the properties and signals PetItem.qml uses)
    QVERIFY(QDir().mkpath(stub + u"/Murasaki/Puppet"_s));
    testutil::writeFile(stub + u"/Murasaki/Puppet/qmldir"_s, "module Murasaki.Puppet\nPuppetItem 1.0 PuppetItem.qml\n");
    testutil::writeFile(stub + u"/Murasaki/Puppet/PuppetItem.qml"_s,
                        "import QtQuick\nItem {\n property string itemDir; property string modelFile; property bool sandbox; property bool requireSandbox; property bool blocked; property bool look\n"
                        " property string expression; property bool paused; property int fps; property string fill; property real maxPixels; signal failed(string reason); signal pictureShown()\n"
                        " function playMotion(g, p) {} function stopMotion() {} function pointerAt(x, y) {} function clickAt(u, v, f) {}\n}\n");
    testutil::writeFile(stub + u"/Murasaki/Render/VideoItem.qml"_s,
                        "import QtQuick\nItem {\n property bool transparent; property bool audible; property string fill; property bool paused\n"
                        " property real speed; property int frameRateLimit; property string source\n signal failed(string reason); signal started()\n signal wrapped(real ns, real loop); signal wrapCancelled()\n}\n");
    QQmlEngine engine;
    engine.addImportPath(stub);
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(QT_TESTCASE_SOURCEDIR "/../src/overlay/PetItem.qml")));
    QVERIFY2(component.status() == QQmlComponent::Ready, qPrintable(component.errorString()));
    std::unique_ptr<QQuickItem> root(qobject_cast<QQuickItem *>(component.create()));
    QVERIFY(root);
    Scene scene;
    setUp(scene);
    root->setParentItem(scene.window->contentItem());
    root->setSize(QSizeF(kSize, kSize));
    auto *sprite = root->findChild<SpriteItem *>();
    QVERIFY(sprite);
    QSignalSpy loaded(sprite, &SpriteItem::loaded);
    root->setProperty("source", m_dir.filePath(u"rgb.apng"_s));
    root->setProperty("petW", double(kSize));
    root->setProperty("petH", double(kSize));
    root->setProperty("spriteEnabled", true);
    root->setProperty("spriteW", kSize);
    root->setProperty("spriteH", kSize);
    root->setProperty("spriteBudget", double(1 << 24));
    root->setProperty("spriteKey", u"k1"_s); // what starts the decode
    QVERIFY(renderUntil(scene, [&] { return loaded.count() == 1; }));
    const qint64 bytes = sprite->cacheBytes();

    const auto opaqueAt = [&](int x, int y) { return qAlpha(frame(scene).pixel(x, y)) > 200; };
    const auto clearAt = [&](int x, int y) { return qAlpha(frame(scene).pixel(x, y)) == 0; };
    const auto set = [&](double rotation, bool flipH, bool flipV) {
        root->setProperty("petRotation", rotation);
        root->setProperty("flipH", flipH);
        root->setProperty("flipV", flipV);
        frame(scene);
        QTest::qWait(30);
    };
    set(0, false, false);
    QVERIFY(opaqueAt(24, 32) && clearAt(40, 32));
    set(0, true, false); // flipped left to right: the colored half is the right one
    QVERIFY(opaqueAt(40, 32) && clearAt(24, 32));
    set(0, false, true); // flipped top to bottom: this picture does not change (every row is alike)
    QVERIFY(opaqueAt(24, 8) && opaqueAt(24, 56) && clearAt(40, 32));
    set(90, false, false); // clockwise: the left half goes to the top
    QVERIFY(opaqueAt(32, 24) && clearAt(32, 40));
    set(180, false, false);
    QVERIFY(opaqueAt(40, 32) && clearAt(24, 32));
    set(270, false, false); // the left half goes to the bottom
    QVERIFY(opaqueAt(32, 40) && clearAt(32, 24));
    set(90, true, false); // mirrored first, then turned: right half, then bottom
    QVERIFY(opaqueAt(32, 40) && clearAt(32, 24));
    set(90, false, true); // a vertical flip first changes nothing here, then the left half goes to the top
    QVERIFY(opaqueAt(32, 24) && clearAt(32, 40));
    // a sweep of angles: the frames are the same ones, decoded once, and the key (what would decode again) did not change
    for (int degrees = 0; degrees < 360; degrees += 15)
        set(degrees, degrees % 30 == 0, false);
    QCOMPARE(loaded.count(), 1);
    QCOMPARE(sprite->cacheBytes(), bytes);
    QVERIFY(sprite->hasFrames());
    QCOMPARE(root->property("spriteKey").toString(), u"k1"_s);
}

void TstSpriteItem::theHandoverFromTheVideoDoesNotSoundTheFirstLoopTwice_data()
{
    QTest::addColumn<bool>("videoAnnounced");
    QTest::newRow("the video had announced its loop 0") << true;
    QTest::newRow("no video (the frames were quicker)") << false;
}

void TstSpriteItem::theHandoverFromTheVideoDoesNotSoundTheFirstLoopTwice()
{
    // PetItem.qml with the stand-in video: the libmpv path announces loop 0 when it starts; the frames that replace it must not announce a loop 0 of their own
    // (the sound is scheduled for each announced wrap, so that would play it again). Without a video the frames announce their loop 0.
    QFETCH(bool, videoAnnounced);
    static const bool registered = (qmlRegisterType<SpriteItem>("Murasaki.Sprite", 1, 0, "SpriteItem"), true);
    Q_UNUSED(registered);
    const QString stub = m_dir.filePath(u"qml-handover"_s);
    QVERIFY(QDir().mkpath(stub + u"/Murasaki/Render"_s));
    testutil::writeFile(stub + u"/Murasaki/Render/qmldir"_s, "module Murasaki.Render\nVideoItem 1.0 VideoItem.qml\n");
    // (the puppet's picture is not needed either: a stand-in with the properties and signals PetItem.qml uses)
    QVERIFY(QDir().mkpath(stub + u"/Murasaki/Puppet"_s));
    testutil::writeFile(stub + u"/Murasaki/Puppet/qmldir"_s, "module Murasaki.Puppet\nPuppetItem 1.0 PuppetItem.qml\n");
    testutil::writeFile(stub + u"/Murasaki/Puppet/PuppetItem.qml"_s,
                        "import QtQuick\nItem {\n property string itemDir; property string modelFile; property bool sandbox; property bool requireSandbox; property bool blocked; property bool look\n"
                        " property string expression; property bool paused; property int fps; property string fill; property real maxPixels; signal failed(string reason); signal pictureShown()\n"
                        " function playMotion(g, p) {} function stopMotion() {} function pointerAt(x, y) {} function clickAt(u, v, f) {}\n}\n");
    testutil::writeFile(stub + u"/Murasaki/Render/VideoItem.qml"_s,
                        "import QtQuick\nItem {\n property bool transparent; property bool audible; property string fill; property bool paused\n"
                        " property real speed; property int frameRateLimit; property string source\n signal failed(string reason); signal started()\n signal wrapped(real ns, real loop); signal wrapCancelled()\n}\n");
    QQmlEngine engine;
    engine.addImportPath(stub);
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(QT_TESTCASE_SOURCEDIR "/../src/overlay/PetItem.qml")));
    QVERIFY2(component.status() == QQmlComponent::Ready, qPrintable(component.errorString()));
    std::unique_ptr<QQuickItem> root(qobject_cast<QQuickItem *>(component.create()));
    QVERIFY(root);
    Scene scene;
    setUp(scene);
    root->setParentItem(scene.window->contentItem());
    root->setSize(QSizeF(kSize, kSize));
    auto *sprite = root->findChild<SpriteItem *>();
    QVERIFY(sprite);
    QVector<qint64> loops;
    connect(sprite, &SpriteItem::wrapped, this, [&](qint64, qint64 loop) { loops.append(loop); });
    QSignalSpy atRoot(root.get(), SIGNAL(wrapped(double, double)));
    QSignalSpy loaded(sprite, &SpriteItem::loaded);
    root->setProperty("source", m_dir.filePath(u"rgb.apng"_s));
    root->setProperty("petW", double(kSize));
    root->setProperty("petH", double(kSize));
    root->setProperty("spriteEnabled", true);
    root->setProperty("spriteW", kSize);
    root->setProperty("spriteH", kSize);
    root->setProperty("spriteBudget", double(1 << 24));
    if (videoAnnounced) { // the video is on screen first and says that its loop began
        QQuickItem *video = nullptr;
        QTRY_VERIFY(([&] {
            for (auto *item : root->findChildren<QQuickItem *>(QString(), Qt::FindChildrenRecursively))
                if (item->property("frameRateLimit").isValid())
                    video = item;
            return video != nullptr;
        })());
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const double now = double(ts.tv_sec) * 1e9 + double(ts.tv_nsec);
        QVERIFY(QMetaObject::invokeMethod(video, "wrapped", Q_ARG(double, now), Q_ARG(double, 0)));
        QVERIFY(QMetaObject::invokeMethod(video, "wrapped", Q_ARG(double, now + 5e9), Q_ARG(double, 1)));
        QCOMPARE(atRoot.size(), 2); // (the pet's window hears it from the root)
    }
    root->setProperty("spriteKey", u"k1"_s); // what starts the decode
    QVERIFY(renderUntil(scene, [&] { return loaded.count() == 1 && !loops.isEmpty(); }));
    QVERIFY(!loops.contains(0) == videoAnnounced);
    QCOMPARE(loops.first(), videoAnnounced ? 1 : 0); // the next wrap is still announced, as always
}

void TstSpriteItem::aPreloadedPictureIsSwappedInWithoutDecodingAndTheOldSetIsDropped()
{
    // The real PetItem.qml: the picture of the state likely to come next is decoded into the second set while the first plays; when the pet changes state the sets
    // change places (no decode, so no wait), and the set that was in front is dropped.
    static const bool registered = (qmlRegisterType<SpriteItem>("Murasaki.Sprite", 1, 0, "SpriteItem"), true);
    Q_UNUSED(registered);
    const QString stub = m_dir.filePath(u"qml-preload"_s);
    QVERIFY(QDir().mkpath(stub + u"/Murasaki/Render"_s));
    testutil::writeFile(stub + u"/Murasaki/Render/qmldir"_s, "module Murasaki.Render\nVideoItem 1.0 VideoItem.qml\n");
    // (the puppet's picture is not needed either: a stand-in with the properties and signals PetItem.qml uses)
    QVERIFY(QDir().mkpath(stub + u"/Murasaki/Puppet"_s));
    testutil::writeFile(stub + u"/Murasaki/Puppet/qmldir"_s, "module Murasaki.Puppet\nPuppetItem 1.0 PuppetItem.qml\n");
    testutil::writeFile(stub + u"/Murasaki/Puppet/PuppetItem.qml"_s,
                        "import QtQuick\nItem {\n property string itemDir; property string modelFile; property bool sandbox; property bool requireSandbox; property bool blocked; property bool look\n"
                        " property string expression; property bool paused; property int fps; property string fill; property real maxPixels; signal failed(string reason); signal pictureShown()\n"
                        " function playMotion(g, p) {} function stopMotion() {} function pointerAt(x, y) {} function clickAt(u, v, f) {}\n}\n");
    testutil::writeFile(stub + u"/Murasaki/Render/VideoItem.qml"_s,
                        "import QtQuick\nItem {\n property bool transparent; property bool audible; property string fill; property bool paused\n"
                        " property real speed; property int frameRateLimit; property string source\n signal failed(string reason); signal started()\n signal wrapped(real ns, real loop); signal wrapCancelled()\n}\n");
    QQmlEngine engine;
    engine.addImportPath(stub);
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(QT_TESTCASE_SOURCEDIR "/../src/overlay/PetItem.qml")));
    QVERIFY2(component.status() == QQmlComponent::Ready, qPrintable(component.errorString()));
    std::unique_ptr<QQuickItem> root(qobject_cast<QQuickItem *>(component.create()));
    QVERIFY(root);
    Scene scene;
    setUp(scene);
    root->setParentItem(scene.window->contentItem());
    root->setSize(QSizeF(kSize, kSize));
    const auto sprites = root->findChildren<SpriteItem *>();
    QCOMPARE(sprites.size(), 2);
    SpriteItem *a = sprites.at(0);
    SpriteItem *b = sprites.at(1);
    QSignalSpy loadedA(a, &SpriteItem::loaded);
    QSignalSpy loadedB(b, &SpriteItem::loaded);
    QSignalSpy done(root.get(), SIGNAL(spriteDone(bool, double, QString)));
    QSignalSpy preloaded(root.get(), SIGNAL(preloadDone(bool, double, QString, QString)));
    const QString rgb = m_dir.filePath(u"rgb.apng"_s);
    const QString yellow = m_dir.filePath(u"still.apng"_s);
    root->setProperty("source", rgb);
    root->setProperty("petW", double(kSize));
    root->setProperty("petH", double(kSize));
    root->setProperty("spriteEnabled", true);
    root->setProperty("spriteW", kSize);
    root->setProperty("spriteH", kSize);
    root->setProperty("spriteBudget", double(1 << 24));
    root->setProperty("spriteKey", u"k1"_s);
    QVERIFY(renderUntil(scene, [&] { return loadedA.count() == 1; }));
    QCOMPARE(root->property("frontSprite").value<QObject *>(), a);
    const auto isYellow = [&] {
        const QRgb p = frame(scene).pixel(24, 32);
        return qRed(p) > 200 && qGreen(p) > 200 && qBlue(p) < 80;
    };

    // the next picture is prepared behind the first: it is decoded, invisible and not playing
    root->setProperty("preloadSource", yellow);
    root->setProperty("preloadW", kSize);
    root->setProperty("preloadH", kSize);
    root->setProperty("preloadBudget", double(1 << 24));
    root->setProperty("preloadKey", u"k2"_s);
    QVERIFY(renderUntil(scene, [&] { return preloaded.count() == 1; }));
    QCOMPARE(preloaded.at(0).at(0).toBool(), true);
    QCOMPARE(preloaded.at(0).at(2).toString(), u"k2"_s);
    QVERIFY(preloaded.at(0).at(1).toDouble() > 0);
    QVERIFY(b->hasFrames());
    QCOMPARE(done.size(), 1); // (the front's own picture; it did not speak for the prepared one)
    for (int i = 0; i < 6; ++i) {
        QVERIFY(!isYellow());
        QTest::qWait(40);
    }
    QCOMPARE(loadedA.count(), 1);

    // the pet changes state: the same key, no decode, the sets change places
    root->setProperty("source", yellow);
    root->setProperty("spriteKey", u"k2"_s);
    root->setProperty("preloadKey", QString()); // (nothing more to prepare)
    QCOMPARE(root->property("frontSprite").value<QObject *>(), b);
    QVERIFY(renderUntil(scene, [&] { return isYellow(); }, 1000));
    QTRY_COMPARE(done.size(), 2);
    QCOMPARE(done.at(1).at(0).toBool(), true);
    QCOMPARE(done.at(1).at(1).toDouble(), preloaded.at(0).at(1).toDouble());
    QCOMPARE(loadedA.count(), 1); // nothing was decoded for the change
    QCOMPARE(loadedB.count(), 1);
    QVERIFY(!a->hasFrames()); // the old set is gone
    QVERIFY(b->hasFrames());

    // a key that was not preloaded decodes in the front, as it always did
    root->setProperty("source", rgb);
    root->setProperty("spriteKey", u"k3"_s);
    QVERIFY(renderUntil(scene, [&] { return loadedB.count() == 2; }));
    QTRY_COMPARE(done.size(), 3);

    // a preload that cannot be made says so and leaves the front alone; an empty key drops what was prepared
    root->setProperty("preloadSource", m_dir.filePath(u"missing.apng"_s));
    root->setProperty("preloadKey", u"k4"_s);
    QVERIFY(renderUntil(scene, [&] { return preloaded.count() == 2; }));
    QCOMPARE(preloaded.at(1).at(0).toBool(), false);
    QCOMPARE(preloaded.at(1).at(2).toString(), u"k4"_s);
    QVERIFY(b->hasFrames());
    root->setProperty("preloadSource", yellow);
    root->setProperty("preloadKey", u"k5"_s);
    QVERIFY(renderUntil(scene, [&] { return preloaded.count() == 3 && a->hasFrames(); }));
    root->setProperty("preloadKey", QString());
    QVERIFY(!a->hasFrames());
    // and turning the cache off drops both
    root->setProperty("preloadKey", u"k6"_s);
    QVERIFY(renderUntil(scene, [&] { return a->hasFrames(); }));
    root->setProperty("spriteEnabled", false);
    QVERIFY(!a->hasFrames() && !b->hasFrames());
}

void TstSpriteItem::aRestartPlaysFromTheFirstFrameWithoutDecoding()
{
    Scene scene;
    setUp(scene);
    QSignalSpy loaded(scene.item, &SpriteItem::loaded);
    scene.item->request(m_dir.filePath(u"rgb.apng"_s), kSize, kSize, 1 << 24);
    QVERIFY(renderUntil(scene, [&] { return scene.item->hasFrames(); }));
    QVERIFY(renderUntil(scene, [&] { return scene.item->currentFrame() == 2; }, 3000)); // the third frame
    scene.item->restart();
    QCOMPARE(scene.item->currentFrame(), 0);
    QCOMPARE(loaded.count(), 1); // nothing was decoded
    QVERIFY(renderUntil(scene, [&] { return scene.item->currentFrame() == 1; }, 2000)); // and it plays on
    // nothing to restart without frames
    scene.item->clear();
    scene.item->restart();
    QCOMPARE(scene.item->currentFrame(), 0);
}

void TstSpriteItem::aChangeOfTimingIsLiveAndDecodesNothing()
{
    Scene scene;
    setUp(scene);
    QSignalSpy loaded(scene.item, &SpriteItem::loaded);
    QSignalSpy changed(scene.item, &SpriteItem::timingChanged);
    scene.item->request(m_dir.filePath(u"rgb.apng"_s), kSize, kSize, 1 << 24);
    QVERIFY(renderUntil(scene, [&] { return loaded.count() == 1; }));
    QCOMPARE(scene.item->timingFps(), 0);
    QCOMPARE(scene.item->timingSpeed(), 1.0);
    const qint64 bytes = scene.item->cacheBytes();

    scene.item->setTimingFps(60);
    QCOMPARE(scene.item->timingFps(), 60);
    scene.item->setTimingSpeed(2); // the other mode: the fixed rate goes
    QCOMPARE(scene.item->timingFps(), 0);
    QCOMPARE(scene.item->timingSpeed(), 2.0);
    scene.item->setTimingFps(15);
    QCOMPARE(scene.item->timingSpeed(), 1.0);
    QCOMPARE(changed.count(), 3);
    scene.item->setTimingFps(15); // no change, no signal
    QCOMPARE(changed.count(), 3);
    QTest::qWait(150);
    // the frames are the same ones: nothing was decoded or uploaded again
    QCOMPARE(loaded.count(), 1);
    QVERIFY(scene.item->hasFrames());
    QCOMPARE(scene.item->cacheBytes(), bytes);
    QCOMPARE(scene.item->frameCount(), 3);
    QCOMPARE(scene.item->frameDuration(0), 100); // the file's own delays stay in the frames
}

void TstSpriteItem::theCapLimitsHowOftenTheFrameChanges()
{
    Scene scene;
    setUp(scene);
    QSignalSpy loaded(scene.item, &SpriteItem::loaded);
    scene.item->request(m_dir.filePath(u"rgb.apng"_s), kSize, kSize, 1 << 24);
    QVERIFY(renderUntil(scene, [&] { return loaded.count() == 1; }));
    scene.item->setTimingFps(100); // a frame every 10 ms
    const auto changesIn = [&](int milliseconds) {
        int changes = 0;
        int last = scene.item->currentFrame();
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < milliseconds) {
            QTest::qWait(2);
            const int now = scene.item->currentFrame();
            changes += now != last;
            last = now;
        }
        return changes;
    };
    QVERIFY2(changesIn(500) > 20, "uncapped, a 100 fps pet changes frame all the time");
    scene.item->setFpsCap(10);
    QCOMPARE(scene.item->effectiveFpsCap(), 10);
    const int capped = changesIn(1000);
    QVERIFY2(capped >= 6 && capped <= 12, qPrintable(u"%1 changes in a second at a cap of 10"_s.arg(capped)));
    scene.item->setFpsCap(0);
    QCOMPARE(scene.item->effectiveFpsCap(), 0);
    QVERIFY(changesIn(500) > 20);
}

void TstSpriteItem::aLateTimerLowersTheCapAndAnEasedOneRaisesIt()
{
    Scene scene;
    setUp(scene);
    scene.item->setFpsCap(30);
    QCOMPARE(scene.item->effectiveFpsCap(), 30);
    for (int i = 0; i < 40; ++i)
        scene.item->noteLateness(60); // every frame a whole interval late
    QVERIFY2(scene.item->effectiveFpsCap() < 30 && scene.item->effectiveFpsCap() >= 20,
             qPrintable(u"cap %1"_s.arg(scene.item->effectiveFpsCap())));
    for (int i = 0; i < 600; ++i)
        scene.item->noteLateness(0);
    QCOMPARE(scene.item->effectiveFpsCap(), 30); // back to the outside limit
    scene.item->setFpsCap(0);
    for (int i = 0; i < 40; ++i)
        scene.item->noteLateness(60);
    QVERIFY(scene.item->effectiveFpsCap() > 0); // with no limit of its own it still steps down, from the display's rate
    scene.item->setFpsCap(0); // (same value: nothing changes)
    QVERIFY(scene.item->effectiveFpsCap() > 0);
    scene.item->setFpsCap(30); // a new outside limit starts again from it
    QCOMPARE(scene.item->effectiveFpsCap(), 30);
}

void TstSpriteItem::newFramesReplaceTheOldOnesWithoutAGap()
{
    Scene scene;
    setUp(scene);
    QSignalSpy loaded(scene.item, &SpriteItem::loaded);
    QSignalSpy changes(scene.item, &SpriteItem::hasFramesChanged);
    scene.item->request(m_dir.filePath(u"rgb.apng"_s), kSize, kSize, 1 << 24);
    QVERIFY(renderUntil(scene, [&] { return loaded.count() == 1; }));
    QCOMPARE(changes.count(), 1);

    scene.item->request(m_dir.filePath(u"still.apng"_s), kSize, kSize, 1 << 24);
    bool gap = false;
    QVERIFY(renderUntil(scene, [&] {
        gap = gap || !scene.item->hasFrames();
        return loaded.count() == 2;
    }));
    QVERIFY2(!gap, "the old frames must stay until the new ones are in");
    QCOMPARE(changes.count(), 1); // hasFrames never went away
    QCOMPARE(scene.item->cacheBytes(), qint64((kSize / 2 + 1) * kSize * 4)); // the one frame of the new animation (cut down to what is drawn)
    QVERIFY(renderUntil(scene, [&] {
        const QRgb color = strip(frame(scene));
        return qRed(color) == 255 && qGreen(color) == 255; // yellow
    }, 2000));
}

void TstSpriteItem::aFailedRequestDropsTheFrames()
{
    Scene scene;
    setUp(scene);
    QSignalSpy failed(scene.item, &SpriteItem::failed);
    scene.item->request(m_dir.filePath(u"rgb.apng"_s), kSize, kSize, 1 << 24);
    QVERIFY(renderUntil(scene, [&] { return scene.item->hasFrames(); }));
    // Over the budget: refused, and the caller falls back to the video, so nothing stays on screen.
    scene.item->request(m_dir.filePath(u"rgb.apng"_s), kSize, kSize, 1000);
    QVERIFY(renderUntil(scene, [&] { return failed.count() == 1; }));
    QVERIFY(failed.at(0).at(0).toString().contains(u"budget"_s));
    QVERIFY(!scene.item->hasFrames());
    QCOMPARE(scene.item->cacheBytes(), qint64(0));
    frame(scene);
    QCOMPARE(qAlpha(strip(frame(scene))), 0);
}

void TstSpriteItem::clearDropsTheFrames()
{
    Scene scene;
    setUp(scene);
    scene.item->request(m_dir.filePath(u"rgb.apng"_s), kSize, kSize, 1 << 24);
    QVERIFY(renderUntil(scene, [&] { return scene.item->hasFrames(); }));
    scene.item->clear();
    QVERIFY(!scene.item->hasFrames());
    frame(scene);
    QCOMPARE(qAlpha(strip(frame(scene))), 0);
    // A request that is cleared before it finishes never shows.
    scene.item->request(m_dir.filePath(u"rgb.apng"_s), kSize, kSize, 1 << 24);
    scene.item->clear();
    QTest::qWait(300);
    frame(scene);
    QVERIFY(!scene.item->hasFrames());
}

namespace {
// A frame whose buffer reports when the last QImage sharing it lets go.
struct TrackedBuffer {
    std::unique_ptr<uchar[]> pixels;
    std::atomic_int *released;
};

QImage trackedImage(std::atomic_int *released)
{
    auto *tracked = new TrackedBuffer{std::make_unique<uchar[]>(kSize * kSize * 4), released};
    return QImage(tracked->pixels.get(), kSize, kSize, QImage::Format_RGBA8888_Premultiplied,
                  [](void *info) {
                      auto *buffer = static_cast<TrackedBuffer *>(info);
                      ++*buffer->released;
                      delete buffer;
                  },
                  tracked);
}
} // namespace

void TstSpriteItem::theCpuCopiesAreFreedOnceUploaded()
{
    Scene scene;
    setUp(scene);
    std::atomic_int released(0);
    constexpr int kCount = 14; // more than one upload batch
    SpriteFrames frames;
    for (int i = 0; i < kCount; ++i) {
        frames.images.append(trackedImage(&released));
        frames.durationsMs.append(50);
    }
    frames.size = QSize(kSize, kSize);
    frames.bytes = qint64(kCount) * kSize * kSize * 4;
    scene.item->showFrames(std::move(frames));
    QCOMPARE(scene.item->pendingCpuFrames(), kCount);
    QVERIFY(renderUntil(scene, [&] { return scene.item->hasFrames(); }));
    QCOMPARE(scene.item->cacheBytes(), qint64(kCount) * kSize * kSize * 4);
    // The item keeps none of the images.
    QCOMPARE(scene.item->pendingCpuFrames(), 0);
    frame(scene);
    // The textures of a GPU backend let go of theirs after the upload. The software scene graph draws from the CPU
    // copy by design, so there is nothing to release (and nothing to measure) there.
    if (scene.window->rendererInterface()->graphicsApi() != QSGRendererInterface::Software)
        QTRY_COMPARE_WITH_TIMEOUT(released.load(), kCount, 3000);
}

void TstSpriteItem::destroyingTheItemDoesNotWaitForTheDecoder()
{
    // Keep the only worker busy, as another pet's long decode does.
    std::mutex mutex;
    std::condition_variable wake;
    bool release = false;
    std::atomic_bool blockerRunning(false);
    SpriteItem::workerPool().start([&] {
        blockerRunning = true;
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return release; });
    });
    QTRY_VERIFY(blockerRunning.load());

    Scene scene;
    setUp(scene);
    QSignalSpy loaded(scene.item, &SpriteItem::loaded);
    QSignalSpy failed(scene.item, &SpriteItem::failed);
    scene.item->request(m_dir.filePath(u"rgb.apng"_s), kSize, kSize, 1 << 24); // queued behind the blocker
    QElapsedTimer timer;
    timer.start();
    delete scene.item; // must not wait for the queue
    scene.item = nullptr;
    QVERIFY2(timer.elapsed() < 500, qPrintable(u"%1 ms"_s.arg(timer.elapsed())));

    {
        std::lock_guard lock(mutex);
        release = true;
    }
    wake.notify_all();
    QVERIFY(SpriteItem::workerPool().waitForDone(10000)); // the orphaned task ends quietly
    QCOMPARE(loaded.count(), 0);
    QCOMPARE(failed.count(), 0);
}

void TstSpriteItem::aCancelledRequestNeverRuns()
{
    std::mutex mutex;
    std::condition_variable wake;
    bool release = false;
    std::atomic_bool blockerRunning(false);
    SpriteItem::workerPool().start([&] {
        blockerRunning = true;
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return release; });
    });
    QTRY_VERIFY(blockerRunning.load());
    Scene scene;
    setUp(scene);
    QSignalSpy loaded(scene.item, &SpriteItem::loaded);
    QSignalSpy failed(scene.item, &SpriteItem::failed);
    scene.item->request(m_dir.filePath(u"rgb.apng"_s), kSize, kSize, 1 << 24);
    scene.item->clear(); // cancels it while it waits
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    wake.notify_all();
    QVERIFY(SpriteItem::workerPool().waitForDone(10000));
    QTest::qWait(100);
    QCOMPARE(loaded.count(), 0);
    QCOMPARE(failed.count(), 0);
    QVERIFY(!scene.item->hasFrames());
}

void TstSpriteItem::aRequestForAFixedRateUsesIt()
{
    Scene scene;
    setUp(scene);
    scene.item->request(m_dir.filePath(u"rgb.apng"_s), kSize, kSize, 1 << 24, 10); // the file says 100 ms; 10 fps too
    QVERIFY(renderUntil(scene, [&] { return scene.item->hasFrames(); }));
    QCOMPARE(scene.item->frameDuration(0), 100);
    scene.item->request(m_dir.filePath(u"rgb.apng"_s), kSize, kSize, 1 << 24, 60);
    QVERIFY(renderUntil(scene, [&] { return scene.item->frameDuration(0) == 17; }));
}

int main(int argc, char *argv[])
{
    // Never the desktop session: the offscreen platform and the software scene graph, whatever the environment says.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc, argv);
    TstSpriteItem test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_spriteitem.moc"
