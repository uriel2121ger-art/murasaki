// murasaki-scene-vk text: pixel sizes, the layout box and wrapping, with whatever font the machine has (skips without one).

#include "text/textraster.h"

#include <QFile>
#include <QTest>

#include <algorithm>

using namespace murasaki::scene;

class TstSceneVkText : public QObject
{
    Q_OBJECT

    std::unique_ptr<TextRasterizer> m_font;

    static size_t ink(const TextBitmap &b)
    {
        return static_cast<size_t>(std::count_if(b.coverage.begin(), b.coverage.end(), [](uint8_t v) { return v > 32; }));
    }

private slots:
    void initTestCase()
    {
        if (!textSupported())
            return;
        auto bytes = systemFontBytes("systemfont_sans");
        if (bytes.empty())
            bytes = systemFontBytes("sans");
        std::string error;
        m_font = TextRasterizer::create(std::move(bytes), &error);
    }

    void pointSizesAreFourPixelsAtScaleOne()
    {
        if (!textSupported())
            QSKIP("built without FreeType");
        QCOMPARE(textPixelSize(32.0f, 1.0f), 128u);
        QCOMPARE(textPixelSize(32.0f, 0.5f), 64u);
        QCOMPARE(textPixelSize(33.0f, 1.0f), 132u);
        QCOMPARE(textPixelSize(0.0f, 1.0f), 1u);
        QCOMPARE(textPixelSize(1000.0f, 8.0f), 1024u);
        QCOMPARE(textPixelSize(10.0f, 100.0f), 320u); // the scale is capped at 8
    }

    void aBrokenFontIsRefused()
    {
        if (!textSupported())
            QSKIP("built without FreeType");
        std::string error;
        QVERIFY(!TextRasterizer::create({1, 2, 3, 4}, &error));
        QVERIFY(!error.empty());
    }

    void theBitmapIsTheLayoutBoxNotTheInk()
    {
        if (!m_font)
            QSKIP("no usable font");
        const TextBitmap one = m_font->render("Hi", 64);
        QVERIFY(one.width > 20 && one.width < 140);
        QVERIFY(one.height >= 64 && one.height < 120); // ascender plus descender of a 64 pixel em
        QVERIFY(ink(one) > 100);
        QCOMPARE(one.lines, 1u);
        // the same height whatever the letters: a line of low letters is as tall as one with tall ones
        QCOMPARE(m_font->render("aaaa", 64).height, m_font->render("HHHH", 64).height);
        // more text, wider box
        QVERIFY(m_font->render("Hi Hi", 64).width > one.width * 2 - 5);
    }

    void linesAddOneLineHeightEach()
    {
        if (!m_font)
            QSKIP("no usable font");
        const TextBitmap a = m_font->render("one", 48);
        const TextBitmap b = m_font->render("one\ntwo", 48);
        const TextBitmap c = m_font->render("one\ntwo\nthree", 48);
        QCOMPARE(b.lines, 2u);
        QCOMPARE(c.lines, 3u);
        const uint32_t step = b.height - a.height;
        QVERIFY(step >= 40 && step <= 90);
        QCOMPARE(c.height - b.height, step);
        QVERIFY(c.width >= b.width);
        // an empty line still counts
        QCOMPARE(m_font->render("a\n\nb", 48).lines, 3u);
    }

    void maxWidthWrapsAtSpaces()
    {
        if (!m_font)
            QSKIP("no usable font");
        const TextBitmap wide = m_font->render("alpha beta gamma delta", 48);
        QCOMPARE(wide.lines, 1u);
        const TextBitmap wrapped = m_font->render("alpha beta gamma delta", 48, static_cast<float>(wide.width) / 2.0f);
        QVERIFY2(wrapped.lines >= 2, qPrintable(QString::number(wrapped.lines)));
        QVERIFY(wrapped.width <= wide.width / 2 + 60);
        QVERIFY(wrapped.height > wide.height);
        // a word wider than the limit stays whole
        QCOMPARE(m_font->render("extraordinarily", 48, 10.0f).lines, 1u);
    }

    void inkSitsInsideTheBox()
    {
        if (!m_font)
            QSKIP("no usable font");
        const TextBitmap b = m_font->render("Wg", 80);
        // some ink in the top half (the W) and in the bottom rows (the descender of g)
        size_t top = 0, bottom = 0;
        for (uint32_t y = 0; y < b.height; ++y)
            for (uint32_t x = 0; x < b.width; ++x)
                if (b.coverage[static_cast<size_t>(y) * b.width + x] > 64)
                    (y < b.height / 2 ? top : bottom)++;
        QVERIFY(top > 50 && bottom > 5);
    }

    void hostileTextStaysWithinTheBitmapLimits()
    {
        auto raster = TextRasterizer::create(systemFontBytes("sans-serif"));
        if (!raster)
            QSKIP("no font");
        // a megabyte of words, a thousand lines, and a huge em: cut, never allocated at full size
        std::string words;
        for (int i = 0; i < 200000; ++i)
            words += "word ";
        const TextBitmap wide = raster->render(words, 64, 0.0f);
        QVERIFY(wide.width <= kMaxTextDimension && wide.height <= kMaxTextDimension);
        const TextBitmap wrapped = raster->render(words, 64, 400.0f);
        QVERIFY(wrapped.width <= kMaxTextDimension && wrapped.height <= kMaxTextDimension);
        const TextBitmap lines = raster->render(std::string(5000, '\n') + "x", 1024, 0.0f);
        QVERIFY(lines.height <= kMaxTextDimension);
        QVERIFY(static_cast<size_t>(lines.width) * lines.height <= size_t{kMaxTextDimension} * kMaxTextDimension);
        QVERIFY(raster->render(std::string("\xff\xfe\xc0\x80", 4), 32, 0.0f).width >= 1);
    }

    void bitmapFontsWithOneBitGlyphsAreDrawn()
    {
        // an X11 bitmap font: its glyphs come back as FT_PIXEL_MODE_MONO, which must be expanded and not read as 8-bit rows
        std::vector<uint8_t> bytes;
        for (const char *path : {"/usr/share/fonts/X11/misc/7x13-ISO8859-1.pcf.gz", "/usr/share/fonts/X11/misc/7x13.pcf.gz"}) {
            QFile f(QString::fromLatin1(path));
            if (f.open(QIODevice::ReadOnly)) {
                const QByteArray all = f.readAll();
                bytes.assign(all.begin(), all.end());
                break;
            }
        }
        if (bytes.empty())
            QSKIP("no X11 bitmap font installed");
        auto raster = TextRasterizer::create(bytes);
        if (!raster)
            QSKIP("FreeType here cannot read a compressed bitmap font");
        const TextBitmap bm = raster->render("HHH", 13, 0.0f);
        QVERIFY(bm.width >= 10 && bm.height >= 8);
        int full = 0, partial = 0;
        for (uint8_t v : bm.coverage) {
            full += v == 255;
            partial += v != 0 && v != 255;
        }
        QVERIFY2(full > 10, qPrintable(QString::number(full)));
        QCOMPARE(partial, 0); // one bit a pixel: either lit or not
    }

    void utf8IsDecoded()
    {
        if (!m_font)
            QSKIP("no usable font");
        QVERIFY(m_font->render("\xC3\xA9", 48).width > 5); // e acute: one glyph, not two bytes
        QVERIFY(m_font->render("\xC3\xA9", 48).width < m_font->render("ab", 48).width + 5);
    }
};

QTEST_GUILESS_MAIN(TstSceneVkText)
#include "tst_scenevk_text.moc"
