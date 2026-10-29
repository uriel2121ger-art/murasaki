#include "steamlocator.h"
#include "testutil.h"
#include "workshopstate.h"

#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

QByteArray acf(const QStringList &installedIds)
{
    QByteArray text = "\"AppWorkshop\"\n{\n\t\"appid\"\t\t\"431960\"\n\t\"WorkshopItemsInstalled\"\n\t{\n";
    for (const QString &id : installedIds)
        text += "\t\t\"" + id.toUtf8() + "\"\n\t\t{\n\t\t\t\"size\"\t\t\"10\"\n\t\t\t\"timeupdated\"\t\t\"1\"\n"
                "\t\t\t\"manifest\"\t\t\"2\"\n\t\t}\n";
    text += "\t}\n\t\"WorkshopItemDetails\"\n\t{\n\t}\n}\n";
    return text;
}

void writeAcf(const QString &root, const QStringList &ids)
{
    testutil::writeFile(root + u"/appworkshop_431960.acf"_s, acf(ids));
}

void atomicWriteAcf(const QString &root, const QStringList &ids)
{
    QSaveFile file(root + u"/appworkshop_431960.acf"_s);
    if (!file.open(QIODevice::WriteOnly) || file.write(acf(ids)) < 0 || !file.commit())
        qFatal("cannot atomically write the acf");
}

void makeDir(const QString &path)
{
    if (!QDir().mkpath(path))
        qFatal("cannot create %s", qPrintable(path));
}

// One burst of filesystem activity must give exactly one (debounced) signal.
void expectOneChange(QSignalSpy &spy)
{
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 3000);
    QTest::qWait(700);
    QCOMPARE(spy.count(), 1);
}

} // namespace

class TestWorkshopState : public QObject
{
    Q_OBJECT

private slots:
    void reportsEveryState();
    void installedNeedsContentDir();
    void downloadingBeatsInstalled();
    void missingAcfMeansNone();
    void malformedAcfCountsAsNotInstalled();
    void combinesLibraries();
    void rejectsNonNumericIds();
    void changedAfterAtomicAcfReplace();
    void changedAfterDownloadsDirAppears();
    void acfAppearingAfterConstructionIsPickedUp();
    void workshopRootsFromLocator();
};

void TestWorkshopState::reportsEveryState()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"workshop"_s);
    writeAcf(root, {u"100"_s});
    makeDir(root + u"/content/431960/100"_s);
    makeDir(root + u"/downloads/431960/200"_s);

    const WorkshopState state({root}, u"431960"_s);
    QCOMPARE(state.state(u"100"_s), u"installed"_s);
    QCOMPARE(state.state(u"200"_s), u"downloading"_s);
    QCOMPARE(state.state(u"300"_s), u"none"_s);
}

void TestWorkshopState::installedNeedsContentDir()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"workshop"_s);
    writeAcf(root, {u"100"_s});
    // Listed in the manifest but the files are gone (or content exists but is not listed).
    makeDir(root + u"/content/431960/101"_s);

    const WorkshopState state({root}, u"431960"_s);
    QCOMPARE(state.state(u"100"_s), u"none"_s);
    QCOMPARE(state.state(u"101"_s), u"none"_s);
}

void TestWorkshopState::downloadingBeatsInstalled()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"workshop"_s);
    writeAcf(root, {u"100"_s});
    makeDir(root + u"/content/431960/100"_s);
    makeDir(root + u"/downloads/431960/100"_s);

    const WorkshopState state({root}, u"431960"_s);
    QCOMPARE(state.state(u"100"_s), u"downloading"_s);
}

void TestWorkshopState::missingAcfMeansNone()
{
    QTemporaryDir tmp;
    makeDir(tmp.filePath(u"workshop/content/431960/100"_s));
    const WorkshopState state({tmp.filePath(u"workshop"_s)}, u"431960"_s);
    QCOMPARE(state.state(u"100"_s), u"none"_s);
    const WorkshopState missingRoot({tmp.filePath(u"nowhere"_s)}, u"431960"_s);
    QCOMPARE(missingRoot.state(u"100"_s), u"none"_s);
}

