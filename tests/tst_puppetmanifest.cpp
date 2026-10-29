// The puppet item's manifest check (src/puppet/manifest.*, strictjson.*, check.*), the limits table (src/core/puppetlimits.h) and the names table
// (src/core/puppetnames.h). Every fixture is written here; no real model is in the repository.
#include "puppet/check.h"
#include "puppet/manifest.h"
#include "puppetlimits.h"
#include "puppetnames.h"
#include "puppet/strictjson.h"
#include "testutil.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>

#include <sys/stat.h>

#include <functional>

using namespace Qt::StringLiterals;
using namespace murasaki::puppet;

namespace {

std::string_view view(const QByteArray &b)
{
    return std::string_view(b.constData(), size_t(b.size()));
}

QString s(std::string_view v)
{
    return QString::fromUtf8(v.data(), qsizetype(v.size()));
}

// A minimal Cubism manifest as text; `extra` is inserted into FileReferences, `top` after Version.
QByteArray manifestText(const QString &fileRefs = u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"]"_s, const QString &top = {})
{
    return ("{\"Version\": 3," + top + "\"FileReferences\": {" + fileRefs + "}}").toUtf8();
}

QString writeModel(const QString &dir, const QByteArray &manifest = manifestText(), bool withFiles = true)
{
    testutil::writeFile(dir + u"/m.model3.json"_s, manifest);
    if (withFiles) {
        testutil::writeFile(dir + u"/m.moc3"_s, "MOC3");
        testutil::writeFile(dir + u"/t0.png"_s, "png");
    }
    return dir;
}

void sparse(const QString &path, qint64 size)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !file.resize(size))
        qFatal("cannot make a sparse file %s", qPrintable(path));
}

QString firstReasonName(const ManifestResult &r)
{
    return r.reasons.empty() ? QString() : s(r.reasons.front().name);
}

QString reasonsText(const ManifestResult &r)
{
    QStringList parts;
    for (const Problem &p : r.reasons)
        parts << s(p.text());
    return parts.join(u"; "_s);
}

// "[" * n + "]" * n
QString nested(int n)
{
    return QString(n, u'[') + QString(n, u']');
}

} // namespace

class TestPuppetManifest : public QObject
{
    Q_OBJECT
private slots:
    // ---- the strict JSON reader ----
    void jsonAccepts_data();
    void jsonAccepts();
    void jsonRejects_data();
    void jsonRejects();
    void jsonLimits();
    void utf8();

    // ---- the path rules ----
    void pathRules_data();
    void pathRules();

    // ---- a manifest ----
    void aFullCubismModelIsRead();
    void validManifests_data();
    void validManifests();
    void invalidManifests_data();
    void invalidManifests();
    void namedPathsThatLeaveTheFolder_data();
    void namedPathsThatLeaveTheFolder();
    void missingAndWrongFiles();
    void symbolicLinksAndSpecialFiles();
    void theRootMayBeALink();
    void notAPuppet();

    // ---- limits ----
    void countLimits_data();
    void countLimits();
    void sizeLimits();
    void folderLimits();
    void hostileFolders();
    void mutatedManifestsNeverCrash();
    void openInItemCannotBeRedirected();
    void nulCharactersAndOddNames();
    void motionSyncAndGroups();
    void tidyDetailIsSafe();

    // ---- the open container ----
    void openContainers();

    // ---- the check ----
    void checkLine();
    void checkLineIsValidJsonWhateverTheNames();
    void theBinaryPrintsTheSameLine();

    // ---- tables ----
    void limitsTableMatchesTheDocument();
    void namesTableMatchesTheDocument();
    void everyNameStartsWithPuppet();
    void everyReasonIsAName();
};

// ---------------------------------------------------------------------------------------------------------------------------------------------

void TestPuppetManifest::jsonAccepts_data()
{
    QTest::addColumn<QByteArray>("text");
    QTest::newRow("object") << QByteArray(R"({"a": 1, "b": [true, false, null, "x"]})");
    QTest::newRow("bom") << QByteArray("\xEF\xBB\xBF{\"a\":1}");
    QTest::newRow("spaces") << QByteArray(" \t\r\n{ }\n");
    QTest::newRow("numbers") << QByteArray("[0, -0, 1.5, -2.25e3, 1E-2, 0.0, 12345678901234567890]");
    QTest::newRow("escapes") << QByteArray(R"(["a\"b\\c\/d\b\f\n\r\t\u0041\u00e9\uD83D\uDE00"])");
    QTest::newRow("unicode text") << QString::fromUtf8("{\"名前\": \"ひよりmodel\"}").toUtf8();
    QTest::newRow("empty containers") << QByteArray(R"({"a": [], "b": {}})");
    QTest::newRow("same key in two objects") << QByteArray(R"([{"a":1},{"a":2}])");
    QTest::newRow("scalar") << QByteArray("3");
}

void TestPuppetManifest::jsonAccepts()
{
    QFETCH(QByteArray, text);
    const auto r = json::parse(std::string_view(text.constData(), size_t(text.size())), {16, 1000, 4096});
    QVERIFY2(r.ok, r.error.c_str());
}

void TestPuppetManifest::jsonRejects_data()
{
    QTest::addColumn<QByteArray>("text");
    QTest::newRow("empty") << QByteArray();
    QTest::newRow("only space") << QByteArray("  ");
    QTest::newRow("trailing comma array") << QByteArray("[1,]");
    QTest::newRow("trailing comma object") << QByteArray(R"({"a":1,})");
    QTest::newRow("leading comma") << QByteArray("[,1]");
    QTest::newRow("comment") << QByteArray("// x\n{}");
    QTest::newRow("block comment") << QByteArray("{/* x */}");
    QTest::newRow("single quotes") << QByteArray("{'a':1}");
    QTest::newRow("unquoted key") << QByteArray("{a:1}");
    QTest::newRow("NaN") << QByteArray("[NaN]");
    QTest::newRow("Infinity") << QByteArray("[Infinity]");
    QTest::newRow("-Infinity") << QByteArray("[-Infinity]");
    QTest::newRow("leading zero") << QByteArray("[01]");
    QTest::newRow("plus sign") << QByteArray("[+1]");
    QTest::newRow("bare dot") << QByteArray("[.5]");
    QTest::newRow("trailing dot") << QByteArray("[1.]");
    QTest::newRow("bare minus") << QByteArray("[-]");
    QTest::newRow("empty exponent") << QByteArray("[1e]");
    QTest::newRow("overflow") << QByteArray("[1e999]");
    QTest::newRow("hex") << QByteArray("[0x10]");
    QTest::newRow("duplicate key") << QByteArray(R"({"a":1,"a":2})");
    QTest::newRow("duplicate key escaped") << QByteArray(R"({"a":1,"\u0061":2})");
    QTest::newRow("duplicate nested") << QByteArray(R"({"x":{"a":1,"b":2,"a":3}})");
    QTest::newRow("text after") << QByteArray("{} {}");
    QTest::newRow("text after 2") << QByteArray("{}x");
    QTest::newRow("unterminated string") << QByteArray(R"({"a)");
    QTest::newRow("unterminated object") << QByteArray(R"({"a":1)");
    QTest::newRow("unterminated array") << QByteArray("[1, 2");
    QTest::newRow("raw newline in string") << QByteArray("[\"a\nb\"]");
    QTest::newRow("raw tab in string") << QByteArray("[\"a\tb\"]");
    QTest::newRow("bad escape") << QByteArray(R"(["\q"])");
    QTest::newRow("short unicode escape") << QByteArray(R"(["\u12"])");
    QTest::newRow("bad hex") << QByteArray(R"(["\u12g4"])");
    QTest::newRow("lone high surrogate") << QByteArray(R"(["\uD800"])");
    QTest::newRow("lone low surrogate") << QByteArray(R"(["\uDC00"])");
    QTest::newRow("high then non-low") << QByteArray(R"(["\uD800\u0041"])");
    QTest::newRow("invalid utf8") << QByteArray("[\"\xFF\"]");
    QTest::newRow("overlong") << QByteArray("[\"\xC0\x80\"]");
    QTest::newRow("surrogate in utf8") << QByteArray("[\"\xED\xA0\x80\"]");
    QTest::newRow("truncated utf8") << QByteArray("[\"\xE3\x81\"]");
    QTest::newRow("above 10FFFF") << QByteArray("[\"\xF4\x90\x80\x80\"]");
    QTest::newRow("NUL byte") << QByteArray("{\"a\":1}\0", 8);
    QTest::newRow("literal typo") << QByteArray("[tru]");
    QTest::newRow("capital literal") << QByteArray("[True]");
    QTest::newRow("key not string") << QByteArray("{1:2}");
    QTest::newRow("missing colon") << QByteArray(R"({"a" 1})");
    QTest::newRow("two bom") << QByteArray("\xEF\xBB\xBF\xEF\xBB\xBF{}");
}

void TestPuppetManifest::jsonRejects()
{
    QFETCH(QByteArray, text);
    const auto r = json::parse(std::string_view(text.constData(), size_t(text.size())), {16, 1000, 4096});
    QVERIFY(!r.ok);
    QVERIFY(!r.error.empty());
}

void TestPuppetManifest::jsonLimits()
{
    const auto parse = [](const QString &t, json::Limits l) {
        const QByteArray b = t.toUtf8();
        return json::parse(std::string_view(b.constData(), size_t(b.size())), l);
    };
    // depth: 16 levels are fine, 17 are not (and 100000 do not blow the stack)
    QVERIFY(parse(nested(16), {16, 1000, 4096}).ok);
    QVERIFY(!parse(nested(17), {16, 1000, 4096}).ok);
    QVERIFY(!parse(nested(100000), {16, 1000, 4096}).ok);
    QVERIFY(!parse(QString(100000, u'{'), {16, 1000, 4096}).ok);
    // values: 10 values in an array of 9 plus the array
    QVERIFY(parse(u"[1,1,1,1,1,1,1,1,1]"_s, {16, 10, 4096}).ok);
    QVERIFY(!parse(u"[1,1,1,1,1,1,1,1,1,1]"_s, {16, 10, 4096}).ok);
    // strings: after unescaping
    QVERIFY(parse(u"[\""_s + QString(10, u'a') + u"\"]"_s, {16, 100, 10}).ok);
    QVERIFY(!parse(u"[\""_s + QString(11, u'a') + u"\"]"_s, {16, 100, 10}).ok);
    QVERIFY(parse(u"[\""_s + QString(10, u'\u00e9') + u"\"]"_s, {16, 100, 20}).ok);
    QVERIFY(!parse(u"[\""_s + QString(11, u'\u00e9') + u"\"]"_s, {16, 100, 20}).ok);
    // keys count against the string limit too
    QVERIFY(!parse(u"{\""_s + QString(11, u'k') + u"\":1}"_s, {16, 100, 10}).ok);
}

