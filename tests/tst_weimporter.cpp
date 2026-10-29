#include "testutil.h"
#include "wallpaperengineimporter.h"

#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestWeImporter : public QObject
{
    Q_OBJECT

private slots:
    void scansVideoItemWithBom();
    void acceptsPackedScene();
    void marksMissingEntryInvalid();
    void marksUnsupportedTypeInvalid();
    void marksBrokenJsonInvalid();
    void recognisesEditorAssetPacks();
    void scanDirectorySkipsDirsWithoutProject();
    void summaryJsonOmitsProperties();
    void acceptsScenePackNamedAfterEntry();
    void rejectsEntryEscapingItemDir();
    void dropsPreviewEscapingItemDir();
    void rejectsAbsoluteEntry();
    void directoryEntryIsNotAFile();
    void parsesPresetItem();
    void ignoresInvalidDependency();
    void rejectsNonNumericDependency();
    void acceptsMixedCaseType();
    void rejectsNonObjectRoot();
    void rejectsOversizedProject();
    void parsesLocalization();
};

void TestWeImporter::scansVideoItemWithBom()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"123"_s);
    testutil::writeProject(dir, testutil::json(R"({
        "type": "Video", "file": "clip.mp4", "title": "Rainy Night", "preview": "preview.jpg",
        "tags": ["Anime", "Relaxing"], "contentrating": "Everyone",
        "general": {"properties": {"volume": {"type": "slider", "value": 30, "min": 0, "max": 100}}}
    })"), /*withBom=*/true);
    testutil::writeFile(dir + u"/clip.mp4"_s, "v");
    testutil::writeFile(dir + u"/preview.jpg"_s, "p");

    const auto item = WallpaperEngineImporter::scanItem(dir);
    QVERIFY(item.has_value());
    QCOMPARE(item->id, u"we:123"_s);
    QCOMPARE(item->source, u"wallpaper-engine"_s);
    QCOMPARE(item->workshopId, u"123"_s);
    QCOMPARE(item->title, u"Rainy Night"_s);
    QCOMPARE(item->kind, ItemKind::Video);
    QCOMPARE(item->status, ItemStatus::Ok);
    QCOMPARE(item->entryFile, dir + u"/clip.mp4"_s);
    QCOMPARE(item->contentDir, dir);
    QCOMPARE(item->previewPath, dir + u"/preview.jpg"_s);
    QCOMPARE(item->tags, QStringList({u"Anime"_s, u"Relaxing"_s}));
    QCOMPARE(item->contentRating, u"Everyone"_s);
    QCOMPARE(item->properties.size(), 1);
}

void TestWeImporter::acceptsPackedScene()
{
    // scene.json usually lives inside scene.pkg (71 of 72 scenes in the reference library).
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"456"_s);
    testutil::writeProject(dir, testutil::json(R"({"type": "scene", "file": "scene.json", "title": "S"})"));
    testutil::writeFile(dir + u"/scene.pkg"_s, "PKGV0001");

    const auto item = WallpaperEngineImporter::scanItem(dir);
    QCOMPARE(item->kind, ItemKind::Scene);
    QCOMPARE(item->status, ItemStatus::Ok);
}

void TestWeImporter::marksMissingEntryInvalid()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"789"_s);
    testutil::writeProject(dir, testutil::json(R"({"type": "video", "file": "gone.mp4", "title": "V"})"));

    const auto item = WallpaperEngineImporter::scanItem(dir);
    QCOMPARE(item->status, ItemStatus::Invalid);
    QVERIFY(item->statusDetail.contains(u"gone.mp4"_s));
}

void TestWeImporter::marksUnsupportedTypeInvalid()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"321"_s);
    testutil::writeProject(dir, testutil::json(R"({"type": "application", "file": "app.exe"})"));

    const auto item = WallpaperEngineImporter::scanItem(dir);
    QCOMPARE(item->kind, ItemKind::Unknown);
    QCOMPARE(item->status, ItemStatus::Invalid);
    QVERIFY(item->statusDetail.contains(u"application"_s));
}

// The Workshop also holds packs for the editor (clocks, visualizers): no "type", "category": "Asset", "file": "assets.json".
// They are not broken wallpapers, they are not wallpapers at all.
void TestWeImporter::recognisesEditorAssetPacks()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"2760603383"_s);
    testutil::writeProject(dir, testutil::json(R"({"category": "Asset", "contentrating": "Everyone", "file": "assets.json",
        "title": "Clock pack", "tags": ["Asset"], "visibility": "public"})"));
    auto item = WallpaperEngineImporter::scanItem(dir);
    QVERIFY(item.has_value());
    QCOMPARE(item->kind, ItemKind::Asset);
    QCOMPARE(item->status, ItemStatus::Ok);
    QCOMPARE(itemKindToString(item->kind), u"asset"_s);

    // Only one of the two marks is enough; a typeless project without either is still unsupported.
    testutil::writeProject(tmp.filePath(u"2"_s), testutil::json(R"({"file": "assets.json", "title": "x"})"));
    QCOMPARE(WallpaperEngineImporter::scanItem(tmp.filePath(u"2"_s))->kind, ItemKind::Asset);
    testutil::writeProject(tmp.filePath(u"3"_s), testutil::json(R"({"file": "x.bin", "title": "x"})"));
    QCOMPARE(WallpaperEngineImporter::scanItem(tmp.filePath(u"3"_s))->status, ItemStatus::Invalid);
    // A preset (it has a dependency) keeps being resolved against its base.
    testutil::writeProject(tmp.filePath(u"4"_s), testutil::json(R"({"dependency": "1", "file": "assets.json", "title": "p"})"));
    QCOMPARE(WallpaperEngineImporter::scanItem(tmp.filePath(u"4"_s))->kind, ItemKind::Unknown);
}

