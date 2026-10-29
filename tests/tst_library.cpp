#include "animfixtures.h"
#include "library.h"
#include "testutil.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestLibrary : public QObject
{
    Q_OBJECT

private slots:
    void firstScanReportsAllAdded();
    void rescanWithoutChangesIsSilent();
    void reportsChangedAndRemoved();
    void itemsAreSortedByTitleAndLookupWorks();
    void duplicateIdAcrossDirsFirstDirWins();
    void duplicateIdPrefersOkOverInvalid();
    void changingDirsReportsOldItemsRemoved();
    void missingDirYieldsNothingThenPicksUpLaterItems();
    void mergesWallpaperAndPetItems();
    void petChangesAreReported();
    void duplicatePetIdAcrossAnimaDirsFirstDirWins();
    void missingAnimaDirYieldsNothing();
    void petDecoratorFillsMetadataAndRescanReportsChange();
};

void TestLibrary::firstScanReportsAllAdded()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"2"_s, u"Beta"_s);
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha"_s);
    Library library;
    library.setWorkshopDirs({tmp.path()});
    QSignalSpy spy(&library, &Library::itemsChanged);

    library.rescan();

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toStringList(), QStringList({u"we:1"_s, u"we:2"_s}));
    QVERIFY(spy.at(0).at(1).toStringList().isEmpty());
    QVERIFY(spy.at(0).at(2).toStringList().isEmpty());
}

void TestLibrary::rescanWithoutChangesIsSilent()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha"_s);
    Library library;
    library.setWorkshopDirs({tmp.path()});
    library.rescan();
    QSignalSpy spy(&library, &Library::itemsChanged);

    library.rescan();

    QCOMPARE(spy.count(), 0);
}

void TestLibrary::reportsChangedAndRemoved()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha"_s);
    const QString second = testutil::writeVideoItem(tmp.path(), u"2"_s, u"Beta"_s);
    Library library;
    library.setWorkshopDirs({tmp.path()});
    library.rescan();
    QSignalSpy spy(&library, &Library::itemsChanged);

    testutil::writeVideoItem(tmp.path(), u"1"_s, u"Alpha Remastered"_s);
    QVERIFY(QDir(second).removeRecursively());
    library.rescan();

    QCOMPARE(spy.count(), 1);
    QVERIFY(spy.at(0).at(0).toStringList().isEmpty());
    QCOMPARE(spy.at(0).at(1).toStringList(), QStringList({u"we:1"_s}));
    QCOMPARE(spy.at(0).at(2).toStringList(), QStringList({u"we:2"_s}));
}

void TestLibrary::itemsAreSortedByTitleAndLookupWorks()
{
    QTemporaryDir tmp;
    testutil::writeVideoItem(tmp.path(), u"1"_s, u"zebra"_s);
    testutil::writeVideoItem(tmp.path(), u"2"_s, u"Apple"_s);
    Library library;
    library.setWorkshopDirs({tmp.path()});
    library.rescan();

    const QList<WallpaperItem> items = library.items();
    QCOMPARE(items.size(), 2);
    QCOMPARE(items[0].title, u"Apple"_s);
    QCOMPARE(library.item(u"we:1"_s)->title, u"zebra"_s);
    QVERIFY(!library.item(u"we:404"_s).has_value());
}

void TestLibrary::duplicateIdAcrossDirsFirstDirWins()
{
    QTemporaryDir a;
    QTemporaryDir b;
    testutil::writeVideoItem(a.path(), u"1"_s, u"From A"_s);
    testutil::writeVideoItem(b.path(), u"1"_s, u"From B"_s);
    testutil::writeVideoItem(b.path(), u"2"_s, u"Only B"_s);
    Library library;
    library.setWorkshopDirs({a.path(), b.path()});
    QSignalSpy spy(&library, &Library::itemsChanged);

    library.rescan();

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toStringList(), QStringList({u"we:1"_s, u"we:2"_s}));
    QCOMPARE(library.items().size(), 2);
    QCOMPARE(library.item(u"we:1"_s)->title, u"From A"_s);

    library.rescan();
    QCOMPARE(spy.count(), 1);
}