void TestPuppetManifest::utf8()
{
    QVERIFY(json::validUtf8("plain"));
    QVERIFY(json::validUtf8("\xE3\x81\x82"));
    QVERIFY(json::validUtf8("\xF0\x9F\x98\x80"));
    QVERIFY(json::validUtf8(""));
    QVERIFY(!json::validUtf8("\x80"));
    QVERIFY(!json::validUtf8("\xC1\xBF"));
    QVERIFY(!json::validUtf8("\xE0\x80\x80"));
    QVERIFY(!json::validUtf8("\xF0\x80\x80\x80"));
    QVERIFY(!json::validUtf8("\xED\xBF\xBF"));
    QVERIFY(!json::validUtf8("\xF5\x80\x80\x80"));
}

void TestPuppetManifest::pathRules_data()
{
    QTest::addColumn<QByteArray>("path");
    QTest::addColumn<bool>("ok");
    QTest::newRow("file") << QByteArray("a.png") << true;
    QTest::newRow("nested") << QByteArray("tex/a.png") << true;
    QTest::newRow("spaces") << QByteArray("my model/a b.png") << true;
    QTest::newRow("unicode") << QString::fromUtf8("テクスチャ/絵.png").toUtf8() << true;
    QTest::newRow("dot in name") << QByteArray("a..b.png") << true;
    QTest::newRow("dotfile") << QByteArray(".hidden") << true;
    QTest::newRow("empty") << QByteArray() << false;
    QTest::newRow("absolute") << QByteArray("/etc/passwd") << false;
    QTest::newRow("parent") << QByteArray("../a.png") << false;
    QTest::newRow("parent inside") << QByteArray("a/../b.png") << false;
    QTest::newRow("only parent") << QByteArray("..") << false;
    QTest::newRow("current") << QByteArray("./a.png") << false;
    QTest::newRow("only current") << QByteArray(".") << false;
    QTest::newRow("current inside") << QByteArray("a/./b.png") << false;
    QTest::newRow("double slash") << QByteArray("a//b.png") << false;
    QTest::newRow("trailing slash") << QByteArray("a/") << false;
    QTest::newRow("backslash") << QByteArray("a\\b.png") << false;
    QTest::newRow("windows parent") << QByteArray("..\\a.png") << false;
    QTest::newRow("drive") << QByteArray("C:/a.png") << false;
    QTest::newRow("drive relative") << QByteArray("c:a.png") << false;
    QTest::newRow("unc") << QByteArray("//host/share/a.png") << false;
    QTest::newRow("NUL") << QByteArray("a\0b.png", 7) << false;
    QTest::newRow("newline") << QByteArray("a\nb.png") << false;
    QTest::newRow("control") << QByteArray("a\x01.png") << false;
    QTest::newRow("DEL") << QByteArray("a\x7f.png") << false;
    QTest::newRow("invalid utf8") << QByteArray("a\xff.png") << false;
    QTest::newRow("component 255") << QByteArray(255, 'a') << true;
    QTest::newRow("component 256") << QByteArray(256, 'a') << false;
    QTest::newRow("component 255 unicode bytes") << QByteArray("\xE3\x81\x82").repeated(85) << true;
    QTest::newRow("component 258 unicode bytes") << QByteArray("\xE3\x81\x82").repeated(86) << false;
    QTest::newRow("path 1024") << (QByteArray(255, 'a') + "/" + QByteArray(255, 'b') + "/" + QByteArray(255, 'c') + "/" + QByteArray(254, 'd') + "/e") << true;
    QTest::newRow("path 1025") << (QByteArray(255, 'a') + "/" + QByteArray(255, 'b') + "/" + QByteArray(255, 'c') + "/" + QByteArray(255, 'd') + "/e") << false;
}

void TestPuppetManifest::pathRules()
{
    QFETCH(QByteArray, path);
    QFETCH(bool, ok);
    std::string why;
    QCOMPARE(relativePathOk(std::string_view(path.constData(), size_t(path.size())), &why), ok);
    if (!ok)
        QVERIFY(!why.empty());
}

// ---------------------------------------------------------------------------------------------------------------------------------------------

void TestPuppetManifest::aFullCubismModelIsRead()
{
    QTemporaryDir tmp;
    const QByteArray text = R"({
  "Version": 3,
  "FileReferences": {
    "Moc": "haru.moc3",
    "Textures": ["haru.2048/texture_00.png", "haru.2048/texture_01.png"],
    "Physics": "haru.physics3.json",
    "Pose": "haru.pose3.json",
    "DisplayInfo": "haru.cdi3.json",
    "UserData": "haru.userdata3.json",
    "Expressions": [{"Name": "smile", "File": "e/smile.exp3.json"}, {"Name": "cry", "File": "e/cry.exp3.json"}],
    "Motions": {
      "Idle": [{"File": "m/idle1.motion3.json", "FadeInTime": 0.5, "FadeOutTime": 0.25, "Sound": "s/a.wav"}, {"File": "m/idle2.motion3.json"}],
      "TapBody": [{"File": "m/tap.motion3.json"}]
    }
  },
  "Groups": [{"Target": "Parameter", "Name": "EyeBlink", "Ids": ["ParamEyeLOpen", "ParamEyeROpen"]}, {"Target": "Parameter", "Name": "LipSync", "Ids": ["ParamMouthOpenY"]}],
  "HitAreas": [{"Name": "Body", "Id": "HitArea"}],
  "Layout": {"center_x": 0, "width": 2}
})";
    const QString d = tmp.path();
    testutil::writeFile(d + u"/haru.model3.json"_s, text);
    for (const char *f : {"haru.moc3", "haru.2048/texture_00.png", "haru.2048/texture_01.png", "haru.physics3.json", "haru.pose3.json", "haru.cdi3.json", "haru.userdata3.json",
                          "e/smile.exp3.json", "e/cry.exp3.json", "m/idle1.motion3.json", "m/idle2.motion3.json", "m/tap.motion3.json", "s/a.wav"})
        testutil::writeFile(d + u'/' + QString::fromLatin1(f), "x");
    const ManifestResult r = loadManifest(d.toStdString());
    QVERIFY2(r.ok, qPrintable(reasonsText(r)));
    QVERIFY(r.format && *r.format == Format::Cubism);
    const Manifest &m = r.manifest;
    QCOMPARE(m.version, 3);
    QCOMPARE(QString::fromStdString(m.file), u"haru.model3.json"_s);
    QCOMPARE(QString::fromStdString(m.moc), u"haru.moc3"_s);
    QCOMPARE(m.textures.size(), size_t(2));
    QCOMPARE(QString::fromStdString(m.textures[1]), u"haru.2048/texture_01.png"_s);
    QCOMPARE(QString::fromStdString(m.physics), u"haru.physics3.json"_s);
    QCOMPARE(QString::fromStdString(m.pose), u"haru.pose3.json"_s);
    QCOMPARE(QString::fromStdString(m.displayInfo), u"haru.cdi3.json"_s);
    QCOMPARE(QString::fromStdString(m.userData), u"haru.userdata3.json"_s);
    QCOMPARE(m.expressions.size(), size_t(2));
    QCOMPARE(QString::fromStdString(m.expressions[0].name), u"smile"_s);
    QCOMPARE(m.motionGroups.size(), size_t(2));
    QCOMPARE(QString::fromStdString(m.motionGroups[0].name), u"Idle"_s);
    QCOMPARE(m.motionGroups[0].motions.size(), size_t(2));
    QCOMPARE(*m.motionGroups[0].motions[0].fadeIn, 0.5);
    QCOMPARE(*m.motionGroups[0].motions[0].fadeOut, 0.25);
    QVERIFY(!m.motionGroups[0].motions[1].fadeIn);
    QCOMPARE(QString::fromStdString(m.motionGroups[0].motions[0].sound), u"s/a.wav"_s);
    QCOMPARE(m.motionCount(), 3);
    QCOMPARE(m.parameterGroups.size(), size_t(2));
    QCOMPARE(QString::fromStdString(m.parameterGroups[0].name), u"EyeBlink"_s);
    QCOMPARE(m.parameterGroups[0].ids.size(), size_t(2));
    QCOMPARE(m.hitAreas.size(), size_t(1));
    QCOMPARE(QString::fromStdString(m.hitAreas[0].id), u"HitArea"_s);
    QVERIFY(m.fileCount == 18); // 14 files (the manifest and 13 named) and four folders (haru.2048, e, m, s)

}

void TestPuppetManifest::validManifests_data()
{
    QTest::addColumn<QByteArray>("text");
    QTest::newRow("minimal") << manifestText();
    QTest::newRow("version 3.0") << QByteArray(R"({"Version": 3.0, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}})");
    QTest::newRow("exponent version") << QByteArray(R"({"Version": 0.3e1, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}})");
    QTest::newRow("bom") << (QByteArray("\xEF\xBB\xBF") + manifestText());
    QTest::newRow("unknown keys are ignored") << QByteArray(R"({"Version": 3, "Whatever": [1, {"x": null}], "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"], "Extra": 1}})");
    QTest::newRow("depth 16 in an ignored key") << QByteArray(("{\"Version\": 3, \"x\": " + nested(15) + ", \"FileReferences\": {\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"]}}").toUtf8());
    QTest::newRow("empty optionals") << QByteArray(R"({"Version": 3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"], "Expressions": [], "Motions": {}}, "Groups": [], "HitAreas": []})");
    QTest::newRow("empty motion group name") << QByteArray(R"({"Version": 3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"], "Motions": {"": []}}})");
    QTest::newRow("same texture twice") << manifestText(u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\", \"t0.png\"]"_s);
    QTest::newRow("layout") << QByteArray(R"({"Version": 3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}, "Layout": {"x": 1, "y": -2.5, "width": 3}})");
    QTest::newRow("lots of space") << QByteArray(" \n\t " + manifestText() + "\r\n\r\n");
    QTest::newRow("escaped unicode in a name") << QByteArray(R"({"Version": 3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"], "Expressions": [{"Name": "\u3042", "File": "a.exp3.json"}]}})");
}

void TestPuppetManifest::validManifests()
{
    QFETCH(QByteArray, text);
    QTemporaryDir tmp;
    writeModel(tmp.path(), text);
    testutil::writeFile(tmp.path() + u"/a.exp3.json"_s, "{}");
    const ManifestResult r = loadManifest(tmp.path().toStdString());
    QVERIFY2(r.ok, qPrintable(reasonsText(r)));
    QVERIFY(r.reasons.empty());
}

