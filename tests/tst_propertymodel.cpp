#include "propertymodel.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

using namespace Qt::StringLiterals;
using murasaki::PropertyModel;

namespace {

QVariantMap prop(const QString &key, const QString &type, const QVariant &value, double order = 0,
                 const QString &condition = {})
{
    QVariantMap p{{u"key"_s, key}, {u"type"_s, type}, {u"text"_s, key}, {u"value"_s, value}, {u"order"_s, order}};
    if (!condition.isEmpty())
        p.insert(u"condition"_s, condition);
    return p;
}

QVariantMap slider(const QString &key, double value, double min, double max, double step = 0, double order = 0)
{
    QVariantMap p = prop(key, u"slider"_s, value, order);
    p.insert(u"min"_s, min);
    p.insert(u"max"_s, max);
    if (step > 0)
        p.insert(u"step"_s, step);
    return p;
}

QVariantMap combo(const QString &key, const QVariant &value, const QVariantList &values, double order = 0)
{
    QVariantList options;
    for (const QVariant &v : values)
        options.append(QVariantMap{{u"label"_s, v.toString()}, {u"value"_s, v}});
    QVariantMap p = prop(key, u"combo"_s, value, order);
    p.insert(u"options"_s, options);
    return p;
}

QVariantMap item(const QVariantList &properties, const QVariantMap &presetValues = {})
{
    return QVariantMap{{u"properties"_s, properties}, {u"presetValues"_s, presetValues}};
}

} // namespace

class TstPropertyModel : public QObject
{
    Q_OBJECT

private:
    static QVariant value(const PropertyModel &m, int row, int role)
    {
        return m.data(m.index(row, 0), role);
    }
    static QStringList keys(const PropertyModel &m)
    {
        QStringList out;
        for (int i = 0; i < m.rowCount(); ++i)
            out << value(m, i, PropertyModel::KeyRole).toString();
        return out;
    }
    static int rowOf(const PropertyModel &m, const QString &key)
    {
        return keys(m).indexOf(key);
    }
    static bool visible(const PropertyModel &m, const QString &key)
    {
        return value(m, rowOf(m, key), PropertyModel::VisibleRole).toBool();
    }
    // A condition on row "t" driven by switch "a" (default 1) and text "x" (default "x").
    static PropertyModel *withCondition(const QString &condition, const QVariant &a = 1, QObject *parent = nullptr)
    {
        auto *m = new PropertyModel(parent);
        m->load(item({prop(u"a"_s, u"slider"_s, a, 1), prop(u"b"_s, u"bool"_s, true, 2),
                      prop(u"t"_s, u"textinput"_s, u"hello"_s, 3, condition)}),
                {});
        return m;
    }

private slots:
    void onlyEditableTypesBecomeRows()
    {
        PropertyModel m;
        m.load(item({prop(u"l"_s, u"label"_s, QVariant()), prop(u"g"_s, u"group"_s, QVariant()),
                     prop(u"u"_s, u"unknown"_s, 1), prop(u"s"_s, u"slider"_s, 1), prop(u"b"_s, u"bool"_s, true),
                     prop(u"c"_s, u"color"_s, u"1 0 0"_s), prop(u"m"_s, u"combo"_s, 1),
                     prop(u"t"_s, u"textinput"_s, u"x"_s), prop(u"f"_s, u"file"_s, u""_s),
                     prop(u"d"_s, u"directory"_s, u""_s)}),
               {});
        QCOMPARE(m.rowCount(), 7);
        QCOMPARE(keys(m), (QStringList{u"s"_s, u"b"_s, u"c"_s, u"m"_s, u"t"_s, u"f"_s, u"d"_s}));
        QCOMPARE(value(m, 2, PropertyModel::ValueRole).toString(), u"1 0 0"_s);
    }