void TestLibrary::duplicateIdPrefersOkOverInvalid()
{
    QTemporaryDir a;
    QTemporaryDir b;
    const QString broken = testutil::writeVideoItem(a.path(), u"1"_s, u"Broken"_s);
    QVERIFY(QFile::remove(broken + u"/clip.mp4"_s));
    testutil::writeVideoItem(b.path(), u"1"_s, u"Good"_s);
    Library library;
    library.setWorkshopDirs({a.path(), b.path()});

    library.rescan();

    QCOMPARE(library.items().size(), 1);
    QCOMPARE(library.item(u"we:1"_s)->title, u"Good"_s);
    QCOMPARE(library.item(u"we:1"_s)->status, ItemStatus::Ok);
}

void TestLibrary::changingDirsReportsOldItemsRemoved()
{
    QTemporaryDir a;
    QTemporaryDir b;
    testutil::writeVideoItem(a.path(), u"1"_s, u"Alpha"_s);
    testutil::writeVideoItem(b.path(), u"2"_s, u"Beta"_s);
    Library library;
    library.setWorkshopDirs({a.path()});
    library.rescan();
    QSignalSpy spy(&library, &Library::itemsChanged);

    library.setWorkshopDirs({b.path()});
    library.rescan();

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toStringList(), QStringList({u"we:2"_s}));
    QVERIFY(spy.at(0).at(1).toStringList().isEmpty());
    QCOMPARE(spy.at(0).at(2).toStringList(), QStringList({u"we:1"_s}));
}

void TestLibrary::mergesWallpaperAndPetItems()
{
    QTemporaryDir we;
    QTemporaryDir anima;
    testutil::writeVideoItem(we.path(), u"1"_s, u"Zeta"_s);
    testutil::writeFile(anima.filePath(u"2/pet.apng"_s), animfixtures::apng(32, 16, {{1, 60}, {1, 60}}));
    Library library;
    library.setWorkshopDirs({we.path()});
    library.setAnimaDirs({anima.path()});
    QSignalSpy spy(&library, &Library::itemsChanged);

    library.rescan();

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toStringList(), QStringList({u"anima:2"_s, u"we:1"_s}));
    const auto pet = library.item(u"anima:2"_s);
    QVERIFY(pet);
    QCOMPARE(pet->kind, ItemKind::Pet);
    QCOMPARE(pet->variants.size(), 1);
    QCOMPARE(pet->variants.at(0).width, 32);
    QCOMPARE(library.item(u"we:1"_s)->kind, ItemKind::Video);
    // Sorted by title like every other item: the pet's title is its folder name until metadata arrives.
    const QList<WallpaperItem> all = library.items();
    QCOMPARE(all.size(), 2);
    QCOMPARE(all.at(0).id, u"anima:2"_s);
    QCOMPARE(all.at(1).id, u"we:1"_s);
    QVERIFY(spy.at(0).at(1).toStringList().isEmpty());
    QVERIFY(spy.at(0).at(2).toStringList().isEmpty());

    library.rescan();
    QCOMPARE(spy.count(), 1); // unchanged: silent
}

void TestLibrary::petChangesAreReported()
{
    QTemporaryDir anima;
    const QString file = anima.filePath(u"2/pet.apng"_s);
    testutil::writeFile(file, animfixtures::apng(32, 16, {{1, 60}, {1, 60}}));
    testutil::writeFile(anima.filePath(u"3/pet.gif"_s), animfixtures::gif(8, 8, {10, 10}));
    Library library;
    library.setAnimaDirs({anima.path()});
    library.rescan();
    QSignalSpy spy(&library, &Library::itemsChanged);

    // A second variant lands in item 2 (e.g. Steam finished a download); item 3 is deleted; item 4 appears.
    testutil::writeFile(anima.filePath(u"2/second.apng"_s), animfixtures::apng(8, 8, {{1, 30}, {1, 30}}));
    QVERIFY(QDir(anima.filePath(u"3"_s)).removeRecursively());
    testutil::writeFile(anima.filePath(u"4/pet.apng"_s), animfixtures::apng(8, 8, {{1, 30}, {1, 30}}));
    library.rescan();

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toStringList(), QStringList({u"anima:4"_s}));
    QCOMPARE(spy.at(0).at(1).toStringList(), QStringList({u"anima:2"_s}));
    QCOMPARE(spy.at(0).at(2).toStringList(), QStringList({u"anima:3"_s}));
    QCOMPARE(library.item(u"anima:2"_s)->variants.size(), 2);
}