void TestPuppetManifest::invalidManifests_data()
{
    QTest::addColumn<QByteArray>("text");
    QTest::addColumn<QString>("name");
    const QString invalid = s(kManifestInvalid);
    const auto row = [&](const char *tag, const QByteArray &text, const QString &name = {}) { QTest::newRow(tag) << text << (name.isEmpty() ? invalid : name); };
    const auto fr = [](const char *refs) { return QByteArray("{\"Version\": 3, \"FileReferences\": {") + refs + "}}"; };
    row("empty file", QByteArray());
    row("not json", QByteArray("hello"));
    row("array", QByteArray("[]"));
    row("number", QByteArray("3"));
    row("no Version", QByteArray(R"({"FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}})"));
    row("Version string", QByteArray(R"({"Version": "3", "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}})"));
    row("Version null", QByteArray(R"({"Version": null, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}})"));
    row("Version 2", QByteArray(R"({"Version": 2, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}})"), s(kUnsupportedManifestVersion));
    row("Version 4", QByteArray(R"({"Version": 4, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}})"), s(kUnsupportedManifestVersion));
    row("Version 3.5", QByteArray(R"({"Version": 3.5, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}})"), s(kUnsupportedManifestVersion));
    row("Version negative", QByteArray(R"({"Version": -3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}})"), s(kUnsupportedManifestVersion));
    row("Version huge", QByteArray(R"({"Version": 1e300, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}})"), s(kUnsupportedManifestVersion));
    row("no FileReferences", QByteArray(R"({"Version": 3})"));
    row("FileReferences array", QByteArray(R"({"Version": 3, "FileReferences": []})"));
    row("no Moc", fr(R"("Textures": ["t0.png"])"));
    row("Moc number", fr(R"("Moc": 1, "Textures": ["t0.png"])"));
    row("Moc empty", fr(R"("Moc": "", "Textures": ["t0.png"])"), s(kUnsafePath));
    row("no Textures", fr(R"("Moc": "m.moc3")"));
    row("Textures string", fr(R"("Moc": "m.moc3", "Textures": "t0.png")"));
    row("Textures empty", fr(R"("Moc": "m.moc3", "Textures": [])"));
    row("Textures with number", fr(R"("Moc": "m.moc3", "Textures": [1])"));
    row("Textures with null", fr(R"("Moc": "m.moc3", "Textures": [null])"));
    row("Physics number", fr(R"("Moc": "m.moc3", "Textures": ["t0.png"], "Physics": 1)"));
    row("Pose object", fr(R"("Moc": "m.moc3", "Textures": ["t0.png"], "Pose": {})"));
    row("DisplayInfo array", fr(R"("Moc": "m.moc3", "Textures": ["t0.png"], "DisplayInfo": [])"));
    row("UserData bool", fr(R"("Moc": "m.moc3", "Textures": ["t0.png"], "UserData": true)"));
    row("Expressions object", fr(R"("Moc": "m.moc3", "Textures": ["t0.png"], "Expressions": {})"));
    row("Expressions item string", fr(R"("Moc": "m.moc3", "Textures": ["t0.png"], "Expressions": ["a"])"));
    row("Expression without File", fr(R"("Moc": "m.moc3", "Textures": ["t0.png"], "Expressions": [{"Name": "a"}])"));
    row("Expression without Name", fr(R"("Moc": "m.moc3", "Textures": ["t0.png"], "Expressions": [{"File": "a.exp3.json"}])"));
    row("Expression Name empty", fr(R"("Moc": "m.moc3", "Textures": ["t0.png"], "Expressions": [{"Name": "", "File": "a.exp3.json"}])"));
    row("Expression Name number", fr(R"("Moc": "m.moc3", "Textures": ["t0.png"], "Expressions": [{"Name": 1, "File": "a.exp3.json"}])"));
    row("Expression Name twice", fr(R"("Moc": "m.moc3", "Textures": ["t0.png"], "Expressions": [{"Name": "a", "File": "a.exp3.json"}, {"Name": "a", "File": "b.exp3.json"}])"));
    row("Motions array", fr(R"("Moc": "m.moc3", "Textures": ["t0.png"], "Motions": [])"));
    row("Motions group object", fr(R"("Moc": "m.moc3", "Textures": ["t0.png"], "Motions": {"Idle": {}})"));
    row("Motion string", fr(R"("Moc": "m.moc3", "Textures": ["t0.png"], "Motions": {"Idle": ["a"]})"));
    row("Motion without File", fr(R"("Moc": "m.moc3", "Textures": ["t0.png"], "Motions": {"Idle": [{}]})"));
    row("Motion File number", fr(R"("Moc": "m.moc3", "Textures": ["t0.png"], "Motions": {"Idle": [{"File": 1}]})"));
    row("Fade negative", fr(R"("Moc": "m.moc3", "Textures": ["t0.png"], "Motions": {"Idle": [{"File": "a.motion3.json", "FadeInTime": -1}]})"));
    row("Fade string", fr(R"("Moc": "m.moc3", "Textures": ["t0.png"], "Motions": {"Idle": [{"File": "a.motion3.json", "FadeOutTime": "1"}]})"));
    row("Fade huge", fr(R"("Moc": "m.moc3", "Textures": ["t0.png"], "Motions": {"Idle": [{"File": "a.motion3.json", "FadeOutTime": 1e9}]})"));
    row("Sound number", fr(R"("Moc": "m.moc3", "Textures": ["t0.png"], "Motions": {"Idle": [{"File": "a.motion3.json", "Sound": 1}]})"));
    row("Groups object", QByteArray(R"({"Version": 3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}, "Groups": {}})"));
    row("Group without Ids", QByteArray(R"({"Version": 3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}, "Groups": [{"Target": "Parameter", "Name": "EyeBlink"}]})"));
    row("Group Ids with number", QByteArray(R"({"Version": 3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}, "Groups": [{"Target": "Parameter", "Name": "EyeBlink", "Ids": [1]}]})"));
    row("Group Name number", QByteArray(R"({"Version": 3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}, "Groups": [{"Target": "Parameter", "Name": 1, "Ids": []}]})"));
    row("Group Target missing", QByteArray(R"({"Version": 3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}, "Groups": [{"Name": "x", "Ids": []}]})"));
    row("HitAreas without Id", QByteArray(R"({"Version": 3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}, "HitAreas": [{"Name": "Body"}]})"));
    row("HitAreas Name number", QByteArray(R"({"Version": 3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}, "HitAreas": [{"Name": 1, "Id": "x"}]})"));
    row("HitAreas item array", QByteArray(R"({"Version": 3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}, "HitAreas": [[]]})"));
    row("Layout array", QByteArray(R"({"Version": 3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}, "Layout": []})"));
    row("Layout width string", QByteArray(R"({"Version": 3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}, "Layout": {"width": "2"}})"));
    row("repeated key", QByteArray(R"({"Version": 3, "Version": 3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}})"));
    row("repeated nested key", QByteArray(R"({"Version": 3, "FileReferences": {"Moc": "m.moc3", "Moc": "n.moc3", "Textures": ["t0.png"]}})"));
    row("repeated motion group", QByteArray(R"({"Version": 3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"], "Motions": {"Idle": [], "Idle": []}}})"));
    row("trailing comma", QByteArray(R"({"Version": 3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"],}})"));
    row("comment", QByteArray("{\"Version\": 3, // x\n \"FileReferences\": {\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"]}}"));
    row("NaN", QByteArray(R"({"Version": NaN, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}})"));
    row("invalid utf8", QByteArray("{\"Version\": 3, \"x\": \"\xff\", \"FileReferences\": {\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"]}}"));
    row("truncated", manifestText().left(40));
    row("NUL inside", QByteArray(manifestText()).replace("Version", QByteArray("Ver\0sion", 8)));
    row("depth 17 in an ignored key", QByteArray(("{\"Version\": 3, \"x\": " + nested(16) + ", \"FileReferences\": {\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"]}}").toUtf8()));
    row("string over the limit", QByteArray("{\"Version\": 3, \"x\": \"") + QByteArray(4097, 'a') + "\", \"FileReferences\": {\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"]}}");
    row("id over the limit", QByteArray(R"({"Version": 3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}, "HitAreas": [{"Name": ")") + QByteArray(129, 'n') + R"(", "Id": "x"}]})");
}

void TestPuppetManifest::invalidManifests()
{
    QFETCH(QByteArray, text);
    QFETCH(QString, name);
    QTemporaryDir tmp;
    writeModel(tmp.path(), text);
    const ManifestResult r = loadManifest(tmp.path().toStdString());
    QVERIFY2(!r.ok, qPrintable(QTest::currentDataTag()));
    // "depth 17": a nested(16) array is at depth 2..17 inside the object, so it is the case that goes over
    QCOMPARE(firstReasonName(r), name);
    QVERIFY(!r.reasons.empty());
}

void TestPuppetManifest::namedPathsThatLeaveTheFolder_data()
{
    QTest::addColumn<QString>("path");
    QTest::newRow("parent") << u"../outside.png"_s;
    QTest::newRow("deep parent") << u"a/../../outside.png"_s;
    QTest::newRow("absolute") << u"/etc/hostname"_s;
    QTest::newRow("absolute of the folder") << u"%FOLDER%/t0.png"_s;
    QTest::newRow("backslash") << u"..\\\\outside.png"_s;
    QTest::newRow("windows drive") << u"C:/t0.png"_s;
    QTest::newRow("current dir") << u"./t0.png"_s;
    QTest::newRow("double slash") << u"sub//t0.png"_s;
    QTest::newRow("trailing slash") << u"sub/"_s;
    QTest::newRow("percent encoded") << u"..%2foutside.png"_s; // not a traversal on disk: it is simply a missing file
}

