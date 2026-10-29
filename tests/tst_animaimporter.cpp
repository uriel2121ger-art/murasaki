#include "animaengineimporter.h"
#include "animfixtures.h"
#include "testutil.h"
#include "wallpaperengineimporter.h"

#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;
using namespace animfixtures;

class TestAnimaImporter : public QObject
{
    Q_OBJECT

private slots:
    void scansSingleApngItem();
    void variantsAreSortedByNameAndMixFormats();
    void skipsBrokenVariants();
    void allBrokenVariantsMarkItemInvalid();
    void dirWithoutAnimationFilesIsSkipped();
    void nonNumericDirHasNoWorkshopId();
    void symlinkOutOfTheItemDirIsNotAVariant();
    void variantFileIsCanonicalWhenTheItemDirIsReachedThroughASymlink();
    void scanDirectoryListsEveryItem();
    void jsonShapes();
    void kindStringsRoundTrip();
    void flagsProgramsShippedWithAPet();
    void anApngNamedPngIsAVariantAndAStillPngIsNot();
};

static QString makeItem(const QString &root, const QString &id)
{
    return root + u'/' + id;
}

void TestAnimaImporter::anApngNamedPngIsAVariantAndAStillPngIsNot()
{
    QTemporaryDir tmp;
    const QString dir = makeItem(tmp.path(), u"3819174039"_s);
    testutil::writeFile(dir + u"/Malfoid Animation.png"_s, apng(40, 30, {{1, 10}, {1, 10}}));
    testutil::writeFile(dir + u"/preview.png"_s, apng(40, 30, {}));
    const auto item = AnimaEngineImporter::scanItem(dir);
    QVERIFY(item);
    QCOMPARE(item->variants.size(), 1);
    QCOMPARE(item->variants.at(0).file, testutil::canonical(dir + u"/Malfoid Animation.png"_s));
}

void TestAnimaImporter::scansSingleApngItem()
{
    QTemporaryDir tmp;
    const QString dir = makeItem(tmp.path(), u"3753131028"_s);
    const QByteArray data = apng(500, 666, {{1, 60}, {1, 60}, {1, 60}});
    testutil::writeFile(dir + u"/animated.apng"_s, data);

    const auto item = AnimaEngineImporter::scanItem(dir);
    QVERIFY(item);
    QCOMPARE(item->id, u"anima:3753131028"_s);
    QCOMPARE(item->source, u"anima-engine"_s);
    QCOMPARE(item->kind, ItemKind::Pet);
    QCOMPARE(item->status, ItemStatus::Ok);
    QCOMPARE(item->workshopId, u"3753131028"_s);
    QCOMPARE(item->title, u"3753131028"_s);
    QCOMPARE(item->dir, dir);
    QCOMPARE(item->contentDir, dir);
    QVERIFY(item->entryFile.isEmpty());
    QVERIFY(item->previewPath.isEmpty());
    QCOMPARE(item->variants.size(), 1);
    const PetVariant &variant = item->variants.at(0);
    QCOMPARE(variant.name, u"animated"_s);
    QCOMPARE(variant.file, testutil::canonical(dir + u"/animated.apng"_s));
    QCOMPARE(variant.width, 500);
    QCOMPARE(variant.height, 666);
    QCOMPARE(variant.frames, 3);
    QCOMPARE(variant.fps, 60.0);
    QCOMPARE(variant.bytes, qint64(data.size()));
}

void TestAnimaImporter::variantsAreSortedByNameAndMixFormats()
{
    QTemporaryDir tmp;
    const QString dir = makeItem(tmp.path(), u"1"_s);
    testutil::writeFile(dir + u"/b one.apng"_s, apng(10, 10, {{1, 10}, {1, 10}}));
    testutil::writeFile(dir + u"/A two.GIF"_s, gif(20, 20, {10, 10}));
    testutil::writeFile(dir + u"/c three.webp"_s, webpAnimated(30, 30, {100, 100}));
    testutil::writeFile(dir + u"/readme.txt"_s, "ignored");
    testutil::writeFile(dir + u"/clip.webm"_s, "ignored: webm has no header reader");

    const auto item = AnimaEngineImporter::scanItem(dir);
    QVERIFY(item);
    QCOMPARE(item->status, ItemStatus::Ok);
    QCOMPARE(item->variants.size(), 3);
    QCOMPARE(item->variants.at(0).name, u"A two"_s);
    QCOMPARE(item->variants.at(0).width, 20);
    QCOMPARE(item->variants.at(1).name, u"b one"_s);
    QCOMPARE(item->variants.at(2).name, u"c three"_s);
    QCOMPARE(item->variants.at(2).fps, 10.0);
}