    void aUserShortcutIsAnEditableTextProperty()
    {
        PropertyModel m;
        m.load(item({prop(u"launcher1"_s, u"usershortcut"_s, u""_s)}), {});
        QCOMPARE(m.rowCount(), 1);
        QCOMPARE(value(m, 0, PropertyModel::TypeRole).toString(), u"usershortcut"_s);
        QCOMPARE(value(m, 0, PropertyModel::ValueRole).toString(), QString());
        QSignalSpy edited(&m, &PropertyModel::edited);
        m.setValue(0, u"org.mozilla.firefox.desktop"_s); // the stored value is the desktop id
        QCOMPARE(edited.size(), 1);
        QCOMPARE(value(m, 0, PropertyModel::ValueRole).toString(), u"org.mozilla.firefox.desktop"_s);
        m.setValue(0, u""_s); // cleared: back to the default, so no override is left
        QCOMPARE(value(m, 0, PropertyModel::ValueRole).toString(), QString());
    }

    void orderingWithFractionalOrderIsStable()
    {
        PropertyModel m;
        m.load(item({prop(u"e"_s, u"bool"_s, true, 2), prop(u"a"_s, u"bool"_s, true, 1.5),
                     prop(u"d"_s, u"bool"_s, true, 1.25), prop(u"b"_s, u"bool"_s, true, 1.5),
                     prop(u"c"_s, u"bool"_s, true, 0)}),
               {});
        QCOMPARE(keys(m), (QStringList{u"c"_s, u"d"_s, u"a"_s, u"b"_s, u"e"_s}));
    }

    void precedenceDefaultPresetOverrideEdit()
    {
        const QVariantList props{slider(u"s"_s, 10, 0, 100, 0, 1), prop(u"b"_s, u"bool"_s, false, 2),
                                 prop(u"t"_s, u"textinput"_s, u"def"_s, 3)};
        PropertyModel m;
        m.load(item(props), {});
        QCOMPARE(value(m, 0, PropertyModel::ValueRole).toDouble(), 10.0);

        m.load(item(props, {{u"s"_s, 20}, {u"t"_s, QVariant()}}), {});
        QCOMPARE(value(m, 0, PropertyModel::ValueRole).toDouble(), 20.0);
        QCOMPARE(value(m, 2, PropertyModel::ValueRole).toString(), u"def"_s); // null preset keeps the default
        QVERIFY(m.overrides().isEmpty());

        m.load(item(props, {{u"s"_s, 20}}), {{u"s"_s, 30}, {u"b"_s, true}});
        QCOMPARE(value(m, 0, PropertyModel::ValueRole).toDouble(), 30.0);
        QCOMPARE(value(m, 1, PropertyModel::ValueRole).toBool(), true);
        QCOMPARE(m.overrides(), (QVariantMap{{u"s"_s, 30.0}, {u"b"_s, true}}));

        m.setValue(0, 40);
        QCOMPARE(value(m, 0, PropertyModel::ValueRole).toDouble(), 40.0);
        QCOMPARE(m.overrides().value(u"s"_s).toDouble(), 40.0);
    }

    void missingValuesGetSensibleDefaults()
    {
        PropertyModel m;
        m.load(item({prop(u"b"_s, u"bool"_s, QVariant(), 1), slider(u"s"_s, 0, 5, 9)}), {});
        QCOMPARE(value(m, 0, PropertyModel::ValueRole), QVariant(false));
        QVERIFY(m.overrides().isEmpty());
    }

    void overridesAreMinimal()
    {
        PropertyModel m;
        m.load(item({slider(u"s"_s, 5, 0, 10, 0, 1), prop(u"b"_s, u"bool"_s, true, 2),
                     combo(u"m"_s, 2, {1, 2, 3}, 3), prop(u"t"_s, u"textinput"_s, u"a"_s, 4)}),
               {});
        QSignalSpy edited(&m, &PropertyModel::edited);
        QVERIFY(m.overrides().isEmpty());

        m.setValue(0, 5.0); // equal to default
        m.setValue(1, true);
        m.setValue(2, u"2"_s); // numeric string equals the number
        m.setValue(3, u"a"_s);
        QVERIFY(m.overrides().isEmpty());
        QCOMPARE(edited.size(), 0); // equal-to-current edits are not edits

        m.setValue(0, u"7"_s); // numeric string for a slider is a number
        QCOMPARE(m.overrides().value(u"s"_s).toDouble(), 7.0);
        m.setValue(0, 5);
        QVERIFY(!m.overrides().contains(u"s"_s));

        m.setValue(2, 3);
        m.setValue(3, u"b"_s);
        QCOMPARE(m.overrides().size(), 2);
        QCOMPARE(m.overrides().value(u"t"_s).toString(), u"b"_s);
    }