void TestPuppetManifest::namedPathsThatLeaveTheFolder()
{
    QFETCH(QString, path);
    QTemporaryDir tmp;
    const QString folder = tmp.path() + u"/model"_s;
    testutil::writeFile(tmp.path() + u"/outside.png"_s, "x"); // exists, next to the folder
    path.replace(u"%FOLDER%"_s, folder);
    writeModel(folder, manifestText(u"\"Moc\": \"m.moc3\", \"Textures\": [\""_s + path + u"\"]"_s));
    const ManifestResult r = loadManifest(folder.toStdString());
    QVERIFY2(!r.ok, qPrintable(path));
    if (path.contains(u"%2f"_s))
        QCOMPARE(firstReasonName(r), s(kFileMissing));
    else
        QCOMPARE(firstReasonName(r), s(kUnsafePath));
    // the same names in the other roles
    for (const QString &role : {u"Moc"_s, u"Physics"_s, u"Pose"_s, u"DisplayInfo"_s, u"UserData"_s}) {
        QString refs = u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"], \""_s + role + u"\": \""_s + path + u"\""_s;
        if (role == u"Moc"_s)
            refs = u"\"Moc\": \""_s + path + u"\", \"Textures\": [\"t0.png\"]"_s;
        writeModel(folder, manifestText(refs));
        QVERIFY2(!loadManifest(folder.toStdString()).ok, qPrintable(role + u' ' + path));
    }
    writeModel(folder, manifestText(u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"], \"Expressions\": [{\"Name\": \"a\", \"File\": \""_s + path + u"\"}]"_s));
    QVERIFY(!loadManifest(folder.toStdString()).ok);
    writeModel(folder, manifestText(u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"], \"Motions\": {\"Idle\": [{\"File\": \""_s + path + u"\"}]}"_s));
    QVERIFY(!loadManifest(folder.toStdString()).ok);
    // a Sound that is never loaded still has to be a safe name
    writeModel(folder, manifestText(u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"], \"Motions\": {\"Idle\": [{\"File\": \"m.moc3\", \"Sound\": \""_s + path + u"\"}]}"_s));
    QVERIFY(!loadManifest(folder.toStdString()).ok);
}

void TestPuppetManifest::missingAndWrongFiles()
{
    QTemporaryDir tmp;
    const QString d = tmp.path();
    // a missing texture, moc, physics and so on
    writeModel(d, manifestText(), false);
    ManifestResult r = loadManifest(d.toStdString());
    QVERIFY(!r.ok);
    QCOMPARE(firstReasonName(r), s(kFileMissing));
    QCOMPARE(r.reasons.size(), size_t(2)); // the moc and the texture, both said
    testutil::writeFile(d + u"/m.moc3"_s, "MOC3");
    r = loadManifest(d.toStdString());
    QCOMPARE(r.reasons.size(), size_t(1));
    QVERIFY(reasonsText(r).contains(u"t0.png"_s));
    // a different case is a different file
    testutil::writeFile(d + u"/T0.png"_s, "png");
    r = loadManifest(d.toStdString());
    QCOMPARE(firstReasonName(r), s(kFileMissing));
    testutil::writeFile(d + u"/t0.png"_s, "png");
    QVERIFY(loadManifest(d.toStdString()).ok);
    // a folder is not a file
    QVERIFY(QFile::remove(d + u"/t0.png"_s));
    QVERIFY(QDir().mkpath(d + u"/t0.png"_s));
    r = loadManifest(d.toStdString());
    QVERIFY(!r.ok);
    QVERIFY(QDir(d + u"/t0.png"_s).removeRecursively());
    testutil::writeFile(d + u"/t0.png"_s, "png");
    // the roles and their extensions
    struct Case { const char *refs; const char *file; };
    const Case wrong[] = {
        {R"("Moc": "m.bin", "Textures": ["t0.png"])", "m.bin"},
        {R"("Moc": "m.moc3", "Textures": ["t0.jpg"])", "t0.jpg"},
        {R"("Moc": "m.moc3", "Textures": ["t0.png"], "Physics": "p.json")", "p.json"},
        {R"("Moc": "m.moc3", "Textures": ["t0.png"], "Pose": "p.physics3.json")", "p.physics3.json"},
        {R"("Moc": "m.moc3", "Textures": ["t0.png"], "DisplayInfo": "c.json")", "c.json"},
        {R"("Moc": "m.moc3", "Textures": ["t0.png"], "UserData": "u.txt")", "u.txt"},
        {R"("Moc": "m.moc3", "Textures": ["t0.png"], "Expressions": [{"Name": "a", "File": "a.motion3.json"}])", "a.motion3.json"},
        {R"("Moc": "m.moc3", "Textures": ["t0.png"], "Motions": {"Idle": [{"File": "a.exp3.json"}]})", "a.exp3.json"},
        {R"("Moc": "m.moc3", "Textures": ["t0.png"], "Motions": {"Idle": [{"File": "a"}]})", "a"},
    };
    for (const Case &c : wrong) {
        testutil::writeFile(d + u'/' + QString::fromLatin1(c.file), "x");
        writeModel(d, manifestText(QString::fromLatin1(c.refs)));
        r = loadManifest(d.toStdString());
        QVERIFY2(!r.ok, c.refs);
        QCOMPARE(firstReasonName(r), s(kUnsupportedFormat));
    }
    // upper-case extensions are accepted for the roles
    testutil::writeFile(d + u"/A.PNG"_s, "x");
    writeModel(d, manifestText(u"\"Moc\": \"m.moc3\", \"Textures\": [\"A.PNG\"]"_s));
    QVERIFY(loadManifest(d.toStdString()).ok);
}

void TestPuppetManifest::symbolicLinksAndSpecialFiles()
{
    QTemporaryDir tmp;
    const QString d = tmp.path() + u"/model"_s;
    writeModel(d);
    testutil::writeFile(tmp.path() + u"/secret.png"_s, "secret");
    QVERIFY(loadManifest(d.toStdString()).ok);
    // a link to a file outside, not even named
    QVERIFY(QFile::link(tmp.path() + u"/secret.png"_s, d + u"/unused.png"_s));
    ManifestResult r = loadManifest(d.toStdString());
    QVERIFY(!r.ok);
    QCOMPARE(firstReasonName(r), s(kUnsafePath));
    QVERIFY(QFile::remove(d + u"/unused.png"_s));
    // a link to a file inside
    QVERIFY(QFile::link(d + u"/t0.png"_s, d + u"/alias.png"_s));
    QCOMPARE(firstReasonName(loadManifest(d.toStdString())), s(kUnsafePath));
    QVERIFY(QFile::remove(d + u"/alias.png"_s));
    // a dangling link
    QVERIFY(QFile::link(d + u"/nowhere"_s, d + u"/dangling"_s));
    QCOMPARE(firstReasonName(loadManifest(d.toStdString())), s(kUnsafePath));
    QVERIFY(QFile::remove(d + u"/dangling"_s));
    // a link to a folder outside, and a link inside a sub folder
    QVERIFY(QDir().mkpath(tmp.path() + u"/outdir"_s));
    QVERIFY(QFile::link(tmp.path() + u"/outdir"_s, d + u"/linkdir"_s));
    QCOMPARE(firstReasonName(loadManifest(d.toStdString())), s(kUnsafePath));
    QVERIFY(QFile::remove(d + u"/linkdir"_s));
    QVERIFY(QDir().mkpath(d + u"/sub"_s));
    QVERIFY(QFile::link(tmp.path() + u"/secret.png"_s, d + u"/sub/x.png"_s));
    QCOMPARE(firstReasonName(loadManifest(d.toStdString())), s(kUnsafePath));
    QVERIFY(QFile::remove(d + u"/sub/x.png"_s));
    // a named texture that is a link
    QVERIFY(QFile::remove(d + u"/t0.png"_s));
    QVERIFY(QFile::link(tmp.path() + u"/secret.png"_s, d + u"/t0.png"_s));
    r = loadManifest(d.toStdString());
    QVERIFY(!r.ok);
    QCOMPARE(firstReasonName(r), s(kUnsafePath));
    QVERIFY(QFile::remove(d + u"/t0.png"_s));
    testutil::writeFile(d + u"/t0.png"_s, "png");
    // the manifest itself a link
    QVERIFY(QFile::rename(d + u"/m.model3.json"_s, tmp.path() + u"/real.model3.json"_s));
    QVERIFY(QFile::link(tmp.path() + u"/real.model3.json"_s, d + u"/m.model3.json"_s));
    QCOMPARE(firstReasonName(loadManifest(d.toStdString())), s(kUnsafePath));
    QVERIFY(QFile::remove(d + u"/m.model3.json"_s));
    testutil::writeFile(d + u"/m.model3.json"_s, manifestText());
    QVERIFY(loadManifest(d.toStdString()).ok);
    // a named pipe
    QVERIFY(::mkfifo(qPrintable(d + u"/pipe"_s), 0600) == 0);
    r = loadManifest(d.toStdString());
    QVERIFY(!r.ok);
    QCOMPARE(firstReasonName(r), s(kUnsafePath));
    QVERIFY(QFile::remove(d + u"/pipe"_s));
    QVERIFY(loadManifest(d.toStdString()).ok);
}

void TestPuppetManifest::theRootMayBeALink()
{
    QTemporaryDir tmp;
    writeModel(tmp.path() + u"/real"_s);
    QVERIFY(QFile::link(tmp.path() + u"/real"_s, tmp.path() + u"/link"_s));
    const ManifestResult r = loadManifest((tmp.path() + u"/link"_s).toStdString());
    QVERIFY2(r.ok, qPrintable(reasonsText(r)));
    QCOMPARE(QString::fromStdString(r.manifest.folder), QFileInfo(tmp.path() + u"/real"_s).canonicalFilePath());
    // a trailing slash and a dot component are fine for the root
    QVERIFY(loadManifest((tmp.path() + u"/real/"_s).toStdString()).ok);
    QVERIFY(loadManifest((tmp.path() + u"/real/../real"_s).toStdString()).ok);
}

void TestPuppetManifest::notAPuppet()
{
    QTemporaryDir tmp;
    const QString name = s(kNotAPuppet);
    // missing
    ManifestResult r = loadManifest((tmp.path() + u"/nothing"_s).toStdString());
    QVERIFY(!r.ok);
    QCOMPARE(firstReasonName(r), name);
    // an empty path
    QCOMPARE(firstReasonName(loadManifest("")), name);
    // a file
    testutil::writeFile(tmp.path() + u"/f.model3.json"_s, manifestText());
    QCOMPARE(firstReasonName(loadManifest((tmp.path() + u"/f.model3.json"_s).toStdString())), name);
    // an empty folder
    QVERIFY(QDir().mkpath(tmp.path() + u"/empty"_s));
    QCOMPARE(firstReasonName(loadManifest((tmp.path() + u"/empty"_s).toStdString())), name);
    // other files only
    testutil::writeFile(tmp.path() + u"/other/a.gif"_s, "GIF89a");
    QCOMPARE(firstReasonName(loadManifest((tmp.path() + u"/other"_s).toStdString())), name);
    // a manifest in a sub folder is not a manifest of the item
    writeModel(tmp.path() + u"/nested/inner"_s);
    QCOMPARE(firstReasonName(loadManifest((tmp.path() + u"/nested"_s).toStdString())), name);
    // two manifests
    writeModel(tmp.path() + u"/two"_s);
    testutil::writeFile(tmp.path() + u"/two/n.model3.json"_s, manifestText());
    QCOMPARE(firstReasonName(loadManifest((tmp.path() + u"/two"_s).toStdString())), name);
    // the name needs a stem before the suffix, and the suffix is exact
    testutil::writeFile(tmp.path() + u"/dot/.model3.json"_s, manifestText());
    QCOMPARE(firstReasonName(loadManifest((tmp.path() + u"/dot"_s).toStdString())), name); // no stem: not a manifest
    testutil::writeFile(tmp.path() + u"/upper/M.MODEL3.JSON"_s, manifestText());
    testutil::writeFile(tmp.path() + u"/upper/m.moc3"_s, "x");
    testutil::writeFile(tmp.path() + u"/upper/t0.png"_s, "x");
    QVERIFY(loadManifest((tmp.path() + u"/upper"_s).toStdString()).format.has_value()); // detected: the extension is compared without regard to case
}

