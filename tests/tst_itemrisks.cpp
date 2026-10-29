#include "itemrisks.h"
#include "testutil.h"
#include "wallpaperengineimporter.h"

#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestItemRisks : public QObject
{
    Q_OBJECT

private slots:
    void cleanFolderHasNoRisks();
    void flagsExecutableMagics_data();
    void flagsExecutableMagics();
    void flagsShebangScripts();
    void flagsRiskyExtensions_data();
    void flagsRiskyExtensions();
    void neverFlagsJavaScriptOrWebFiles();
    void flagsNestedFilesWithRelativePath();
    void boundsDepthAndFileCount();
    void doesNotFollowSymlinks();
    void flagsProgramsInsideScenePackages();
    void malformedPackagesAreIgnored();
    void mutatedPackagesNeverCrashTheScan();
    void weItemCarriesRisksInJson();
    void weApplicationTypeIsFlagged();
};

static QString makeDir(const QTemporaryDir &tmp, const QString &name = u"item"_s)
{
    const QString dir = tmp.filePath(name);
    QDir().mkpath(dir);
    return dir;
}

void TestItemRisks::cleanFolderHasNoRisks()
{
    QTemporaryDir tmp;
    const QString dir = makeDir(tmp);
    testutil::writeFile(dir + u"/clip.mp4"_s, "\x00\x00\x00\x18" "ftypmp42");
    testutil::writeFile(dir + u"/notes.txt"_s, "hello");
    testutil::writeFile(dir + u"/tiny"_s, "x");
    testutil::writeFile(dir + u"/empty"_s, "");
    QVERIFY(scanItemRisks(dir).isEmpty());
}

void TestItemRisks::flagsExecutableMagics_data()
{
    QTest::addColumn<QByteArray>("content");
    QTest::newRow("elf") << QByteArray("\x7f" "ELF\x02\x01\x01", 7);
    QTest::newRow("macho64") << QByteArray("\xcf\xfa\xed\xfe\x07\x00\x00\x01", 8);
    QTest::newRow("macho32") << QByteArray("\xce\xfa\xed\xfe\x07\x00\x00\x00", 8);
    QTest::newRow("macho-fat") << QByteArray("\xca\xfe\xba\xbe\x00\x00\x00\x02", 8);
    QByteArray pe(128, '\0');
    pe.replace(0, 2, "MZ");
    pe[0x3c] = 0x40;
    pe.replace(0x40, 4, QByteArray("PE\0\0", 4));
    QTest::newRow("pe") << pe;
}

void TestItemRisks::flagsExecutableMagics()
{
    QFETCH(QByteArray, content);
    QTemporaryDir tmp;
    const QString dir = makeDir(tmp);
    testutil::writeFile(dir + u"/data.bin"_s, content);
    const QList<ItemRisk> risks = scanItemRisks(dir);
    QCOMPARE(risks.size(), 1);
    QCOMPARE(risks.first().kind, u"executable"_s);
    QCOMPARE(risks.first().file, u"data.bin"_s);
}

void TestItemRisks::flagsShebangScripts()
{
    QTemporaryDir tmp;
    const QString dir = makeDir(tmp);
    testutil::writeFile(dir + u"/run"_s, "#!/bin/sh\necho hi\n");
    testutil::writeFile(dir + u"/notashebang.txt"_s, "# comment\n");
    const QList<ItemRisk> risks = scanItemRisks(dir);
    QCOMPARE(risks.size(), 1);
    QCOMPARE(risks.first().file, u"run"_s);
}

void TestItemRisks::flagsRiskyExtensions_data()
{
    QTest::addColumn<QString>("name");
    for (const char *name : {"a.exe", "a.dll", "a.scr", "a.bat", "a.cmd", "a.ps1", "a.vbs", "a.sh", "a.AppImage", "a.jar",
                             "a.msi", "a.lnk", "UPPER.EXE", "mixed.Bat", "x.appimage"})
        QTest::newRow(name) << QString::fromLatin1(name);
}

void TestItemRisks::flagsRiskyExtensions()
{
    QFETCH(QString, name);
    QTemporaryDir tmp;
    const QString dir = makeDir(tmp);
    testutil::writeFile(dir + u'/' + name, "not really a program");
    const QList<ItemRisk> risks = scanItemRisks(dir);
    QCOMPARE(risks.size(), 1);
    QCOMPARE(risks.first().kind, u"executable"_s);
    QCOMPARE(risks.first().file, name);
}

