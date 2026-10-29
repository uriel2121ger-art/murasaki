#include "animfixtures.h"
#include "localpets.h"
#include "petcheck.h"
#include "petlimits.h"
#include "testutil.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;
using namespace animfixtures;

class TestPetCheck : public QObject
{
    Q_OBJECT

private slots:
    void aPlainItemIsComplete();
    void theJsonLineHasTheShapeOfTheSceneEngines();
    void aLocalFileIsChecked();
    void animaIdsResolveInsideTheWorkshopDirs();
    void aBrokenHeaderIsRefused();
    void anItemWithNoAnimationIsRefused();
    void anApngNamedPngIsAnAnimationAndAStillPngIsNot();
    void joinedGifStreamsAreNotSkipped();
    void aContainerThatIsNotImportedIsRefused();
    void aRenamedContainerIsRefused();
    void aFileOverTheSizeLimitIsRefused();
    void aSymlinkOutOfTheItemIsRefused();
    void aMissingItemIsRefused();
    void anUnreadableVariantDegradesTheItemWhenAnotherPlays();
    void filesThatAreNotImportedAreNamed();
    void aProgramInTheItemIsNamed();
    void theFrameCacheLimitsDegrade();
    void aSymlinkedItemDirOutOfTheWorkshopIsNotFound();
    void aCanvasOverTheVideoLimitIsRefused();
    void aPetThatDoesNotFitTheFrameCacheIsNamed();
    void anItemWithManyFilesIsCapped();
    void theOverlayRefusesEveryOtherFormOfTheOption();
    void controlCharactersNeverReachTheTerminal();
    void aNameIsNeverCutInsideASurrogatePair();
    void aLive2dModelNextToAnAnimationIsNoGap();
    void everyEntryStartsWithAnameOfTheTable();
    void theDocumentListsTheTable();
    void theCommandLineToolsPrintTheSameLine();

private:
    static QStringList names(const QStringList &entries)
    {
        QStringList out;
        for (const QString &entry : entries)
            out.append(entry.section(u": "_s, 0, 0));
        return out;
    }
    static QString item(const QTemporaryDir &tmp, const QString &id = u"3753131028"_s) { return tmp.path() + u'/' + id; }
};

void TestPetCheck::aPlainItemIsComplete()
{
    QTemporaryDir tmp;
    testutil::writeFile(item(tmp) + u"/a.apng"_s, apng(40, 30, {{1, 30}, {1, 30}, {1, 30}}));
    testutil::writeFile(item(tmp) + u"/b.gif"_s, gif(20, 10, {5, 5}));
    testutil::writeFile(item(tmp) + u"/c.webp"_s, webpAnimated(16, 16, {40, 40}));
    const PetCheckResult result = checkPetItem(item(tmp), {});
    QVERIFY(result.ok);
    QVERIFY2(result.reasons.isEmpty(), qPrintable(result.reasons.join(u"; "_s)));
    QVERIFY2(result.degraded.isEmpty(), qPrintable(result.degraded.join(u"; "_s)));
}

void TestPetCheck::theJsonLineHasTheShapeOfTheSceneEngines()
{
    QTemporaryDir tmp;
    testutil::writeFile(item(tmp) + u"/a.apng"_s, apng(8, 8, {{1, 30}, {1, 30}}));
    testutil::writeFile(item(tmp) + u"/song.mp3"_s, "x");
    const QByteArray line = petCheckJson(checkPetItem(item(tmp), {}));
    QVERIFY(line.startsWith("{\"ok\":true,\"reasons\":[],\"degraded\":[\"audio not played\"],\"engine\":\"murasaki-overlay\",\"version\":\""));
    QVERIFY(line.endsWith("\"}"));
    QVERIFY(!line.contains('\n'));
    const QJsonObject parsed = QJsonDocument::fromJson(line).object();
    QCOMPARE(parsed.value(u"engine"_s).toString(), u"murasaki-overlay"_s);
    QVERIFY(!parsed.value(u"version"_s).toString().isEmpty());

    const QByteArray refused = petCheckJson(checkPetItem(tmp.path() + u"/missing"_s, {}));
    QVERIFY(refused.startsWith("{\"ok\":false,\"reasons\":[\"item not found: "));
    QVERIFY(refused.contains("],\"degraded\":[],\"engine\":"));
}

