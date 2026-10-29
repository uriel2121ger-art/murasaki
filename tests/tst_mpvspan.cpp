// The mpv properties of a spanned video (mpvViewport in canvas.h), checked against mpv itself: the left and right
// half of a canvas, each drawn by its own player at its own window size, must be the picture one player draws over
// the whole canvas with mpv's plain fill, fit or stretch. Uses libmpv's software renderer, so it needs no GPU.

#include "canvas.h"
#include "testutil.h"

#include <mpv/client.h>
#include <mpv/render.h>

#include <QElapsedTimer>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>

#include <clocale>
#include <map>
#include <memory>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

using Props = std::map<std::string, std::string>;

// Plays the file in a window of this size until a frame is out and returns it; a null image when mpv cannot.
QImage renderWithMpv(const QString &file, int width, int height, const Props &props)
{
    mpv_handle *mpv = mpv_create();
    if (!mpv)
        return {};
    const std::unique_ptr<mpv_handle, void (*)(mpv_handle *)> guard(mpv, mpv_terminate_destroy);
    for (const auto &[name, value] : Props{{"vo", "libmpv"}, {"terminal", "no"}, {"keep-open", "yes"}, {"audio", "no"},
                                           {"msg-level", "all=error"}, {"scale", "bilinear"}, {"dscale", "bilinear"}})
        mpv_set_option_string(mpv, name.c_str(), value.c_str());
    if (mpv_initialize(mpv) < 0)
        return {};
    mpv_render_context *context = nullptr;
    mpv_render_param create[] = {{MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW)},
                                 {MPV_RENDER_PARAM_INVALID, nullptr}};
    if (mpv_render_context_create(&context, mpv, create) < 0)
        return {};
    struct Free {
        mpv_render_context *context;
        ~Free() { mpv_render_context_free(context); }
    } free{context};
    for (const auto &[name, value] : props)
        mpv_set_property_string(mpv, name.c_str(), value.c_str());
    const QByteArray path = file.toUtf8();
    const char *command[] = {"loadfile", path.constData(), nullptr};
    mpv_command(mpv, command);

    QImage image(width, height, QImage::Format_RGB32);
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000) {
        mpv_wait_event(mpv, 0.02);
        if (!(mpv_render_context_update(context) & MPV_RENDER_UPDATE_FRAME))
            continue;
        int size[2] = {width, height};
        size_t stride = static_cast<size_t>(image.bytesPerLine());
        mpv_render_param render[] = {{MPV_RENDER_PARAM_SW_SIZE, size},
                                     {MPV_RENDER_PARAM_SW_FORMAT, const_cast<char *>("bgr0")},
                                     {MPV_RENDER_PARAM_SW_STRIDE, &stride},
                                     {MPV_RENDER_PARAM_SW_POINTER, image.bits()},
                                     {MPV_RENDER_PARAM_INVALID, nullptr}};
        if (mpv_render_context_render(context, render) < 0)
            return {};
        return image;
    }
    return {};
}

Props propsFor(const MpvViewport &view)
{
    return {{"keepaspect", "yes"},
            {"panscan", "0"},
            {"video-aspect-override", QByteArray::number(view.aspect, 'g', 12).toStdString()},
            {"video-zoom", QByteArray::number(view.zoom, 'g', 12).toStdString()},
            {"video-pan-x", QByteArray::number(view.panX, 'g', 12).toStdString()},
            {"video-pan-y", QByteArray::number(view.panY, 'g', 12).toStdString()}};
}

} // namespace

class TestMpvSpan : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        std::setlocale(LC_NUMERIC, "C"); // libmpv refuses to start with another numeric locale
        QVERIFY(m_dir.isValid());
        // 100x100, four flat quadrants: red green / blue yellow.
        QImage image(100, 100, QImage::Format_RGB32);
        for (int y = 0; y < 100; ++y) {
            for (int x = 0; x < 100; ++x)
                image.setPixelColor(x, y, y < 50 ? (x < 50 ? QColor(255, 0, 0) : QColor(0, 255, 0))
                                                 : (x < 50 ? QColor(0, 0, 255) : QColor(255, 255, 0)));
        }
        m_file = m_dir.filePath(u"quadrants.png"_s);
        QVERIFY(image.save(m_file));
        if (renderWithMpv(m_file, 40, 20, {}).isNull())
            QSKIP("libmpv cannot render in software here");
    }

    void halvesAreTheWholePicture_data()
    {
        QTest::addColumn<int>("fill");
        QTest::addRow("fill") << int(FillMode::Fill);
        QTest::addRow("fit") << int(FillMode::Fit);
        QTest::addRow("stretch") << int(FillMode::Stretch);
    }

    void halvesAreTheWholePicture()
    {
        QFETCH(int, fill);
        const FillMode mode = FillMode(fill);
        const QSizeF canvas(400, 100);
        const QSizeF content(100, 100);
        // mpv's own behaviour over the whole canvas.
        const Props plain = mode == FillMode::Fill ? Props{{"keepaspect", "yes"}, {"panscan", "1"}}
            : mode == FillMode::Fit                ? Props{{"keepaspect", "yes"}, {"panscan", "0"}}
                                                   : Props{{"keepaspect", "no"}};
        const QImage whole = renderWithMpv(m_file, 400, 100, plain);
        QVERIFY(!whole.isNull());
        int compared = 0;
        int different = 0;
        for (const double x0 : {0.0, 200.0}) {
            const MpvViewport view = mpvViewport(canvas, QRectF(x0, 0, 200, 100), content, mode);
            QVERIFY(view.visible);
            const QImage half = renderWithMpv(m_file, 200, 100, propsFor(view));
            QVERIFY(!half.isNull());
            for (int y = 2; y < 100; y += 6) {
                for (int x = 2; x < 200; x += 6) {
                    const QColor a = half.pixelColor(x, y);
                    const QColor b = whole.pixelColor(static_cast<int>(x0) + x, y);
                    ++compared;
                    if (std::abs(a.red() - b.red()) > 48 || std::abs(a.green() - b.green()) > 48
                        || std::abs(a.blue() - b.blue()) > 48)
                        ++different;
                }
            }
        }
        QVERIFY(compared > 400);
        QVERIFY2(different * 50 <= compared, qPrintable(u"%1 of %2 samples differ"_s.arg(different).arg(compared)));
    }

    void anOutputOffTheEdgeOfTheMapShowsTheRightPart()
    {
        // A third output beside the two: with fit, the square picture is in the middle of 600 px, so the outer
        // outputs have no picture and the middle one has all of it.
        const QSizeF canvas(600, 100);
        QVERIFY(!mpvViewport(canvas, QRectF(0, 0, 200, 100), QSizeF(100, 100), FillMode::Fit).visible);
        QVERIFY(!mpvViewport(canvas, QRectF(400, 0, 200, 100), QSizeF(100, 100), FillMode::Fit).visible);
        const MpvViewport middle = mpvViewport(canvas, QRectF(200, 0, 200, 100), QSizeF(100, 100), FillMode::Fit);
        QVERIFY(middle.visible);
        const QImage image = renderWithMpv(m_file, 200, 100, propsFor(middle));
        QVERIFY(!image.isNull());
        QCOMPARE(image.pixelColor(75, 25), QColor(255, 0, 0));
        QCOMPARE(image.pixelColor(125, 75), QColor(255, 255, 0));
        QCOMPARE(image.pixelColor(10, 50), QColor(0, 0, 0));
    }

private:
    QTemporaryDir m_dir;
    QString m_file;
};

QTEST_MAIN(TestMpvSpan)
#include "tst_mpvspan.moc"