void TestItemRisks::neverFlagsJavaScriptOrWebFiles()
{
    QTemporaryDir tmp;
    const QString dir = makeDir(tmp);
    testutil::writeFile(dir + u"/index.html"_s, "<html></html>");
    testutil::writeFile(dir + u"/app.js"_s, "#!/usr/bin/env node\nconsole.log(1)\n");
    testutil::writeFile(dir + u"/mod.mjs"_s, "export default 1");
    testutil::writeFile(dir + u"/style.css"_s, "a{}");
    testutil::writeFile(dir + u"/data.json"_s, "{}");
    QVERIFY(scanItemRisks(dir).isEmpty());
}

void TestItemRisks::flagsNestedFilesWithRelativePath()
{
    QTemporaryDir tmp;
    const QString dir = makeDir(tmp);
    testutil::writeFile(dir + u"/sub/deeper/payload.exe"_s, "MZ");
    const QList<ItemRisk> risks = scanItemRisks(dir);
    QCOMPARE(risks.size(), 1);
    QCOMPARE(risks.first().file, u"sub/deeper/payload.exe"_s);
}

void TestItemRisks::boundsDepthAndFileCount()
{
    QTemporaryDir tmp;
    const QString dir = makeDir(tmp);
    // Deeper than the scan depth: not seen.
    testutil::writeFile(dir + u"/a/b/c/d/e/f/g/deep.exe"_s, "x");
    QVERIFY(scanItemRisks(dir).isEmpty());

    // More entries than the scan budget (directories count too): the scan stops instead of walking them all.
    QTemporaryDir many;
    const QString big = makeDir(many);
    for (int i = 0; i < kRiskScanMaxFiles + 300; ++i)
        QVERIFY(QDir().mkpath(big + u"/d%1/e"_s.arg(i)));
    QVERIFY(scanItemRisks(big).isEmpty()); // finishes, nothing risky in there

    // The risk list itself is capped.
    QTemporaryDir lots;
    const QString risky = makeDir(lots);
    for (int i = 0; i < kRiskListMax + 10; ++i)
        testutil::writeFile(risky + u"/p%1.exe"_s.arg(i), "x");
    QCOMPARE(scanItemRisks(risky).size(), kRiskListMax);
}

void TestItemRisks::doesNotFollowSymlinks()
{
    QTemporaryDir tmp;
    const QString dir = makeDir(tmp);
    const QString outside = makeDir(tmp, u"outside"_s);
    testutil::writeFile(outside + u"/evil.exe"_s, "x");
    QVERIFY(QFile::link(outside, dir + u"/linkdir"_s));
    QVERIFY(QFile::link(outside + u"/evil.exe"_s, dir + u"/linkfile"_s));
    QVERIFY(scanItemRisks(dir).isEmpty());
}

static QByteArray pkg(const QList<QPair<QString, QByteArray>> &entries)
{
    const auto le = [](quint32 v) { QByteArray b(4, 0); qToLittleEndian(v, b.data()); return b; };
    QByteArray out = le(8) + "PKGV0001" + le(entries.size());
    quint32 offset = 0;
    for (const auto &e : entries) {
        out += le(e.first.toUtf8().size()) + e.first.toUtf8() + le(offset) + le(e.second.size());
        offset += e.second.size();
    }
    for (const auto &e : entries)
        out += e.second;
    return out;
}

void TestItemRisks::flagsProgramsInsideScenePackages()
{
    QTemporaryDir tmp;
    const QString dir = makeDir(tmp);
    testutil::writeFile(dir + u"/scene.pkg"_s, pkg({{u"scene.json"_s, "{}"},
                                                    {u"materials\\tex.tex"_s, "TEXV0005"},
                                                    {u"scripts/run.sh"_s, "echo"},
                                                    {u"data/blob.bin"_s, QByteArray("\x7f" "ELF\x02\x01\x01\x00", 8)},
                                                    {u"data/hook.txt"_s, "#!/bin/sh\n"},
                                                    {u"scripts/app.js"_s, "#!/usr/bin/env node\n"}}));
    const QList<ItemRisk> risks = scanItemRisks(dir);
    QStringList files;
    for (const ItemRisk &risk : risks)
        files << risk.file;
    files.sort();
    QCOMPARE(files, (QStringList{u"scene.pkg:data/blob.bin"_s, u"scene.pkg:data/hook.txt"_s, u"scene.pkg:scripts/run.sh"_s}));
}