void TestPetCheck::aLocalFileIsChecked()
{
    QTemporaryDir tmp;
    const QString file = tmp.path() + u"/pet.gif"_s;
    testutil::writeFile(file, gif(12, 12, {5, 5, 5}));
    const PetCheckResult result = checkPetItem(file, {});
    QVERIFY(result.ok);
    QVERIFY(result.degraded.isEmpty());

    testutil::writeFile(tmp.path() + u"/notes.txt"_s, "hello");
    const PetCheckResult text = checkPetItem(tmp.path() + u"/notes.txt"_s, {});
    QVERIFY(!text.ok);
    QCOMPARE(names(text.reasons), QStringList{u"unsupported file type"_s});

    testutil::writeFile(tmp.path() + u"/clip.mp4"_s, "x");
    QCOMPARE(names(checkPetItem(tmp.path() + u"/clip.mp4"_s, {}).reasons), QStringList{u"unsupported container"_s});
}

void TestPetCheck::animaIdsResolveInsideTheWorkshopDirs()
{
    QTemporaryDir tmp;
    testutil::writeFile(tmp.path() + u"/content/42/a.apng"_s, apng(8, 8, {{1, 30}, {1, 30}}));
    const QStringList dirs{tmp.path() + u"/nowhere"_s, tmp.path() + u"/content"_s};
    QVERIFY(checkPetItem(u"anima:42"_s, dirs).ok);
    for (const QString &id : {u"anima:43"_s, u"anima:"_s, u"anima:../content/42"_s, u"anima:."_s, u"anima:.."_s}) {
        const PetCheckResult result = checkPetItem(id, dirs);
        QVERIFY2(!result.ok, qPrintable(id));
        QCOMPARE(names(result.reasons), QStringList{u"item not found"_s});
    }
}

void TestPetCheck::aBrokenHeaderIsRefused()
{
    QTemporaryDir tmp;
    QByteArray good = apng(30, 30, {{1, 30}, {1, 30}});
    testutil::writeFile(item(tmp) + u"/cut.apng"_s, good.left(good.size() / 2));
    PetCheckResult result = checkPetItem(item(tmp), {});
    QVERIFY(!result.ok);
    QCOMPARE(result.reasons, QStringList{u"not a readable animation: cut.apng"_s});

    // not even a magic number
    testutil::writeFile(tmp.path() + u"/pet.gif"_s, "this is not a gif");
    result = checkPetItem(tmp.path() + u"/pet.gif"_s, {});
    QCOMPARE(names(result.reasons), QStringList{u"not a readable animation"_s});
}

void TestPetCheck::anApngNamedPngIsAnAnimationAndAStillPngIsNot()
{
    // Anima Engine plays a .png that holds an acTL before its first IDAT (3819174039's `Malfoid Animation.png`)
    QTemporaryDir tmp;
    testutil::writeFile(item(tmp) + u"/Malfoid Animation.png"_s, apng(12, 12, {{1, 10}, {1, 10}, {1, 10}}));
    const PetCheckResult animated = checkPetItem(item(tmp), {});
    QVERIFY2(animated.ok, qPrintable(animated.reasons.join(u"; "_s)));
    QVERIFY(animated.degraded.isEmpty());
    QVERIFY(checkPetItem(item(tmp) + u"/Malfoid Animation.png"_s, {}).ok); // and as a local file

    // a still .png keeps today's behaviour: not an animation file
    QTemporaryDir other;
    testutil::writeFile(item(other) + u"/picture.png"_s, apng(12, 12, {}));
    QCOMPARE(checkPetItem(item(other), {}).reasons, QStringList{u"no animation file"_s});
    QCOMPARE(names(checkPetItem(item(other) + u"/picture.png"_s, {}).reasons), QStringList{u"unsupported file type"_s});
}