// ---------------------------------------------------------------------------------------------------------------------------------------------

namespace {

QString texturesRefs(int n)
{
    QStringList items;
    for (int i = 0; i < n; ++i)
        items << u"\"t0.png\""_s;
    return u"\"Moc\": \"m.moc3\", \"Textures\": ["_s + items.join(u","_s) + u"]"_s;
}

QString motionGroupsRefs(int groups, int perGroup)
{
    QStringList gs;
    for (int g = 0; g < groups; ++g) {
        QStringList ms;
        for (int i = 0; i < perGroup; ++i)
            ms << u"{\"File\": \"a.motion3.json\"}"_s;
        gs << u"\"G%1\": [%2]"_s.arg(g).arg(ms.join(u","_s));
    }
    return u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"], \"Motions\": {"_s + gs.join(u","_s) + u"}"_s;
}

QString expressionsRefs(int n)
{
    QStringList items;
    for (int i = 0; i < n; ++i)
        items << u"{\"Name\": \"e%1\", \"File\": \"a.exp3.json\"}"_s.arg(i);
    return u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"], \"Expressions\": ["_s + items.join(u","_s) + u"]"_s;
}

QByteArray withTop(const QString &key, const QString &array)
{
    return ("{\"Version\": 3, \"FileReferences\": {\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"]}, \"" + key + "\": " + array + "}").toUtf8();
}

QString hitAreas(int n)
{
    QStringList items;
    for (int i = 0; i < n; ++i)
        items << u"{\"Name\": \"h%1\", \"Id\": \"d%1\"}"_s.arg(i);
    return u"["_s + items.join(u","_s) + u"]"_s;
}

QString groups(int n, int ids)
{
    QStringList items;
    for (int i = 0; i < n; ++i) {
        QStringList list;
        for (int k = 0; k < ids; ++k)
            list << u"\"p%1\""_s.arg(k);
        items << u"{\"Target\": \"Parameter\", \"Name\": \"g%1\", \"Ids\": [%2]}"_s.arg(i).arg(list.join(u","_s));
    }
    return u"["_s + items.join(u","_s) + u"]"_s;
}

} // namespace

void TestPuppetManifest::countLimits_data()
{
    QTest::addColumn<QByteArray>("atLimit");
    QTest::addColumn<QByteArray>("overLimit");
    QTest::addColumn<QString>("what");
    QTest::newRow("textures") << manifestText(texturesRefs(int(kMaxTextures))) << manifestText(texturesRefs(int(kMaxTextures) + 1)) << u"kMaxTextures"_s;
    QTest::newRow("motion groups") << manifestText(motionGroupsRefs(int(kMaxMotionGroups), 1)) << manifestText(motionGroupsRefs(int(kMaxMotionGroups) + 1, 1)) << u"kMaxMotionGroups"_s;
    QTest::newRow("motions per group") << manifestText(motionGroupsRefs(1, int(kMaxMotionsPerGroup))) << manifestText(motionGroupsRefs(1, int(kMaxMotionsPerGroup) + 1)) << u"kMaxMotionsPerGroup"_s;
    QTest::newRow("motions in all") << manifestText(motionGroupsRefs(int(kMaxMotions / kMaxMotionsPerGroup), int(kMaxMotionsPerGroup)))
                                    << manifestText(motionGroupsRefs(int(kMaxMotions / kMaxMotionsPerGroup) + 1, int(kMaxMotionsPerGroup))) << u"kMaxMotions"_s;
    QTest::newRow("expressions") << manifestText(expressionsRefs(int(kMaxExpressions))) << manifestText(expressionsRefs(int(kMaxExpressions) + 1)) << u"kMaxExpressions"_s;
    QTest::newRow("hit areas") << withTop(u"HitAreas"_s, hitAreas(int(kMaxHitAreas))) << withTop(u"HitAreas"_s, hitAreas(int(kMaxHitAreas) + 1)) << u"kMaxHitAreas"_s;
    QTest::newRow("parameter groups") << withTop(u"Groups"_s, groups(int(kMaxParameterGroups), 1)) << withTop(u"Groups"_s, groups(int(kMaxParameterGroups) + 1, 1)) << u"kMaxParameterGroups"_s;
    QTest::newRow("ids per group") << withTop(u"Groups"_s, groups(1, int(kMaxIdsPerGroup))) << withTop(u"Groups"_s, groups(1, int(kMaxIdsPerGroup) + 1)) << u"kMaxIdsPerGroup"_s;
}

void TestPuppetManifest::countLimits()
{
    QFETCH(QByteArray, atLimit);
    QFETCH(QByteArray, overLimit);
    QFETCH(QString, what);
    QTemporaryDir tmp;
    const QString d = tmp.path();
    writeModel(d, atLimit);
    testutil::writeFile(d + u"/a.motion3.json"_s, "{}");
    testutil::writeFile(d + u"/a.exp3.json"_s, "{}");
    ManifestResult r = loadManifest(d.toStdString());
    QVERIFY2(r.ok, qPrintable(what + u": "_s + reasonsText(r)));
    writeModel(d, overLimit);
    r = loadManifest(d.toStdString());
    QVERIFY2(!r.ok, qPrintable(what));
    QCOMPARE(firstReasonName(r), s(kTooLarge));
    QVERIFY2(reasonsText(r).contains(what), qPrintable(reasonsText(r)));
}

void TestPuppetManifest::sizeLimits()
{
    QTemporaryDir tmp;
    const QString d = tmp.path();
    struct Case { QString refs; QString file; qint64 limit; const char *what; };
    const Case cases[] = {
        {u"\"Moc\": \"x.moc3\", \"Textures\": [\"t0.png\"]"_s, u"x.moc3"_s, kMaxMocBytes, "kMaxMocBytes"},
        {u"\"Moc\": \"m.moc3\", \"Textures\": [\"big.png\"]"_s, u"big.png"_s, kMaxTextureFileBytes, "kMaxTextureFileBytes"},
        {u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"], \"Physics\": \"x.physics3.json\""_s, u"x.physics3.json"_s, kMaxPhysicsFileBytes, "kMaxPhysicsFileBytes"},
        {u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"], \"Pose\": \"x.pose3.json\""_s, u"x.pose3.json"_s, kMaxPoseFileBytes, "kMaxPoseFileBytes"},
        {u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"], \"DisplayInfo\": \"x.cdi3.json\""_s, u"x.cdi3.json"_s, kMaxDisplayInfoFileBytes, "kMaxDisplayInfoFileBytes"},
        {u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"], \"UserData\": \"x.userdata3.json\""_s, u"x.userdata3.json"_s, kMaxUserDataFileBytes, "kMaxUserDataFileBytes"},
        {u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"], \"Expressions\": [{\"Name\": \"a\", \"File\": \"x.exp3.json\"}]"_s, u"x.exp3.json"_s, kMaxExpressionFileBytes, "kMaxExpressionFileBytes"},
        {u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"], \"Motions\": {\"I\": [{\"File\": \"x.motion3.json\"}]}"_s, u"x.motion3.json"_s, kMaxMotionFileBytes, "kMaxMotionFileBytes"},
    };
    for (const Case &c : cases) {
        QVERIFY(QDir(d).removeRecursively());
        QVERIFY(QDir().mkpath(d));
        writeModel(d, manifestText(c.refs));
        sparse(d + u'/' + c.file, c.limit);
        ManifestResult r = loadManifest(d.toStdString());
        QVERIFY2(r.ok, c.what);
        sparse(d + u'/' + c.file, c.limit + 1);
        r = loadManifest(d.toStdString());
        QVERIFY2(!r.ok, c.what);
        QCOMPARE(firstReasonName(r), s(kTooLarge));
        QVERIFY2(reasonsText(r).contains(QString::fromLatin1(c.what)), qPrintable(reasonsText(r)));
    }
    // the manifest itself: exactly the limit is fine (padded with spaces), one byte more is not
    QVERIFY(QDir(d).removeRecursively());
    QVERIFY(QDir().mkpath(d));
    QByteArray padded = manifestText();
    padded += QByteArray(int(kMaxManifestBytes) - padded.size(), ' ');
    writeModel(d, padded);
    QVERIFY(loadManifest(d.toStdString()).ok);
    writeModel(d, padded + ' ');
    ManifestResult r = loadManifest(d.toStdString());
    QVERIFY(!r.ok);
    QCOMPARE(firstReasonName(r), s(kTooLarge));
    QVERIFY(reasonsText(r).contains(u"kMaxManifestBytes"_s));
    // a manifest of 3 GiB is refused without being read (it is a sparse file: reading it would take a while)
    sparse(d + u"/m.model3.json"_s, qint64(3) << 30);
    QElapsedTimer timer;
    timer.start();
    r = loadManifest(d.toStdString());
    QVERIFY(!r.ok);
    QVERIFY2(timer.elapsed() < 2000, "a huge manifest must be refused from its size");
    // the total of the item
    QVERIFY(QDir(d).removeRecursively());
    QVERIFY(QDir().mkpath(d));
    writeModel(d);
    const qint64 used = QFileInfo(d + u"/m.model3.json"_s).size() + 4 + 3;
    sparse(d + u"/filler.bin"_s, kMaxItemBytes - used);
    r = loadManifest(d.toStdString());
    QVERIFY2(r.ok, qPrintable(reasonsText(r)));
    QCOMPARE(r.manifest.totalBytes, kMaxItemBytes);
    sparse(d + u"/filler.bin"_s, kMaxItemBytes - used + 1);
    r = loadManifest(d.toStdString());
    QVERIFY(!r.ok);
    QCOMPARE(firstReasonName(r), s(kTooLarge));
    QVERIFY(reasonsText(r).contains(u"kMaxItemBytes"_s));
}