    void overrideThatMatchesDefaultIsDroppedLoosely()
    {
        PropertyModel m;
        m.load(item({combo(u"m"_s, 2, {1, 2, 3}), slider(u"s"_s, 5, 0, 10, 0, 1)}), {{u"m"_s, u"2"_s}, {u"s"_s, u"5"_s}});
        QVERIFY(m.overrides().isEmpty());
    }

    void comboComparisonIsLoose()
    {
        PropertyModel m;
        m.load(item({combo(u"m"_s, u"x"_s, {u"x"_s, 2, u"y"_s})}), {});
        m.setValue(0, 2);
        QCOMPARE(m.overrides().value(u"m"_s).toInt(), 2);
        m.setValue(0, u"x"_s);
        QVERIFY(m.overrides().isEmpty());
        const QVariantList options = value(m, 0, PropertyModel::OptionsRole).toList();
        QCOMPARE(options.size(), 3);
        QCOMPARE(options.at(1).toMap().value(u"value"_s).toInt(), 2);
        QCOMPARE(options.at(1).toMap().value(u"label"_s).toString(), u"2"_s);
    }

    void htmlIsStrippedFromText()
    {
        QCOMPARE(PropertyModel::plainText(u"<b>Bold</b> &amp; <font color=\"red\">red</font>&nbsp;text"_s),
                 u"Bold & red text"_s);
        QCOMPARE(PropertyModel::plainText(u"a<br>b &lt;i&gt; &#65;&#x42; &bogus;"_s), u"a b <i> AB &bogus;"_s);
        QCOMPARE(PropertyModel::plainText(u"1 < 2 and 3 > 2"_s), u"1 < 2 and 3 > 2"_s);

        PropertyModel m;
        QVariantMap p = prop(u"k"_s, u"bool"_s, true);
        p.insert(u"text"_s, u"<h2>Glow &amp; <i>Bloom</i></h2>"_s);
        m.load(item({p}), {});
        QCOMPARE(value(m, 0, PropertyModel::TextRole).toString(), u"Glow & Bloom"_s);
    }

    void resetWithoutChangesDoesNotEmitEdited()
    {
        PropertyModel m;
        m.load(item({slider(u"s"_s, 5, 0, 10)}), {});
        QSignalSpy edited(&m, &PropertyModel::edited);
        m.resetToDefaults();
        QCOMPARE(edited.size(), 0);
        m.setValue(0, 7);
        QCOMPARE(edited.size(), 1);
        m.resetToDefaults();
        QCOMPARE(edited.size(), 2);
    }

    void labelLocalizationChain()
    {
        const QVariantMap loc{
            {u"en-us"_s, QVariantMap{{u"ui_a"_s, u"Alpha"_s}, {u"ui_only_en"_s, u"English only"_s}}},
            {u"es-es"_s, QVariantMap{{u"ui_a"_s, u"Alfa ES"_s}}},
            {u"es-mx"_s, QVariantMap{{u"ui_a"_s, u"Alfa MX"_s}}},
            {u"de-de"_s, QVariantMap{{u"ui_only_de"_s, u"Nur Deutsch"_s}}},
        };
        using M = PropertyModel;
        QCOMPARE(M::resolveLabel(u"ui_a"_s, loc, u"es_MX"_s), u"Alfa MX"_s); // exact
        QCOMPARE(M::resolveLabel(u"ui_a"_s, loc, u"es_AR"_s), u"Alfa ES"_s); // same language
        QCOMPARE(M::resolveLabel(u"ui_a"_s, loc, u"fr_FR"_s), u"Alpha"_s); // English
        QCOMPARE(M::resolveLabel(u"ui_only_en"_s, loc, u"es_MX"_s), u"English only"_s);
        QCOMPARE(M::resolveLabel(u"ui_only_de"_s, loc, u"es_MX"_s), u"Nur Deutsch"_s); // first available
        // Not in localization: built-in table, then humanized; never a raw ui_ key.
        QCOMPARE(M::resolveLabel(u"ui_browse_properties_scheme_color"_s, {}, u"en_US"_s), u"Scheme color"_s);
        QCOMPARE(M::resolveLabel(u"ui_visual_bar_bargap"_s, loc, u"en_US"_s), u"Visual bar bargap"_s);
        QCOMPARE(M::resolveLabel(u"Plain <b>text</b>"_s, loc, u"en_US"_s), u"Plain text"_s);
        QCOMPARE(M::resolveLabel({}, {}, u"en_US"_s, u"my_key"_s), u"My key"_s);
    }