void TestWeImporter::marksBrokenJsonInvalid()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"654"_s);
    testutil::writeFile(dir + u"/project.json"_s, "{ not json");

    const auto item = WallpaperEngineImporter::scanItem(dir);
    QVERIFY(item.has_value());
    QCOMPARE(item->status, ItemStatus::Invalid);
    QCOMPARE(item->title, u"654"_s);
}

void TestWeImporter::scanDirectorySkipsDirsWithoutProject()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"One"_s);
    testutil::writeVideoItem(tmp.path(), u"2"_s, u"Two"_s);
    QVERIFY(QDir().mkpath(tmp.filePath(u"downloading"_s)));

    QVERIFY(!WallpaperEngineImporter::scanItem(tmp.filePath(u"downloading"_s)).has_value());
    QCOMPARE(WallpaperEngineImporter::scanDirectory(tmp.path()).size(), 2);
}

void TestWeImporter::summaryJsonOmitsProperties()
{
    QTemporaryDir tmp;
    const QString dir = testutil::writeVideoItem(tmp.path(), u"1"_s, u"One"_s);
    const auto item = WallpaperEngineImporter::scanItem(dir);

    const QJsonObject summary = item->toSummaryJson();
    QCOMPARE(summary.value(u"kind"_s).toString(), u"video"_s);
    QCOMPARE(summary.value(u"status"_s).toString(), u"ok"_s);
    QVERIFY(!summary.contains(u"properties"_s));
    QVERIFY(item->toJson().contains(u"properties"_s));
    QCOMPARE(item->toJson().value(u"entryFile"_s).toString(), dir + u"/clip.mp4"_s);
}

void TestWeImporter::acceptsScenePackNamedAfterEntry()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"3766"_s);
    testutil::writeProject(dir, testutil::json(R"({"type": "scene", "file": "gifscene.json", "title": "G"})"));
    testutil::writeFile(dir + u"/gifscene.pkg"_s, "PKGV0001");
    const auto item = WallpaperEngineImporter::scanItem(dir);
    QCOMPARE(item->status, ItemStatus::Ok);
}

void TestWeImporter::rejectsEntryEscapingItemDir()
{
    QTemporaryDir tmp;
    testutil::writeFile(tmp.filePath(u"outside/x.mp4"_s), "v");
    const QString dir = tmp.filePath(u"1"_s);
    testutil::writeProject(dir, testutil::json(R"({"type": "video", "file": "../outside/x.mp4"})"));
    const auto item = WallpaperEngineImporter::scanItem(dir);
    QCOMPARE(item->status, ItemStatus::Invalid);
    QCOMPARE(item->statusDetail, u"entry path escapes item dir"_s);
}

void TestWeImporter::dropsPreviewEscapingItemDir()
{
    QTemporaryDir tmp;
    testutil::writeFile(tmp.filePath(u"outside/p.jpg"_s), "p");
    const QString dir = tmp.filePath(u"1"_s);
    testutil::writeProject(dir, testutil::json(R"({"type": "video", "file": "clip.mp4", "preview": "../outside/p.jpg"})"));
    testutil::writeFile(dir + u"/clip.mp4"_s, "v");
    const auto item = WallpaperEngineImporter::scanItem(dir);
    QCOMPARE(item->status, ItemStatus::Ok);
    QVERIFY(item->previewPath.isEmpty());
}

void TestWeImporter::rejectsAbsoluteEntry()
{
    QTemporaryDir tmp;
    testutil::writeFile(tmp.filePath(u"abs.mp4"_s), "v");
    const QString dir = tmp.filePath(u"1"_s);
    QJsonObject project{{u"type"_s, u"video"_s}, {u"file"_s, tmp.filePath(u"abs.mp4"_s)}};
    testutil::writeProject(dir, project);
    const auto item = WallpaperEngineImporter::scanItem(dir);
    QCOMPARE(item->status, ItemStatus::Invalid);
    QCOMPARE(item->statusDetail, u"entry path escapes item dir"_s);
}

void TestWeImporter::directoryEntryIsNotAFile()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"1"_s);
    testutil::writeProject(dir, testutil::json(R"({"type": "video", "file": "clip.mp4"})"));
    QVERIFY(QDir().mkpath(dir + u"/clip.mp4"_s));
    QCOMPARE(WallpaperEngineImporter::scanItem(dir)->status, ItemStatus::Invalid);
}