void TestAnimaImporter::skipsBrokenVariants()
{
    QTemporaryDir tmp;
    const QString dir = makeItem(tmp.path(), u"1"_s);
    testutil::writeFile(dir + u"/good.apng"_s, apng(10, 10, {{1, 10}, {1, 10}}));
    testutil::writeFile(dir + u"/half.apng"_s, apng(10, 10, {{1, 10}, {1, 10}}).left(70)); // still downloading

    const auto item = AnimaEngineImporter::scanItem(dir);
    QVERIFY(item);
    QCOMPARE(item->status, ItemStatus::Ok);
    QCOMPARE(item->variants.size(), 1);
    QCOMPARE(item->variants.at(0).name, u"good"_s);
}

void TestAnimaImporter::allBrokenVariantsMarkItemInvalid()
{
    QTemporaryDir tmp;
    const QString dir = makeItem(tmp.path(), u"1"_s);
    testutil::writeFile(dir + u"/half.apng"_s, QByteArray("\x89PNG\r\n\x1a\n", 8) + "junk");

    const auto item = AnimaEngineImporter::scanItem(dir);
    QVERIFY(item);
    QCOMPARE(item->status, ItemStatus::Invalid);
    QVERIFY(item->variants.isEmpty());
    QVERIFY(item->statusDetail.contains(u"half.apng"_s));
}

void TestAnimaImporter::dirWithoutAnimationFilesIsSkipped()
{
    QTemporaryDir tmp;
    QVERIFY(QDir().mkpath(makeItem(tmp.path(), u"empty"_s)));
    const QString other = makeItem(tmp.path(), u"other"_s);
    testutil::writeFile(other + u"/notes.txt"_s, "x");
    QVERIFY(!AnimaEngineImporter::scanItem(makeItem(tmp.path(), u"empty"_s)));
    QVERIFY(!AnimaEngineImporter::scanItem(other));
    QVERIFY(!AnimaEngineImporter::scanItem(makeItem(tmp.path(), u"missing"_s)));
    QVERIFY(AnimaEngineImporter::scanDirectory(tmp.path()).isEmpty());
}

void TestAnimaImporter::nonNumericDirHasNoWorkshopId()
{
    QTemporaryDir tmp;
    const QString dir = makeItem(tmp.path(), u"my pet"_s);
    testutil::writeFile(dir + u"/a.gif"_s, gif(8, 8, {10, 10}));
    const auto item = AnimaEngineImporter::scanItem(dir);
    QVERIFY(item);
    QCOMPARE(item->id, u"anima:my pet"_s);
    QVERIFY(item->workshopId.isEmpty());
}

void TestAnimaImporter::symlinkOutOfTheItemDirIsNotAVariant()
{
    QTemporaryDir tmp;
    const QString outside = tmp.filePath(u"outside/real.apng"_s);
    testutil::writeFile(outside, apng(10, 10, {{1, 10}, {1, 10}}));
    const QString dir = makeItem(tmp.path(), u"1"_s);
    testutil::writeFile(dir + u"/own.apng"_s, apng(10, 10, {{1, 10}, {1, 10}}));
    QVERIFY(QFile::link(outside, dir + u"/escape.apng"_s));

    const auto item = AnimaEngineImporter::scanItem(dir);
    QVERIFY(item);
    QCOMPARE(item->variants.size(), 1);
    QCOMPARE(item->variants.at(0).name, u"own"_s);
}

void TestAnimaImporter::variantFileIsCanonicalWhenTheItemDirIsReachedThroughASymlink()
{
    QTemporaryDir tmp;
    const QString real = tmp.filePath(u"real/1"_s);
    testutil::writeFile(real + u"/a.apng"_s, apng(10, 10, {{1, 10}, {1, 10}}));
    QVERIFY(QFile::link(tmp.filePath(u"real"_s), tmp.filePath(u"link"_s)));

    const auto item = AnimaEngineImporter::scanItem(tmp.filePath(u"link/1"_s));
    QVERIFY(item);
    QCOMPARE(item->variants.size(), 1);
    QCOMPARE(item->variants.at(0).file, testutil::canonical(real + u"/a.apng"_s));
}