void TestPuppetManifest::folderLimits()
{
    QTemporaryDir tmp;
    const QString d = tmp.path();
    writeModel(d); // 3 files
    // entries: the limit counts every file and folder (the three files, the folder 'f', and what is in it)
    QVERIFY(QDir().mkpath(d + u"/f"_s));
    for (int i = 0; i < int(kMaxItemFiles) - 4; ++i)
        testutil::writeFile(d + u"/f/%1.bin"_s.arg(i), "");
    ManifestResult r = loadManifest(d.toStdString());
    QVERIFY2(r.ok, qPrintable(reasonsText(r)));
    QCOMPARE(r.manifest.fileCount, kMaxItemFiles);
    testutil::writeFile(d + u"/f/extra.bin"_s, "");
    r = loadManifest(d.toStdString());
    QVERIFY(!r.ok);
    QCOMPARE(firstReasonName(r), s(kTooLarge));
    QVERIFY(reasonsText(r).contains(u"kMaxItemFiles"_s));
    // depth
    QVERIFY(QDir(d + u"/f"_s).removeRecursively());
    QString deep = d;
    for (int i = 0; i < int(kMaxItemDirDepth); ++i)
        deep += u"/d"_s;
    testutil::writeFile(deep + u"/x.bin"_s, "");
    QVERIFY(loadManifest(d.toStdString()).ok);
    testutil::writeFile(deep + u"/d/x.bin"_s, "");
    r = loadManifest(d.toStdString());
    QVERIFY(!r.ok);
    QCOMPARE(firstReasonName(r), s(kTooLarge));
    QVERIFY(reasonsText(r).contains(u"kMaxItemDirDepth"_s));
}

void TestPuppetManifest::hostileFolders()
{
    QTemporaryDir tmp;
    const QString d = tmp.path();
    writeModel(d);
    // 10 000 files: refused, and not by listing them all
    for (int i = 0; i < 10000; ++i)
        testutil::writeFile(d + u"/many/%1"_s.arg(i), "");
    QElapsedTimer timer;
    timer.start();
    ManifestResult r = loadManifest(d.toStdString());
    QVERIFY(!r.ok);
    QCOMPARE(firstReasonName(r), s(kTooLarge));
    QVERIFY2(timer.elapsed() < 3000, "the walk must stop at the limit");
    QVERIFY(QDir(d + u"/many"_s).removeRecursively());
    // names that are strange but legal on disk
    testutil::writeFile(d + u"/テクスチャ.png"_s, "x");
    testutil::writeFile(d + u"/with space.moc3"_s, "x");
    testutil::writeFile(d + u"/line\nbreak.bin"_s, "x");
    testutil::writeFile(d + u"/quote\"back\\slash.bin"_s, "x");
    r = loadManifest(d.toStdString());
    QVERIFY2(r.ok, qPrintable(reasonsText(r)));
    // unicode names can be named
    writeModel(d, manifestText(u"\"Moc\": \"with space.moc3\", \"Textures\": [\"テクスチャ.png\"]"_s), false);
    r = loadManifest(d.toStdString());
    QVERIFY2(r.ok, qPrintable(reasonsText(r)));
    QCOMPARE(QString::fromStdString(r.manifest.textures.front()), u"テクスチャ.png"_s);
}

void TestPuppetManifest::mutatedManifestsNeverCrash()
{
    // a fixed-seed mutation fuzz of a full manifest: whatever the bytes, the answer is a verdict, and a refusal says why
    const QByteArray base = R"({"Version": 3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"], "Physics": "p.physics3.json",
      "Expressions": [{"Name": "a", "File": "a.exp3.json"}], "Motions": {"Idle": [{"File": "a.motion3.json", "FadeInTime": 0.5}]}},
      "Groups": [{"Target": "Parameter", "Name": "EyeBlink", "Ids": ["x", "y"]}], "HitAreas": [{"Name": "Body", "Id": "h"}], "Layout": {"width": 2}})";
    QTemporaryDir tmp;
    const QString d = tmp.path();
    testutil::writeFile(d + u"/m.moc3"_s, "x");
    testutil::writeFile(d + u"/t0.png"_s, "x");
    testutil::writeFile(d + u"/p.physics3.json"_s, "{}");
    testutil::writeFile(d + u"/a.exp3.json"_s, "{}");
    testutil::writeFile(d + u"/a.motion3.json"_s, "{}");
    quint32 state = 12345;
    const auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    int accepted = 0;
    for (int round = 0; round < 3000; ++round) {
        QByteArray text = base;
        const int edits = 1 + int(next() % 4);
        for (int e = 0; e < edits && !text.isEmpty(); ++e) {
            const int at = int(next() % quint32(text.size()));
            switch (next() % 4) {
            case 0: text[at] = char(next() & 0xFF); break;
            case 1: text.remove(at, 1 + int(next() % 8)); break;
            case 2: text.insert(at, QByteArray(1 + int(next() % 4), "\"{}[],:"[next() % 7])); break;
            default: text.truncate(at); break;
            }
        }
        testutil::writeFile(d + u"/m.model3.json"_s, text);
        const ManifestResult r = loadManifest(d.toStdString());
        if (r.ok) {
            ++accepted;
            QVERIFY(r.reasons.empty());
            QVERIFY(!r.manifest.moc.empty());
            QVERIFY(!r.manifest.textures.empty());
        } else {
            QVERIFY(!r.reasons.empty());
        }
    }
    QVERIFY(accepted > 0); // some mutations only touch ignored bytes
}

void TestPuppetManifest::openInItemCannotBeRedirected()
{
    QTemporaryDir tmp;
    const QString d = tmp.path() + u"/model"_s;
    writeModel(d);
    testutil::writeFile(d + u"/sub/deep/a.bin"_s, "inside");
    testutil::writeFile(tmp.path() + u"/outside/a.bin"_s, "outside");
    for (const bool fallback : {false, true}) {
        const std::string folder = d.toStdString();
        int fd = openInItem(folder, "sub/deep/a.bin", fallback);
        QVERIFY(fd >= 0);
        ::close(fd);
        QVERIFY(openInItem(folder, "sub/deep/missing", fallback) < 0);
        QVERIFY(openInItem(folder, "../outside/a.bin", fallback) < 0);
        QVERIFY(openInItem(folder, "/etc/hostname", fallback) < 0);
        // a folder on the way replaced by a link after the scan
        QVERIFY(QDir(d + u"/sub"_s).rename(d + u"/sub"_s, d + u"/sub-moved"_s));
        QVERIFY(QFile::link(tmp.path() + u"/outside"_s, d + u"/sub"_s));
        QVERIFY(openInItem(folder, "sub/a.bin", fallback) < 0);
        QVERIFY(openInItem(folder, "sub/deep/a.bin", fallback) < 0);
        QVERIFY(QFile::remove(d + u"/sub"_s));
        QVERIFY(QDir(d + u"/sub-moved"_s).rename(d + u"/sub-moved"_s, d + u"/sub"_s));
        // the last component replaced by a link
        QVERIFY(QFile::remove(d + u"/sub/deep/a.bin"_s));
        QVERIFY(QFile::link(tmp.path() + u"/outside/a.bin"_s, d + u"/sub/deep/a.bin"_s));
        QVERIFY(openInItem(folder, "sub/deep/a.bin", fallback) < 0);
        QVERIFY(QFile::remove(d + u"/sub/deep/a.bin"_s));
        testutil::writeFile(d + u"/sub/deep/a.bin"_s, "inside");
        // the helper that reads
        std::string out, why;
        QVERIFY(readItemFile(folder, "sub/deep/a.bin", 100, out, &why));
        QCOMPARE(out, std::string("inside"));
        QVERIFY(readItemFile(folder, "sub/deep/a.bin", 3, out, &why));
        QCOMPARE(out.size(), size_t(4)); // one more than asked, to tell that it is longer
        QVERIFY(!readItemFile(folder, "nope", 100, out, &why));
        QCOMPARE(why, std::string("missing"));
        QVERIFY(!readItemFile(folder, "sub", 100, out, &why)); // a folder
    }
    // and a manifest whose folder turned into a link between scan and read fails closed in loadManifest (read through the helper)
    QVERIFY(loadManifest(d.toStdString()).ok);
}

void TestPuppetManifest::nulCharactersAndOddNames()
{
    // \u0000 is accepted by the reader as text and refused wherever a name or a path is meant
    const auto parse = [](const QByteArray &b) { return json::parse(view(b), {16, 1000, 4096}); };
    const auto r = parse(QByteArray(R"({"a\u0000b": "c\u0000d"})"));
    QVERIFY(r.ok);
    QCOMPARE(r.value.members[0].first.size(), size_t(3));
    QCOMPARE(r.value.members[0].second.text.size(), size_t(3));
    QVERIFY(!parse(QByteArray(R"({"a\u0000b": 1, "a\u0000b": 2})")).ok); // repeated after unescaping
    QVERIFY(parse(QByteArray(R"({"a\u0000b": 1, "a": 2})")).ok);
    QTemporaryDir tmp;
    writeModel(tmp.path(), manifestText(u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0\\u0000.png\"]"_s));
    ManifestResult m = loadManifest(tmp.path().toStdString());
    QVERIFY(!m.ok);
    QCOMPARE(firstReasonName(m), s(kUnsafePath));
    writeModel(tmp.path(), manifestText(u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"], \"Expressions\": [{\"Name\": \"a\\u0000b\", \"File\": \"a.exp3.json\"}]"_s));
    testutil::writeFile(tmp.path() + u"/a.exp3.json"_s, "{}");
    QVERIFY(loadManifest(tmp.path().toStdString()).ok); // a name that is only a label may hold it; it is never a path
}