void TestItemRisks::malformedPackagesAreIgnored()
{
    QTemporaryDir tmp;
    const QString dir = makeDir(tmp);
    QByteArray valid = pkg({{u"a.exe"_s, "x"}});
    testutil::writeFile(dir + u"/truncated.pkg"_s, valid.left(valid.size() - 10)); // table fine, data cut: entry skipped
    testutil::writeFile(dir + u"/garbage.pkg"_s, QByteArray(300, '\xff'));
    testutil::writeFile(dir + u"/huge.pkg"_s, QByteArray("\x08\0\0\0PKGV0001\xff\xff\xff\x7f", 16));
    QByteArray longName = QByteArray("\x08\0\0\0PKGV0001\x01\0\0\0\xff\xff\xff\x7f", 20);
    testutil::writeFile(dir + u"/longname.pkg"_s, longName);
    QVERIFY(scanItemRisks(dir).isEmpty());
}

// A valid package with bytes flipped, cut and extended in a deterministic pseudo-random way: the scan must come back (the
// sanitizer presets turn any out-of-range read of the index reader into a failure).
void TestItemRisks::mutatedPackagesNeverCrashTheScan()
{
    const QByteArray valid = pkg({{u"scene.json"_s, "{}"},
                                  {u"scripts/run.sh"_s, "echo"},
                                  {u"data/blob.bin"_s, QByteArray("\x7f" "ELF\x02\x01\x01\x00", 8)},
                                  {u"materials\\tex.tex"_s, "TEXV0005"}});
    quint32 state = 12345;
    const auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    QTemporaryDir tmp;
    const QString dir = makeDir(tmp);
    for (int round = 0; round < 400; ++round) {
        QByteArray data = valid;
        const int flips = 1 + int(next() % 6);
        for (int i = 0; i < flips; ++i)
            data[int(next() % quint32(data.size()))] = char(next() & 0xff);
        if (round % 5 == 0)
            data.truncate(int(next() % quint32(data.size() + 1)));
        if (round % 7 == 0)
            data.append(QByteArray(int(next() % 64), char(next() & 0xff)));
        testutil::writeFile(dir + u"/scene.pkg"_s, data);
        const QList<ItemRisk> risks = scanItemRisks(dir);
        QVERIFY(risks.size() <= kRiskListMax);
    }
}

void TestItemRisks::weItemCarriesRisksInJson()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"321"_s);
    testutil::writeProject(dir, testutil::json(R"({"type": "video", "file": "clip.mp4", "title": "V"})"));
    testutil::writeFile(dir + u"/clip.mp4"_s, "v");
    testutil::writeFile(dir + u"/helper.exe"_s, "MZ");
    const auto item = WallpaperEngineImporter::scanItem(dir);
    QVERIFY(item.has_value());
    QCOMPARE(item->status, ItemStatus::Ok); // flagged, not rejected
    QCOMPARE(item->risks.size(), 1);
    const QJsonArray summary = item->toSummaryJson().value(u"risks"_s).toArray();
    QCOMPARE(summary.size(), 1);
    QCOMPARE(summary.first().toObject().value(u"kind"_s).toString(), u"executable"_s);
    QCOMPARE(summary.first().toObject().value(u"file"_s).toString(), u"helper.exe"_s);

    // An item without risks still carries an (empty) array so the GUI can rely on it.
    const QString clean = tmp.filePath(u"322"_s);
    testutil::writeProject(clean, testutil::json(R"({"type": "video", "file": "clip.mp4"})"));
    testutil::writeFile(clean + u"/clip.mp4"_s, "v");
    const auto cleanItem = WallpaperEngineImporter::scanItem(clean);
    QVERIFY(cleanItem->toSummaryJson().value(u"risks"_s).isArray());
    QVERIFY(cleanItem->toSummaryJson().value(u"risks"_s).toArray().isEmpty());
}

void TestItemRisks::weApplicationTypeIsFlagged()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(u"654"_s);
    testutil::writeProject(dir, testutil::json(R"({"type": "Application", "file": "game.exe", "title": "A"})"));
    testutil::writeFile(dir + u"/game.exe"_s, "MZ");
    const auto item = WallpaperEngineImporter::scanItem(dir);
    QVERIFY(item.has_value());
    QCOMPARE(item->status, ItemStatus::Invalid); // never runnable
    bool application = false;
    bool executable = false;
    for (const ItemRisk &risk : item->risks) {
        application |= risk.kind == u"application"_s && risk.file == u"game.exe"_s;
        executable |= risk.kind == u"executable"_s && risk.file == u"game.exe"_s;
    }
    QVERIFY(application);
    QVERIFY(executable);
}

QTEST_APPLESS_MAIN(TestItemRisks)
#include "tst_itemrisks.moc"