void TestAnimaImporter::scanDirectoryListsEveryItem()
{
    QTemporaryDir tmp;
    testutil::writeFile(makeItem(tmp.path(), u"20"_s) + u"/a.apng"_s, apng(5, 5, {{1, 10}, {1, 10}}));
    testutil::writeFile(makeItem(tmp.path(), u"10"_s) + u"/a.apng"_s, apng(6, 6, {{1, 10}, {1, 10}}));
    testutil::writeFile(tmp.filePath(u"stray.apng"_s), "a file, not an item dir");

    const QList<WallpaperItem> items = AnimaEngineImporter::scanDirectory(tmp.path());
    QCOMPARE(items.size(), 2);
    QCOMPARE(items.at(0).id, u"anima:10"_s);
    QCOMPARE(items.at(1).id, u"anima:20"_s);
}

void TestAnimaImporter::jsonShapes()
{
    QTemporaryDir tmp;
    const QString dir = makeItem(tmp.path(), u"7"_s);
    testutil::writeFile(dir + u"/a.apng"_s, apng(12, 34, {{1, 30}, {1, 30}}));
    testutil::writeFile(dir + u"/b.gif"_s, gif(8, 8, {10, 10}));
    const auto item = AnimaEngineImporter::scanItem(dir);
    QVERIFY(item);

    const QJsonObject summary = item->toSummaryJson();
    QCOMPARE(summary.value(u"kind"_s).toString(), u"pet"_s);
    QCOMPARE(summary.value(u"variantCount"_s).toInt(), 2);
    QVERIFY(!summary.contains(u"variants"_s)); // listings stay small and path-free

    const QJsonObject full = item->toJson();
    QCOMPARE(full.value(u"kind"_s).toString(), u"pet"_s);
    const QJsonArray variants = full.value(u"variants"_s).toArray();
    QCOMPARE(variants.size(), 2);
    const QJsonObject first = variants.at(0).toObject();
    QCOMPARE(first.value(u"name"_s).toString(), u"a"_s);
    QCOMPARE(first.value(u"file"_s).toString(), testutil::canonical(dir + u"/a.apng"_s));
    QCOMPARE(first.value(u"width"_s).toInt(), 12);
    QCOMPARE(first.value(u"height"_s).toInt(), 34);
    QCOMPARE(first.value(u"fps"_s).toDouble(), 30.0);
    QCOMPARE(first.value(u"frames"_s).toInt(), 2);
    QVERIFY(first.value(u"bytes"_s).toInteger() > 0);

    // Wallpaper Engine items keep their old shape.
    const QString weDir = testutil::writeVideoItem(tmp.filePath(u"we"_s), u"1"_s, u"V"_s);
    const auto video = WallpaperEngineImporter::scanItem(weDir);
    QVERIFY(video);
    QVERIFY(!video->toSummaryJson().contains(u"variantCount"_s));
    QVERIFY(!video->toJson().contains(u"variants"_s));
}

void TestAnimaImporter::kindStringsRoundTrip()
{
    QCOMPARE(itemKindToString(ItemKind::Pet), u"pet"_s);
    QCOMPARE(itemKindFromString(u"Pet"_s), ItemKind::Pet);
}

void TestAnimaImporter::flagsProgramsShippedWithAPet()
{
    QTemporaryDir tmp;
    const QString dir = makeItem(tmp.path(), u"42"_s);
    testutil::writeFile(dir + u"/animated.apng"_s, apng(10, 10, {{1, 60}}));
    testutil::writeFile(dir + u"/install.sh"_s, "#!/bin/sh\n");
    const auto item = AnimaEngineImporter::scanItem(dir);
    QVERIFY(item);
    QCOMPARE(item->status, ItemStatus::Ok);
    QCOMPARE(item->risks.size(), 1);
    QCOMPARE(item->risks.first().file, u"install.sh"_s);
}

QTEST_GUILESS_MAIN(TestAnimaImporter)
#include "tst_animaimporter.moc"