    void labelsAndOptionsAreLocalizedOnLoad()
    {
        QVariantMap p = combo(u"c"_s, u"0"_s, {u"0"_s, u"1"_s});
        p.insert(u"text"_s, u"ui_c"_s);
        p.insert(u"options"_s, QVariantList{QVariantMap{{u"label"_s, u"ui_c_0"_s}, {u"value"_s, u"0"_s}},
                                            QVariantMap{{u"label"_s, u"ui_c_1"_s}, {u"value"_s, u"1"_s}}});
        QVariantMap it = item({p});
        it.insert(u"localization"_s, QVariantMap{{u"es-es"_s, QVariantMap{{u"ui_c"_s, u"Modo"_s}, {u"ui_c_0"_s, u"Uno"_s}}}});
        PropertyModel m;
        m.setLocale(u"es_ES"_s);
        m.load(it, {});
        QCOMPARE(value(m, 0, PropertyModel::TextRole).toString(), u"Modo"_s);
        const QVariantList options = value(m, 0, PropertyModel::OptionsRole).toList();
        QCOMPARE(options.at(0).toMap().value(u"label"_s).toString(), u"Uno"_s);
        QCOMPARE(options.at(1).toMap().value(u"label"_s).toString(), u"C 1"_s);
        QCOMPARE(options.at(1).toMap().value(u"value"_s).toString(), u"1"_s);
    }

    void stepComputation()
    {
        QCOMPARE(PropertyModel::effectiveStep(0, 100, 0), 1.0);
        QCOMPARE(PropertyModel::effectiveStep(0, 100, 0.5), 0.5);
        QCOMPARE(PropertyModel::effectiveStep(0, 1, 0), 1.0); // integer range: 1 even if coarse
        QCOMPARE(PropertyModel::effectiveStep(0.5, 2.5, 0), 0.02);
        QCOMPARE(PropertyModel::effectiveStep(0, 0, 0), 1.0);

        PropertyModel m;
        m.load(item({slider(u"a"_s, 1, 0, 10, 0, 1), slider(u"b"_s, 1, 0, 2.5, 0, 2), slider(u"c"_s, 1, 0, 10, 0.25, 3)}), {});
        QCOMPARE(value(m, 0, PropertyModel::StepRole).toDouble(), 0.0);
        QCOMPARE(value(m, 0, PropertyModel::EffectiveStepRole).toDouble(), 1.0);
        QCOMPARE(value(m, 1, PropertyModel::EffectiveStepRole).toDouble(), 0.025);
        QCOMPARE(value(m, 2, PropertyModel::StepRole).toDouble(), 0.25);
        QCOMPARE(value(m, 2, PropertyModel::EffectiveStepRole).toDouble(), 0.25);
        QCOMPARE(value(m, 1, PropertyModel::MaxRole).toDouble(), 2.5);
        QCOMPARE(value(m, 1, PropertyModel::MinRole).toDouble(), 0.0);
    }

