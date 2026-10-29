// VideoItem in pet mode (`transparent`): a file with an alpha channel must reach the render target with its alpha.
// The item is rendered offscreen through a QQuickRenderControl into a texture and read back, so this needs a Qt
// platform with OpenGL (and libmpv); without one the tests skip.

#include "animfixtures.h"
#include "testutil.h"
#include "videoitem.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QProcess>
#include <QQuickGraphicsDevice>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <clocale>
#include <memory>

using namespace murasaki;

namespace {

constexpr int kSize = 64;

// 64x64: x 0..15 half transparent (alpha 128), x 16..31 opaque, the right half fully transparent. The second frame
// is the same in green. Straight (not premultiplied) pixels, as PNG stores them.
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

QByteArray alphaApng()
{
    using namespace animfixtures;
    const QByteArray first = rawPixels(halfImage(Qt::red));
    const QByteArray second = rawPixels(halfImage(Qt::green));
    QByteArray out("\x89PNG\r\n\x1a\n", 8);
    out += pngChunk("IHDR", be32(kSize) + be32(kSize) + QByteArray("\x08\x06\x00\x00\x00", 5));
    out += pngChunk("acTL", be32(2) + be32(0));
    const auto fcTL = [](quint32 sequence) {
        return pngChunk("fcTL", be32(sequence) + be32(kSize) + be32(kSize) + be32(0) + be32(0) + be16(1) + be16(10)
                                    + QByteArray("\x00\x00", 2));
    };
    out += fcTL(0) + pngChunk("IDAT", first);
    out += fcTL(1) + pngChunk("fdAT", be32(2) + second);
    out += pngChunk("IEND", {});
    return out;
}

} // namespace

class TstVideoItem : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void apngKeepsItsAlpha();
    void opaqueModeBlendsOverBlack();
    void fillResetUsesNoDeprecatedMpvOption();
    void partialAlphaIsPremultiplied();
    void webmKeepsItsAlpha();

private:
    // Renders an item playing `file` until `done(image)` holds or the time is up; returns the last image.
    QImage renderUntil(const QString &file, bool transparent, const std::function<bool(const QImage &)> &done);

    bool makeCurrent() { return m_context->makeCurrent(m_surface.get()); }

    QTemporaryDir m_dir;
    std::unique_ptr<QOpenGLContext> m_context;
    std::unique_ptr<QOffscreenSurface> m_surface;
};

void TstVideoItem::initTestCase()
{
    std::setlocale(LC_NUMERIC, "C"); // libmpv refuses to start with another numeric locale
    if (!QOpenGLContext::globalShareContext())
        QSKIP("the Qt platform has no OpenGL");
    QVERIFY(m_dir.isValid());
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    m_context = std::make_unique<QOpenGLContext>();
    m_context->setFormat(format);
    m_context->setShareContext(QOpenGLContext::globalShareContext());
    if (!m_context->create())
        QSKIP("cannot create an OpenGL 3.3 core context");
    m_surface = std::make_unique<QOffscreenSurface>();
    m_surface->setFormat(m_context->format());
    m_surface->create();
    if (!makeCurrent())
        QSKIP("cannot make the OpenGL context current");
    {
        // The offscreen platform can have a GLX context that Qt Quick's RHI refuses.
        QQuickRenderControl probe;
        QQuickWindow window(&probe);
        window.setGraphicsDevice(QQuickGraphicsDevice::fromOpenGLContext(m_context.get()));
        const bool usable = probe.initialize();
        probe.invalidate();
        if (!usable)
            QSKIP("Qt Quick cannot render offscreen on this platform");
    }
    testutil::writeFile(m_dir.filePath(QStringLiteral("pet.apng")), alphaApng());
}

void TstVideoItem::cleanupTestCase()
{
    if (m_context)
        m_context->doneCurrent();
}

QImage TstVideoItem::renderUntil(const QString &file, bool transparent, const std::function<bool(const QImage &)> &done)
{
    QImage last;
    if (!makeCurrent())
        return last;
    auto *control = new QQuickRenderControl;
    auto *window = new QQuickWindow(control);
    window->setGraphicsDevice(QQuickGraphicsDevice::fromOpenGLContext(m_context.get()));
    if (!control->initialize()) {
        delete window;
        delete control;
        return last;
    }
    QOpenGLFramebufferObject fbo(kSize, kSize, QOpenGLFramebufferObject::CombinedDepthStencil);
    window->setRenderTarget(QQuickRenderTarget::fromOpenGLTexture(fbo.texture(), fbo.size()));
    window->setColor(Qt::transparent);
    window->resize(kSize, kSize);

    auto *item = new VideoItem;
    item->setParentItem(window->contentItem());
    item->setSize(QSizeF(kSize, kSize));
    item->setTransparent(transparent);
    item->setSource(file);

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 15000) {
        QTest::qWait(50);
        makeCurrent();
        control->polishItems();
        control->beginFrame();
        control->sync();
        control->render();
        control->endFrame();
        last = fbo.toImage(true).convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (done(last))
            break;
    }
    delete item; // frees mpv's GL objects: the context is current
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    control->invalidate();
    delete window;
    delete control;
    return last;
}