void TestPetCheck::joinedGifStreamsAreNotSkipped()
{
    // a GIF followed by another GIF with no trailer between (3431910992's `cx.gif.gif`): readable, every frame counted, no skipped variant
    QByteArray joined = gif(12, 12, {5, 5, 5});
    joined.chop(1);
    joined += gif(12, 12, {5});
    QTemporaryDir tmp;
    testutil::writeFile(item(tmp) + u"/cx.gif.gif"_s, joined);
    testutil::writeFile(item(tmp) + u"/cx3.gif"_s, gif(12, 12, {5, 5}));
    const PetCheckResult result = checkPetItem(item(tmp), {});
    QVERIFY2(result.ok, qPrintable(result.reasons.join(u"; "_s)));
    QVERIFY2(result.degraded.isEmpty(), qPrintable(result.degraded.join(u"; "_s)));
}

void TestPetCheck::anItemWithNoAnimationIsRefused()
{
    QTemporaryDir tmp;
    testutil::writeFile(item(tmp) + u"/readme.txt"_s, "x");
    testutil::writeFile(item(tmp) + u"/song.ogg"_s, "x");
    const PetCheckResult result = checkPetItem(item(tmp), {});
    QVERIFY(!result.ok);
    QCOMPARE(result.reasons, QStringList{u"no animation file"_s});
}

void TestPetCheck::aContainerThatIsNotImportedIsRefused()
{
    QTemporaryDir tmp;
    testutil::writeFile(item(tmp) + u"/pack.mdaa"_s, "PK");
    testutil::writeFile(item(tmp) + u"/song.ogg"_s, "x");
    const PetCheckResult result = checkPetItem(item(tmp), {});
    QVERIFY(!result.ok);
    QCOMPARE(result.reasons, QStringList{u"unsupported container: mdaa"_s});
}

void TestPetCheck::aRenamedContainerIsRefused()
{
    QTemporaryDir tmp;
    testutil::writeFile(item(tmp) + u"/pet.gif"_s, QByteArray("PK\x03\x04", 4) + QByteArray(40, 0));
    const PetCheckResult result = checkPetItem(item(tmp), {});
    QVERIFY(!result.ok);
    QCOMPARE(result.reasons, QStringList{u"unsupported container: zip"_s});

    testutil::writeFile(tmp.path() + u"/video.apng"_s, QByteArray("\0\0\0\x18" "ftypisom", 12) + QByteArray(40, 0));
    QCOMPARE(checkPetItem(tmp.path() + u"/video.apng"_s, {}).reasons, QStringList{u"unsupported container: mp4"_s});
}

void TestPetCheck::aFileOverTheSizeLimitIsRefused()
{
    QTemporaryDir tmp;
    const QString path = item(tmp) + u"/huge.apng"_s;
    testutil::writeFile(path, apng(8, 8, {{1, 30}, {1, 30}}));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    if (!file.resize(kMaxLocalPetBytes + 1))
        QSKIP("the file system has no sparse files this large");
    file.close();
    const PetCheckResult result = checkPetItem(item(tmp), {});
    QVERIFY(!result.ok);
    QCOMPARE(result.reasons, QStringList{u"file over the size limit: huge.apng"_s});
    QCOMPARE(names(checkPetItem(path, {}).reasons), QStringList{u"file over the size limit"_s});
}

void TestPetCheck::aSymlinkOutOfTheItemIsRefused()
{
    QTemporaryDir tmp;
    testutil::writeFile(tmp.path() + u"/outside/a.apng"_s, apng(8, 8, {{1, 30}, {1, 30}}));
    QVERIFY(QDir().mkpath(item(tmp)));
    QVERIFY(QFile::link(tmp.path() + u"/outside/a.apng"_s, item(tmp) + u"/a.apng"_s));
    const PetCheckResult result = checkPetItem(item(tmp), {});
    QVERIFY(!result.ok);
    QCOMPARE(result.reasons, QStringList{u"not a regular file inside the item folder: a.apng"_s});
}

