// The puppet folders a user adds as pets (src/core/localpuppets.*, the store, the library scan): no daemon, no process.
#include "library.h"
#include "localpets.h"
#include "localpuppets.h"
#include "testutil.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

QString openFolder(const QTemporaryDir &dir, const QString &name = u"haru"_s)
{
    const QString folder = dir.filePath(name);
    testutil::writeFile(folder + u"/model.inp"_s, QByteArray("TRNSRTS2") + QByteArray(40, 'x'));
    return folder;
}

QString cubismFolder(const QTemporaryDir &dir, const QString &name = u"kei"_s)
{
    const QString folder = dir.filePath(name);
    testutil::writeFile(folder + u"/kei.model3.json"_s, R"({"Version":3,"FileReferences":{"Moc":"kei.moc3","Textures":["t.png"],
        "Expressions":[{"Name":"smile","File":"smile.exp3.json"}],"Motions":{"Idle":[{"File":"idle.motion3.json"}],"TapBody":[{"File":"tap.motion3.json"},{"File":"tap2.motion3.json"}]}},
        "HitAreas":[{"Id":"HitAreaBody","Name":"Body"}]})");
    for (const char *f : {"kei.moc3", "t.png", "smile.exp3.json", "idle.motion3.json", "tap.motion3.json", "tap2.motion3.json"})
        testutil::writeFile(folder + u'/' + QString::fromLatin1(f), "x");
    return folder;
}

} // namespace

class TestLocalPuppets : public QObject
{
    Q_OBJECT
private slots:
    void validateAcceptsAPuppetFolderAndNamesWhatIsWrong();
    void theIdIsOfTheCanonicalPath();
    void anOpenPuppetBecomesAPetItem();
    void aCubismPuppetIsListedAndMissingItsRuntime();
    void theCheckAnswerIsMerged();
    void aVanishedFolderIsInvalidNotGone();
    void theStoreRemovesByPuppetId();
    void whatTheCheckerSaysIsCleaned();
    void aChangedFolderIsReadAgain();
    void theLibraryListsPuppetsBesidePets();
};

void TestLocalPuppets::validateAcceptsAPuppetFolderAndNamesWhatIsWrong()
{
    QTemporaryDir dir;
    QString error;
    const QString folder = openFolder(dir);
    QCOMPARE(LocalPuppetImporter::validate(folder, &error), QFileInfo(folder).canonicalFilePath());
    // a link to the folder gives the canonical path
    QVERIFY(QFile::link(folder, dir.filePath(u"link"_s)));
    QCOMPARE(LocalPuppetImporter::validate(dir.filePath(u"link"_s), &error), QFileInfo(folder).canonicalFilePath());
    for (const QString &bad : {QString(), u"relative/path"_s, dir.filePath(u"missing"_s), dir.filePath(u"haru/model.inp"_s)}) {
        error.clear();
        QVERIFY2(LocalPuppetImporter::validate(bad, &error).isEmpty(), qPrintable(bad));
        QVERIFY2(!error.isEmpty(), qPrintable(bad));
    }
    // an empty folder, and a folder with a symlink in it: the manifest's reasons come back
    QVERIFY(QDir().mkpath(dir.filePath(u"empty"_s)));
    QVERIFY(LocalPuppetImporter::validate(dir.filePath(u"empty"_s), &error).isEmpty());
    QVERIFY2(error.contains(u"puppet: not a puppet"_s), qPrintable(error));
    QVERIFY(QFile::link(u"/etc/passwd"_s, folder + u"/evil.png"_s));
    QVERIFY(LocalPuppetImporter::validate(folder, &error).isEmpty());
    QVERIFY2(error.contains(u"puppet: unsafe path"_s), qPrintable(error));
}

void TestLocalPuppets::theIdIsOfTheCanonicalPath()
{
    const QString id = LocalPuppetImporter::itemIdFor(u"/a/b"_s);
    QVERIFY(id.startsWith(u"local:puppet:"_s));
    QCOMPARE(id.size(), QString(u"local:puppet:"_s).size() + 40);
    QCOMPARE(id, LocalPuppetImporter::itemIdFor(u"/a/b"_s));
    QVERIFY(id != LocalPuppetImporter::itemIdFor(u"/a/c"_s));
    QVERIFY(id != LocalPetImporter::itemIdFor(u"/a/b"_s)); // (never the id of a pet file at the same path)
}

