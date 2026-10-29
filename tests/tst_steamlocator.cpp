#include "steamlocator.h"
#include "testutil.h"

#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestSteamLocator : public QObject
{
    Q_OBJECT

private slots:
    void findsLibrariesFromLibraryFolders();
    void acceptsLegacyLibraryFoldersFormat();
    void dedupesSymlinkedRoots();
    void resolvesAppInstallDirAcrossLibraries();
    void returnsEmptyForMissingApp();
    void findsWorkshopDirAndManifest();
    void rejectsInstallDirTraversal();
    void rejectsMissingInstallDir();
    void followsSymlinkedInstallDir();
    void rejectsInvalidAppIds();
    void keepsNumericLibraryOrder();
    void workshopCandidatesCoverEveryLibrary();
    void workshopCandidatesEmptyForInvalidAppId();
    void survivesGarbledLibraryFolders();
};

void TestSteamLocator::findsLibrariesFromLibraryFolders()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"root"_s), second = tmp.filePath(u"games"_s);
    QVERIFY(QDir().mkpath(second + u"/steamapps"_s));
    testutil::writeFile(root + u"/steamapps/libraryfolders.vdf"_s,
                        QStringLiteral(R"("libraryfolders" {
  "0" { "path" "%1" }
  "1" { "path" "%2" }
  "2" { "path" "%3" }
})").arg(root, second, tmp.filePath(u"missing"_s)).toUtf8());

    const SteamLocator steam({root});
    QCOMPARE(steam.libraries(), QStringList({testutil::canonical(root), testutil::canonical(second)}));
}

void TestSteamLocator::acceptsLegacyLibraryFoldersFormat()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"root"_s), second = tmp.filePath(u"games"_s);
    QVERIFY(QDir().mkpath(second + u"/steamapps"_s));
    testutil::writeFile(root + u"/steamapps/libraryfolders.vdf"_s,
                        QStringLiteral(R"("LibraryFolders" { "TimeNextStatsReport" "123" "1" "%1" })")
                            .arg(second).toUtf8());

    const SteamLocator steam({root});
    QCOMPARE(steam.libraries(), QStringList({testutil::canonical(root), testutil::canonical(second)}));
}

void TestSteamLocator::dedupesSymlinkedRoots()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"real"_s), link = tmp.filePath(u"link"_s);
    QVERIFY(QDir().mkpath(root + u"/steamapps"_s));
    QVERIFY(QFile::link(root, link));

    const SteamLocator steam({link, root});
    QCOMPARE(steam.libraries(), QStringList({testutil::canonical(root)}));
}

void TestSteamLocator::resolvesAppInstallDirAcrossLibraries()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"root"_s), second = tmp.filePath(u"games"_s);
    testutil::writeFile(root + u"/steamapps/libraryfolders.vdf"_s,
                        QStringLiteral(R"("libraryfolders" { "1" { "path" "%1" } })").arg(second).toUtf8());
    testutil::writeFile(second + u"/steamapps/appmanifest_431960.acf"_s,
                        R"("AppState" { "appid" "431960" "installdir" "wallpaper_engine" })");
    QVERIFY(QDir().mkpath(second + u"/steamapps/common/wallpaper_engine"_s));

    const SteamLocator steam({root});
    QCOMPARE(steam.appInstallDir(u"431960"_s),
             testutil::canonical(second) + u"/steamapps/common/wallpaper_engine"_s);
}

void TestSteamLocator::returnsEmptyForMissingApp()
{
    QTemporaryDir tmp;
    QVERIFY(QDir().mkpath(tmp.filePath(u"root/steamapps"_s)));
    const SteamLocator steam({tmp.filePath(u"root"_s)});
    QVERIFY(steam.appInstallDir(u"431960"_s).isEmpty());
    QVERIFY(steam.workshopDir(u"431960"_s).isEmpty());
    QVERIFY(steam.workshopManifest(u"431960"_s).isEmpty());
    QVERIFY(SteamLocator({tmp.filePath(u"nowhere"_s)}).libraries().isEmpty());
}

void TestSteamLocator::findsWorkshopDirAndManifest()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"root"_s);
    QVERIFY(QDir().mkpath(root + u"/steamapps/workshop/content/431960/123"_s));
    testutil::writeFile(root + u"/steamapps/workshop/appworkshop_431960.acf"_s, R"("AppWorkshop" {})");

    const SteamLocator steam({root});
    const QString lib = testutil::canonical(root);
    QCOMPARE(steam.workshopDir(u"431960"_s), lib + u"/steamapps/workshop/content/431960"_s);
    QCOMPARE(steam.workshopManifest(u"431960"_s), lib + u"/steamapps/workshop/appworkshop_431960.acf"_s);
}