void TestPetCheck::aMissingItemIsRefused()
{
    const PetCheckResult result = checkPetItem(u"/nonexistent/murasaki/item"_s, {});
    QVERIFY(!result.ok);
    QCOMPARE(names(result.reasons), QStringList{u"item not found"_s});
    QVERIFY(result.degraded.isEmpty());
}

void TestPetCheck::anUnreadableVariantDegradesTheItemWhenAnotherPlays()
{
    QTemporaryDir tmp;
    testutil::writeFile(item(tmp) + u"/good.apng"_s, apng(8, 8, {{1, 30}, {1, 30}}));
    testutil::writeFile(item(tmp) + u"/bad.gif"_s, "GIF89a");
    const PetCheckResult result = checkPetItem(item(tmp), {});
    QVERIFY(result.ok);
    QCOMPARE(result.degraded, QStringList{u"variant skipped: bad.gif"_s});
}

void TestPetCheck::filesThatAreNotImportedAreNamed()
{
    QTemporaryDir tmp;
    testutil::writeFile(item(tmp) + u"/a.apng"_s, apng(8, 8, {{1, 30}, {1, 30}}));
    for (const char *name : {"pack.mdaa", "clip.mp4", "clip2.webm", "frames.zip", "model.moc3", "song.mp3", "song2.wav"})
        testutil::writeFile(item(tmp) + u'/' + QString::fromLatin1(name), "x");
    const PetCheckResult result = checkPetItem(item(tmp), {});
    QVERIFY(result.ok);
    QCOMPARE(result.degraded, (QStringList{
                                  u"audio not played"_s,
                                  u"frame archive not imported"_s,
                                  u"mdaa package not imported"_s,
                                  u"video source not imported"_s,
                              }));
}

void TestPetCheck::aProgramInTheItemIsNamed()
{
    QTemporaryDir tmp;
    testutil::writeFile(item(tmp) + u"/a.apng"_s, apng(8, 8, {{1, 30}, {1, 30}}));
    testutil::writeFile(item(tmp) + u"/core.exe"_s, QByteArray("MZ") + QByteArray(100, 0));
    const PetCheckResult result = checkPetItem(item(tmp), {});
    QVERIFY(result.ok);
    QCOMPARE(result.degraded, QStringList{u"program not run"_s});

    // the same for a local pet: the file itself is what is scanned, its folder is not
    const QString elf = tmp.path() + u"/pet.gif"_s;
    testutil::writeFile(elf, gif(8, 8, {5, 5}));
    QVERIFY(checkPetItem(elf, {}).degraded.isEmpty());
}

void TestPetCheck::theFrameCacheLimitsDegrade()
{
    QTemporaryDir tmp;
    testutil::writeFile(item(tmp) + u"/long.gif"_s, gifFrames(2, 2, kMaxPetFrames + 1));
    testutil::writeFile(item(tmp) + u"/wide.apng"_s, apng(kMaxPetCanvasSide + 1, 2, {{1, 30}, {1, 30}}));
    testutil::writeFile(item(tmp) + u"/fits.gif"_s, gifFrames(2, 2, kMaxPetFrames));
    const PetCheckResult result = checkPetItem(item(tmp), {});
    QVERIFY(result.ok);
    QCOMPARE(result.degraded,
             (QStringList{u"canvas too large for the frame cache: %1x2"_s.arg(kMaxPetCanvasSide + 1),
                          u"too many frames for the frame cache: %1"_s.arg(kMaxPetFrames + 1)}));
    // the limit itself is fine
    QTemporaryDir exact;
    testutil::writeFile(item(exact) + u"/fits.gif"_s, gifFrames(2, 2, kMaxPetFrames));
    QVERIFY(checkPetItem(item(exact), {}).degraded.isEmpty());
}

