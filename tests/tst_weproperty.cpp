#include "testutil.h"
#include "weproperty.h"

#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestWeProperty : public QObject
{
    Q_OBJECT

private slots:
    void parsesTypesAndSortsByOrder();
    void stripsDollarFromConditions();
    void keepsUnknownTypes();
    void presetOverridesValuesAndSkipsNull();
    void toJsonOnlyIncludesRelevantFields();
    void keepsFractionalOrder();
    void acceptsNumericStrings();
    void skipsNonObjectEntries();
};

void TestWeProperty::parsesTypesAndSortsByOrder()
{
    const auto props = parseWeProperties(testutil::json(R"({
        "speed":  {"type": "slider", "text": "Speed", "value": 50, "min": 0, "max": 200, "step": 5, "order": 3},
        "mode":   {"type": "combo", "text": "Mode", "value": 2, "order": 1,
                   "options": [{"label": "A", "value": 1}, {"label": "B", "value": 2}]},
        "glow":   {"type": "bool", "text": "Glow", "value": true, "order": 2},
        "credit": {"text": "<a href='x'>Thanks</a>", "order": 0}
    })"));

    QCOMPARE(props.size(), 4);
    QCOMPARE(props[0].key, u"credit"_s);
    QCOMPARE(props[0].type, WeProperty::Type::Label);
    QCOMPARE(props[1].key, u"mode"_s);
    QCOMPARE(props[1].options.size(), 2);
    QCOMPARE(props[1].options[1].label, u"B"_s);
    QCOMPARE(props[1].options[1].value.toInt(), 2);
    QCOMPARE(props[2].type, WeProperty::Type::Bool);
    QCOMPARE(props[2].value.toBool(), true);
    QCOMPARE(props[3].type, WeProperty::Type::Slider);
    QCOMPARE(props[3].max, 200.0);
    QCOMPARE(props[3].step, 5.0);
}

void TestWeProperty::stripsDollarFromConditions()
{
    const auto props = parseWeProperties(testutil::json(R"({
        "a": {"type": "bool", "value": false, "condition": "$enable.value && $kind.value == 1"}
    })"));
    QCOMPARE(props.size(), 1);
    QCOMPARE(props[0].condition, u"enable.value && kind.value == 1"_s);
}

void TestWeProperty::keepsUnknownTypes()
{
    const auto props = parseWeProperties(testutil::json(R"({"x": {"type": "Fancy", "value": 1}})"));
    QCOMPARE(props.size(), 1);
    QCOMPARE(props[0].type, WeProperty::Type::Unknown);
    QCOMPARE(weTypeFromString(u"TextInput"_s), WeProperty::Type::TextInput);
    QCOMPARE(weTypeToString(WeProperty::Type::Directory), u"directory"_s);
}

void TestWeProperty::presetOverridesValuesAndSkipsNull()
{
    auto props = parseWeProperties(testutil::json(R"({
        "a": {"type": "slider", "value": 1, "min": 0, "max": 10},
        "b": {"type": "color", "value": "1 0 0"},
        "c": {"type": "bool", "value": true}
    })"));
    QCOMPARE(props.size(), 3);
    applyPresetValues(props, testutil::json(R"({"a": 7, "b": null, "unrelated": 3})"));
    QCOMPARE(props[0].value.toInt(), 7);
    QCOMPARE(props[1].value.toString(), u"1 0 0"_s);
    QCOMPARE(props[2].value.toBool(), true);
}

void TestWeProperty::toJsonOnlyIncludesRelevantFields()
{
    const auto props = parseWeProperties(testutil::json(R"({
        "s": {"type": "slider", "value": 3, "min": 1, "max": 9, "order": 1},
        "b": {"type": "bool", "value": false, "order": 2}
    })"));
    QCOMPARE(props.size(), 2);
    const QJsonObject slider = props[0].toJson();
    QCOMPARE(slider.value(u"type"_s).toString(), u"slider"_s);
    QCOMPARE(slider.value(u"max"_s).toDouble(), 9.0);
    const QJsonObject boolean = props[1].toJson();
    QVERIFY(!boolean.contains(u"min"_s));
    QVERIFY(!boolean.contains(u"options"_s));
    QVERIFY(!boolean.contains(u"condition"_s));
}

void TestWeProperty::keepsFractionalOrder()
{
    const auto props = parseWeProperties(testutil::json(R"({
        "a": {"type": "bool", "order": 151.0002},
        "b": {"type": "bool", "order": 151.0001}
    })"));
    QCOMPARE(props.size(), 2);
    QCOMPARE(props[0].key, u"b"_s);
    QCOMPARE(props[1].key, u"a"_s);
    QCOMPARE(props[1].order, 151.0002);
}

void TestWeProperty::acceptsNumericStrings()
{
    const auto props = parseWeProperties(testutil::json(R"({
        "a": {"type": "slider", "order": "3", "min": "0.5", "max": " 4 ", "step": true}
    })"));
    QCOMPARE(props.size(), 1);
    QCOMPARE(props[0].order, 3.0);
    QCOMPARE(props[0].min, 0.5);
    QCOMPARE(props[0].max, 4.0);
    QCOMPARE(props[0].step, 1.0);
}

void TestWeProperty::skipsNonObjectEntries()
{
    const auto props = parseWeProperties(testutil::json(R"({
        "a": 5, "b": "text", "c": null, "d": [1], "e": {"type": "bool"}
    })"));
    QCOMPARE(props.size(), 1);
    QCOMPARE(props[0].key, u"e"_s);
}

QTEST_GUILESS_MAIN(TestWeProperty)
#include "tst_weproperty.moc"
