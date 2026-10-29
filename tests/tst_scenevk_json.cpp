// murasaki-scene-vk JSON reader: the dialect of Wallpaper Engine files and the failure modes.

#include "util/json.h"

#include <QTest>

using namespace murasaki::scene;

class TstSceneVkJson : public QObject
{
    Q_OBJECT

private slots:
    void parsesScalarsAndKeepsMemberOrder()
    {
        std::string error;
        const auto j = parseJson("{\"b\": 1, \"a\": [true, false, null, \"x\"], \"c\": {\"n\": -0.000}}", &error);
        QVERIFY2(j.has_value(), error.c_str());
        QVERIFY(j->isObject());
        QCOMPARE(j->members().size(), size_t{3});
        QCOMPARE(j->members()[0].first, std::string("b"));
        QCOMPARE(j->members()[1].first, std::string("a"));
        QCOMPARE(j->numberOr("b", 9), 1.0);
        const Json *a = j->find("a");
        QVERIFY(a && a->isArray());
        QCOMPARE(a->items().size(), size_t{4});
        QVERIFY(a->items()[0].asBool(false));
        QVERIFY(a->items()[2].isNull());
        QCOMPARE(a->items()[3].asString(), std::string("x"));
        QCOMPARE(j->find("c")->numberOr("n", 5), -0.0);
    }

    void numbersInEveryPrintfForm()
    {
        const auto j = parseJson("[0.7, 1e-05, 2E+3, -12, 0.65, 1.5e0]");
        QVERIFY(j.has_value());
        QCOMPARE(j->items()[0].asNumber(), 0.7);
        QCOMPARE(j->items()[1].asNumber(), 1e-05);
        QCOMPARE(j->items()[2].asNumber(), 2000.0);
        QCOMPARE(j->items()[3].asNumber(), -12.0);
    }

    void byteOrderMarkAndWhitespaceAreAccepted()
    {
        const auto j = parseJson("\xEF\xBB\xBF \n\t{ \"k\" :\r\n \"v\" } \n");
        QVERIFY(j.has_value());
        QCOMPARE(j->stringOr("k"), std::string("v"));
    }

    void stringEscapes()
    {
        const auto j = parseJson("[\"a\\\"b\\\\c\\/d\\n\\t\", \"\\u00e9\\u20ac\", \"\\ud83d\\ude00\"]");
        QVERIFY(j.has_value());
        QCOMPARE(j->items()[0].asString(), std::string("a\"b\\c/d\n\t"));
        QCOMPARE(j->items()[1].asString(), std::string("\xC3\xA9\xE2\x82\xAC"));
        QCOMPARE(j->items()[2].asString(), std::string("\xF0\x9F\x98\x80"));
    }

    void accessorsToleratePresentButWrongTypes()
    {
        const auto j = parseJson("{\"s\": 1, \"n\": \"x\", \"b\": 2}");
        QVERIFY(j.has_value());
        QCOMPARE(j->stringOr("s", "fb"), std::string("fb"));
        QCOMPARE(j->numberOr("n", 7.0), 7.0);
        QVERIFY(j->boolOr("b", true));
        QVERIFY(j->find("missing") == nullptr);
        QVERIFY(Json().find("x") == nullptr);
    }

    void rejectsMalformedDocuments()
    {
        for (const char *bad : {"", "{", "[1,,]", "{\"a\"}", "{\"a\":1,,}", "[,]", "[1 2]", "tru", "\"abc", "01x", "{\"a\":1} x", "[\"\\q\"]",
                                "[\"\\u12\"]", "nul", "-", "[1e999]"}) {
            std::string error;
            QVERIFY2(!parseJson(bad, &error).has_value(), bad);
            QVERIFY2(!error.empty(), bad);
        }
    }

    void trailingCommasAreTolerated()
    {
        const auto j = parseJson("{\"a\": [1, 2, ], \"b\": {\"c\": 1, }, }");
        QVERIFY(j.has_value());
        QCOMPARE(j->find("a")->items().size(), size_t{2});
        QCOMPARE(j->find("b")->numberOr("c", 0), 1.0);
        QVERIFY(parseJson("[1,\n]").has_value());
    }

    void nestingIsBounded()
    {
        QVERIFY(parseJson(std::string(60, '[') + std::string(60, ']')).has_value());
        QVERIFY(!parseJson(std::string(100000, '[')).has_value());
    }
};

QTEST_GUILESS_MAIN(TstSceneVkJson)
#include "tst_scenevk_json.moc"
