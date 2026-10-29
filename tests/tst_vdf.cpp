#include "vdf.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestVdf : public QObject
{
    Q_OBJECT

private slots:
    void parsesNestedBlocks();
    void parsesEscapesAndComments();
    void ignoresPlatformConditionals();
    void rejectsMalformedInput();
    void lookupIsCaseInsensitive();
    void rejectsDeepNesting();
    void survivesManyConditionals();
    void stripsUtf8Bom();
    void parsesLegacyLibraryFolders();
    void parsesCrlf();
    void readsFiles();
};

void TestVdf::parsesNestedBlocks()
{
    const auto root = parseVdf(R"("libraryfolders"
{
	"0"
	{
		"path"		"/home/u/.steam/root"
		"apps" { "431960" "826275581" }
	}
})");
    QVERIFY(root.has_value());
    const QVariantMap lib0 = root->value(u"libraryfolders"_s).toMap().value(u"0"_s).toMap();
    QCOMPARE(lib0.value(u"path"_s).toString(), u"/home/u/.steam/root"_s);
    QCOMPARE(lib0.value(u"apps"_s).toMap().value(u"431960"_s).toString(), u"826275581"_s);
}

void TestVdf::parsesEscapesAndComments()
{
    const auto root = parseVdf("// header\n\"k\" \"a\\\"b\\\\c\" // trailing\n\"n\" \"x\\ty\"\nbare value\n");
    QVERIFY(root.has_value());
    QCOMPARE(root->value(u"k"_s).toString(), u"a\"b\\c"_s);
    QCOMPARE(root->value(u"n"_s).toString(), u"x\ty"_s);
    QCOMPARE(root->value(u"bare"_s).toString(), u"value"_s);
}

void TestVdf::ignoresPlatformConditionals()
{
    const auto root = parseVdf(R"("AppState" { "installdir" "wallpaper_engine" [$LINUX] })");
    QVERIFY(root.has_value());
    QCOMPARE(root->value(u"AppState"_s).toMap().value(u"installdir"_s).toString(), u"wallpaper_engine"_s);
}

void TestVdf::rejectsMalformedInput()
{
    QVERIFY(!parseVdf("\"a\" { \"b\" \"c\"").has_value());
    QVERIFY(!parseVdf("\"a\" \"unterminated").has_value());
    QVERIFY(!parseVdf("\"a\" }").has_value());
}

void TestVdf::lookupIsCaseInsensitive()
{
    const auto root = parseVdf(R"("LibraryFolders" { "1" "/mnt/games" })");
    QVERIFY(root.has_value());
    QCOMPARE(vdfValue(vdfValue(*root, u"libraryfolders").toMap(), u"1").toString(), u"/mnt/games"_s);
    QVERIFY(!vdfValue(*root, u"missing").isValid());
}

void TestVdf::rejectsDeepNesting()
{
    QVERIFY(!parseVdf(QByteArray("\"a\"{").repeated(10000)).has_value());
    QVERIFY(!parseVdf(QByteArray("\"a\"{").repeated(100000)).has_value());
    // Reasonable nesting still works.
    const QByteArray ok = QByteArray("\"a\"{").repeated(20) + QByteArray("}").repeated(20);
    QVERIFY(parseVdf(ok).has_value());
}

void TestVdf::survivesManyConditionals()
{
    const QByteArray text = "\"k\" \"v\"" + QByteArray("[x]").repeated(1000000) + "\"z\" \"w\"";
    const auto root = parseVdf(text);
    QVERIFY(root.has_value());
    QCOMPARE(root->value(u"k"_s).toString(), u"v"_s);
    QCOMPARE(root->value(u"z"_s).toString(), u"w"_s);
}

void TestVdf::stripsUtf8Bom()
{
    const auto root = parseVdf(QByteArray("\xEF\xBB\xBF\"k\" \"v\""));
    QVERIFY(root.has_value());
    QCOMPARE(root->value(u"k"_s).toString(), u"v"_s);
}

void TestVdf::parsesLegacyLibraryFolders()
{
    const auto root = parseVdf(
        "\"LibraryFolders\" { \"TimeNextStatsReport\" \"123\" \"1\" \"/mnt/games\" }");
    QVERIFY(root.has_value());
    const QVariantMap lf = vdfValue(*root, u"libraryfolders").toMap();
    QCOMPARE(lf.value(u"TimeNextStatsReport"_s).toString(), u"123"_s);
    QCOMPARE(lf.value(u"1"_s).toString(), u"/mnt/games"_s);
}

void TestVdf::parsesCrlf()
{
    const auto root = parseVdf("// c\r\n\"a\"\r\n{\r\n\t\"k\"\t\"v\"\r\n}\r\n");
    QVERIFY(root.has_value());
    QCOMPARE(root->value(u"a"_s).toMap().value(u"k"_s).toString(), u"v"_s);
}

void TestVdf::readsFiles()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(u"x.vdf"_s);
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("\"k\" \"v\"");
    f.close();
    const auto root = readVdfFile(path);
    QVERIFY(root.has_value());
    QCOMPARE(root->value(u"k"_s).toString(), u"v"_s);
    QVERIFY(!readVdfFile(dir.filePath(u"missing.vdf"_s)).has_value());
}

QTEST_GUILESS_MAIN(TestVdf)
#include "tst_vdf.moc"