void TestLibrary::duplicatePetIdAcrossAnimaDirsFirstDirWins()
{
    QTemporaryDir first;
    QTemporaryDir second;
    testutil::writeFile(first.filePath(u"5/a.apng"_s), animfixtures::apng(10, 10, {{1, 10}, {1, 10}}));
    testutil::writeFile(second.filePath(u"5/a.apng"_s), animfixtures::apng(20, 20, {{1, 10}, {1, 10}}));
    Library library;
    library.setAnimaDirs({first.path(), second.path()});
    library.rescan();
    QCOMPARE(library.items().size(), 1);
    QCOMPARE(library.item(u"anima:5"_s)->variants.at(0).width, 10);

    // A broken copy in the first library yields to a playable one in the second.
    testutil::writeFile(first.filePath(u"5/a.apng"_s), "broken");
    library.rescan();
    QCOMPARE(library.item(u"anima:5"_s)->status, ItemStatus::Ok);
    QCOMPARE(library.item(u"anima:5"_s)->variants.at(0).width, 20);
}

void TestLibrary::missingAnimaDirYieldsNothing()
{
    QTemporaryDir tmp;
    Library library;
    library.setAnimaDirs({tmp.filePath(u"nope"_s)});
    QSignalSpy spy(&library, &Library::itemsChanged);
    library.rescan();
    QCOMPARE(spy.count(), 0);
    QVERIFY(library.items().isEmpty());
}

void TestLibrary::petDecoratorFillsMetadataAndRescanReportsChange()
{
    QTemporaryDir we;
    QTemporaryDir anima;
    testutil::writeVideoItem(we.path(), u"1"_s, u"Alpha"_s);
    testutil::writeFile(anima.filePath(u"77/pet.apng"_s), animfixtures::apng(32, 16, {{1, 60}, {1, 60}}));
    Library library;
    library.setWorkshopDirs({we.path()});
    library.setAnimaDirs({anima.path()});
    QString title; // what the decorator knows; empty until the metadata arrives
    int calls = 0;
    library.setPetDecorator([&](WallpaperItem &item) {
        ++calls;
        if (!title.isEmpty()) {
            item.title = title;
            item.tags = {u"Pet"_s};
            item.previewPath = u"/cache/previews/77.jpg"_s;
        }
    });
    library.rescan();
    QCOMPARE(calls, 1); // pets only, not the wallpaper
    QCOMPARE(library.item(u"anima:77"_s)->title, u"77"_s);

    QSignalSpy spy(&library, &Library::itemsChanged);
    title = u"Teto"_s;
    library.rescan();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(1).toStringList(), QStringList({u"anima:77"_s}));
    const auto pet = library.item(u"anima:77"_s);
    QCOMPARE(pet->title, u"Teto"_s);
    QCOMPARE(pet->tags, QStringList({u"Pet"_s}));
    QCOMPARE(pet->previewPath, u"/cache/previews/77.jpg"_s);
    library.rescan();
    QCOMPARE(spy.count(), 1); // stable afterwards
}

QTEST_GUILESS_MAIN(TestLibrary)
void TestLibrary::missingDirYieldsNothingThenPicksUpLaterItems()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"workshop/content/431960"_s);
    Library library;
    library.setWorkshopDirs({dir});
    QSignalSpy spy(&library, &Library::itemsChanged);

    library.rescan();
    QVERIFY(library.items().isEmpty());
    QCOMPARE(spy.count(), 0);

    testutil::writeVideoItem(dir, u"7"_s, u"Late"_s);
    library.rescan();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toStringList(), QStringList({u"we:7"_s}));
}

#include "tst_library.moc"