void TstVideoItem::apngKeepsItsAlpha()
{
    const QImage image = renderUntil(m_dir.filePath(QStringLiteral("pet.apng")), true, [](const QImage &img) {
        return qAlpha(img.pixel(kSize / 4, kSize / 2)) == 255 && qRed(img.pixel(kSize / 4, kSize / 2)) > 200;
    });
    QVERIFY2(!image.isNull(), "nothing was rendered");
    const QRgb opaque = image.pixel(kSize / 4, kSize / 2);
    const QRgb clear = image.pixel(kSize * 3 / 4, kSize / 2);
    QCOMPARE(qAlpha(opaque), 255);
    QVERIFY2(qRed(opaque) > 200, "the opaque half lost its color");
    QCOMPARE(qAlpha(clear), 0);
    // Premultiplied: a transparent pixel carries no color either.
    QCOMPARE(qRed(clear) + qGreen(clear) + qBlue(clear), 0);
}

void TstVideoItem::partialAlphaIsPremultiplied()
{
    // Qt Quick composites textures as premultiplied: a red pixel of alpha 128 must read back as (128, 0, 0, 128).
    // Straight output would show as a bright halo around every soft edge of a pet.
    const QImage image = renderUntil(m_dir.filePath(QStringLiteral("pet.apng")), true, [](const QImage &img) {
        return qAlpha(img.pixel(kSize / 8, kSize / 2)) > 100 && qAlpha(img.pixel(kSize / 8, kSize / 2)) < 160;
    });
    QVERIFY2(!image.isNull(), "nothing was rendered");
    const QRgb half = image.pixel(kSize / 8, kSize / 2);
    QVERIFY2(qAbs(qAlpha(half) - 128) <= 6, qPrintable(QStringLiteral("alpha %1").arg(qAlpha(half))));
    QVERIFY2(qAbs(qRed(half) - qAlpha(half)) <= 6,
             qPrintable(QStringLiteral("red %1 with alpha %2: not premultiplied").arg(qRed(half)).arg(qAlpha(half))));
    QCOMPARE(qGreen(half) + qBlue(half) <= 6, true);
    // The opaque neighbour is untouched.
    QCOMPARE(qAlpha(image.pixel(kSize / 4 + 4, kSize / 2)), 255);
    QVERIFY(qRed(image.pixel(kSize / 4 + 4, kSize / 2)) > 245);
}

namespace {
QStringList g_mpvWarnings;
void collectMpvWarnings(QtMsgType, const QMessageLogContext &, const QString &message)
{
    if (message.startsWith(u"mpv["))
        g_mpvWarnings << message;
}
} // namespace

// mpv 0.41 warns about video-aspect-override=-1 (the old way to drop the override): the item must use what the mpv in use
// understands, so that its own warnings stay worth reading.
void TstVideoItem::fillResetUsesNoDeprecatedMpvOption()
{
    g_mpvWarnings.clear();
    const QtMessageHandler previous = qInstallMessageHandler(collectMpvWarnings);
    const QImage image = renderUntil(m_dir.filePath(QStringLiteral("pet.apng")), false,
                                     [](const QImage &img) { return qRed(img.pixel(kSize / 4, kSize / 2)) > 200; });
    qInstallMessageHandler(previous);
    QVERIFY2(!image.isNull(), "nothing was rendered");
    for (const QString &warning : std::as_const(g_mpvWarnings))
        QVERIFY2(!warning.contains(QStringLiteral("deprecated")), qPrintable(warning));
}

void TstVideoItem::opaqueModeBlendsOverBlack()
{
    const QImage image = renderUntil(m_dir.filePath(QStringLiteral("pet.apng")), false, [](const QImage &img) {
        return qAlpha(img.pixel(kSize / 4, kSize / 2)) == 255 && qRed(img.pixel(kSize / 4, kSize / 2)) > 200;
    });
    QVERIFY2(!image.isNull(), "nothing was rendered");
    // Without pet mode the transparent half is flattened over black: it is opaque.
    QCOMPARE(qAlpha(image.pixel(kSize * 3 / 4, kSize / 2)), 255);
}

void TstVideoItem::webmKeepsItsAlpha()
{
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty())
        QSKIP("ffmpeg not found");
    const QString apng = m_dir.filePath(QStringLiteral("pet.apng"));
    const QString webm = m_dir.filePath(QStringLiteral("pet.webm"));
    QProcess process;
    process.start(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-y"), QStringLiteral("-i"), apng,
                           QStringLiteral("-c:v"), QStringLiteral("libvpx-vp9"), QStringLiteral("-pix_fmt"),
                           QStringLiteral("yuva420p"), QStringLiteral("-auto-alt-ref"), QStringLiteral("0"), webm});
    if (!process.waitForFinished(30000) || process.exitCode() != 0)
        QSKIP("ffmpeg cannot write VP9 with alpha");
    const QImage image = renderUntil(webm, true, [](const QImage &img) {
        return qAlpha(img.pixel(kSize / 4, kSize / 2)) > 200 && qRed(img.pixel(kSize / 4, kSize / 2)) > 200;
    });
    QVERIFY2(!image.isNull(), "nothing was rendered");
    QVERIFY(qAlpha(image.pixel(kSize / 4, kSize / 2)) > 200);
    QVERIFY2(qAlpha(image.pixel(kSize * 3 / 4, kSize / 2)) < 16, "the WebM lost its alpha plane");
}

int main(int argc, char *argv[])
{
    // No display at all (CI): the offscreen platform has no OpenGL, so the tests skip.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")
        && qEnvironmentVariableIsEmpty("DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL); // VideoItem renders through libmpv's OpenGL API
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QGuiApplication app(argc, argv);
    TstVideoItem test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_videoitem.moc"
