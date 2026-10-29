#include "testutil.h"
#include "wallpaperengineimporter.h"

#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestPresets : public QObject
{
    Q_OBJECT

private slots:
    void resolvesPresetFromBase();
    void marksMissingBase();
    void rejectsPresetOfPreset();
    void acceptsNumericDependencyAndKeepsOwnTitle();
    void rejectsInvalidBase();
    void prefersOkItemForDuplicateWorkshopId();
    void resolvesWhenPresetSortsBeforeBase();
    void rejectsSelfDependency();

private:
    static WallpaperItem find(const QList<WallpaperItem> &items, QString id);
};

WallpaperItem TestPresets::find(const QList<WallpaperItem> &items, QString id)
{
    for (const WallpaperItem &item : items) {
        if (item.id == id)
            return item;
    }
    qFatal("item %s not found", qPrintable(id));
}

void TestPresets::resolvesPresetFromBase()
{
    QTemporaryDir tmp;
    const QString base = tmp.filePath(u"100"_s);
    testutil::writeProject(base, testutil::json(R"({
        "type": "Web", "file": "index.html", "title": "Base",
        "general": {"properties": {
            "a": {"type": "slider", "value": 1, "min": 0, "max": 10, "order": 1},
            "b": {"type": "bool", "value": true, "order": 2}
        }}
    })"));
    testutil::writeFile(base + u"/index.html"_s, "<html></html>");
    testutil::writeProject(tmp.filePath(u"200"_s), testutil::json(R"({
        "title": "Preset", "dependency": "100", "preset": {"a": 5, "b": null}
    })"));

    QList<WallpaperItem> items = WallpaperEngineImporter::scanDirectory(tmp.path());
    WallpaperEngineImporter::resolveDependencies(items);

    const WallpaperItem preset = find(items, u"we:200"_s);
    QCOMPARE(preset.status, ItemStatus::Ok);
    QCOMPARE(preset.kind, ItemKind::Web);
    QCOMPARE(preset.entryFile, base + u"/index.html"_s);
    QCOMPARE(preset.contentDir, base);
    QCOMPARE(preset.title, u"Preset"_s);
    QCOMPARE(preset.properties.size(), 2);
    QCOMPARE(preset.properties[0].value.toInt(), 5);
    QCOMPARE(preset.properties[1].value.toBool(), true); // null keeps the base default
    QCOMPARE(find(items, u"we:100"_s).properties[0].value.toInt(), 1);
}

void TestPresets::marksMissingBase()
{
    QTemporaryDir tmp;
    testutil::writeProject(tmp.filePath(u"200"_s), testutil::json(R"({"title": "P", "dependency": "999"})"));

    QList<WallpaperItem> items = WallpaperEngineImporter::scanDirectory(tmp.path());
    WallpaperEngineImporter::resolveDependencies(items);

    QCOMPARE(items[0].status, ItemStatus::MissingDependency);
    QCOMPARE(items[0].statusDetail, u"999"_s);
}

void TestPresets::rejectsPresetOfPreset()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"100"_s, u"Base"_s);
    testutil::writeProject(tmp.filePath(u"200"_s), testutil::json(R"({"title": "P1", "dependency": "100"})"));
    testutil::writeProject(tmp.filePath(u"300"_s), testutil::json(R"({"title": "P2", "dependency": "200"})"));

    QList<WallpaperItem> items = WallpaperEngineImporter::scanDirectory(tmp.path());
    WallpaperEngineImporter::resolveDependencies(items);

    QCOMPARE(find(items, u"we:200"_s).status, ItemStatus::Ok);
    QCOMPARE(find(items, u"we:300"_s).status, ItemStatus::Invalid);
}

void TestPresets::acceptsNumericDependencyAndKeepsOwnTitle()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"100"_s, u"Base"_s);
    testutil::writeProject(tmp.filePath(u"200"_s), testutil::json(R"({"title": "P", "dependency": 100})"));

    QList<WallpaperItem> items = WallpaperEngineImporter::scanDirectory(tmp.path());
    WallpaperEngineImporter::resolveDependencies(items);

    const WallpaperItem preset = find(items, u"we:200"_s);
    QCOMPARE(preset.status, ItemStatus::Ok);
    QCOMPARE(preset.kind, ItemKind::Video);
    QCOMPARE(preset.contentDir, tmp.filePath(u"100"_s));
    QCOMPARE(preset.entryFile, tmp.filePath(u"100/clip.mp4"_s));
}

void TestPresets::rejectsInvalidBase()
{
    QTemporaryDir tmp;
    testutil::writeProject(tmp.filePath(u"100"_s), testutil::json(R"({"type": "video", "file": "missing.mp4"})"));
    testutil::writeProject(tmp.filePath(u"200"_s), testutil::json(R"({"title": "P", "dependency": "100"})"));

    QList<WallpaperItem> items = WallpaperEngineImporter::scanDirectory(tmp.path());
    WallpaperEngineImporter::resolveDependencies(items);

    QCOMPARE(find(items, u"we:200"_s).status, ItemStatus::Invalid);
}

void TestPresets::prefersOkItemForDuplicateWorkshopId()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"100"_s, u"Base"_s);
    testutil::writeProject(tmp.filePath(u"200"_s), testutil::json(R"({"title": "P", "dependency": "100"})"));

    QList<WallpaperItem> items = WallpaperEngineImporter::scanDirectory(tmp.path());
    WallpaperItem broken = find(items, u"we:100"_s);
    broken.id = u"we:100-copy"_s;
    broken.status = ItemStatus::Invalid;
    items.append(broken); // same workshopId, later in the list, not playable
    WallpaperEngineImporter::resolveDependencies(items);

    QCOMPARE(find(items, u"we:200"_s).status, ItemStatus::Ok);
}

void TestPresets::resolvesWhenPresetSortsBeforeBase()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"100"_s, u"Base"_s);
    testutil::writeProject(tmp.filePath(u"050"_s), testutil::json(R"({"title": "P", "dependency": "100"})"));

    QList<WallpaperItem> items = WallpaperEngineImporter::scanDirectory(tmp.path());
    QCOMPARE(items[0].id, u"we:050"_s);
    WallpaperEngineImporter::resolveDependencies(items);

    QCOMPARE(find(items, u"we:050"_s).status, ItemStatus::Ok);
    QCOMPARE(find(items, u"we:050"_s).kind, ItemKind::Video);
}

void TestPresets::rejectsSelfDependency()
{
    QTemporaryDir tmp;
    testutil::writeProject(tmp.filePath(u"100"_s), testutil::json(R"({"title": "Loop", "dependency": "100"})"));

    QList<WallpaperItem> items = WallpaperEngineImporter::scanDirectory(tmp.path());
    WallpaperEngineImporter::resolveDependencies(items);

    QCOMPARE(items[0].status, ItemStatus::Invalid);
}

QTEST_GUILESS_MAIN(TestPresets)
#include "tst_presets.moc"