void TestPuppetManifest::motionSyncAndGroups()
{
    QTemporaryDir tmp;
    const QString d = tmp.path();
    writeModel(d, manifestText(u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"], \"MotionSync\": \"s.motionsync3.json\""_s));
    ManifestResult r = loadManifest(d.toStdString());
    QCOMPARE(firstReasonName(r), s(kFileMissing));
    testutil::writeFile(d + u"/s.motionsync3.json"_s, "{}");
    r = loadManifest(d.toStdString());
    QVERIFY2(r.ok, qPrintable(reasonsText(r)));
    QCOMPARE(QString::fromStdString(r.manifest.motionSync), u"s.motionsync3.json"_s);
    sparse(d + u"/s.motionsync3.json"_s, kMaxMotionSyncFileBytes + 1);
    QCOMPARE(firstReasonName(loadManifest(d.toStdString())), s(kTooLarge));
    for (const char *bad : {"../s.motionsync3.json", "/etc/passwd", "s.json", "", "a\\\\b"}) {
        writeModel(d, manifestText(u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"], \"MotionSync\": \""_s + QString::fromLatin1(bad) + u"\""_s));
        QVERIFY2(!loadManifest(d.toStdString()).ok, bad);
    }
    // inside a motion it is a name
    testutil::writeFile(d + u"/a.motion3.json"_s, "{}");
    writeModel(d, manifestText(u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"], \"Motions\": {\"I\": [{\"File\": \"a.motion3.json\", \"MotionSync\": \"Mouth\"}]}"_s));
    r = loadManifest(d.toStdString());
    QVERIFY2(r.ok, qPrintable(reasonsText(r)));
    QCOMPARE(QString::fromStdString(r.manifest.motionGroups[0].motions[0].motionSync), u"Mouth"_s);
    writeModel(d, manifestText(u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\"], \"Motions\": {\"I\": [{\"File\": \"a.motion3.json\", \"MotionSync\": 5}]}"_s));
    QVERIFY(!loadManifest(d.toStdString()).ok);
    // group target and name may not be empty
    for (const char *group : {R"({"Target": "", "Name": "EyeBlink", "Ids": []})", R"({"Target": "Parameter", "Name": "", "Ids": []})"}) {
        writeModel(d, QByteArray(R"({"Version": 3, "FileReferences": {"Moc": "m.moc3", "Textures": ["t0.png"]}, "Groups": [)") + group + "]}");
        QVERIFY(!loadManifest(d.toStdString()).ok);
    }
    // an .inp says that its payload was not read
    testutil::writeFile(d + u"/o/p.inp"_s, QByteArray("TRNSRTS2") + QByteArray(40, 'x'));
    r = loadManifest((d + u"/o"_s).toStdString());
    QVERIFY(r.ok);
    QCOMPARE(r.degraded.size(), size_t(1));
    QCOMPARE(r.degraded[0].name, kPayloadNotRead);
    // a truncated container: exactly its signature and length is enough for the manifest, one byte less is not
    testutil::writeFile(d + u"/t/p.inp"_s, QByteArray("TRNSRTS2") + QByteArray(4, 'x'));
    QVERIFY(loadManifest((d + u"/t"_s).toStdString()).ok);
    testutil::writeFile(d + u"/t/p.inp"_s, QByteArray("TRNSRTS2") + QByteArray(3, 'x'));
    QCOMPARE(firstReasonName(loadManifest((d + u"/t"_s).toStdString())), s(kUnsupportedFormat));
}

void TestPuppetManifest::tidyDetailIsSafe()
{
    const auto tidy = [](const QByteArray &b) { return QString::fromStdString(tidyDetail(view(b))); };
    QCOMPARE(tidy("plain.png"), u"plain.png"_s);
    QCOMPARE(tidy(QString::fromUtf8("テクスチャ").toUtf8()), u"テクスチャ"_s);
    const QString rep = QString(QChar(0xFFFD));
    QCOMPARE(tidy("a\x01" "b"), u"a"_s + rep + u"b"_s);
    QCOMPARE(tidy("a\x7f" "b"), u"a"_s + rep + u"b"_s);
    QCOMPARE(tidy("a\xc2\x85" "b"), u"a"_s + rep + u"b"_s);                 // U+0085, a C1 control
    QCOMPARE(tidy("a\xe2\x80\xae" "b"), u"a"_s + rep + u"b"_s);             // U+202E, right-to-left override
    QCOMPARE(tidy("a\xe2\x80\x8b" "b"), u"a"_s + rep + u"b"_s);             // U+200B, zero width space
    QCOMPARE(tidy("a\xe2\x81\xa6" "b"), u"a"_s + rep + u"b"_s);             // U+2066, an isolate
    QCOMPARE(tidy("a\xe2\x80\xa8" "b"), u"a"_s + rep + u"b"_s);             // U+2028
    QCOMPARE(tidy("a\xef\xbb\xbf" "b"), u"a"_s + rep + u"b"_s);             // the byte order mark
    QCOMPARE(tidy("a\xff" "b"), u"a"_s + rep + u"b"_s);
    QCOMPARE(tidy("a\xe3\x81"), u"a"_s + rep + rep);                         // a cut character: each byte
    // the cut is at a character boundary within 120 bytes
    const QByteArray many = QString::fromUtf8("あ").toUtf8().repeated(100);
    const std::string cut = tidyDetail(view(many));
    QVERIFY(cut.size() <= 123); // 120 and "..."
    QVERIFY(json::validUtf8(cut));
    const std::string bad = tidyDetail(view(QByteArray("\xff").repeated(500)));
    QVERIFY(bad.size() <= 123);
    QVERIFY(json::validUtf8(bad));
}

void TestPuppetManifest::openContainers()
{
    QTemporaryDir tmp;
    const QString d = tmp.path();
    const auto inp1 = [](quint32 jsonLength, qsizetype total) {
        QByteArray b("TRNSRTS\0", 8);
        b += char(jsonLength >> 24);
        b += char(jsonLength >> 16);
        b += char(jsonLength >> 8);
        b += char(jsonLength);
        b += QByteArray(qMax<qsizetype>(0, total - 12), '{');
        return b;
    };
    // version 1
    testutil::writeFile(d + u"/a/p.inp"_s, inp1(20, 12 + 20));
    ManifestResult r = loadManifest((d + u"/a"_s).toStdString());
    QVERIFY2(r.ok, qPrintable(reasonsText(r)));
    QVERIFY(r.format && *r.format == Format::Open);
    QCOMPARE(r.manifest.version, 1);
    QCOMPARE(QString::fromStdString(r.manifest.file), u"p.inp"_s);
    QCOMPARE(r.degraded.size(), size_t(1)); // payload not read
    // version 2
    testutil::writeFile(d + u"/b/p.inp"_s, QByteArray("TRNSRTS2") + QByteArray(40, 'x'));
    r = loadManifest((d + u"/b"_s).toStdString());
    QVERIFY2(r.ok, qPrintable(reasonsText(r)));
    QCOMPARE(r.manifest.version, 2);
    // the JSON length of version 1 cannot run past the file
    testutil::writeFile(d + u"/c/p.inp"_s, inp1(21, 12 + 20));
    r = loadManifest((d + u"/c"_s).toStdString());
    QVERIFY(!r.ok);
    QCOMPARE(firstReasonName(r), s(kUnsupportedFormat));
    testutil::writeFile(d + u"/c/p.inp"_s, inp1(0xFFFFFFFFu, 100));
    QVERIFY(!loadManifest((d + u"/c"_s).toStdString()).ok);
    // bad signatures and short files
    for (const QByteArray &bad : {QByteArray(), QByteArray("TRNSRTS"), QByteArray("TRNSRTS\0", 8), QByteArray("TRNSRTS3") + QByteArray(40, 'x'), QByteArray("NOTANINP") + QByteArray(40, 'x'),
                                  QByteArray("trnsrts2") + QByteArray(40, 'x'), QByteArray("TRNSRTS2")}) {
        testutil::writeFile(d + u"/e/p.inp"_s, bad);
        r = loadManifest((d + u"/e"_s).toStdString());
        QVERIFY(!r.ok);
        QCOMPARE(firstReasonName(r), s(kUnsupportedFormat));
    }
    // size
    sparse(d + u"/f/p.inp"_s, kMaxInpBytes);
    {
        QFile f(d + u"/f/p.inp"_s);
        QVERIFY(f.open(QIODevice::ReadWrite));
        f.write("TRNSRTS2");
    }
    r = loadManifest((d + u"/f"_s).toStdString());
    QVERIFY2(r.ok, qPrintable(reasonsText(r)));
    sparse(d + u"/f/p.inp"_s, kMaxInpBytes + 1);
    r = loadManifest((d + u"/f"_s).toStdString());
    QVERIFY(!r.ok);
    QCOMPARE(firstReasonName(r), s(kTooLarge));
    QVERIFY(reasonsText(r).contains(u"kMaxInpBytes"_s));
    // an .inp beside a model3.json, or two .inp files
    writeModel(d + u"/g"_s);
    testutil::writeFile(d + u"/g/p.inp"_s, QByteArray("TRNSRTS2") + QByteArray(40, 'x'));
    QCOMPARE(firstReasonName(loadManifest((d + u"/g"_s).toStdString())), s(kNotAPuppet));
    testutil::writeFile(d + u"/h/a.inp"_s, QByteArray("TRNSRTS2") + QByteArray(40, 'x'));
    testutil::writeFile(d + u"/h/b.inp"_s, QByteArray("TRNSRTS2") + QByteArray(40, 'x'));
    QCOMPARE(firstReasonName(loadManifest((d + u"/h"_s).toStdString())), s(kNotAPuppet));
    // other files in an open item are allowed (they count against the folder limits)
    testutil::writeFile(d + u"/b/readme.txt"_s, "hello");
    QVERIFY(loadManifest((d + u"/b"_s).toStdString()).ok);
    // a link as the container
    QVERIFY(QDir().mkpath(d + u"/i"_s));
    QVERIFY(QFile::link(d + u"/b/p.inp"_s, d + u"/i/p.inp"_s));
    QCOMPARE(firstReasonName(loadManifest((d + u"/i"_s).toStdString())), s(kUnsafePath));
}

// ---------------------------------------------------------------------------------------------------------------------------------------------