    void resetToDefaultsDropsEditsAndOverrides()
    {
        PropertyModel m;
        m.load(item({slider(u"s"_s, 5, 0, 10, 0, 1), prop(u"b"_s, u"bool"_s, false, 2),
                     prop(u"t"_s, u"textinput"_s, u"d"_s, 3, u"b.value"_s)},
                    {{u"s"_s, 6}}),
               {{u"s"_s, 9}});
        m.setValue(1, true);
        QVERIFY(visible(m, u"t"_s));
        QCOMPARE(m.overrides().size(), 2);

        QSignalSpy edited(&m, &PropertyModel::edited);
        QSignalSpy changed(&m, &PropertyModel::dataChanged);
        m.resetToDefaults();
        QCOMPARE(edited.size(), 1);
        QVERIFY(changed.size() >= 2);
        QVERIFY(m.overrides().isEmpty());
        QCOMPARE(value(m, 0, PropertyModel::ValueRole).toDouble(), 6.0); // preset survives
        QCOMPARE(value(m, 1, PropertyModel::ValueRole).toBool(), false);
        QVERIFY(!visible(m, u"t"_s));
    }

    void conditionsFollowEdits()
    {
        std::unique_ptr<PropertyModel> m(withCondition(u"b.value"_s));
        QVERIFY(visible(*m, u"t"_s));
        QSignalSpy changed(m.get(), &PropertyModel::dataChanged);
        m->setValue(rowOf(*m, u"b"_s), false);
        QVERIFY(!visible(*m, u"t"_s));
        // value of b and visibility of t changed; nothing else.
        QCOMPARE(changed.size(), 2);
        QSet<int> rows;
        for (const auto &args : changed)
            rows.insert(args.at(0).toModelIndex().row());
        QCOMPARE(rows, (QSet<int>{rowOf(*m, u"b"_s), rowOf(*m, u"t"_s)}));

        changed.clear();
        m->setValue(rowOf(*m, u"a"_s), 3); // does not affect t
        QCOMPARE(changed.size(), 1);
        m->setValue(rowOf(*m, u"b"_s), true);
        QVERIFY(visible(*m, u"t"_s));
    }

    void conditionForms()
    {
        struct Case
        {
            QString condition;
            QVariant a;
            bool expected;
        };
        const QList<Case> cases{
            {u"a.value"_s, 1, true},
            {u"a.value"_s, 0, false},
            {u"a.value && b.value"_s, 1, true},
            {u"a.value && !b.value"_s, 1, false},
            {u"!a.value"_s, 0, true},
            {u"!a.value"_s, 1, false},
            {u"a.value == 2"_s, 2, true},
            {u"a.value == 2"_s, 3, false},
            {u"a.value != \"x\""_s, 2, true},
            {u"a.value == \"2\""_s, 2, true},
            {u"$a.value"_s, 1, true},
            {u"$a.value"_s, 0, false},
            {u"$a.value && $$b.value"_s, 0, false},
            {u"a.value >= 2 || a.value <= 0"_s, 2, true},
            {u"a.value >= 2 || a.value <= 0"_s, 1, false},
            {u"(a.value > 1 && a.value < 5) || a.value === 9"_s, 9, true},
            {u"unknown.value"_s, 1, true}, // ReferenceError keeps the row visible
        };
        for (const Case &c : cases) {
            std::unique_ptr<PropertyModel> m(withCondition(c.condition, c.a));
            QVERIFY2(visible(*m, u"t"_s) == c.expected,
                     qPrintable(c.condition + u" with a="_s + c.a.toString()));
        }
    }

    void conditionsSeeNonEditableProperties()
    {
        PropertyModel m;
        m.load(item({prop(u"hidden"_s, u"label"_s, 0, 1), prop(u"t"_s, u"bool"_s, true, 2, u"hidden.value"_s)}), {});
        QVERIFY(!visible(m, u"t"_s));
    }

    void conditionCanUseKeysThatAreNotIdentifiers()
    {
        PropertyModel m;
        m.load(item({prop(u"my key"_s, u"bool"_s, false, 1), prop(u"class"_s, u"bool"_s, false, 2),
                     prop(u"a"_s, u"bool"_s, false, 3), prop(u"t"_s, u"bool"_s, true, 4, u"a.value"_s)}),
               {});
        QVERIFY(!visible(m, u"t"_s)); // odd keys are skipped but do not break the function
    }