void TestSteamLocator::rejectsInstallDirTraversal()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"root"_s);
    QVERIFY(QDir().mkpath(root + u"/evil"_s));
    QVERIFY(QDir().mkpath(root + u"/steamapps/common"_s));
    testutil::writeFile(root + u"/steamapps/appmanifest_1.acf"_s,
                        "\"AppState\" { \"installdir\" \"../../evil\" }");
    const SteamLocator steam({root});
    QVERIFY(steam.appInstallDir(u"1"_s).isEmpty());
}

void TestSteamLocator::rejectsMissingInstallDir()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"root"_s);
    QVERIFY(QDir().mkpath(root + u"/steamapps/common"_s));
    testutil::writeFile(root + u"/steamapps/appmanifest_1.acf"_s,
                        "\"AppState\" { \"installdir\" \"gone\" }");
    const SteamLocator steam({root});
    QVERIFY(steam.appInstallDir(u"1"_s).isEmpty());
}

void TestSteamLocator::followsSymlinkedInstallDir()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"root"_s), elsewhere = tmp.filePath(u"otherdisk/game"_s);
    QVERIFY(QDir().mkpath(root + u"/steamapps/common"_s));
    QVERIFY(QDir().mkpath(elsewhere));
    QVERIFY(QFile::link(elsewhere, root + u"/steamapps/common/moved"_s));
    testutil::writeFile(root + u"/steamapps/appmanifest_1.acf"_s,
                        "\"AppState\" { \"installdir\" \"moved\" }");
    const SteamLocator steam({root});
    QCOMPARE(steam.appInstallDir(u"1"_s), testutil::canonical(elsewhere));
}

void TestSteamLocator::rejectsInvalidAppIds()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"root"_s);
    QVERIFY(QDir().mkpath(root + u"/x"_s));
    QVERIFY(QDir().mkpath(root + u"/steamapps/workshop/content"_s));
    const SteamLocator steam({root});
    for (const QString &bad : {u"../x"_s, u""_s, u"12a"_s}) {
        QVERIFY(steam.appInstallDir(bad).isEmpty());
        QVERIFY(steam.workshopDir(bad).isEmpty());
        QVERIFY(steam.workshopManifest(bad).isEmpty());
    }
}

void TestSteamLocator::keepsNumericLibraryOrder()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"root"_s), two = tmp.filePath(u"two"_s), ten = tmp.filePath(u"ten"_s);
    QVERIFY(QDir().mkpath(two + u"/steamapps"_s));
    QVERIFY(QDir().mkpath(ten + u"/steamapps"_s));
    testutil::writeFile(root + u"/steamapps/libraryfolders.vdf"_s,
                        QStringLiteral("\"libraryfolders\" { \"10\" { \"path\" \"%1\" } \"2\" { \"path\" \"%2\" } }")
                            .arg(ten, two).toUtf8());
    const SteamLocator steam({root});
    QCOMPARE(steam.libraries(),
             QStringList({testutil::canonical(root), testutil::canonical(two), testutil::canonical(ten)}));
}

void TestSteamLocator::survivesGarbledLibraryFolders()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"root"_s);
    testutil::writeFile(root + u"/steamapps/libraryfolders.vdf"_s, "\"libraryfolders\" { \"0\" { \"path");
    const SteamLocator steam({root});
    QCOMPARE(steam.libraries(), QStringList({testutil::canonical(root)}));
}

void TestSteamLocator::workshopCandidatesCoverEveryLibrary()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"root"_s), second = tmp.filePath(u"games"_s);
    QVERIFY(QDir().mkpath(second + u"/steamapps"_s));
    testutil::writeFile(root + u"/steamapps/libraryfolders.vdf"_s,
                        QStringLiteral(R"("libraryfolders" { "0" { "path" "%1" } "1" { "path" "%2" } })")
                            .arg(root, second).toUtf8());

    const SteamLocator steam({root});
    const QString a = testutil::canonical(root), b = testutil::canonical(second);
    QVERIFY(steam.workshopDir(u"431960"_s).isEmpty());
    QCOMPARE(steam.workshopDirCandidates(u"431960"_s),
             QStringList({a + u"/steamapps/workshop/content/431960"_s, b + u"/steamapps/workshop/content/431960"_s}));
    QCOMPARE(steam.workshopManifestCandidates(u"431960"_s),
             QStringList({a + u"/steamapps/workshop/appworkshop_431960.acf"_s,
                          b + u"/steamapps/workshop/appworkshop_431960.acf"_s}));
}

void TestSteamLocator::workshopCandidatesEmptyForInvalidAppId()
{
    QTemporaryDir tmp;
    QVERIFY(QDir().mkpath(tmp.filePath(u"root/steamapps"_s)));
    const SteamLocator steam({tmp.filePath(u"root"_s)});
    for (const QString &id : {QString(), u"../x"_s, u"43a"_s}) {
        QVERIFY(steam.workshopDirCandidates(id).isEmpty());
        QVERIFY(steam.workshopManifestCandidates(id).isEmpty());
    }
}

QTEST_GUILESS_MAIN(TestSteamLocator)
#include "tst_steamlocator.moc"
