#include "dmabufimport.h"
#include "eglcontext.h"

#include <QDir>
#include <QTest>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <unistd.h>

#include <cstdint>
#include <vector>

using namespace murasaki;
using testegl::EglContext;
using testegl::hasExt;
using testegl::haveRenderNode;

namespace {

using GlGenTextures = void (*)(int, unsigned *);
using GlBindTexture = void (*)(unsigned, unsigned);
using GlTexParameteri = void (*)(unsigned, unsigned, int);
using GlTexImage2D = void (*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
using GlGetTexImage = void (*)(unsigned, int, unsigned, unsigned, void *);
using GlFinish = void (*)();

constexpr unsigned kTexture2D = 0x0DE1;
constexpr unsigned kRgba = 0x1908;
constexpr unsigned kUnsignedByte = 0x1401;
constexpr unsigned kMinFilter = 0x2801;
constexpr unsigned kNearest = 0x2600;

template <class F>
F proc(const char *name)
{
    return reinterpret_cast<F>(DmabufImporter::procAddress(name));
}

std::vector<uint8_t> pattern(int w, int h)
{
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t *p = &px[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>(x * 255 / (w - 1));
            p[1] = static_cast<uint8_t>(y * 255 / (h - 1));
            p[2] = static_cast<uint8_t>((x ^ y) * 7);
            p[3] = 255;
        }
    }
    return px;
}

} // namespace

class TstDmabufImport : public QObject
{
    Q_OBJECT

private slots:
    void withoutEglContextImportReportsUnsupported()
    {
        DmabufImporter importer;
        std::string error;
        QVERIFY(!importer.init(&error));
        QVERIFY(!error.empty());
        DmabufDescription d;
        QVERIFY(!importer.import(d, &error).has_value());
    }

    void exportedTextureImportsPixelExact()
    {
        if (!haveRenderNode())
            QSKIP("no /dev/dri/renderD* render node");
        EglContext egl;
        if (!egl.create())
            QSKIP("cannot create a surfaceless desktop-GL EGL context");
        const char *ext = eglQueryString(egl.display, EGL_EXTENSIONS);
        if (!hasExt(ext, "EGL_MESA_image_dma_buf_export") || !hasExt(ext, "EGL_KHR_gl_texture_2D_image"))
            QSKIP("EGL_MESA_image_dma_buf_export is not available");
        auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        auto exportQuery = reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC>(
            eglGetProcAddress("eglExportDMABUFImageQueryMESA"));
        auto exportImage = reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEMESAPROC>(eglGetProcAddress("eglExportDMABUFImageMESA"));
        auto genTextures = proc<GlGenTextures>("glGenTextures");
        auto bindTexture = proc<GlBindTexture>("glBindTexture");
        auto texParameteri = proc<GlTexParameteri>("glTexParameteri");
        auto texImage2D = proc<GlTexImage2D>("glTexImage2D");
        auto getTexImage = proc<GlGetTexImage>("glGetTexImage");
        auto finish = proc<GlFinish>("glFinish");
        QVERIFY(createImage && destroyImage && exportQuery && exportImage && genTextures && bindTexture && texParameteri
                && texImage2D && getTexImage && finish);

        constexpr int w = 97, h = 53; // odd sizes catch pitch mistakes
        const std::vector<uint8_t> source = pattern(w, h);
        unsigned srcTexture = 0;
        genTextures(1, &srcTexture);
        bindTexture(kTexture2D, srcTexture);
        texParameteri(kTexture2D, kMinFilter, static_cast<int>(kNearest));
        texImage2D(kTexture2D, 0, static_cast<int>(kRgba), w, h, 0, kRgba, kUnsignedByte, source.data());
        finish();

        EGLImageKHR srcImage = createImage(egl.display, egl.context, EGL_GL_TEXTURE_2D,
                                           reinterpret_cast<EGLClientBuffer>(static_cast<uintptr_t>(srcTexture)), nullptr);
        QVERIFY2(srcImage != EGL_NO_IMAGE_KHR, "cannot create an EGLImage from the pattern texture");
        int fourcc = 0, planes = 0;
        EGLuint64KHR modifier = 0;
        if (!exportQuery(egl.display, srcImage, &fourcc, &planes, &modifier)) {
            destroyImage(egl.display, srcImage);
            QSKIP("the driver cannot export this image as a dma-buf");
        }
        QVERIFY(planes >= 1 && planes <= 4);
        int fds[4] = {-1, -1, -1, -1};
        EGLint strides[4] = {};
        EGLint offsets[4] = {};
        QVERIFY(exportImage(egl.display, srcImage, fds, strides, offsets));
        destroyImage(egl.display, srcImage);

        DmabufDescription d;
        d.width = w;
        d.height = h;
        d.fourcc = static_cast<uint32_t>(fourcc);
        d.modifier = modifier;
        d.planeCount = static_cast<uint32_t>(planes);
        for (int i = 0; i < planes; ++i)
            d.planes[i] = {fds[i], static_cast<uint32_t>(offsets[i]), static_cast<uint32_t>(strides[i])};

        DmabufImporter importer;
        std::string error;
        QVERIFY2(importer.init(&error), error.c_str());
        auto imported = importer.import(d, &error);
        QVERIFY2(imported.has_value(), error.c_str());
        QVERIFY(imported->texture != 0);

        bindTexture(kTexture2D, imported->texture);
        std::vector<uint8_t> readback(source.size(), 0xAA);
        getTexImage(kTexture2D, 0, kRgba, kUnsignedByte, readback.data());
        bindTexture(kTexture2D, 0);
        QVERIFY2(readback == source, "dma-buf round trip is not pixel exact");

        // The same buffer imports again with the modifier left out (implicit modifier).
        d.modifier = kDrmModInvalid;
        auto again = importer.import(d, &error);
        if (again)
            importer.destroy(*again);

        importer.destroy(*imported);
        QCOMPARE(imported->texture, 0u);
        for (int i = 0; i < planes; ++i)
            ::close(fds[i]);
    }

    void invalidFdFailsCleanly()
    {
        if (!haveRenderNode())
            QSKIP("no /dev/dri/renderD* render node");
        EglContext egl;
        if (!egl.create())
            QSKIP("cannot create a surfaceless desktop-GL EGL context");
        DmabufImporter importer;
        std::string error;
        if (!importer.init(&error))
            QSKIP("dma-buf import is not supported by this driver");
        DmabufDescription d;
        d.width = 16;
        d.height = 16;
        d.fourcc = 0x34324241; // AB24
        d.modifier = 0;
        d.planeCount = 1;
        d.planes[0] = {-1, 0, 64};
        QVERIFY(!importer.import(d, &error).has_value());
        QVERIFY(!error.empty());
    }
};

QTEST_GUILESS_MAIN(TstDmabufImport)
#include "tst_dmabufimport.moc"