void TestWorkshopState::malformedAcfCountsAsNotInstalled()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"workshop"_s);
    testutil::writeFile(root + u"/appworkshop_431960.acf"_s, "\"AppWorkshop\" { \"WorkshopItemsInstalled\" { \"100\" {");
    makeDir(root + u"/content/431960/100"_s);

    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"WorkshopState: cannot parse"_s));
    const WorkshopState state({root}, u"431960"_s);
    QCOMPARE(state.state(u"100"_s), u"none"_s);
}

void TestWorkshopState::combinesLibraries()
{
    QTemporaryDir tmp;
    const QString a = tmp.filePath(u"a/workshop"_s), b = tmp.filePath(u"b/workshop"_s);
    writeAcf(a, {u"100"_s});
    makeDir(a + u"/content/431960/100"_s);
    // 100 is also listed in b's manifest, but its files only live in a: still installed through a.
    writeAcf(b, {u"200"_s, u"100"_s});
    makeDir(b + u"/content/431960/200"_s);
    makeDir(b + u"/downloads/431960/300"_s);

    const WorkshopState state({a, b}, u"431960"_s);
    QCOMPARE(state.state(u"100"_s), u"installed"_s);
    QCOMPARE(state.state(u"200"_s), u"installed"_s);
    QCOMPARE(state.state(u"300"_s), u"downloading"_s);
}

void TestWorkshopState::rejectsNonNumericIds()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"workshop"_s);
    makeDir(root + u"/downloads/431960/x"_s);
    const WorkshopState state({root}, u"431960"_s);
    QCOMPARE(state.state(u"x"_s), u"none"_s);
    QCOMPARE(state.state(u"../431960/x"_s), u"none"_s);
    QCOMPARE(state.state(QString()), u"none"_s);
}

void TestWorkshopState::changedAfterAtomicAcfReplace()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"workshop"_s);
    writeAcf(root, {});
    makeDir(root + u"/content/431960/100"_s);

    WorkshopState state({root}, u"431960"_s);
    QCOMPARE(state.state(u"100"_s), u"none"_s);
    QSignalSpy spy(&state, &WorkshopState::changed);

    atomicWriteAcf(root, {u"100"_s});
    expectOneChange(spy);
    QCOMPARE(state.state(u"100"_s), u"installed"_s);

    // The watch survived the replace: a second atomic rewrite is noticed too.
    spy.clear();
    atomicWriteAcf(root, {});
    expectOneChange(spy);
    QCOMPARE(state.state(u"100"_s), u"none"_s);
}

void TestWorkshopState::changedAfterDownloadsDirAppears()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"workshop"_s);
    writeAcf(root, {});

    WorkshopState state({root}, u"431960"_s);
    QSignalSpy spy(&state, &WorkshopState::changed);

    makeDir(root + u"/downloads/431960/100"_s);
    expectOneChange(spy);
    QCOMPARE(state.state(u"100"_s), u"downloading"_s);

    // And a new item appearing inside the already watched downloads dir.
    spy.clear();
    makeDir(root + u"/downloads/431960/101"_s);
    expectOneChange(spy);
    QCOMPARE(state.state(u"101"_s), u"downloading"_s);
}

void TestWorkshopState::acfAppearingAfterConstructionIsPickedUp()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"workshop"_s);
    makeDir(root + u"/content/431960/100"_s);

    WorkshopState state({root}, u"431960"_s);
    QCOMPARE(state.state(u"100"_s), u"none"_s);
    QSignalSpy spy(&state, &WorkshopState::changed);

    atomicWriteAcf(root, {u"100"_s});
    expectOneChange(spy);
    QCOMPARE(state.state(u"100"_s), u"installed"_s);
}

void TestWorkshopState::workshopRootsFromLocator()
{
    QTemporaryDir tmp;
    const QString root = tmp.filePath(u"steam"_s);
    makeDir(root + u"/steamapps"_s);
    const SteamLocator steam({root});
    QCOMPARE(steam.workshopRoots(), QStringList({testutil::canonical(root) + u"/steamapps/workshop"_s}));
}

QTEST_MAIN(TestWorkshopState)
#include "tst_workshopstate.moc"