void TestPetCheck::aSymlinkedItemDirOutOfTheWorkshopIsNotFound()
{
    QTemporaryDir tmp;
    testutil::writeFile(tmp.path() + u"/elsewhere/a.apng"_s, apng(8, 8, {{1, 30}, {1, 30}}));
    QVERIFY(QDir().mkpath(tmp.path() + u"/content"_s));
    QVERIFY(QFile::link(tmp.path() + u"/elsewhere"_s, tmp.path() + u"/content/42"_s));
    const QStringList dirs{tmp.path() + u"/content"_s};
    const PetCheckResult result = checkPetItem(u"anima:42"_s, dirs);
    QVERIFY(!result.ok);
    QCOMPARE(names(result.reasons), QStringList{u"item not found"_s});
    // a content dir reached through a link is fine
    testutil::writeFile(tmp.path() + u"/content/7/a.apng"_s, apng(8, 8, {{1, 30}, {1, 30}}));
    QVERIFY(QFile::link(tmp.path() + u"/content"_s, tmp.path() + u"/rootlink"_s));
    QVERIFY(checkPetItem(u"anima:7"_s, {tmp.path() + u"/rootlink"_s}).ok);
}

void TestPetCheck::aCanvasOverTheVideoLimitIsRefused()
{
    QTemporaryDir tmp;
    testutil::writeFile(item(tmp) + u"/huge.apng"_s, apng(kMaxPetVideoSide + 1, 2, {{1, 30}, {1, 30}}));
    PetCheckResult result = checkPetItem(item(tmp), {});
    QVERIFY(!result.ok);
    QCOMPARE(names(result.reasons), QStringList{u"canvas over the video path limit"_s});
    QCOMPARE(names(checkPetItem(item(tmp) + u"/huge.apng"_s, {}).reasons), QStringList{u"canvas over the video path limit"_s});
    // the limit itself plays (through the video path: no frame cache)
    QTemporaryDir exact;
    testutil::writeFile(item(exact) + u"/wide.apng"_s, apng(kMaxPetVideoSide, 2, {{1, 30}, {1, 30}}));
    result = checkPetItem(item(exact), {});
    QVERIFY(result.ok);
    QCOMPARE(result.degraded, QStringList{u"canvas too large for the frame cache: %1x2"_s.arg(kMaxPetVideoSide)});
    // beside another animation it only skips the file
    testutil::writeFile(item(tmp) + u"/good.gif"_s, gif(8, 8, {5, 5}));
    result = checkPetItem(item(tmp), {});
    QVERIFY(result.ok);
    QCOMPARE(result.degraded, QStringList{u"variant skipped: huge.apng"_s});
}

void TestPetCheck::aPetThatDoesNotFitTheFrameCacheIsNamed()
{
    QTemporaryDir tmp;
    // 300 x 300 x 4 bytes = 351 KiB a frame: 400 frames are 137 MiB, over the 128 MiB of the default budget
    testutil::writeFile(item(tmp) + u"/big.gif"_s, gifFrames(300, 300, 400));
    PetCheckResult result = checkPetItem(item(tmp), {});
    QVERIFY(result.ok);
    QCOMPARE(result.degraded, QStringList{u"exceeds the frame cache budget: 138 MiB"_s});
    // 350 frames fit
    QTemporaryDir fits;
    testutil::writeFile(item(fits) + u"/ok.gif"_s, gifFrames(300, 300, 350));
    QVERIFY(checkPetItem(item(fits), {}).degraded.isEmpty());
    // a bigger canvas is judged at the 650 px the overlay decodes it at, not at its own size
    QTemporaryDir scaled;
    testutil::writeFile(item(scaled) + u"/large.gif"_s, gifFrames(1300, 1300, 100)); // 100 x 650 x 650 x 4 = 161 MiB
    QCOMPARE(checkPetItem(item(scaled), {}).degraded, QStringList{u"exceeds the frame cache budget: 162 MiB"_s});
    // the environment's budget is never read
    qputenv("MURASAKI_FRAME_CACHE_MIB", "16384");
    QCOMPARE(checkPetItem(item(tmp), {}).degraded, QStringList{u"exceeds the frame cache budget: 138 MiB"_s});
    qunsetenv("MURASAKI_FRAME_CACHE_MIB");
    // a still image is never cached
    QTemporaryDir still;
    testutil::writeFile(item(still) + u"/still.apng"_s, apng(650, 650, {}));
    QVERIFY(checkPetItem(item(still), {}).degraded.isEmpty());
}