void TestLocalPuppets::anOpenPuppetBecomesAPetItem()
{
    QTemporaryDir dir;
    const QString folder = QFileInfo(openFolder(dir)).canonicalFilePath();
    const WallpaperItem item = LocalPuppetImporter::scanItem(folder);
    QCOMPARE(item.id, LocalPuppetImporter::itemIdFor(folder));
    QCOMPARE(item.source, u"local"_s);
    QCOMPARE(item.kind, ItemKind::Pet);
    QCOMPARE(item.format, u"puppet"_s);
    QCOMPARE(item.status, ItemStatus::Ok);
    QCOMPARE(item.title, u"model"_s);
    QCOMPARE(item.entryFile, folder + u"/model.inp"_s);
    QCOMPARE(item.variants.size(), 1);
    QCOMPARE(item.variants[0].name, u"model"_s);
    QCOMPARE(item.variants[0].file, item.entryFile);
    QCOMPARE(item.variants[0].frames, 0); // not an animation: nothing to decode or transcode
    QVERIFY(!item.variants[0].isHeavy());
    QCOMPARE(item.puppet.value(u"backend"_s).toString(), u"open"_s);
    QCOMPARE(item.puppet.value(u"checked"_s).toBool(), false);
    // the summary and the full form say it is a puppet
    QCOMPARE(item.toSummaryJson().value(u"format"_s).toString(), u"puppet"_s);
    QCOMPARE(item.toJson().value(u"puppet"_s).toObject().value(u"backend"_s).toString(), u"open"_s);
    QVERIFY(!item.toJson().value(u"variants"_s).toArray().isEmpty());
}

void TestLocalPuppets::aCubismPuppetIsListedAndMissingItsRuntime()
{
    QTemporaryDir dir;
    const QString folder = QFileInfo(cubismFolder(dir)).canonicalFilePath();
    const WallpaperItem item = LocalPuppetImporter::scanItem(folder);
    QCOMPARE(item.status, ItemStatus::MissingDependency);
    QCOMPARE(item.statusDetail, u"puppet: needs the Cubism runtime"_s);
    QCOMPARE(item.title, u"kei"_s);
    QStringList names;
    for (const PetVariant &v : item.variants)
        names << v.name;
    QCOMPARE(names, (QStringList{u"model"_s, u"motion:Idle"_s, u"motion:TapBody"_s, u"expression:smile"_s}));
    const QJsonObject p = item.puppet;
    QCOMPARE(p.value(u"backend"_s).toString(), u"cubism"_s);
    QCOMPARE(p.value(u"motionGroups"_s).toArray().size(), 2);
    QCOMPARE(p.value(u"motionGroups"_s).toArray().at(1).toObject().value(u"motions"_s).toInt(), 2);
    QCOMPARE(p.value(u"expressions"_s).toArray().at(0).toString(), u"smile"_s);
    QCOMPARE(p.value(u"hitAreas"_s).toArray().at(0).toString(), u"Body"_s);
}

void TestLocalPuppets::theCheckAnswerIsMerged()
{
    QTemporaryDir dir;
    const QString folder = QFileInfo(openFolder(dir)).canonicalFilePath();
    QJsonObject check{{u"ok"_s, true}, {u"parameters"_s, 12}, {u"drawables"_s, 30}, {u"textures"_s, 2}, {u"physics"_s, 1}, {u"canvas"_s, QJsonArray{800, 1200}},
                      {u"reasons"_s, QJsonArray()}, {u"degraded"_s, QJsonArray{u"puppet: physics rig skipped: Hair"_s}}};
    WallpaperItem item = LocalPuppetImporter::scanItem(folder, check);
    QCOMPARE(item.status, ItemStatus::Ok);
    QCOMPARE(item.variants[0].width, 800);
    QCOMPARE(item.variants[0].height, 1200);
    QCOMPARE(item.puppet.value(u"checked"_s).toBool(), true);
    QCOMPARE(item.puppet.value(u"parameters"_s).toInt(), 12);
    QCOMPARE(item.puppet.value(u"degraded"_s).toArray().at(0).toString(), u"puppet: physics rig skipped: Hair"_s);
    // a refusal makes the item invalid, with the first reason
    check = QJsonObject{{u"ok"_s, false}, {u"reasons"_s, QJsonArray{u"puppet: too large: kMaxVertices"_s}}, {u"degraded"_s, QJsonArray()}};
    item = LocalPuppetImporter::scanItem(folder, check);
    QCOMPARE(item.status, ItemStatus::Invalid);
    QCOMPARE(item.statusDetail, u"puppet: too large: kMaxVertices"_s);
    QVERIFY(item.variants.isEmpty());
    // nonsense sizes are not taken
    check = QJsonObject{{u"ok"_s, true}, {u"canvas"_s, QJsonArray{-5, 0}}};
    item = LocalPuppetImporter::scanItem(folder, check);
    QCOMPARE(item.variants[0].width, 512);
}

void TestLocalPuppets::aVanishedFolderIsInvalidNotGone()
{
    QTemporaryDir dir;
    const WallpaperItem item = LocalPuppetImporter::scanItem(dir.filePath(u"gone"_s));
    QCOMPARE(item.status, ItemStatus::Invalid);
    QVERIFY(item.statusDetail.contains(u"gone"_s));
    QVERIFY(item.variants.isEmpty());
    QCOMPARE(item.format, u"puppet"_s);
}