    void hostileExpressionsAreHarmless()
    {
        const QStringList hostile{
            u"this.constructor.constructor(\"return 0\")()"_s,
            u"a.value = 5"_s,
            u"(()=>0)()"_s,
            u"a.value.constructor"_s,
            u"a.value; 0"_s,
            u"a.value += 1"_s,
            u"a.value >>= 1"_s,
            u"a.value == 1 || a.__proto__"_s,
            u"a.value.prototype"_s,
            u"a[\"value\"]"_s,
            u"new Date()"_s,
            u"import(\"x\")"_s,
            u"function(){return 0}()"_s,
            u"`${0}`"_s,
            u"{}"_s,
            u"a.value++"_s,
            u"a\\u002evalue"_s,
            QString(u"a.value"_s) + QString(600, u' ') + u"&& 0"_s,
        };
        for (const QString &expr : hostile) {
            std::unique_ptr<PropertyModel> m(new PropertyModel);
            QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"unsafe condition"_s));
            m->load(item({prop(u"a"_s, u"slider"_s, 1, 1), prop(u"t"_s, u"bool"_s, true, 2, expr)}), {});
            QVERIFY2(visible(*m, u"t"_s), qPrintable(expr.left(40)));
            // The scope was not modified by the attempt.
            QCOMPARE(value(*m, 0, PropertyModel::ValueRole).toDouble(), 1.0);
        }
    }

    void sameRejectedExpressionWarnsOnce()
    {
        PropertyModel m;
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"unsafe condition"_s));
        m.load(item({prop(u"a"_s, u"bool"_s, true, 1, u"a.value = 1"_s), prop(u"b"_s, u"bool"_s, true, 2, u"a.value = 1"_s)}),
               {});
        m.setValue(0, false); // would print a second warning, which fails the test, if it were not cached
        QVERIFY(visible(m, u"a"_s));
    }

    void groupingParenthesesStillWork()
    {
        const QList<QPair<QString, bool>> cases{{u"(a.value || b.value) && c.value"_s, true},
                                                {u"(a.value || b.value) && !c.value"_s, false},
                                                {u"!(a.value)"_s, false},
                                                {u"!(a.value && b.value) && (c.value)"_s, true},
                                                {u"a.value == (1)"_s, true}};
        for (const auto &c : cases) {
            PropertyModel m;
            m.load(item({prop(u"a"_s, u"bool"_s, true, 1), prop(u"b"_s, u"bool"_s, false, 2),
                         prop(u"c"_s, u"bool"_s, true, 3), prop(u"t"_s, u"bool"_s, true, 4, c.first)}),
                   {});
            QCOMPARE(visible(m, u"t"_s), c.second);
        }
    }

    void unknownOverrideKeysArePreserved()
    {
        PropertyModel m;
        m.load(item({prop(u"b"_s, u"bool"_s, false)}), {{u"gone"_s, 7}, {u"b"_s, true}});
        QCOMPARE(m.overrides(), (QVariantMap{{u"gone"_s, 7}, {u"b"_s, true}}));
        m.setValue(0, false);
        QCOMPARE(m.overrides(), (QVariantMap{{u"gone"_s, 7}}));
        m.resetToDefaults();
        QVERIFY(m.overrides().isEmpty());
    }

    void colorsCompareByComponent()
    {
        PropertyModel m;
        m.load(item({prop(u"c"_s, u"color"_s, u"1 0 0.5"_s)}), {});
        m.setValue(0, u"1.0 0.00 0.50001"_s);
        QVERIFY(m.overrides().isEmpty());
        m.setValue(0, u"1 0 0.6"_s);
        QCOMPARE(m.overrides().size(), 1);
    }

    void editedOnlyWhenChanged()
    {
        PropertyModel m;
        m.load(item({prop(u"b"_s, u"bool"_s, false)}), {});
        QSignalSpy edited(&m, &PropertyModel::edited);
        m.setValue(0, false);
        QCOMPARE(edited.size(), 0);
        m.setValue(0, true);
        QCOMPARE(edited.size(), 1);
    }

    void callsAndBypassesAreRejected()
    {
        const QStringList rejected{
            u"\"a\".repeat(1e9)"_s, u"\"a\"?.repeat?.(1e9)"_s, u"\"a\".repeat/**/(1e9)"_s,
            u"\"a\".repeat//\n(1e9)"_s, u"Math.max(1,2)"_s, u"Math.max?.(1,2)"_s, u"a.value.toString()"_s,
            u"a.value.toString ()"_s, u"(a.value)(1)"_s, u"a.value (1)"_s, u"a.value?.x"_s, u"a.value ? 1 : 0"_s,
            u"a.value, 1"_s, u"a.value + 1"_s, u"a.value % 2"_s, u"a.value / 2"_s, u"a.value.length"_s,
            u"a.value.value"_s, u"a . x"_s, u"a.value.toFixed"_s, u"Math.PI > 3"_s, u"String"_s, u"unknown.value"_s,
            u"a.value = 1"_s, u"a.value &&"_s, u"&& a.value"_s, u"(a.value"_s, u"a.value)"_s, u"()"_s,
            u"a.value 1"_s, u"1 a.value"_s, u"1(2)"_s, u"true.value"_s, u"1.5.x"_s, u"0x10"_s, u"1abc"_s,
            u"a.value == 'unterminated"_s, u"a.value == \"a\\x\""_s, u"a.value == \"" + QString(300, u'x') + u"\""_s,
            u"a.value !== = 1"_s, u"a.value === === 1"_s, u"a.value // x"_s, u"a.value /* x */"_s,
            u"a.value; 0"_s, u"a[\"value\"]"_s, u"{}"_s, u"`x`"_s, u"a.value@"_s, u"a.value\\u0020"_s,
        };
        for (const QString &expr : rejected) {
            PropertyModel m;
            QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"unsafe condition"_s));
            m.load(item({prop(u"a"_s, u"slider"_s, 1, 1), prop(u"t"_s, u"bool"_s, true, 2, expr)}), {});
            QVERIFY2(visible(m, u"t"_s), qPrintable(expr));
        }
    }

    void realisticConditionsAreAccepted()
    {
        // Each accepted condition is false for these values (a=1, b=false, c=true, d=2), so a visible row
        // would mean it was rejected rather than evaluated.
        const QStringList falseConditions{
            u"b.value"_s, u"a.value && b.value"_s, u"a.value && c.value && d.value >= 3"_s,
            u"c.value && a.value == 2"_s, u"c.value && a.value && !(d.value == 2)"_s,
            u"a.value && c.value && b.value && a.value"_s, u"!c.value && a.value"_s,
            u"a.value === 2 && c.value"_s, u"$b.value"_s, u"c.value && !a.value"_s,
            u"a.value && (d.value == 5 || d.value == 7)"_s, u"d.value != 2"_s, u"d.value == \"3\""_s,
            u"d.value == 'x' || false"_s, u"null"_s, u"undefined"_s, u"d.value > 2.5e0"_s,
            u"  ( ( b.value ) )  "_s, u"a . value && b . value"_s, u"c.value\n&&\nb.value"_s,
        };
        for (const QString &cond : falseConditions) {
            PropertyModel m;
            m.load(item({prop(u"a"_s, u"bool"_s, true, 1), prop(u"b"_s, u"bool"_s, false, 2),
                         prop(u"c"_s, u"bool"_s, true, 3), prop(u"d"_s, u"slider"_s, 2, 4),
                         prop(u"t"_s, u"bool"_s, true, 5, cond)}),
                   {});
            QVERIFY2(!visible(m, u"t"_s), qPrintable(cond));
        }
        const QStringList trueConditions{u"a.value && c.value"_s, u"!b.value"_s, u"d.value == 2 && (a.value || b.value)"_s,
                                         u"true"_s};
        for (const QString &cond : trueConditions) {
            PropertyModel m;
            m.load(item({prop(u"a"_s, u"bool"_s, true, 1), prop(u"b"_s, u"bool"_s, false, 2),
                         prop(u"c"_s, u"bool"_s, true, 3), prop(u"d"_s, u"slider"_s, 2, 4),
                         prop(u"t"_s, u"bool"_s, true, 5, cond)}),
                   {});
            QVERIFY2(visible(m, u"t"_s), qPrintable(cond));
        }
    }

    // The token filter is bypassed here to check that the engine itself is hardened as defense in depth.
    void engineIsHardenedWithoutTheFilter()
    {
        struct Unfiltered : PropertyModel
        {
            bool conditionAllowed(const QString &, const QSet<QString> &) const override { return true; }
        };
        // Every probe either throws (row stays visible) or, if it ran, would evaluate to false.
        const QStringList probes{
            u"\"a\".repeat(1e9)"_s,           u"\"a\".padStart(1e9)"_s, u"Function(\"return 0\")()"_s,
            u"eval(\"0\")"_s,                 u"Object.keys(a).length == 99"_s, u"Reflect.ownKeys(a).length == 99"_s,
            u"JSON.stringify(a) == \"x\""_s, u"Qt.platform.os == \"x\""_s,
            u"(a.value = 0) == 99"_s,         u"a.value.constructor.prototype.valueOf = 0"_s,
            u"(\"x\".__lookupGetter__(\"length\")) == 99"_s,
        };
        QElapsedTimer timer;
        timer.start();
        for (const QString &probe : probes) {
            Unfiltered m;
            m.load(item({prop(u"a"_s, u"slider"_s, 1, 1), prop(u"t"_s, u"bool"_s, true, 2, probe)}), {});
            QVERIFY2(visible(m, u"t"_s), qPrintable(probe));
            QCOMPARE(value(m, 0, PropertyModel::ValueRole).toDouble(), 1.0);
        }
        QVERIFY2(timer.elapsed() < 3000, "an unfiltered probe was slow");

        // One row cannot alter what another sees.
        Unfiltered m;
        m.load(item({prop(u"a"_s, u"slider"_s, 1, 1), prop(u"x"_s, u"bool"_s, true, 2, u"a.value = 0"_s),
                     prop(u"y"_s, u"bool"_s, true, 3, u"Math.PI = 3"_s),
                     prop(u"t"_s, u"bool"_s, true, 4, u"a.value == 1 && Math.PI > 3.14"_s)}),
               {});
        QVERIFY(visible(m, u"x"_s));
        QVERIFY(visible(m, u"t"_s));
    }

    void loadResetsModelAndCount()
    {
        PropertyModel m;
        QSignalSpy count(&m, &PropertyModel::countChanged);
        QSignalSpy reset(&m, &PropertyModel::modelReset);
        m.load(item({prop(u"a"_s, u"bool"_s, true)}), {});
        QCOMPARE(m.rowCount(), 1);
        QCOMPARE(count.size(), 1);
        m.setValue(0, false);
        m.load(item({prop(u"a"_s, u"bool"_s, true)}), {});
        QCOMPARE(value(m, 0, PropertyModel::ValueRole).toBool(), true); // edits do not survive a load
        QCOMPARE(reset.size(), 2);
        m.load(QVariantMap(), QVariantMap());
        QCOMPARE(m.rowCount(), 0);
        QCOMPARE(count.size(), 2);
    }

    void invalidRowsAreIgnored()
    {
        PropertyModel m;
        m.load(item({prop(u"a"_s, u"bool"_s, true)}), {});
        m.setValue(-1, true);
        m.setValue(5, true);
        QVERIFY(m.overrides().isEmpty());
        QVERIFY(!m.index(5, 0).isValid());
        QVERIFY(value(m, 5, PropertyModel::KeyRole).isNull());
    }
};

QTEST_GUILESS_MAIN(TstPropertyModel)
#include "tst_propertymodel.moc"
