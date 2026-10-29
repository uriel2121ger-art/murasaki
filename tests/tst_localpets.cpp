#include "animfixtures.h"
#include "library.h"
#include "localpets.h"
#include "testutil.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestLocalPets : public QObject
{
    Q_OBJECT

private slots:
    void validateAcceptsAnimationsOfEveryFormat();
    void validateRejectsEverythingElse();
    void validateResolvesSymlinks();
    void scanItemDescribesTheFile();
    void scanItemOfAVanishedFileIsInvalid();
    void idIsStableAndDependsOnThePath();
    void storeAddRemovePersist();
    void storeQuarantinesBadFiles();
    void blockedOrFailedSavesFailTheCall();
    void validateRefusesHugeFiles();
    void libraryListsLocalPets();
};

void TestLocalPets::validateAcceptsAnimationsOfEveryFormat()
{
    QTemporaryDir tmp;
    testutil::writeFile(tmp.filePath(u"a.apng"_s), animfixtures::apng(8, 8, {{1, 10}, {1, 10}}));
    testutil::writeFile(tmp.filePath(u"b.gif"_s), animfixtures::gif(8, 8, {5, 5}));
    testutil::writeFile(tmp.filePath(u"c.WEBP"_s), animfixtures::webpAnimated(8, 8, {50, 50}));
    testutil::writeFile(tmp.filePath(u"d.png"_s), animfixtures::apng(8, 8, {{1, 10}, {1, 10}})); // an APNG named .png
    for (const QString &name : {u"a.apng"_s, u"b.gif"_s, u"c.WEBP"_s, u"d.png"_s}) {
        QString error;
        QCOMPARE(LocalPetImporter::validate(tmp.filePath(name), &error), testutil::canonical(tmp.filePath(name)));
        QVERIFY(error.isEmpty());
    }
}

void TestLocalPets::validateRejectsEverythingElse()
{
    QTemporaryDir tmp;
    testutil::writeFile(tmp.filePath(u"notes.txt"_s), "hi");
    testutil::writeFile(tmp.filePath(u"broken.gif"_s), "GIF89a");
    testutil::writeFile(tmp.filePath(u"movie.webm"_s), "x");
    testutil::writeFile(tmp.filePath(u"still.png"_s), animfixtures::apng(8, 8, {})); // a picture, not an animation
    QDir().mkpath(tmp.filePath(u"dir.gif"_s));
    for (const QString &path : {QString(), u"x.gif"_s, tmp.filePath(u"missing.gif"_s), tmp.filePath(u"notes.txt"_s),
                                tmp.filePath(u"broken.gif"_s), tmp.filePath(u"movie.webm"_s), tmp.filePath(u"still.png"_s), tmp.filePath(u"dir.gif"_s)}) {
        QString error;
        QVERIFY2(LocalPetImporter::validate(path, &error).isEmpty(), qPrintable(path));
        QVERIFY2(!error.isEmpty(), qPrintable(path));
    }
}

void TestLocalPets::validateResolvesSymlinks()
{
    QTemporaryDir tmp;
    testutil::writeFile(tmp.filePath(u"real/pet.gif"_s), animfixtures::gif(8, 8, {5, 5}));
    QVERIFY(QFile::link(tmp.filePath(u"real/pet.gif"_s), tmp.filePath(u"link.gif"_s)));
    QCOMPARE(LocalPetImporter::validate(tmp.filePath(u"link.gif"_s)), testutil::canonical(tmp.filePath(u"real/pet.gif"_s)));
}

void TestLocalPets::scanItemDescribesTheFile()
{
    QTemporaryDir tmp;
    const QString file = tmp.filePath(u"Little Cat.apng"_s);
    testutil::writeFile(file, animfixtures::apng(40, 30, {{1, 10}, {1, 10}, {1, 10}}));
    testutil::writeFile(tmp.filePath(u"neighbour.exe"_s), "MZ"); // the folder is not the pet's

    const WallpaperItem item = LocalPetImporter::scanItem(testutil::canonical(file));
    QCOMPARE(item.kind, ItemKind::Pet);
    QCOMPARE(item.source, u"local"_s);
    QCOMPARE(item.status, ItemStatus::Ok);
    QCOMPARE(item.title, u"Little Cat"_s);
    QCOMPARE(item.dir, testutil::canonical(tmp.path()));
    QVERIFY(item.workshopId.isEmpty());
    QCOMPARE(item.variants.size(), 1);
    QCOMPARE(item.variants.at(0).name, u"Little Cat"_s);
    QCOMPARE(item.variants.at(0).width, 40);
    QCOMPARE(item.variants.at(0).height, 30);
    QCOMPARE(item.variants.at(0).frames, 3);
    QCOMPARE(item.variants.at(0).file, testutil::canonical(file));
    QCOMPARE(item.variants.at(0).bytes, QFileInfo(file).size());
    QVERIFY(item.risks.isEmpty());
    QCOMPARE(item.toSummaryJson().value(u"variantCount"_s).toInt(), 1);
}

void TestLocalPets::scanItemOfAVanishedFileIsInvalid()
{
    QTemporaryDir tmp;
    const WallpaperItem item = LocalPetImporter::scanItem(tmp.filePath(u"gone.gif"_s));
    QCOMPARE(item.status, ItemStatus::Invalid);
    QVERIFY(item.statusDetail.contains(u"gone.gif"_s));
    QVERIFY(item.variants.isEmpty());
    QCOMPARE(item.id, LocalPetImporter::itemIdFor(tmp.filePath(u"gone.gif"_s)));
}