void TestLocalPuppets::theStoreRemovesByPuppetId()
{
    QTemporaryDir dir;
    LocalPetStore store(dir.filePath(u"local-puppets.json"_s));
    store.setItemIdFunction(&LocalPuppetImporter::itemIdFor);
    store.load();
    QString error;
    QVERIFY(store.add(u"/p/one"_s, &error));
    QVERIFY(store.add(u"/p/two"_s, &error));
    QVERIFY(!store.remove(LocalPetImporter::itemIdFor(u"/p/one"_s), &error)); // a pet id does not remove a puppet
    QCOMPARE(store.files().size(), 2);
    QCOMPARE(store.remove(LocalPuppetImporter::itemIdFor(u"/p/one"_s), &error).value_or(QString()), u"/p/one"_s);
    QCOMPARE(store.files(), QStringList{u"/p/two"_s});
    LocalPetStore again(dir.filePath(u"local-puppets.json"_s)); // (saved)
    again.load();
    QCOMPARE(again.files(), QStringList{u"/p/two"_s});
}

void TestLocalPuppets::whatTheCheckerSaysIsCleaned()
{
    QTemporaryDir dir;
    const QString folder = QFileInfo(openFolder(dir)).canonicalFilePath();
    QJsonArray many;
    for (int i = 0; i < 100; ++i)
        many.append(QString(5000, QLatin1Char('x')));
    many.append(12); // not a string
    QJsonObject check{{u"ok"_s, true}, {u"parameters"_s, 5e9}, {u"drawables"_s, -3}, {u"textures"_s, 1.5}, {u"physics"_s, u"many"_s}, {u"canvas"_s, QJsonArray{2000000000, 2000000000}},
                      {u"reasons"_s, many}, {u"degraded"_s, many}};
    const WallpaperItem item = LocalPuppetImporter::scanItem(folder, check);
    QCOMPARE(item.variants[0].width, 512); // a canvas no picture can be is not taken
    const QJsonObject p = item.puppet;
    for (const char *key : {"parameters", "drawables", "physics"})
        QVERIFY2(p.value(QLatin1String(key)).isNull(), key);
    QCOMPARE(p.value(u"textures"_s).toInt(), 0); // (the count the manifest gave)
    QCOMPARE(p.value(u"degraded"_s).toArray().size(), 16);
    QCOMPARE(p.value(u"degraded"_s).toArray().at(0).toString().size(), 200);
    QCOMPARE(p.value(u"reasons"_s).toArray().size(), 16);
}

void TestLocalPuppets::aChangedFolderIsReadAgain()
{
    // the manifest is cached by the folder's top level; a texture that goes, or a file that arrives, is seen
    QTemporaryDir dir;
    const QString folder = QFileInfo(cubismFolder(dir)).canonicalFilePath();
    QCOMPARE(LocalPuppetImporter::scanItem(folder).status, ItemStatus::MissingDependency);
    QVERIFY(QFile::remove(folder + u"/t.png"_s));
    const WallpaperItem broken = LocalPuppetImporter::scanItem(folder);
    QCOMPARE(broken.status, ItemStatus::Invalid);
    QVERIFY2(broken.statusDetail.contains(u"puppet: file missing"_s), qPrintable(broken.statusDetail));
    testutil::writeFile(folder + u"/t.png"_s, "x");
    QCOMPARE(LocalPuppetImporter::scanItem(folder).status, ItemStatus::MissingDependency);
}

void TestLocalPuppets::theLibraryListsPuppetsBesidePets()
{
    QTemporaryDir dir;
    const QString puppet = QFileInfo(openFolder(dir)).canonicalFilePath();
    Library library;
    library.setLocalPuppetFolders({puppet});
    QJsonObject answer{{u"ok"_s, true}, {u"canvas"_s, QJsonArray{300, 600}}, {u"reasons"_s, QJsonArray()}, {u"degraded"_s, QJsonArray()}};
    int asked = 0;
    library.setPuppetCheckProvider([&](const QString &folder) {
        ++asked;
        return folder == puppet ? answer : QJsonObject();
    });
    library.rescan();
    const std::optional<WallpaperItem> item = library.item(LocalPuppetImporter::itemIdFor(puppet));
    QVERIFY(item);
    QCOMPARE(item->variants[0].width, 300);
    QVERIFY(asked >= 1);
    // a changed answer is a changed item
    answer.insert(u"canvas"_s, QJsonArray{400, 400});
    library.rescan();
    QCOMPARE(library.item(LocalPuppetImporter::itemIdFor(puppet))->variants[0].width, 400);
}

QTEST_GUILESS_MAIN(TestLocalPuppets)
#include "tst_localpuppets.moc"