void TestWeImporter::parsesPresetItem()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"900"_s);
    testutil::writeProject(dir, testutil::json(R"({"dependency": "123", "preset": {"volume": 5}, "title": "P"})"));
    const auto item = WallpaperEngineImporter::scanItem(dir);
    QCOMPARE(item->dependencyId, u"123"_s);
    QCOMPARE(item->presetValues.value(u"volume"_s).toInt(), 5);
    QCOMPARE(item->toJson().value(u"presetValues"_s).toObject().value(u"volume"_s).toInt(), 5);
    QVERIFY(!item->toSummaryJson().contains(u"presetValues"_s));
}

void TestWeImporter::ignoresInvalidDependency()
{
    QTemporaryDir tmp;
    const QString dir = testutil::writeVideoItem(tmp.path(), u"1"_s, u"One"_s);
    testutil::writeProject(dir, testutil::json(R"({"type": "video", "file": "clip.mp4", "dependency": {"a": 1}})"));
    QVERIFY(WallpaperEngineImporter::scanItem(dir)->dependencyId.isEmpty());
    testutil::writeProject(dir, testutil::json(R"({"type": "video", "file": "clip.mp4", "dependency": 42})"));
    QCOMPARE(WallpaperEngineImporter::scanItem(dir)->dependencyId, u"42"_s);
    testutil::writeProject(dir, testutil::json(R"({"type": "video", "file": "clip.mp4", "dependency": 1.5})"));
    QVERIFY(WallpaperEngineImporter::scanItem(dir)->dependencyId.isEmpty());
}

void TestWeImporter::rejectsNonNumericDependency()
{
    QTemporaryDir tmp;
    const QString dir = testutil::writeVideoItem(tmp.path(), u"1"_s, u"One"_s);
    for (const char *bad : {"\"../x\"", "\"12a\"", "\"\"", "\"12\\n\"", "\"-5\"", "1.5", "-3", "{\"a\": 1}", "true"}) {
        testutil::writeProject(dir, testutil::json((QByteArray(R"({"type": "video", "file": "clip.mp4", "dependency": )")
                                                   + bad + "}").constData()));
        const auto item = WallpaperEngineImporter::scanItem(dir);
        QVERIFY2(item.has_value(), bad);
        QVERIFY2(item->dependencyId.isEmpty(), bad);
        QVERIFY2(item->status == ItemStatus::Invalid, bad);
        QCOMPARE(item->statusDetail, u"invalid dependency id"_s);
    }
    testutil::writeProject(dir, testutil::json(R"({"type": "video", "file": "clip.mp4", "dependency": null})"));
    QVERIFY(WallpaperEngineImporter::scanItem(dir)->status == ItemStatus::Ok);
}

void TestWeImporter::acceptsMixedCaseType()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"1"_s);
    testutil::writeProject(dir, testutil::json(R"({"type": "VIDEO", "file": "clip.mp4"})"));
    testutil::writeFile(dir + u"/clip.mp4"_s, "v");
    const auto item = WallpaperEngineImporter::scanItem(dir);
    QCOMPARE(item->kind, ItemKind::Video);
    QCOMPARE(item->status, ItemStatus::Ok);
}

void TestWeImporter::rejectsNonObjectRoot()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"1"_s);
    testutil::writeFile(dir + u"/project.json"_s, "[1, 2]");
    const auto item = WallpaperEngineImporter::scanItem(dir);
    QCOMPARE(item->status, ItemStatus::Invalid);
    QCOMPARE(item->statusDetail, u"project.json: not a JSON object"_s);
}

void TestWeImporter::rejectsOversizedProject()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"1"_s);
    testutil::writeFile(dir + u"/project.json"_s, QByteArray(16 * 1024 * 1024 + 1, ' '));
    const auto item = WallpaperEngineImporter::scanItem(dir);
    QCOMPARE(item->status, ItemStatus::Invalid);
    QCOMPARE(item->statusDetail, u"project.json too large"_s);
}

void TestWeImporter::parsesLocalization()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"7"_s);
    testutil::writeProject(dir, testutil::json(R"({
        "type": "Video", "file": "clip.mp4", "title": "T",
        "general": {"localization": {
            "en-US": {"ui_a": "Alpha", "bad": 5},
            "es-es": {"ui_a": "Alfa"},
            "empty": {"x": 1},
            "junk": "no"
        }}
    })"));
    testutil::writeFile(dir + u"/clip.mp4"_s, "v");
    const auto item = WallpaperEngineImporter::scanItem(dir);
    QVERIFY(item.has_value());
    QCOMPARE(item->localization.keys().size(), 2);
    QCOMPARE(item->localization.value(u"en-us"_s).toObject().value(u"ui_a"_s).toString(), u"Alpha"_s);
    QVERIFY(!item->localization.value(u"en-us"_s).toObject().contains(u"bad"_s));
    QCOMPARE(item->localization.value(u"es-es"_s).toObject().value(u"ui_a"_s).toString(), u"Alfa"_s);
    QVERIFY(item->toJson().contains(u"localization"_s));
    QVERIFY(!item->toSummaryJson().contains(u"localization"_s));
}

QTEST_GUILESS_MAIN(TestWeImporter)
#include "tst_weimporter.moc"
