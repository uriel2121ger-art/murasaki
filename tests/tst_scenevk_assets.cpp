// murasaki-scene-vk asset layer: PKG archives, the virtual file system and TEX textures. Every file is built by
// the test itself (never copied from Workshop items): the writers in scenevkfixtures.h follow the format description in
// src/scene-vk/assets/tex.h and docs/scene-vk/formats.md.

#include "assets/pkg.h"
#include "assets/tex.h"
#include "assets/vfs.h"
#include "render/texture.h"
#include "scenevkfixtures.h"
#include "testutil.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTest>

#include <cstring>
#include <string>
#include <vector>

using namespace Qt::StringLiterals;
using namespace murasaki::scene;

using namespace scenevkfix;

class TstSceneVkAssets : public QObject
{
    Q_OBJECT

private slots:
    // ---------------- PKG ----------------

    void pkgListsAndReadsEntries()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(u"scene.pkg"_s);
        testutil::writeFile(path, toQ(makePkg({{"scene.json", "{}"}, {"materials/a.json", "hello"}, {"empty.bin", ""}}, "PKGV0021")));
        std::string error;
        auto pkg = PkgArchive::open(path.toStdString(), &error);
        QVERIFY2(pkg.has_value(), error.c_str());
        QCOMPARE(pkg->version(), std::string("PKGV0021"));
        QCOMPARE(pkg->entries().size(), size_t{3});
        QCOMPARE(pkg->read("materials/a.json").value(), (Bytes{'h', 'e', 'l', 'l', 'o'}));
        QCOMPARE(pkg->read("scene.json").value().size(), size_t{2});
        QVERIFY(pkg->read("empty.bin").value().empty());
        QVERIFY(!pkg->read("missing").has_value());
        QVERIFY(pkg->find("MATERIALS/A.JSON") != nullptr); // case-insensitive fallback
    }

    void pkgBackslashNamesBecomeSlashes()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(u"a.pkg"_s);
        testutil::writeFile(path, toQ(makePkg({{"models\\x.json", "1"}})));
        auto pkg = PkgArchive::open(path.toStdString());
        QVERIFY(pkg.has_value());
        QVERIFY(pkg->read("models/x.json").has_value());
    }

    void pkgRejectsBrokenArchives()
    {
        QTemporaryDir dir;
        const auto tryOpen = [&](const Bytes &content) -> bool {
            const QString path = dir.filePath(u"bad.pkg"_s);
            testutil::writeFile(path, toQ(content));
            std::string error;
            const bool ok = PkgArchive::open(path.toStdString(), &error).has_value();
            return ok || error.empty() ? ok : false; // a failure always carries an explanation (checked below)
        };
        QVERIFY(!tryOpen({}));
        QVERIFY(!tryOpen({1, 2, 3}));
        Bytes notPkg;
        put32(notPkg, 8);
        for (char c : std::string("XXXX0001"))
            notPkg.push_back(static_cast<uint8_t>(c));
        put32(notPkg, 0);
        QVERIFY(!tryOpen(notPkg));

        Bytes good = makePkg({{"a", "xyz"}});
        QVERIFY(tryOpen(good));
        // every truncation of the table or the data is an error, never a crash
        for (size_t n = 0; n + 1 < good.size(); ++n)
            QVERIFY(!tryOpen(Bytes(good.begin(), good.begin() + static_cast<long>(n))));

        // an entry that points past the end of the file
        Bytes liar = makePkg({{"a", "xyz"}});
        liar[liar.size() - 3 - 4] = 0xFF; // length field of the only entry
        QVERIFY(!tryOpen(liar));

        // absurd entry count
        Bytes many;
        put32(many, 8);
        for (char c : std::string("PKGV0001"))
            many.push_back(static_cast<uint8_t>(c));
        put32(many, 0xFFFFFFFFu);
        QVERIFY(!tryOpen(many));

        std::string error;
        QVERIFY(!PkgArchive::open(dir.filePath(u"nope.pkg"_s).toStdString(), &error).has_value());
        QVERIFY(!error.empty());
        testutil::writeFile(dir.filePath(u"bad.pkg"_s), "xx");
        error.clear();
        QVERIFY(!PkgArchive::open(dir.filePath(u"bad.pkg"_s).toStdString(), &error).has_value());
        QVERIFY(!error.empty());
    }

    // ---------------- VFS ----------------

    void normalizePathsRejectTraversal()
    {
        QCOMPARE(normalizeAssetPath("materials//a/./b.json"), std::string("materials/a/b.json"));
        QCOMPARE(normalizeAssetPath("\\models\\x.json"), std::string("models/x.json"));
        QCOMPARE(normalizeAssetPath("/a"), std::string("a"));
        QVERIFY(normalizeAssetPath("../etc/passwd").empty());
        QVERIFY(normalizeAssetPath("a/../../b").empty());
        QVERIFY(normalizeAssetPath("").empty());
        QVERIFY(normalizeAssetPath("./").empty());
    }

    void vfsSearchesItemThenPackageThenAssets()
    {
        QTemporaryDir root;
        const QString item = root.filePath(u"item"_s);
        const QString assets = root.filePath(u"assets"_s);
        testutil::writeFile(item + "/only-item.txt", "item");
        testutil::writeFile(item + "/shadow.txt", "from item folder");
        testutil::writeFile(item + "/scene.pkg",
                            toQ(makePkg({{"shadow.txt", "from package"}, {"only-pkg.txt", "pkg"}, {"both.txt", "package wins"}})));
        testutil::writeFile(assets + "/shadow.txt", "from assets");
        testutil::writeFile(assets + "/both.txt", "assets lose");
        testutil::writeFile(assets + "/shaders/Common.h", "common");

        std::string error;
        Vfs vfs = Vfs::forItem(item.toStdString(), assets.toStdString(), &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(vfs.readText("only-item.txt").value(), std::string("item"));
        QCOMPARE(vfs.readText("shadow.txt").value(), std::string("from item folder"));
        QCOMPARE(vfs.readText("only-pkg.txt").value(), std::string("pkg"));
        QCOMPARE(vfs.readText("both.txt").value(), std::string("package wins"));
        QCOMPARE(vfs.readText("shaders/common.h").value(), std::string("common")); // case-insensitive on disk
        QCOMPARE(vfs.readText("SHADERS\\COMMON.H").value(), std::string("common"));
        QVERIFY(vfs.exists("only-pkg.txt"));
        QVERIFY(!vfs.exists("nothing.txt"));
        QVERIFY(!vfs.read("../outside").has_value());
        QVERIFY(!vfs.read("shaders").has_value()); // a directory is not a file
    }

    void vfsPrefersSceneAndReportsBrokenPackages()
    {
        QTemporaryDir root;
        const QString item = root.path();
        testutil::writeFile(item + "/a.pkg", toQ(makePkg({{"x.txt", "from a"}})));
        testutil::writeFile(item + "/scene.pkg", toQ(makePkg({{"x.txt", "from scene"}})));
        testutil::writeFile(item + "/broken.pkg", "garbage");
        std::string error;
        Vfs vfs = Vfs::forItem(item.toStdString(), {}, &error);
        QVERIFY(!error.empty()); // broken.pkg
        QCOMPARE(vfs.readText("x.txt").value(), std::string("from scene"));
    }

    void assetsDirectoryLookup()
    {
        QTemporaryDir steam;
        const QString assets = steam.filePath(u"steamapps/common/wallpaper_engine/assets"_s);
        QVERIFY(QDir().mkpath(assets));
        const QByteArray old = qgetenv("MURASAKI_STEAM_ROOT");
        qputenv("MURASAKI_STEAM_ROOT", steam.path().toUtf8());
        QCOMPARE(findAssetsDir({}, {}), assets.toStdString());
        qunsetenv("MURASAKI_STEAM_ROOT");
        // implied by the item path
        QCOMPARE(findAssetsDir({}, (steam.path() + "/steamapps/workshop/content/431960/1").toStdString()), assets.toStdString());
        // an explicit hint wins when it exists
        QTemporaryDir other;
        QCOMPARE(findAssetsDir(other.path().toStdString(), {}), other.path().toStdString());
        if (!old.isNull())
            qputenv("MURASAKI_STEAM_ROOT", old);
    }

    // ---------------- TEX ----------------

    void texReadsUncompressedRgba()
    {
        TexSpec s = simpleRgba(8, 4);
        s.flags = TexFlags::ClampUVs | TexFlags::NoInterpolation;
        s.texW = 8;
        s.imgW = 6;
        s.imgH = 3;
        const Bytes file = makeTex(s);
        std::string error;
        auto tex = parseTex(span(file), {}, &error);
        QVERIFY2(tex.has_value(), error.c_str());
        QCOMPARE(tex->containerVersion, 3);
        QCOMPARE(tex->textureWidth, 8u);
        QCOMPARE(tex->imageWidth, 6u);
        QCOMPARE(tex->imageHeight, 3u);
        QVERIFY(tex->nearest());
        QVERIFY(tex->clampUVs());
        QVERIFY(!tex->animated());
        QCOMPARE(tex->format, PixelFormat::Rgba8);
        QCOMPARE(tex->mips.size(), size_t{1});
        QCOMPARE(tex->mips[0].data, s.images[0][0].pixels);
    }

    void texReadsLz4MipChainAndAllContainerVersions()
    {
        for (int version : {1, 2, 3, 4}) {
            TexSpec s = simpleRgba(16, 16);
            s.version = version;
            s.images = {{MipSpec{16, 16, rgbaPattern(16, 16), version >= 2}, MipSpec{8, 8, rgbaPattern(8, 8), version >= 2},
                         MipSpec{4, 4, rgbaPattern(4, 4), false}}};
            const Bytes file = makeTex(s);
            std::string error;
            auto tex = parseTex(span(file), {}, &error);
            QVERIFY2(tex.has_value(), qPrintable(QString("v%1: %2").arg(version).arg(error.c_str())));
            QCOMPARE(tex->containerVersion, version == 4 ? 3 : version); // a picture in a 0004 container reads as 0003
            QCOMPARE(tex->mips.size(), size_t{3});
            QCOMPARE(tex->mips[1].width, 8u);
            QCOMPARE(tex->mips[0].data, s.images[0][0].pixels);
            QCOMPARE(tex->mips[1].data, s.images[0][1].pixels);
            QCOMPARE(tex->mips[2].data, s.images[0][2].pixels);
        }
    }

    void texBlockAndSmallFormats()
    {
        struct Case {
            int32_t raw;
            PixelFormat format;
            uint32_t w, h;
            uint64_t bytes;
        };
        for (const Case &c : {Case{7, PixelFormat::Bc1, 8, 8, 32}, Case{6, PixelFormat::Bc2, 8, 4, 32}, Case{4, PixelFormat::Bc3, 6, 6, 64},
                              Case{12, PixelFormat::Bc7, 4, 4, 16}, Case{9, PixelFormat::R8, 5, 3, 15}, Case{8, PixelFormat::Rg8, 5, 3, 30},
                              Case{11, PixelFormat::R16f, 4, 2, 16}, Case{10, PixelFormat::Rg16f, 4, 2, 32},
                              Case{14, PixelFormat::Rgba16f, 2, 2, 32}}) {
            QCOMPARE(pixelBytes(c.format, c.w, c.h), c.bytes);
            TexSpec s;
            s.format = c.raw;
            s.texW = s.imgW = c.w;
            s.texH = s.imgH = c.h;
            Bytes px(static_cast<size_t>(c.bytes));
            for (size_t i = 0; i < px.size(); ++i)
                px[i] = static_cast<uint8_t>(i);
            s.images = {{MipSpec{c.w, c.h, px, true}}};
            const Bytes file = makeTex(s);
            std::string error;
            auto tex = parseTex(span(file), {}, &error);
            QVERIFY2(tex.has_value(), qPrintable(QString("format %1: %2").arg(c.raw).arg(error.c_str())));
            QCOMPARE(tex->format, c.format);
            QCOMPARE(tex->mips[0].data, px);
        }
    }

    void texReadsPackedAndExpandedFloatFormats()
    {
        // RGBA1010102: one 32 bit word per pixel, red low, alpha in the top two bits
        {
            TexSpec s;
            s.format = 13;
            s.texW = s.imgW = 2;
            s.texH = s.imgH = 1;
            // opaque white, and (r 1023, g 0, b 512, a 3) -> 0xC0 | 512 << 20 | 1023
            const uint32_t white = 0xFFFFFFFFu;
            const uint32_t mixed = (3u << 30) | (512u << 20) | 1023u;
            Bytes px;
            put32(px, white);
            put32(px, mixed);
            s.images = {{MipSpec{2, 1, px, false}}};
            const Bytes file = makeTex(s);
            std::string error;
            auto tex = parseTex(span(file), {}, &error);
            QVERIFY2(tex.has_value(), error.c_str());
            QCOMPARE(tex->format, PixelFormat::Rgb10A2);
            QCOMPARE(tex->rawFormat, 13);
            QCOMPARE(pixelBytes(tex->format, 2, 1), uint64_t{8});
            QCOMPARE(tex->mips[0].data, px);
            QVERIFY(texFormatSupported(13));
            QCOMPARE(std::string(texFormatName(13)), std::string("RGBA1010102"));
            QCOMPARE(vulkanFormat(PixelFormat::Rgb10A2), VK_FORMAT_A2B10G10R10_UNORM_PACK32);
            // header only and LZ4 mips agree
            QVERIFY(parseTex(span(file), TexReadOptions{false}).has_value());
            s.images = {{MipSpec{2, 1, px, true}}};
            const Bytes packed = makeTex(s);
            auto lz = parseTex(span(packed), {}, &error);
            QVERIFY2(lz.has_value(), error.c_str());
            QCOMPARE(lz->mips[0].data, px);
        }
        // RGB161616f: three halves become four with alpha 1.0
        {
            TexSpec s;
            s.format = 15;
            s.texW = s.imgW = 2;
            s.texH = s.imgH = 1;
            const Bytes px = {0x00, 0x3C, 0x00, 0x40, 0x00, 0x42,  // 1.0 2.0 3.0
                              0x00, 0x38, 0x00, 0x00, 0x00, 0xBC}; // 0.5 0.0 -1.0
            s.images = {{MipSpec{2, 1, px, true}}};
            const Bytes file = makeTex(s);
            std::string error;
            auto tex = parseTex(span(file), {}, &error);
            QVERIFY2(tex.has_value(), error.c_str());
            QCOMPARE(tex->format, PixelFormat::Rgba16f);
            QCOMPARE(tex->rawFormat, 15);
            QCOMPARE(tex->mips[0].data, (Bytes{0x00, 0x3C, 0x00, 0x40, 0x00, 0x42, 0x00, 0x3C,   //
                                               0x00, 0x38, 0x00, 0x00, 0x00, 0xBC, 0x00, 0x3C}));
            QCOMPARE(tex->mips[0].data.size(), static_cast<size_t>(pixelBytes(tex->format, 2, 1)));
            QVERIFY(texFormatSupported(15));
            QCOMPARE(std::string(texFormatName(15)), std::string("RGB161616f"));
            // a mip too short for its size is refused
            s.images = {{MipSpec{2, 1, Bytes(8, 0), false}}};
            const Bytes shortFile = makeTex(s);
            QVERIFY(!parseTex(span(shortFile), {}, &error).has_value());
        }
    }

    void texReadsEveryContainerVersion()
    {
        for (int version : {1, 2, 3, 4}) {
            TexSpec s = simpleRgba(2, 2);
            s.version = version;
            s.images = {{MipSpec{2, 2, rgbaPattern(2, 2), version >= 2}, MipSpec{1, 1, Bytes{9, 8, 7, 6}, false}}};
            const Bytes file = makeTex(s);
            std::string error;
            auto tex = parseTex(span(file), {}, &error);
            QVERIFY2(tex.has_value(), error.c_str());
            QCOMPARE(tex->containerVersion, version == 4 ? 3 : version);
            QCOMPARE(tex->mips.size(), size_t{2});
            QCOMPARE(tex->mips[0].data, rgbaPattern(2, 2));
            QCOMPARE(tex->mips[1].data, (Bytes{9, 8, 7, 6}));
        }
    }

    void texKeepsEverySpriteSheetImageAsAPage()
    {
        // a sheet on two images of different sizes: the frames name the image they lie on
        TexSpec s;
        s.version = 3;
        s.flags = TexFlags::IsGif | TexFlags::ClampUVs;
        s.texW = s.imgW = 4;
        s.texH = s.imgH = 2;
        const Bytes first = rgbaPattern(4, 2);
        const Bytes second(2 * 2 * 4, 0x55);
        s.images = {{MipSpec{4, 2, first, false}}, {MipSpec{2, 2, second, true}}};
        s.sheetVersion = 3;
        s.frames = {{0, 0, 4, 0, 0, 2}, {0, 0, 2, 0, 0, 2}};
        s.frameImages = {0, 1};
        const Bytes file = makeTex(s);
        std::string error;
        auto tex = parseTex(span(file), {}, &error);
        QVERIFY2(tex.has_value(), error.c_str());
        QCOMPARE(tex->mips.size(), size_t{1});
        QCOMPARE(tex->mips[0].data, first);
        QCOMPARE(tex->pages.size(), size_t{1});
        QCOMPARE(tex->droppedPages, 0u);
        QCOMPARE(tex->pages[0].mips[0].width, 2u);
        QCOMPARE(tex->pages[0].mips[0].height, 2u);
        QCOMPARE(tex->pages[0].mips[0].data, second);
        QCOMPARE(tex->frames[0].index, 0u);
        QCOMPARE(tex->frames[1].index, 1u);
        // a header read knows the pages' sizes and holds no pixels
        auto header = parseTex(span(file), TexReadOptions{false}, &error);
        QVERIFY2(header.has_value(), error.c_str());
        QCOMPARE(header->pages.size(), size_t{1});
        QCOMPARE(header->pages[0].mips[0].width, 2u);
        QVERIFY(header->pages[0].mips[0].data.empty());
        // a picture that is not a sprite sheet keeps its first image only
        s.flags = TexFlags::ClampUVs;
        s.sheetVersion = 0;
        s.frames.clear();
        const Bytes still = makeTex(s);
        auto plain = parseTex(span(still), {}, &error);
        QVERIFY2(plain.has_value(), error.c_str());
        QVERIFY(plain->pages.empty());
        QCOMPARE(plain->droppedPages, 0u);
        // several pages in order
        s.flags = TexFlags::IsGif;
        s.sheetVersion = 3;
        s.frames = {{0, 0, 4, 0, 0, 2}};
        s.frameImages = {0};
        s.images = {{MipSpec{4, 2, first, false}}, {MipSpec{2, 2, second, false}}, {MipSpec{1, 1, Bytes{1, 2, 3, 4}, false}}};
        const Bytes three = makeTex(s);
        auto many = parseTex(span(three), {}, &error);
        QVERIFY2(many.has_value(), error.c_str());
        QCOMPARE(many->pages.size(), size_t{2});
        QCOMPARE(many->pages[1].mips[0].data, (Bytes{1, 2, 3, 4}));
    }

    void texRefusesUnknownContainersWithTheirNames()
    {
        TexSpec s = simpleRgba(2, 2);
        Bytes file = makeTex(s);
        std::string error;
        file[7] = '6'; // TEXV0006
        QVERIFY(!parseTex(span(file), {}, &error).has_value());
        QVERIFY2(error.find("TEXV0006") != std::string::npos, error.c_str());
        file = makeTex(s);
        s.version = 5;
        const Bytes five = makeTex(s);
        QVERIFY(!parseTex(span(five), {}, &error).has_value());
        QVERIFY2(error.find("TEXB0005") != std::string::npos, error.c_str());
    }

    void texReadsTheExtraHeaderWord()
    {
        // flag bit 6 adds one word to the header; the embedded picture (32 wide, 4 tall) is announced with swapped sizes
        const Bytes pixels = rgbaPattern(4, 8);
        TexSpec s;
        s.version = 4;
        s.freeImage = 13;
        s.flags = TexFlags::ClampUVs | TexFlags::ExtraHeaderWord;
        s.extraWord = 0xFF2F2FD1u;
        s.volumeDepth = 4;
        s.texW = 4;
        s.texH = 4;
        s.imgW = 8;
        s.imgH = 4;
        s.images = {{MipSpec{4, 4, makePng(4, 8, pixels), false}}};
        const Bytes file = makeTex(s);
        std::string error;
        auto tex = parseTex(span(file), {}, &error);
        QVERIFY2(tex.has_value(), error.c_str());
        QCOMPARE(tex->headerExtra, 0xFF2F2FD1u);
        QCOMPARE(tex->volumeDepth, 4u);
        QCOMPARE(tex->mips[0].width, 4u);
        QCOMPARE(tex->mips[0].height, 8u);
        QCOMPARE(tex->mips[0].data, pixels);
        QCOMPARE(tex->imageWidth, 4u);
        QCOMPARE(tex->imageHeight, 8u);
        QCOMPARE(tex->textureHeight, 8u);
        QVERIFY(tex->clampUVs());
        QVERIFY(parseTex(span(file), TexReadOptions{false}).has_value());
    }

    void texExpandsRgb888AndRgb565()
    {
        {
            TexSpec s;
            s.format = 1;
            s.texW = s.imgW = 2;
            s.texH = s.imgH = 1;
            s.images = {{MipSpec{2, 1, {10, 20, 30, 40, 50, 60}, false}}};
            const Bytes file = makeTex(s);
            auto tex = parseTex(span(file));
            QVERIFY(tex.has_value());
            QCOMPARE(tex->mips[0].data, (Bytes{10, 20, 30, 255, 40, 50, 60, 255}));
        }
        {
            TexSpec s;
            s.format = 2;
            s.texW = s.imgW = 1;
            s.texH = s.imgH = 1;
            s.images = {{MipSpec{1, 1, {0x00, 0xF8}, false}}}; // 0xF800: pure red
            const Bytes file = makeTex(s);
            auto tex = parseTex(span(file));
            QVERIFY(tex.has_value());
            QCOMPARE(tex->mips[0].data, (Bytes{255, 0, 0, 255}));
        }
    }

    void texDecodesEmbeddedImages()
    {
        const Bytes pixels = rgbaPattern(5, 3);
        TexSpec s;
        s.version = 3;
        s.freeImage = 13; // PNG
        s.texW = 8;
        s.texH = 4;
        s.imgW = 5;
        s.imgH = 3;
        s.images = {{MipSpec{5, 3, makePng(5, 3, pixels), false}}};
        const Bytes file = makeTex(s);
        std::string error;
        auto tex = parseTex(span(file), {}, &error);
        QVERIFY2(tex.has_value(), error.c_str());
        QCOMPARE(tex->format, PixelFormat::Rgba8);
        QCOMPARE(tex->mips[0].width, 5u);
        QCOMPARE(tex->mips[0].data, pixels);
        QCOMPARE(tex->freeImageFormat, 13);

        // garbage in place of the image
        s.images = {{MipSpec{5, 3, Bytes(40, 0x55), false}}};
        const Bytes bad = makeTex(s);
        QVERIFY(!parseTex(span(bad), {}, &error).has_value());
        QVERIFY(!error.empty());
    }

    void texReadsAnimationSections()
    {
        for (int version : {1, 2, 3}) {
            TexSpec s = simpleRgba(8, 4);
            s.flags = TexFlags::IsGif;
            s.sheetVersion = version;
            s.frames = {{0, 0, 4, 0, 0, 4}, {4, 0, 4, 0, 0, 4}};
            const Bytes file = makeTex(s);
            std::string error;
            auto tex = parseTex(span(file), {}, &error);
            QVERIFY2(tex.has_value(), error.c_str());
            QVERIFY(tex->animated());
            QCOMPARE(tex->animationVersion, version);
            QCOMPARE(tex->frames.size(), size_t{2});
            QCOMPARE(tex->frames[1].x, 4.0f);
            QCOMPARE(tex->frames[1].axisXx, 4.0f);
            QCOMPARE(tex->frames[1].axisYy, 4.0f);
            QCOMPARE(tex->frames[0].duration, 0.25f);
            QCOMPARE(tex->sheetWidth, version == 3 ? 64u : 0u);
        }
    }

    void texHeaderOnlySkipsPixelsButKeepsStructure()
    {
        TexSpec s = simpleRgba(16, 16);
        s.flags = TexFlags::IsGif;
        s.sheetVersion = 2;
        s.frames = {{0, 0, 8, 0, 0, 8}};
        s.images = {{MipSpec{16, 16, rgbaPattern(16, 16), true}, MipSpec{8, 8, rgbaPattern(8, 8), false}}};
        const Bytes file = makeTex(s);
        auto tex = parseTex(span(file), TexReadOptions{false});
        QVERIFY(tex.has_value());
        QCOMPARE(tex->mips.size(), size_t{2});
        QVERIFY(tex->mips[0].data.empty());
        QCOMPARE(tex->frames.size(), size_t{1});

        // formats this reader does not know still parse in header-only mode, and fail when pixels are wanted
        s.format = 99;
        s.sheetVersion = 0;
        s.flags = 0;
        const Bytes odd = makeTex(s);
        QVERIFY(parseTex(span(odd), TexReadOptions{false}).has_value());
        std::string error;
        QVERIFY(!parseTex(span(odd), {}, &error).has_value());
        QVERIFY(error.find("unsupported texture format 99 (unknown)") != std::string::npos);
    }

    void texVideoParsesWithoutMips()
    {
        TexSpec s = simpleRgba(4, 4);
        s.version = 4;
        s.isVideo = 1;
        s.freeImage = 35;
        s.flags = TexFlags::Video;
        s.images = {{MipSpec{4, 4, Bytes(10, 1), false}}};
        Bytes file = makeTex(s);
        file.insert(file.end(), 50, 0xEE); // an MP4 payload the reader never looks at
        std::string error;
        auto tex = parseTex(span(file), {}, &error);
        QVERIFY2(tex.has_value(), error.c_str());
        QVERIFY(tex->isVideo);
        QVERIFY(tex->mips.empty());
    }

    void texRejectsImageBombs()
    {
        // A PNG header that claims 30000 x 30000 pixels must be refused before anything is allocated.
        Bytes png = makePng(5, 3, rgbaPattern(5, 3));
        const uint32_t huge = 30000;
        for (int i = 0; i < 4; ++i) {
            png[16 + static_cast<size_t>(i)] = static_cast<uint8_t>(huge >> (8 * (3 - i))); // IHDR width, big endian
            png[20 + static_cast<size_t>(i)] = static_cast<uint8_t>(huge >> (8 * (3 - i))); // height
        }
        TexSpec s;
        s.freeImage = 13;
        s.texW = s.texH = 8;
        s.imgW = s.imgH = 5;
        s.images = {{MipSpec{5, 3, png, false}}};
        const Bytes file = makeTex(s);
        std::string error;
        QVERIFY(!parseTex(span(file), {}, &error).has_value());
        QVERIFY(error.find("too large") != std::string::npos);

        // LZ4 claiming a million times its stored size
        TexSpec packed = simpleRgba(8, 8);
        packed.images = {{MipSpec{8, 8, rgbaPattern(8, 8), true}}};
        Bytes h = makeTex(packed);
        for (size_t i = 0; i + 8 < h.size(); ++i) {
            if (h[i] == 1 && h[i + 1] == 0 && h[i + 2] == 0 && h[i + 3] == 0 && h[i + 4] == 0 && h[i + 5] == 1 && h[i + 6] == 0 && h[i + 7] == 0) {
                h[i + 6] = 0x10; // 0x00100100: about a megabyte, over 255 times the few hundred stored bytes
                break;
            }
        }
        QVERIFY(!parseTex(span(h), {}, &error).has_value());
        QVERIFY(error.find("implausible") != std::string::npos);
    }

    void texRejectsMalformedFiles()
    {
        TexSpec s = simpleRgba(8, 8);
        s.images = {{MipSpec{8, 8, rgbaPattern(8, 8), true}}};
        s.flags = TexFlags::IsGif;
        s.sheetVersion = 2;
        s.frames = {{0, 0, 8, 0, 0, 8}};
        const Bytes good = makeTex(s);
        QVERIFY(parseTex(span(good)).has_value());
        // every truncation fails cleanly
        for (size_t n = 0; n + 1 < good.size(); ++n) {
            std::string error;
            const auto r = parseTex(std::span<const uint8_t>(good.data(), n), {}, &error);
            // the animation section is optional at the very end of the file, everything before it is mandatory
            if (n < good.size() - 45)
                QVERIFY2(!r.has_value(), qPrintable(QString::number(n)));
        }
        // wrong magic
        Bytes wrong = good;
        wrong[4] = 'X';
        QVERIFY(!parseTex(span(wrong)).has_value());
        // lz4 data that decodes to a different size than declared (uncompressed-size field 256 -> 300)
        TexSpec packed = simpleRgba(8, 8);
        packed.images = {{MipSpec{8, 8, rgbaPattern(8, 8), true}}};
        const auto patchSize = [](Bytes &file, uint8_t lowByte, uint8_t highByte) {
            // the field follows the lz4 flag: bytes 01 00 00 00 | 00 01 00 00 (flag 1, size 256)
            for (size_t i = 0; i + 8 < file.size(); ++i) {
                if (file[i] == 1 && file[i + 1] == 0 && file[i + 2] == 0 && file[i + 3] == 0 && file[i + 4] == 0 && file[i + 5] == 1
                    && file[i + 6] == 0 && file[i + 7] == 0) {
                    file[i + 4] = lowByte;
                    file[i + 7] = highByte;
                    return true;
                }
            }
            return false;
        };
        Bytes corrupt = makeTex(packed);
        QVERIFY(patchSize(corrupt, 0x2C, 0)); // 0x012C = 300
        std::string error;
        QVERIFY(!parseTex(span(corrupt), {}, &error).has_value());
        QVERIFY(!error.empty());
        // zero size
        TexSpec zero = simpleRgba(8, 8);
        zero.texW = 0;
        const Bytes z = makeTex(zero);
        QVERIFY(!parseTex(span(z)).has_value());
        // huge declared uncompressed size
        TexSpec huge = simpleRgba(8, 8);
        huge.images = {{MipSpec{8, 8, rgbaPattern(8, 8), true}}};
        Bytes h = makeTex(huge);
        QVERIFY(patchSize(h, 0, 0x7F));
        QVERIFY(!parseTex(span(h)).has_value());
    }
};

QTEST_GUILESS_MAIN(TstSceneVkAssets)
#include "tst_scenevk_assets.moc"
