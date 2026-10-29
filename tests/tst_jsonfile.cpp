#include "jsonfile.h"
#include "testutil.h"

#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestJsonFile : public QObject
{
    Q_OBJECT

private slots:
    void missingFileIsNotAnError();
    void validObjectIsRead();
    void everythingElseIsAProblemNotACrash_data();
    void everythingElseIsAProblemNotACrash();
    void mutatedDocumentsNeverCrash();
    void writeThenReadRoundTrips();
};

void TestJsonFile::missingFileIsNotAnError()
{
    QTemporaryDir tmp;
    const JsonFileRead read = readJsonObjectFile(tmp.filePath(u"nothing.json"_s));
    QVERIFY(!read.exists);
    QVERIFY(read.problem.isEmpty());
}

void TestJsonFile::validObjectIsRead()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"a.json"_s);
    testutil::writeFile(path, R"({"a": 1, "b": [true, null]})");
    const JsonFileRead read = readJsonObjectFile(path);
    QVERIFY(read.exists);
    QVERIFY2(read.problem.isEmpty(), qPrintable(read.problem));
    QCOMPARE(read.root.value(u"a"_s).toInt(), 1);
}

void TestJsonFile::everythingElseIsAProblemNotACrash_data()
{
    QTest::addColumn<QByteArray>("content");
    QTest::newRow("empty") << QByteArray();
    QTest::newRow("array") << QByteArray("[1, 2]");
    QTest::newRow("scalar") << QByteArray("42");
    QTest::newRow("garbage") << (QByteArray("\xff\xfe\x00\x01", 4) + " not json");
    QTest::newRow("nul bytes") << QByteArray(1000, '\0');
    QTest::newRow("truncated") << QByteArray(R"({"a": [1, 2, {"b": )");
    QTest::newRow("depth bomb") << (QByteArray(200000, '[') + QByteArray(200000, ']'));
    QTest::newRow("object depth bomb") << (QByteArray("{\"a\":", 5).repeated(50000) + "1" + QByteArray(50000, '}'));
    QTest::newRow("huge") << QByteArray(kMaxJsonFileSize + 1, ' ');
}

void TestJsonFile::everythingElseIsAProblemNotACrash()
{
    QFETCH(QByteArray, content);
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"hostile.json"_s);
    testutil::writeFile(path, content);
    const JsonFileRead read = readJsonObjectFile(path);
    QVERIFY(read.exists);
    QVERIFY(!read.problem.isEmpty());
    // A directory in the place of the file is a problem too.
    QVERIFY(QDir().mkpath(tmp.filePath(u"dir.json"_s)));
    QVERIFY(!readJsonObjectFile(tmp.filePath(u"dir.json"_s)).problem.isEmpty());
}

// Valid documents with bytes flipped and cut, deterministically: reading must come back with either an object or a problem.
void TestJsonFile::mutatedDocumentsNeverCrash()
{
    const QByteArray valid = R"({"version": 1, "items": {"we:1": {"network": true}, "we:2": {"x": [1, 2.5e3, "sé", null]}}})";
    quint32 state = 777;
    const auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"mutated.json"_s);
    for (int round = 0; round < 500; ++round) {
        QByteArray data = valid;
        for (int i = 0, flips = 1 + int(next() % 5); i < flips; ++i)
            data[int(next() % quint32(data.size()))] = char(next() & 0xff);
        if (round % 4 == 0)
            data.truncate(int(next() % quint32(data.size() + 1)));
        testutil::writeFile(path, data);
        const JsonFileRead read = readJsonObjectFile(path);
        QVERIFY(read.exists);
        QVERIFY(read.problem.isEmpty() || read.root.isEmpty());
    }
}

void TestJsonFile::writeThenReadRoundTrips()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath(u"sub/dir/out.json"_s);
    QVERIFY(writeJsonObjectFile(path, QJsonObject{{u"k"_s, u"v"_s}}));
    QCOMPARE(readJsonObjectFile(path).root.value(u"k"_s).toString(), u"v"_s);
}

QTEST_GUILESS_MAIN(TestJsonFile)
#include "tst_jsonfile.moc"