void TestLocalPets::idIsStableAndDependsOnThePath()
{
    const QString a = LocalPetImporter::itemIdFor(u"/x/a.gif"_s);
    QVERIFY(a.startsWith(u"local:"_s));
    QCOMPARE(a.size(), 6 + 40);
    QCOMPARE(LocalPetImporter::itemIdFor(u"/x/a.gif"_s), a);
    QVERIFY(LocalPetImporter::itemIdFor(u"/x/b.gif"_s) != a);
}

void TestLocalPets::storeAddRemovePersist()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"config/local-pets.json"_s);
    LocalPetStore store(path);
    store.load();
    QVERIFY(store.files().isEmpty());
    QSignalSpy changed(&store, &LocalPetStore::changed);

    QVERIFY(store.add(u"/a/one.gif"_s));
    QVERIFY(store.add(u"/a/two.gif"_s));
    QVERIFY(!store.add(u"/a/one.gif"_s)); // already there: no signal
    QCOMPARE(changed.count(), 2);

    LocalPetStore again(path);
    again.load();
    QCOMPARE(again.files(), (QStringList{u"/a/one.gif"_s, u"/a/two.gif"_s}));

    QCOMPARE(store.remove(LocalPetImporter::itemIdFor(u"/a/one.gif"_s)), std::optional<QString>(u"/a/one.gif"_s));
    QVERIFY(!store.remove(u"local:nope"_s));
    QCOMPARE(changed.count(), 3);
    LocalPetStore third(path);
    third.load();
    QCOMPARE(third.files(), QStringList{u"/a/two.gif"_s});
}

void TestLocalPets::storeQuarantinesBadFiles()
{
    const auto backups = [](const QTemporaryDir &tmp) {
        return QDir(tmp.path()).entryList({u"local-pets.json.bad-*"_s}, QDir::Files);
    };
    for (const QByteArray &content : {QByteArray("not json"), QByteArray(R"({"version":2,"files":[]})"),
                                      QByteArray(R"({"version":1,"files":["relative.gif"]})"),
                                      QByteArray(R"({"version":1,"files":[7]})")}) {
        QTemporaryDir tmp;
        testutil::writeFile(tmp.filePath(u"local-pets.json"_s), content);
        LocalPetStore store(tmp.filePath(u"local-pets.json"_s));
        store.load();
        QVERIFY(store.files().isEmpty());
        QCOMPARE(backups(tmp).size(), 1);
        QVERIFY(!QFile::exists(tmp.filePath(u"local-pets.json"_s)));
    }
}

void TestLocalPets::blockedOrFailedSavesFailTheCall()
{
    // A bad file that can not be renamed (read-only dir) keeps saving disabled: add and remove must say so.
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"cfg"_s);
    testutil::writeFile(dir + u"/local-pets.json"_s, "not json");
    QVERIFY(QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    const auto restore = qScopeGuard([&] { QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner); });
    if (QFileInfo(dir).isWritable())
        QSKIP("directory permissions are not enforced (running as root?)");
    LocalPetStore store(dir + u"/local-pets.json"_s);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    store.load();
    QSignalSpy changed(&store, &LocalPetStore::changed);
    QString error;
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"murasakid:"_s));
    QVERIFY(!store.add(u"/a/one.gif"_s, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(store.files().isEmpty());
    QCOMPARE(changed.size(), 0);
}

void TestLocalPets::validateRefusesHugeFiles()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"huge.gif"_s);
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.resize(kMaxLocalPetBytes + 1)); // sparse
    file.close();
    QString error;
    QVERIFY(LocalPetImporter::validate(path, &error).isEmpty());
    QVERIFY2(error.contains(u"too large"_s), qPrintable(error));
}

void TestLocalPets::libraryListsLocalPets()
{
    QTemporaryDir tmp;
    const QString file = tmp.filePath(u"pet.gif"_s);
    testutil::writeFile(file, animfixtures::gif(8, 8, {5, 5}));
    Library library;
    library.setLocalPetFiles({testutil::canonical(file), tmp.filePath(u"gone.webp"_s)});
    QSignalSpy changed(&library, &Library::itemsChanged);
    library.rescan();
    QCOMPARE(library.items().size(), 2);
    QCOMPARE(changed.at(0).at(0).toStringList().size(), 2);
    const auto ok = library.item(LocalPetImporter::itemIdFor(testutil::canonical(file)));
    QVERIFY(ok);
    QCOMPARE(ok->status, ItemStatus::Ok);
    const auto gone = library.item(LocalPetImporter::itemIdFor(tmp.filePath(u"gone.webp"_s)));
    QVERIFY(gone);
    QCOMPARE(gone->status, ItemStatus::Invalid);

    // The file turning into garbage is a change of the item.
    testutil::writeFile(file, "junk");
    library.rescan();
    QCOMPARE(changed.size(), 2);
    QCOMPARE(changed.at(1).at(1).toStringList(), QStringList{ok->id});
}

QTEST_GUILESS_MAIN(TestLocalPets)
#include "tst_localpets.moc"
