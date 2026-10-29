// Parameters, parts, breathing, blinking, expressions, pose and looking at the cursor (src/puppet/{params,expression,pose,lookat}.*).
#include "puppet/expression.h"
#include "puppet/lookat.h"
#include "puppet/params.h"
#include "puppet/pose.h"
#include "puppetlimits.h"

#include <QTest>

#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;
using namespace murasaki::puppet;

namespace {

std::string_view view(const QByteArray &b)
{
    return std::string_view(b.constData(), size_t(b.size()));
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

} // namespace

class TestPuppetLife : public QObject
{
    Q_OBJECT
private slots:
    void parameterSet();
    void parameterSetLimits();
    void partSet();
    void breath();
    void blinkIsDeterministic();
    void blinkDoesNotDependOnStepSize();
    void blinkHappensAtTheRightPace();
    void blinkTouchesOnlyEyes();
    void expressionsParse_data();
    void expressionsParse();
    void expressionBlends();
    void expressionFades();
    void expressionLayersAreBounded();
    void poseParse_data();
    void poseParse();
    void poseSwitchesAndFades();
    void lookAtTarget();
    void lookAtSmoothsAndApplies();
};

void TestPuppetLife::parameterSet()
{
    ParameterSet p;
    QVERIFY(p.add("ParamAngleX", -30, 0, 30));
    QVERIFY(p.add("ParamEyeLOpen", 0, 1, 1));
    QCOMPARE(p.size(), size_t(2));
    QCOMPARE(p.find("ParamAngleX"), 0);
    QCOMPARE(p.find("nope"), -1);
    QCOMPARE(p.value(1), 1.0); // the default
    QVERIFY(p.set(0, 100));
    QCOMPARE(p.value(0), 30.0); // clamped
    QVERIFY(p.set(0, -100));
    QCOMPARE(p.value(0), -30.0);
    QVERIFY(p.set(0, 5));
    QVERIFY(p.set(0, kNaN)); // ignored, not an error
    QCOMPARE(p.value(0), 5.0);
    QVERIFY(p.set(0, std::numeric_limits<double>::infinity()));
    QCOMPARE(p.value(0), 5.0);
    QVERIFY(!p.set(2, 1));
    QVERIFY(!p.set(-1, 1));
    QVERIFY(p.setById("ParamAngleX", 7));
    QVERIFY(!p.setById("nope", 7));
    const std::vector<double> saved = p.snapshot();
    QCOMPARE(saved.size(), size_t(2));
    QVERIFY(p.set(0, 1));
    QVERIFY(p.restore(saved));
    QCOMPARE(p.value(0), 7.0);
    QVERIFY(!p.restore({1.0}));
    QCOMPARE(p.value(0), 7.0);
    p.resetToDefaults();
    QCOMPARE(p.value(0), 0.0);
    QCOMPARE(p.value(1), 1.0);
}

void TestPuppetLife::parameterSetLimits()
{
    ParameterSet p;
    QVERIFY(!p.add("", 0, 0, 1));
    QVERIFY(!p.add(std::string(129, 'x'), 0, 0, 1));
    QVERIFY(p.add(std::string(128, 'x'), 0, 0, 1));
    QVERIFY(!p.add(std::string(128, 'x'), 0, 0, 1)); // repeated
    QVERIFY(!p.add("a", 1, 0, 0));                    // min > max
    QVERIFY(!p.add("a", 0, 2, 1));                    // default outside
    QVERIFY(!p.add("a", 0, -1, 1));
    QVERIFY(!p.add("a", kNaN, 0, 1));
    QVERIFY(!p.add("a", 0, 0, std::numeric_limits<double>::infinity()));
    QVERIFY(p.add("a", 0, 0, 0)); // a fixed value is a range too
    ParameterSet many;
    for (qint64 i = 0; i < kMaxParameters; ++i)
        QVERIFY(many.add("p" + std::to_string(i), 0, 0, 1));
    QVERIFY(!many.add("one more", 0, 0, 1));
    QCOMPARE(many.size(), size_t(kMaxParameters));
}

void TestPuppetLife::partSet()
{
    PartSet parts;
    QVERIFY(parts.add("PartArmL"));
    QVERIFY(!parts.add("PartArmL"));
    QVERIFY(!parts.add(""));
    QCOMPARE(parts.opacity(0), 1.0);
    QVERIFY(parts.set(0, 3));
    QCOMPARE(parts.opacity(0), 1.0);
    QVERIFY(parts.set(0, -3));
    QCOMPARE(parts.opacity(0), 0.0);
    QVERIFY(parts.set(0, kNaN));
    QCOMPARE(parts.opacity(0), 0.0);
    QVERIFY(!parts.set(1, 0.5));
    QCOMPARE(parts.find("PartArmL"), 0);
    QCOMPARE(parts.find("x"), -1);
    parts.resetToOpaque();
    QCOMPARE(parts.opacity(0), 1.0);
    PartSet many;
    for (qint64 i = 0; i < kMaxParts; ++i)
        QVERIFY(many.add("p" + std::to_string(i)));
    QVERIFY(!many.add("more"));
}

void TestPuppetLife::breath()
{
    ParameterSet none;
    none.add("Other", 0, 0, 1);
    Breath b;
    QVERIFY(!b.apply(none));
    ParameterSet p;
    p.add("ParamBreath", 0, 0, 2);
    double lo = 9, hi = -9;
    for (int i = 0; i < 1000; ++i) {
        b.update(0.01);
        QVERIFY(b.apply(p));
        lo = std::min(lo, p.value(0));
        hi = std::max(hi, p.value(0));
    }
    QVERIFY(lo >= 0 && hi <= 2);
    QVERIFY(lo < 0.02 && hi > 1.98); // it covers the range
    // periodic
    Breath c({2.0});
    p.resetToDefaults();
    c.update(0.3);
    c.apply(p);
    const double first = p.value(0);
    c.update(2.0);
    c.apply(p);
    QVERIFY(std::abs(p.value(0) - first) < 1e-9);
    // bad steps are ignored
    Breath d;
    d.update(-1);
    d.update(kNaN);
    QCOMPARE(d.phase(), 0.0);
    // a period that is not positive does not divide by zero
    Breath e({0.0});
    e.update(1.0);
    QVERIFY(e.apply(p));
    // centred at the start: sin(0) = 0 => the middle
    Breath f;
    ParameterSet q;
    q.add("ParamBreath", 0, 0, 1);
    f.apply(q);
    QVERIFY(std::abs(q.value(0) - 0.5) < 1e-12);
}

void TestPuppetLife::blinkIsDeterministic()
{
    AutoBlink a(42), b(42), c(43);
    bool differs = false;
    for (int i = 0; i < 30000; ++i) {
        a.update(0.01);
        b.update(0.01);
        c.update(0.01);
        QCOMPARE(a.openness(), b.openness());
        differs = differs || a.openness() != c.openness();
    }
    QVERIFY(differs);
    // a seed of zero still works
    AutoBlink z(0);
    z.update(100);
    QVERIFY(z.openness() >= 0 && z.openness() <= 1);
}

void TestPuppetLife::blinkDoesNotDependOnStepSize()
{
    for (const double step : {0.001, 0.0125, 1.0 / 60, 0.25, 1.0}) {
        AutoBlink reference(7), other(7);
        for (int i = 0; i < 100; ++i) {
            reference.update(0.1); // 10 s in tenths
        }
        const int n = int(std::round(10.0 / step));
        for (int i = 0; i < n; ++i)
            other.update(10.0 / n);
        QVERIFY2(std::abs(reference.openness() - other.openness()) < 1e-6, qPrintable(u"step %1"_s.arg(step)));
    }
    // one huge step lands in a valid state
    AutoBlink huge(7);
    huge.update(1e9);
    QVERIFY(huge.openness() >= 0 && huge.openness() <= 1);
    huge.update(kNaN);
    huge.update(-5);
    QVERIFY(huge.openness() >= 0 && huge.openness() <= 1);
}

void TestPuppetLife::blinkHappensAtTheRightPace()
{
    AutoBlink blink(2026);
    int blinks = 0;
    bool closed = false;
    double lastStart = -1;
    double minGap = 1e9, maxGap = 0;
    for (int i = 0; i < 60000; ++i) { // 600 s
        blink.update(0.01);
        const double o = blink.openness();
        QVERIFY(o >= 0 && o <= 1);
        if (o < 0.5 && !closed) {
            closed = true;
            ++blinks;
            const double now = i * 0.01;
            if (lastStart >= 0) {
                minGap = std::min(minGap, now - lastStart);
                maxGap = std::max(maxGap, now - lastStart);
            }
            lastStart = now;
        } else if (o > 0.9) {
            closed = false;
        }
    }
    QVERIFY2(blinks > 600 / 6.5 && blinks < 600 / 2.3, qPrintable(QString::number(blinks)));
    QVERIFY(minGap > 2.0); // never faster than the shortest wait
    QVERIFY(maxGap < 6.5);
    // the blink itself takes closeTime + holdTime + openTime
    AutoBlink one(1, {1.0, 1.0, 0.1, 0.05, 0.15});
    one.update(1.0);
    QCOMPARE(one.openness(), 1.0);
    one.update(0.05);
    QVERIFY(std::abs(one.openness() - 0.5) < 1e-9);
    one.update(0.05);
    QVERIFY(std::abs(one.openness() - 0.0) < 1e-9);
    one.update(0.05);
    QCOMPARE(one.openness(), 0.0); // held
    one.update(0.075);
    QVERIFY(std::abs(one.openness() - 0.5) < 1e-9);
    one.update(0.075);
    QVERIFY(std::abs(one.openness() - 1.0) < 1e-9);
    // all phases of zero length do not hang
    AutoBlink zero(1, {0, 0, 0, 0, 0});
    zero.update(5);
    QVERIFY(zero.openness() >= 0 && zero.openness() <= 1);
}

void TestPuppetLife::blinkTouchesOnlyEyes()
{
    ParameterSet p;
    p.add("ParamEyeLOpen", 0, 1, 1);
    p.add("ParamEyeROpen", 0, 1, 1);
    p.add("ParamMouthOpenY", 0, 1, 1);
    AutoBlink blink(1, {0.5, 0.5, 0.1, 10, 0.1});
    blink.update(0.6);
    blink.update(0.1);
    blink.update(0.05); // closed and holding
    QCOMPARE(blink.openness(), 0.0);
    QCOMPARE(blink.apply(p, {"ParamEyeLOpen", "ParamEyeROpen", "ParamMissing"}), 2);
    QCOMPARE(p.value(0), 0.0);
    QCOMPARE(p.value(1), 0.0);
    QCOMPARE(p.value(2), 1.0);
    QCOMPARE(blink.apply(p, {}), 0);
}

void TestPuppetLife::expressionsParse_data()
{
    QTest::addColumn<QByteArray>("text");
    QTest::addColumn<bool>("ok");
    const auto e = [](const QByteArray &body, const QByteArray &head = R"("Type":"Live2D Expression")") { return "{" + head + "," + body + "}"; };
    QTest::newRow("minimal") << e(R"("Parameters":[])") << true;
    QTest::newRow("full") << e(R"("FadeInTime":0.5,"FadeOutTime":2,"Parameters":[{"Id":"A","Value":1.5,"Blend":"Multiply"},{"Id":"B","Value":-1},{"Id":"C","Value":0,"Blend":"Overwrite"}])", R"("Type":"Live2D Expression","Version":3)") << true;
    QTest::newRow("not json") << QByteArray("x") << false;
    QTest::newRow("array") << QByteArray("[]") << false;
    QTest::newRow("no type") << QByteArray(R"({"Parameters":[]})") << false;
    QTest::newRow("wrong type") << e(R"("Parameters":[])", R"("Type":"Live2D Pose")") << false;
    QTest::newRow("version 2") << e(R"("Parameters":[])", R"("Type":"Live2D Expression","Version":2)") << false;
    QTest::newRow("no parameters") << e(R"("FadeInTime":1)") << false;
    QTest::newRow("parameters object") << e(R"("Parameters":{})") << false;
    QTest::newRow("parameter number") << e(R"("Parameters":[1])") << false;
    QTest::newRow("no id") << e(R"("Parameters":[{"Value":1}])") << false;
    QTest::newRow("empty id") << e(R"("Parameters":[{"Id":"","Value":1}])") << false;
    QTest::newRow("no value") << e(R"("Parameters":[{"Id":"A"}])") << false;
    QTest::newRow("value string") << e(R"("Parameters":[{"Id":"A","Value":"1"}])") << false;
    QTest::newRow("blend unknown") << e(R"("Parameters":[{"Id":"A","Value":1,"Blend":"Subtract"}])") << false;
    QTest::newRow("blend number") << e(R"("Parameters":[{"Id":"A","Value":1,"Blend":1}])") << false;
    QTest::newRow("fade negative") << e(R"("FadeInTime":-1,"Parameters":[])") << false;
    QTest::newRow("fade huge") << e(R"("FadeOutTime":1e9,"Parameters":[])") << false;
    QTest::newRow("fade string") << e(R"("FadeOutTime":"1","Parameters":[])") << false;
    QTest::newRow("repeated key") << QByteArray(R"({"Type":"Live2D Expression","Type":"Live2D Expression","Parameters":[]})") << false;
}

void TestPuppetLife::expressionsParse()
{
    QFETCH(QByteArray, text);
    QFETCH(bool, ok);
    const ExpressionResult r = parseExpression(view(text));
    QCOMPARE(r.ok, ok);
    if (!ok)
        QVERIFY(!r.error.empty());
    if (QTest::currentDataTag() == QByteArray("full")) {
        QCOMPARE(r.expression.fadeIn, 0.5);
        QCOMPARE(r.expression.fadeOut, 2.0);
        QCOMPARE(r.expression.targets.size(), size_t(3));
        QCOMPARE(r.expression.targets[0].blend, Blend::Multiply);
        QCOMPARE(r.expression.targets[1].blend, Blend::Add); // the default
        QCOMPARE(r.expression.targets[2].blend, Blend::Overwrite);
    }
    if (QTest::currentDataTag() == QByteArray("minimal")) {
        QCOMPARE(r.expression.fadeIn, 1.0);
        QCOMPARE(r.expression.fadeOut, 1.0);
    }
    // too many parameters
    QByteArray many = R"({"Type":"Live2D Expression","Parameters":[)";
    for (qint64 i = 0; i <= kMaxParameters; ++i)
        many += (i ? "," : "") + QByteArray("{\"Id\":\"p") + QByteArray::number(i) + "\",\"Value\":1}";
    many += "]}";
    QVERIFY(!parseExpression(view(many)).ok);
    many.replace(",{\"Id\":\"p" + QByteArray::number(kMaxParameters) + "\",\"Value\":1}", "");
    QVERIFY(parseExpression(view(many)).ok);
}

namespace {

std::shared_ptr<const Expression> expr(Blend blend, double value, double fadeIn = 0, double fadeOut = 0, const char *id = "A")
{
    Expression e;
    e.fadeIn = fadeIn;
    e.fadeOut = fadeOut;
    e.targets.push_back({id, value, blend});
    return std::make_shared<Expression>(e);
}

} // namespace

void TestPuppetLife::expressionBlends()
{
    ParameterSet p;
    p.add("A", -10, 2, 10);
    ExpressionManager m;
    // at full weight
    m.set(expr(Blend::Add, 3));
    m.update(0.01);
    m.apply(p);
    QCOMPARE(p.value(0), 5.0);
    p.resetToDefaults();
    m.set(expr(Blend::Multiply, 3));
    m.update(0.01);
    m.update(5);
    m.apply(p);
    QCOMPARE(p.value(0), 6.0);
    p.resetToDefaults();
    m.set(expr(Blend::Overwrite, -4));
    m.update(5);
    m.apply(p);
    QCOMPARE(p.value(0), -4.0);
    // clamped to the parameter's range
    p.resetToDefaults();
    m.set(expr(Blend::Add, 100));
    m.update(5);
    m.apply(p);
    QCOMPARE(p.value(0), 10.0);
    // an unknown id changes nothing
    p.resetToDefaults();
    m.set(expr(Blend::Add, 1, 0, 0, "Missing"));
    m.update(5);
    m.apply(p);
    QCOMPARE(p.value(0), 2.0);
    // two layers in order: the older fading one first, then the new one
    p.resetToDefaults();
    ExpressionManager two;
    two.set(expr(Blend::Add, 1, 0, 100));
    two.update(0.001);
    two.set(expr(Blend::Multiply, 2, 0, 0));
    two.update(0.001);
    two.apply(p);
    QVERIFY(std::abs(p.value(0) - (2 + 1 * 1.0) * 2) < 1e-3); // the first still at about full weight
}

void TestPuppetLife::expressionFades()
{
    ParameterSet p;
    p.add("A", -10, 0, 10);
    ExpressionManager m;
    m.set(expr(Blend::Add, 4, 1.0, 1.0));
    double last = -1;
    for (int i = 0; i < 100; ++i) {
        m.update(0.01);
        p.resetToDefaults();
        m.apply(p);
        QVERIFY(p.value(0) >= last - 1e-12);
        last = p.value(0);
    }
    QVERIFY(std::abs(last - 4.0) < 1e-6);
    // replaced: the old fades out and is gone after its fade
    m.set(expr(Blend::Add, 1, 0, 1.0));
    QCOMPARE(m.active(), size_t(2));
    double prevOld = 1e9;
    for (int i = 0; i < 99; ++i) {
        m.update(0.01);
    }
    QCOMPARE(m.active(), size_t(2));
    m.update(0.02);
    QCOMPARE(m.active(), size_t(1));
    (void)prevOld;
    p.resetToDefaults();
    m.apply(p);
    QVERIFY(std::abs(p.value(0) - 1.0) < 1e-9);
    // a null expression only fades the others out
    m.set(nullptr);
    QCOMPARE(m.active(), size_t(1));
    m.update(0.0);
    m.update(1e9);
    QCOMPARE(m.active(), size_t(0));
    p.resetToDefaults();
    m.apply(p);
    QCOMPARE(p.value(0), 0.0);
    // bad steps are ignored
    m.set(expr(Blend::Add, 1, 1.0, 1.0));
    m.update(kNaN);
    m.update(-1);
    p.resetToDefaults();
    m.apply(p);
    QCOMPARE(p.value(0), 0.0); // age is still 0
}

void TestPuppetLife::expressionLayersAreBounded()
{
    ExpressionManager m;
    for (int i = 0; i < 100; ++i) {
        m.set(expr(Blend::Add, 1, 1, 1000));
        QVERIFY(m.active() <= ExpressionManager::kMaxExpressionLayers);
    }
}

void TestPuppetLife::poseParse_data()
{
    QTest::addColumn<QByteArray>("text");
    QTest::addColumn<bool>("ok");
    QTest::newRow("full") << QByteArray(R"({"Type":"Live2D Pose","Version":3,"FadeInTime":0.25,"Groups":[[{"Id":"A","Link":["A2"]},{"Id":"B","Link":[]}],[{"Id":"C"}]]})") << true;
    QTest::newRow("empty groups") << QByteArray(R"({"Type":"Live2D Pose","Groups":[]})") << true;
    QTest::newRow("not json") << QByteArray("pose") << false;
    QTest::newRow("no type") << QByteArray(R"({"Groups":[]})") << false;
    QTest::newRow("wrong type") << QByteArray(R"({"Type":"Live2D Expression","Groups":[]})") << false;
    QTest::newRow("version 4") << QByteArray(R"({"Type":"Live2D Pose","Version":4,"Groups":[]})") << false;
    QTest::newRow("no groups") << QByteArray(R"({"Type":"Live2D Pose"})") << false;
    QTest::newRow("groups object") << QByteArray(R"({"Type":"Live2D Pose","Groups":{}})") << false;
    QTest::newRow("group object") << QByteArray(R"({"Type":"Live2D Pose","Groups":[{}]})") << false;
    QTest::newRow("group numbers") << QByteArray(R"({"Type":"Live2D Pose","Groups":[[1,2]]})") << false;
    QTest::newRow("node without id") << QByteArray(R"({"Type":"Live2D Pose","Groups":[[{}]]})") << false;
    QTest::newRow("node empty id") << QByteArray(R"({"Type":"Live2D Pose","Groups":[[{"Id":""}]]})") << false;
    QTest::newRow("link string") << QByteArray(R"({"Type":"Live2D Pose","Groups":[[{"Id":"A","Link":"B"}]]})") << false;
    QTest::newRow("link numbers") << QByteArray(R"({"Type":"Live2D Pose","Groups":[[{"Id":"A","Link":[1]}]]})") << false;
    QTest::newRow("fade negative") << QByteArray(R"({"Type":"Live2D Pose","FadeInTime":-1,"Groups":[]})") << false;
}

void TestPuppetLife::poseParse()
{
    QFETCH(QByteArray, text);
    QFETCH(bool, ok);
    const PoseResult r = parsePose(view(text));
    QCOMPARE(r.ok, ok);
    if (QTest::currentDataTag() == QByteArray("full")) {
        QCOMPARE(r.pose.fadeIn, 0.25);
        QCOMPARE(r.pose.groups.size(), size_t(2));
        QCOMPARE(r.pose.groups[0].size(), size_t(2));
        QCOMPARE(r.pose.groups[0][0].links.size(), size_t(1));
    }
    // limits: groups, then nodes (links count too)
    QByteArray groups = R"({"Type":"Live2D Pose","Groups":[)";
    for (qint64 i = 0; i <= kMaxPoseGroups; ++i)
        groups += (i ? "," : "") + QByteArray("[]");
    QVERIFY(!parsePose(view(groups + "]}")).ok);
    QByteArray nodes = R"({"Type":"Live2D Pose","Groups":[[)";
    for (qint64 i = 0; i < kMaxPoseNodes; ++i)
        nodes += (i ? "," : "") + QByteArray("{\"Id\":\"n") + QByteArray::number(i) + "\"}";
    QVERIFY(parsePose(view(nodes + "]]}")).ok);
    QVERIFY(!parsePose(view(nodes + ",{\"Id\":\"x\"}]]}")).ok);
}

void TestPuppetLife::poseSwitchesAndFades()
{
    Pose pose;
    pose.fadeIn = 1.0;
    pose.groups = {{{"ArmA", {"ArmA2"}}, {"ArmB", {}}}};
    PartSet parts;
    for (const char *id : {"ArmA", "ArmA2", "ArmB"})
        parts.add(id);
    PoseState state(pose);
    // all opaque at the start: the first node is shown at once, the other hidden
    state.update(parts, 0.016);
    QCOMPARE(parts.opacity(0), 1.0);
    QCOMPARE(parts.opacity(1), 1.0); // linked
    QCOMPARE(parts.opacity(2), 0.0);
    // nothing changes while nobody touches the parts
    for (int i = 0; i < 10; ++i)
        state.update(parts, 0.1);
    QCOMPARE(parts.opacity(0), 1.0);
    QCOMPARE(parts.opacity(2), 0.0);
    // a motion raises ArmB: it takes over and fades in while ArmA (and its link) fade out
    parts.set(2, 1.0);
    state.update(parts, 0.0);
    QCOMPARE(parts.opacity(2), 0.0); // progress 0
    double lastB = 0, lastA = 1;
    for (int i = 0; i < 100; ++i) {
        state.update(parts, 0.01);
        QVERIFY(parts.opacity(2) >= lastB - 1e-12);
        QVERIFY(parts.opacity(0) <= lastA + 1e-12);
        QVERIFY(std::abs(parts.opacity(0) - parts.opacity(1)) < 1e-12);
        lastB = parts.opacity(2);
        lastA = parts.opacity(0);
    }
    QVERIFY(std::abs(parts.opacity(2) - 1.0) < 1e-9);
    QVERIFY(parts.opacity(0) < 1e-9);
    // and it stays there: the faded-out ArmA is not mistaken for a new request
    for (int i = 0; i < 50; ++i)
        state.update(parts, 0.1);
    QVERIFY(std::abs(parts.opacity(2) - 1.0) < 1e-9);
    QVERIFY(parts.opacity(0) < 1e-9);
    // a pose naming parts the model lacks, an empty group and a zero fade do no harm
    Pose odd;
    odd.fadeIn = 0.0;
    odd.groups = {{}, {{"Ghost", {"Ghost2"}}, {"ArmB", {}}}};
    PoseState oddState(odd);
    oddState.update(parts, 0.1);
    oddState.update(parts, kNaN);
    oddState.update(parts, -3);
    QVERIFY(parts.opacity(2) >= 0 && parts.opacity(2) <= 1);
}

void TestPuppetLife::lookAtTarget()
{
    LookAt look;
    const Rect pet{1000, 500, 200, 300}; // centre (1100, 650), long side 300, reach 2 => 600 px
    auto t = look.targetFor(1100, 650, pet);
    QCOMPARE(t.x, 0.0);
    QCOMPARE(t.y, 0.0);
    t = look.targetFor(1100 + 300, 650, pet);
    QVERIFY(std::abs(t.x - 0.5) < 1e-12 && t.y == 0.0); // right of the pet => positive x
    t = look.targetFor(1100, 650 - 300, pet);
    QVERIFY(std::abs(t.y - 0.5) < 1e-12 && t.x == 0.0); // above the pet (smaller y) => positive y
    t = look.targetFor(1100 - 3000, 650 + 3000, pet);
    QCOMPARE(t.x, -1.0);
    QCOMPARE(t.y, -1.0);
    // dead zone
    t = look.targetFor(1100 + 6, 650, pet);
    QCOMPARE(t.x, 0.0);
    // bad input
    for (const Rect &bad : {Rect{0, 0, 0, 10}, Rect{0, 0, 10, 0}, Rect{0, 0, -1, 5}, Rect{kNaN, 0, 5, 5}}) {
        t = look.targetFor(5, 5, bad);
        QCOMPARE(t.x, 0.0);
        QCOMPARE(t.y, 0.0);
    }
    t = look.targetFor(kNaN, 5, pet);
    QCOMPARE(t.x, 0.0);
    t = look.targetFor(std::numeric_limits<double>::infinity(), 5, pet);
    QCOMPARE(t.x, 0.0);
}

void TestPuppetLife::lookAtSmoothsAndApplies()
{
    ParameterSet p;
    p.add("ParamAngleX", -30, 0, 30);
    p.add("ParamAngleY", -20, 0, 40);
    p.add("ParamAngleZ", -30, 0, 30);
    p.add("ParamBodyAngleX", -10, 0, 10);
    p.add("ParamEyeBallX", -1, 0, 1);
    p.add("ParamEyeBallY", -1, 0, 1);
    p.add("ParamBrowLY", -1, 0, 1);
    LookAt look;
    QCOMPARE(look.apply(p), 6);
    QCOMPARE(p.value(0), 0.0);
    look.setTarget({1, 1});
    double last = 0;
    for (int i = 0; i < 300; ++i) {
        look.update(0.01);
        QVERIFY(look.current().x >= last - 1e-12);
        QVERIFY(look.current().x <= 1.0 + 1e-12);
        last = look.current().x;
    }
    QVERIFY(last > 0.99);
    look.apply(p);
    QVERIFY(std::abs(p.value(0) - 30 * look.current().x) < 1e-9);
    QVERIFY(std::abs(p.value(1) - 40 * look.current().y) < 1e-9); // up uses the maximum
    QVERIFY(p.value(2) < 0);                                     // leans the other way
    QVERIFY(std::abs(p.value(3) - 10 * look.current().x) < 1e-9);
    QCOMPARE(p.value(6), 0.0); // brows are not touched
    // down uses the minimum
    look.setTarget({-1, -1});
    for (int i = 0; i < 1000; ++i)
        look.update(0.01);
    look.apply(p);
    QVERIFY(std::abs(p.value(0) - -30) < 0.1);
    QVERIFY(std::abs(p.value(1) - -20) < 0.1);
    // bad targets and steps are ignored; targets are clamped
    look.setTarget({kNaN, 0});
    look.setTarget({50, -50});
    look.update(kNaN);
    look.update(-1);
    look.update(0);
    for (int i = 0; i < 1000; ++i)
        look.update(0.01);
    QVERIFY(std::abs(look.current().x - 1.0) < 1e-6);
    QVERIFY(std::abs(look.current().y - -1.0) < 1e-6);
    // no smoothing means the target at once
    LookAt snap({2.0, 0.02, 0.0});
    snap.setTarget({0.3, 0.2});
    snap.update(0.001);
    QCOMPARE(snap.current().x, 0.3);
    // a model without the parameters is left alone
    ParameterSet empty;
    QCOMPARE(look.apply(empty), 0);
}

QTEST_GUILESS_MAIN(TestPuppetLife)
#include "tst_puppetlife.moc"