void TestPetCheck::anItemWithManyFilesIsCapped()
{
    QTemporaryDir tmp;
    const QByteArray data = gif(4, 4, {5, 5});
    for (int i = 0; i < kMaxCheckedFiles + 1; ++i)
        testutil::writeFile(item(tmp) + u"/f%1.gif"_s.arg(i, 3, 10, QLatin1Char('0')), data);
    const PetCheckResult result = checkPetItem(item(tmp), {});
    QVERIFY(result.ok);
    QCOMPARE(result.degraded, QStringList{u"too many files to examine: %1"_s.arg(kMaxCheckedFiles + 1)});
    QTemporaryDir few;
    for (int i = 0; i < kMaxCheckedFiles; ++i)
        testutil::writeFile(item(few) + u"/f%1.gif"_s.arg(i, 3, 10, QLatin1Char('0')), data);
    QVERIFY(checkPetItem(item(few), {}).degraded.isEmpty());
}

void TestPetCheck::theOverlayRefusesEveryOtherFormOfTheOption()
{
#ifdef MURASAKI_OVERLAY_BIN
    QTemporaryDir tmp;
    const QStringList forms[] = {
        {u"--check=%1"_s.arg(tmp.path())},
        {u"--check"_s},
        {u"--check"_s, tmp.path(), u"extra"_s},
        {tmp.path(), u"--check"_s},
        {u"--check"_s, u"--check"_s},
    };
    for (const QStringList &arguments : forms) {
        QProcess process;
        process.setProgram(QStringLiteral(MURASAKI_OVERLAY_BIN));
        process.setArguments(arguments);
        process.start();
        QVERIFY2(process.waitForFinished(30000), qPrintable(arguments.join(u' ')));
        QCOMPARE(process.exitCode(), 2);
        QVERIFY(process.readAllStandardOutput().isEmpty());
        QVERIFY2(process.readAllStandardError().contains("--check ITEM"), qPrintable(arguments.join(u' ')));
    }
#else
    QSKIP("murasaki-overlay is not built");
#endif
}

void TestPetCheck::controlCharactersNeverReachTheTerminal()
{
#ifdef MURASAKICTL_BIN
    QTemporaryDir tmp;
    testutil::writeFile(item(tmp) + u"/good.apng"_s, apng(8, 8, {{1, 30}, {1, 30}}));
    testutil::writeFile(item(tmp) + u"/bad\x1b[31mred.gif"_s, "GIF89a");
    QProcess process;
    process.setProgram(QStringLiteral(MURASAKICTL_BIN));
    process.setArguments({u"pets"_s, u"check"_s, item(tmp)});
    process.start();
    QVERIFY(process.waitForFinished(30000));
    const QByteArray errors = process.readAllStandardError();
    QVERIFY2(errors.startsWith("degraded: variant skipped: "), errors.constData());
    for (const char c : errors) {
        if (c != '\n')
            QVERIFY2(quint8(c) >= 0x20 && c != 0x7f, "a control character reached stderr");
    }
    // stdout is JSON: the escape is escaped there
    QVERIFY(QJsonDocument::fromJson(process.readAllStandardOutput()).isObject());
#else
    QSKIP("murasakictl is not built");
#endif
}

void TestPetCheck::aNameIsNeverCutInsideASurrogatePair()
{
    QTemporaryDir tmp;
    // 199 UTF-16 units, then an emoji (two units): a cut at 200 would split it
    const QString name = QString(199, u'a') + QString::fromUtf8("\xF0\x9F\x98\x80") + u".gif"_s;
    testutil::writeFile(item(tmp) + u'/' + name, "GIF89a");
    const PetCheckResult result = checkPetItem(item(tmp), {});
    QVERIFY(!result.ok);
    const QString reason = result.reasons.constFirst();
    for (qsizetype i = 0; i < reason.size(); ++i) {
        if (reason.at(i).isHighSurrogate()) {
            QVERIFY(i + 1 < reason.size() && reason.at(i + 1).isLowSurrogate());
            ++i;
        } else {
            QVERIFY(!reason.at(i).isLowSurrogate());
        }
    }
    QVERIFY(reason.startsWith(u"not a readable animation: "_s));
}