void TestPuppetManifest::checkLine()
{
    QTemporaryDir tmp;
    const QString d = tmp.path();
    writeModel(d, manifestText(u"\"Moc\": \"m.moc3\", \"Textures\": [\"t0.png\", \"t0.png\"], \"Expressions\": [{\"Name\": \"a\", \"File\": \"a.exp3.json\"}]"_s));
    testutil::writeFile(d + u"/a.exp3.json"_s, "{}");
    const QByteArray line = QByteArray::fromStdString(checkJson(loadManifest(d.toStdString()), "9.9.9"));
    const QJsonObject o = QJsonDocument::fromJson(line).object();
    QCOMPARE(o.value(u"ok"_s).toBool(), true);
    QCOMPARE(o.value(u"format"_s).toString(), u"cubism"_s);
    QCOMPARE(o.value(u"modelVersion"_s).toInt(), 3);
    QVERIFY(o.value(u"parameters"_s).isNull());
    QVERIFY(o.value(u"drawables"_s).isNull());
    QCOMPARE(o.value(u"textures"_s).toInt(), 2);
    QCOMPARE(o.value(u"motions"_s).toInt(), 0);
    QCOMPARE(o.value(u"expressions"_s).toInt(), 1);
    QVERIFY(o.value(u"physics"_s).isNull());
    QCOMPARE(o.value(u"reasons"_s).toArray().size(), 0);
    QCOMPARE(o.value(u"degraded"_s).toArray().size(), 1);
    QCOMPARE(o.value(u"degraded"_s).toArray().at(0).toString(), s(kNeedsCubismRuntime));
    QCOMPARE(o.value(u"engine"_s).toString(), u"murasaki-puppet"_s);
    QCOMPARE(o.value(u"version"_s).toString(), u"9.9.9"_s);
    // key order is part of the contract
    const QStringList order = {u"ok"_s, u"format"_s, u"modelVersion"_s, u"parameters"_s, u"drawables"_s, u"textures"_s, u"motions"_s, u"expressions"_s, u"physics"_s, u"canvas"_s, u"reasons"_s,
                               u"degraded"_s, u"engine"_s, u"version"_s};
    int last = -1;
    for (const QString &key : order) {
        const int at = line.indexOf(("\"" + key + "\":").toUtf8());
        QVERIFY2(at > last, qPrintable(key));
        last = at;
    }
    // refused
    QVERIFY(QFile::remove(d + u"/t0.png"_s));
    const QJsonObject bad = QJsonDocument::fromJson(QByteArray::fromStdString(checkJson(loadManifest(d.toStdString()), "9.9.9"))).object();
    QVERIFY(!bad.value(u"ok"_s).toBool());
    QCOMPARE(bad.value(u"format"_s).toString(), u"cubism"_s);
    QCOMPARE(bad.value(u"reasons"_s).toArray().size(), 1);
    QVERIFY(bad.value(u"reasons"_s).toArray().at(0).toString().startsWith(s(kFileMissing) + u": "_s));
    QVERIFY(bad.value(u"textures"_s).isNull());
    // not a puppet at all: no format
    const QJsonObject none = QJsonDocument::fromJson(QByteArray::fromStdString(checkJson(loadManifest((d + u"/nope"_s).toStdString()), "9.9.9"))).object();
    QVERIFY(none.value(u"format"_s).isNull());
    QVERIFY(none.value(u"modelVersion"_s).isNull());
    QVERIFY(!none.value(u"ok"_s).toBool());
    // open
    testutil::writeFile(d + u"/o/p.inp"_s, QByteArray("TRNSRTS2") + QByteArray(40, 'x'));
    const QJsonObject open = QJsonDocument::fromJson(QByteArray::fromStdString(checkJson(loadManifest((d + u"/o"_s).toStdString()), "9.9.9"))).object();
    QCOMPARE(open.value(u"ok"_s).toBool(), true);
    QCOMPARE(open.value(u"format"_s).toString(), u"open"_s);
    QCOMPARE(open.value(u"modelVersion"_s).toInt(), 2);
    QCOMPARE(open.value(u"degraded"_s).toArray().size(), 1); // payload not read: loadManifest looks at the signature only
}

void TestPuppetManifest::checkLineIsValidJsonWhateverTheNames()
{
    QTemporaryDir tmp;
    const QString d = tmp.path();
    // names with a quote, a backslash, a newline, a control character and bytes that are not UTF-8, in the detail of a reason
    const QByteArray weird = QByteArray("q\"b\\n\nc\x01") + "\xff\xfe" + QByteArray(300, 'x');
    ManifestResult r;
    r.reasons.push_back({kFileMissing, std::string(weird.constData(), size_t(weird.size()))});
    r.degraded.push_back({kNeedsCubismRuntime, std::string(weird.constData(), size_t(weird.size()))});
    const QByteArray line = QByteArray::fromStdString(checkJson(r, "1"));
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
    QVERIFY2(error.error == QJsonParseError::NoError, qPrintable(error.errorString()));
    QVERIFY(!line.contains('\n'));
    const QString reason = doc.object().value(u"reasons"_s).toArray().at(0).toString();
    QVERIFY(reason.startsWith(s(kFileMissing)));
    QVERIFY(reason.size() < 260); // the detail is cut
    // a real folder with such a file name
    writeModel(d, manifestText(u"\"Moc\": \"m.moc3\", \"Textures\": [\"nope\\\"\\n.png\"]"_s));
    const QByteArray line2 = QByteArray::fromStdString(checkJson(loadManifest(d.toStdString()), "1"));
    QVERIFY2(QJsonDocument::fromJson(line2, &error).isObject(), qPrintable(error.errorString()));
}

void TestPuppetManifest::theBinaryPrintsTheSameLine()
{
#ifndef MURASAKI_PUPPET_BIN
    QSKIP("murasaki-puppet is not built");
#else
    QTemporaryDir tmp;
    writeModel(tmp.path() + u"/ok"_s);
    writeModel(tmp.path() + u"/bad"_s, manifestText(), false);
    const auto run = [](const QStringList &args, int &exit) {
        QProcess p;
        p.start(QStringLiteral(MURASAKI_PUPPET_BIN), args);
        if (!p.waitForFinished(30000))
            qFatal("murasaki-puppet did not finish");
        exit = p.exitCode();
        return p.readAllStandardOutput();
    };
    int code = -1;
    QByteArray out = run({u"--check"_s, tmp.path() + u"/ok"_s}, code);
    QCOMPARE(code, 0);
    QVERIFY(out.endsWith('\n'));
    QCOMPARE(out.count('\n'), 1);
    const QJsonObject o = QJsonDocument::fromJson(out).object();
    QCOMPARE(o.value(u"ok"_s).toBool(), true);
    QCOMPARE(o.value(u"engine"_s).toString(), u"murasaki-puppet"_s);
    out = run({u"--check"_s, tmp.path() + u"/bad"_s}, code);
    QCOMPARE(code, 0); // a verdict was printed
    QCOMPARE(QJsonDocument::fromJson(out).object().value(u"ok"_s).toBool(), false);
    out = run({u"--check"_s, tmp.path() + u"/missing"_s}, code);
    QCOMPARE(code, 0);
    QVERIFY(QJsonDocument::fromJson(out).object().value(u"reasons"_s).toArray().at(0).toString().startsWith(s(kNotAPuppet)));
    // usage
    run({}, code);
    QCOMPARE(code, 2);
    run({u"--check"_s}, code);
    QCOMPARE(code, 2);
    run({u"--check"_s, u"a"_s, u"b"_s}, code);
    QCOMPARE(code, 2);
    run({u"--bogus"_s}, code);
    QCOMPARE(code, 2);
    out = run({u"--version"_s}, code);
    QCOMPARE(code, 0);
    QVERIFY(out.startsWith("murasaki-puppet "));
#endif
}

// ---------------------------------------------------------------------------------------------------------------------------------------------

void TestPuppetManifest::limitsTableMatchesTheDocument()
{
    QFile doc(QStringLiteral(PUPPETS_DOC));
    QVERIFY2(doc.open(QIODevice::ReadOnly), "docs/pets/puppets.md is missing");
    const QString text = QString::fromUtf8(doc.readAll());
    QSet<QString> seen;
    for (const Limit &limit : kLimits) {
        const QString name = s(limit.name);
        QVERIFY2(!seen.contains(name), qPrintable(name));
        seen.insert(name);
        QVERIFY2(limit.value > 0, qPrintable(name));
        const QString row = u"| `"_s + name + u"` | "_s + QString::number(limit.value) + u" |"_s;
        QVERIFY2(text.contains(row), qPrintable(row));
    }
    // and the document lists no limit the table does not have
    const QRegularExpression rows(uR"(^\| `(k\w+)` \| \d+ \|)"_s, QRegularExpression::MultilineOption);
    auto it = rows.globalMatch(text);
    int documented = 0;
    while (it.hasNext()) {
        const QString name = it.next().captured(1);
        QVERIFY2(seen.contains(name), qPrintable(u"documented but not in the table: "_s + name));
        ++documented;
    }
    QCOMPARE(documented, int(kLimits.size()));
}

void TestPuppetManifest::namesTableMatchesTheDocument()
{
    QFile doc(QStringLiteral(PUPPETS_DOC));
    QVERIFY(doc.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(doc.readAll());
    QSet<QString> seen;
    for (const Name &name : kNames) {
        const QString n = s(name.text);
        QVERIFY2(!seen.contains(n), qPrintable(n));
        seen.insert(n);
        const QString row = u"| `"_s + n + u"` |"_s;
        QVERIFY2(text.contains(row), qPrintable(row));
    }
    const QRegularExpression rows(uR"(^\| `(puppet: [^`]+)` \|)"_s, QRegularExpression::MultilineOption);
    auto it = rows.globalMatch(text);
    int documented = 0;
    while (it.hasNext()) {
        QVERIFY2(seen.contains(it.next().captured(1)), "a documented name is not in the table");
        ++documented;
    }
    QCOMPARE(documented, int(kNames.size()));
}

void TestPuppetManifest::everyNameStartsWithPuppet()
{
    for (const Name &name : kNames) {
        QVERIFY(name.text.starts_with("puppet: "));
        QVERIFY(!name.meaning.empty());
        QVERIFY(name.text.find(": ", 8) == std::string_view::npos); // a caller matches the part before the first ": "... after the prefix there is none
    }
}

void TestPuppetManifest::everyReasonIsAName()
{
    // over the whole set of cases above, every reason and degraded entry starts with a name of the table: checked here on the cases that are cheap to rebuild
    QTemporaryDir tmp;
    const QString d = tmp.path();
    const QByteArray bad[] = {QByteArray(), manifestText(texturesRefs(40)), QByteArray(R"({"Version": 9, "FileReferences": {}})"), manifestText(u"\"Moc\": \"../x\", \"Textures\": [\"t0.png\"]"_s)};
    for (const QByteArray &text : bad) {
        writeModel(d, text, false);
        const ManifestResult r = loadManifest(d.toStdString());
        QVERIFY(!r.ok);
        for (const Problem &p : r.reasons) {
            bool known = false;
            for (const Name &name : kNames)
                known = known || name.text == p.name;
            QVERIFY2(known, qPrintable(s(p.name)));
            QVERIFY(s(p.text()).startsWith(s(p.name)));
        }
    }
}

QTEST_GUILESS_MAIN(TestPuppetManifest)
#include "tst_puppetmanifest.moc"