void TestPetCheck::aLive2dModelNextToAnAnimationIsNoGap()
{
    QTemporaryDir tmp;
    testutil::writeFile(item(tmp) + u"/a.apng"_s, apng(8, 8, {{1, 30}, {1, 30}}));
    testutil::writeFile(item(tmp) + u"/model.moc3"_s, "MOC3");
    const PetCheckResult result = checkPetItem(item(tmp), {});
    QVERIFY(result.ok);
    QVERIFY(result.reasons.isEmpty());
    QVERIFY2(result.degraded.isEmpty(), qPrintable(result.degraded.join(u"; "_s)));
}

void TestPetCheck::everyEntryStartsWithAnameOfTheTable()
{
    QStringList table;
    for (const petcheck::Name &name : petcheck::kNames)
        table.append(QString::fromUtf8(name.text.data(), qsizetype(name.text.size())));
    QCOMPARE(table.removeDuplicates(), 0);

    QTemporaryDir tmp;
    testutil::writeFile(item(tmp, u"1"_s) + u"/long.gif"_s, gifFrames(2, 2, kMaxPetFrames + 1));
    testutil::writeFile(item(tmp, u"1"_s) + u"/bad.gif"_s, "GIF89a");
    testutil::writeFile(item(tmp, u"1"_s) + u"/song.mp3"_s, "x");
    testutil::writeFile(item(tmp, u"2"_s) + u"/x.txt"_s, "x");
    testutil::writeFile(item(tmp, u"3"_s) + u"/x.apng"_s, "x");
    for (const QString &id : {u"1"_s, u"2"_s, u"3"_s, u"4"_s}) {
        const PetCheckResult result = checkPetItem(item(tmp, id), {});
        for (const QString &name : names(result.reasons + result.degraded))
            QVERIFY2(table.contains(name), qPrintable(name));
    }
}

void TestPetCheck::theDocumentListsTheTable()
{
    QFile doc(QStringLiteral(PETS_CHECK_DOC));
    QVERIFY2(doc.open(QIODevice::ReadOnly), "docs/pets/check.md is missing");
    const QString text = QString::fromUtf8(doc.readAll());
    for (const petcheck::Name &name : petcheck::kNames) {
        const QString row = u"| `"_s + QString::fromUtf8(name.text.data(), qsizetype(name.text.size())) + u"` |"_s;
        QVERIFY2(text.contains(row), qPrintable(row));
    }
}

void TestPetCheck::theCommandLineToolsPrintTheSameLine()
{
    QTemporaryDir tmp;
    testutil::writeFile(item(tmp) + u"/a.apng"_s, apng(8, 8, {{1, 30}, {1, 30}}));
    testutil::writeFile(item(tmp) + u"/song.mp3"_s, "x");
    const QByteArray expected = petCheckJson(checkPetItem(item(tmp), {})) + '\n';
    int ran = 0;
    const auto run = [&](const QString &program, const QStringList &arguments) {
        QProcess process;
        process.setProgram(program);
        process.setArguments(arguments);
        process.start();
        QVERIFY2(process.waitForFinished(30000), qPrintable(program));
        QCOMPARE(process.exitCode(), 0);
        QCOMPARE(process.readAllStandardOutput(), expected);
        ++ran;
    };
#ifdef MURASAKICTL_BIN
    run(QStringLiteral(MURASAKICTL_BIN), {u"pets"_s, u"check"_s, item(tmp)});
#endif
#ifdef MURASAKI_OVERLAY_BIN
    run(QStringLiteral(MURASAKI_OVERLAY_BIN), {u"--check"_s, item(tmp)});
#endif
    if (ran == 0)
        QSKIP("neither murasakictl nor murasaki-overlay is built");
}

QTEST_GUILESS_MAIN(TestPetCheck)
#include "tst_petcheck.moc"
