// Motions (src/puppet/motion.*): the reader, the curve evaluator against golden values computed by tools/compat/puppet_curves.py, the player's fades and loops, and the scheduler.
#include "puppet/motion.h"
#include "puppetcurvegolden.h"
#include "puppetlimits.h"

#include <QRandomGenerator>
#include <QTest>

#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;
using namespace murasaki::puppet;

namespace {

QByteArray number(double v)
{
    return QByteArray::number(v, 'g', 17);
}

QByteArray list(const std::vector<double> &flat)
{
    QByteArray out = "[";
    for (size_t i = 0; i < flat.size(); ++i)
        out += (i ? "," : "") + number(flat[i]);
    return out + "]";
}

QByteArray curveJson(const QByteArray &target, const QByteArray &id, const std::vector<double> &flat, const QByteArray &extra = {})
{
    return "{\"Target\":\"" + target + "\",\"Id\":\"" + id + "\",\"Segments\":" + list(flat) + extra + "}";
}

QByteArray motionJson(const QList<QByteArray> &curves, double duration = 2.0, const QByteArray &meta = ",\"Loop\":false", const QByteArray &version = "3")
{
    return "{\"Version\":" + version + ",\"Meta\":{\"Duration\":" + number(duration) + ",\"Fps\":30,\"CurveCount\":" + QByteArray::number(curves.size())
        + ",\"TotalSegmentCount\":1,\"TotalPointCount\":2" + meta + "},\"Curves\":[" + curves.join(',') + "]}";
}

std::string_view view(const QByteArray &b)
{
    return std::string_view(b.constData(), size_t(b.size()));
}

std::shared_ptr<const Motion> parsed(const QByteArray &text)
{
    const MotionResult r = parseMotion(view(text));
    if (!r.ok)
        qFatal("fixture motion does not parse: %s", r.error.c_str());
    return std::make_shared<Motion>(r.motion);
}

// A motion of one parameter curve "P" going linearly from 0 to 1 over `duration` seconds.
std::shared_ptr<const Motion> ramp(double duration, bool loop, const QByteArray &extraMeta = {})
{
    return parsed(motionJson({curveJson("Parameter", "P", {0, 0, 0, duration, 1})}, duration, QByteArray(loop ? ",\"Loop\":true" : ",\"Loop\":false") + extraMeta));
}

ParameterSet oneParameter(double def = 0, double min = -10, double max = 10)
{
    ParameterSet p;
    p.add("P", min, def, max);
    return p;
}

} // namespace

class TestPuppetMotion : public QObject
{
    Q_OBJECT
private slots:
    void readsAMotion();
    void rejectsBadMotions_data();
    void rejectsBadMotions();
    void limits();
    void goldenCurves();
    void handDerivedFromTheSpecificationWording();
    void fadingMotionsAreBounded();
    void curvesAtTheEdges();
    void easingIsMonotone();
    void playerFades();
    void playerLoops();
    void playerStops();
    void playerIgnoresBadSteps();
    void applyBlendsCurves();
    void managerPriorities();
    void managerQueueAndForce();
    void managerStaysSaneUnderRandomRequests();
};

void TestPuppetMotion::readsAMotion()
{
    const QByteArray text = motionJson({curveJson("Parameter", "ParamAngleX", {0, 0, 0, 1, 10, 1, 1.5, 12, 1.75, -3, 2, 4}, ",\"FadeInTime\":0.25"),
                                        curveJson("PartOpacity", "PartArmL", {0, 1, 2, 1, 0}), curveJson("Model", "Opacity", {0, 1})},
                                       2.0, ",\"Loop\":true,\"FadeInTime\":0.5,\"FadeOutTime\":0.75");
    const MotionResult r = parseMotion(view(text));
    QVERIFY2(r.ok, r.error.c_str());
    QCOMPARE(r.motion.duration, 2.0);
    QVERIFY(r.motion.loop);
    QCOMPARE(*r.motion.fadeIn, 0.5);
    QCOMPARE(*r.motion.fadeOut, 0.75);
    QCOMPARE(r.motion.curves.size(), size_t(3));
    const Curve &c = r.motion.curves[0];
    QCOMPARE(c.target, CurveTarget::Parameter);
    QCOMPARE(QString::fromStdString(c.id), u"ParamAngleX"_s);
    QCOMPARE(*c.fadeIn, 0.25);
    QCOMPARE(c.points.size(), size_t(5)); // 1 + linear + bezier(3)
    QCOMPARE(c.segments.size(), size_t(2));
    QCOMPARE(c.segments[0].kind, SegmentKind::Linear);
    QCOMPARE(c.segments[1].kind, SegmentKind::Bezier);
    QCOMPARE(c.segments[1].first, 1u);
    QCOMPARE(r.motion.curves[1].target, CurveTarget::PartOpacity);
    QCOMPARE(r.motion.curves[2].target, CurveTarget::Model);
    QCOMPARE(r.motion.curves[2].points.size(), size_t(1));
    QVERIFY(r.motion.curves[2].segments.empty());
}

void TestPuppetMotion::rejectsBadMotions_data()
{
    QTest::addColumn<QByteArray>("text");
    const auto ok = curveJson("Parameter", "P", {0, 0, 0, 1, 1});
    const auto wrap = [&](const QByteArray &meta, const QByteArray &curvesIn = {}) {
        const QByteArray curves = curvesIn.isEmpty() ? QByteArray("[") + ok + "]" : curvesIn;
        return "{\"Version\":3,\"Meta\":{" + meta + "},\"Curves\":" + curves + "}";
    };
    QTest::newRow("valid control") << wrap("\"Duration\":1,\"Fps\":30");
    QTest::newRow("not json") << QByteArray("motion");
    QTest::newRow("array") << QByteArray("[]");
    QTest::newRow("no version") << QByteArray(R"({"Meta":{"Duration":1,"Fps":30},"Curves":[]})");
    QTest::newRow("version 2") << QByteArray(R"({"Version":2,"Meta":{"Duration":1,"Fps":30},"Curves":[]})");
    QTest::newRow("no meta") << QByteArray(R"({"Version":3,"Curves":[]})");
    QTest::newRow("no curves") << QByteArray(R"({"Version":3,"Meta":{"Duration":1,"Fps":30}})");
    QTest::newRow("curves object") << wrap("\"Duration\":1,\"Fps\":30", "{}");
    QTest::newRow("no duration") << wrap("\"Fps\":30");
    QTest::newRow("negative duration") << wrap("\"Duration\":-1,\"Fps\":30");
    QTest::newRow("huge duration") << wrap("\"Duration\":3601,\"Fps\":30");
    QTest::newRow("duration string") << wrap("\"Duration\":\"1\",\"Fps\":30");
    QTest::newRow("no fps") << wrap("\"Duration\":1");
    QTest::newRow("fps zero") << wrap("\"Duration\":1,\"Fps\":0");
    QTest::newRow("fps negative") << wrap("\"Duration\":1,\"Fps\":-30");
    QTest::newRow("loop number") << wrap("\"Duration\":1,\"Fps\":30,\"Loop\":1");
    QTest::newRow("fade negative") << wrap("\"Duration\":1,\"Fps\":30,\"FadeInTime\":-1");
    QTest::newRow("fade huge") << wrap("\"Duration\":1,\"Fps\":30,\"FadeOutTime\":99999");
    QTest::newRow("curve not object") << wrap("\"Duration\":1,\"Fps\":30", "[1]");
    QTest::newRow("no target") << wrap("\"Duration\":1,\"Fps\":30", R"([{"Id":"P","Segments":[0,0]}])");
    QTest::newRow("unknown target") << wrap("\"Duration\":1,\"Fps\":30", curveJson("Bogus", "P", {0, 0}).prepend('[').append(']'));
    QTest::newRow("empty id") << wrap("\"Duration\":1,\"Fps\":30", curveJson("Parameter", "", {0, 0}).prepend('[').append(']'));
    QTest::newRow("long id") << wrap("\"Duration\":1,\"Fps\":30", curveJson("Parameter", QByteArray(129, 'x'), {0, 0}).prepend('[').append(']'));
    QTest::newRow("segments missing") << wrap("\"Duration\":1,\"Fps\":30", R"([{"Target":"Parameter","Id":"P"}])");
    QTest::newRow("segments string") << wrap("\"Duration\":1,\"Fps\":30", R"([{"Target":"Parameter","Id":"P","Segments":"0,0"}])");
    QTest::newRow("segments with string") << wrap("\"Duration\":1,\"Fps\":30", R"([{"Target":"Parameter","Id":"P","Segments":[0,0,"a"]}])");
    QTest::newRow("empty curve") << wrap("\"Duration\":1,\"Fps\":30", curveJson("Parameter", "P", {}).prepend('[').append(']'));
    QTest::newRow("half point") << wrap("\"Duration\":1,\"Fps\":30", curveJson("Parameter", "P", {0}).prepend('[').append(']'));
    QTest::newRow("code 4") << wrap("\"Duration\":1,\"Fps\":30", curveJson("Parameter", "P", {0, 0, 4, 1, 1}).prepend('[').append(']'));
    QTest::newRow("code fraction") << wrap("\"Duration\":1,\"Fps\":30", curveJson("Parameter", "P", {0, 0, 0.5, 1, 1}).prepend('[').append(']'));
    QTest::newRow("code negative") << wrap("\"Duration\":1,\"Fps\":30", curveJson("Parameter", "P", {0, 0, -1, 1, 1}).prepend('[').append(']'));
    QTest::newRow("ends inside linear") << wrap("\"Duration\":1,\"Fps\":30", curveJson("Parameter", "P", {0, 0, 0, 1}).prepend('[').append(']'));
    QTest::newRow("ends inside bezier") << wrap("\"Duration\":1,\"Fps\":30", curveJson("Parameter", "P", {0, 0, 1, 0.3, 0.1, 0.6, 0.2}).prepend('[').append(']'));
    QTest::newRow("code alone") << wrap("\"Duration\":1,\"Fps\":30", curveJson("Parameter", "P", {0, 0, 0}).prepend('[').append(']'));
    QTest::newRow("time goes back") << wrap("\"Duration\":1,\"Fps\":30", curveJson("Parameter", "P", {0, 0, 0, 1, 1, 0, 0.5, 2}).prepend('[').append(']'));
    QTest::newRow("bezier control goes back") << wrap("\"Duration\":1,\"Fps\":30", curveJson("Parameter", "P", {0, 0, 1, 0.5, 1, 0.4, 1, 1, 0}).prepend('[').append(']'));
    QTest::newRow("negative first time") << wrap("\"Duration\":1,\"Fps\":30", curveJson("Parameter", "P", {-1, 0, 0, 1, 1}).prepend('[').append(']'));
    QTest::newRow("time past the maximum") << wrap("\"Duration\":1,\"Fps\":30", curveJson("Parameter", "P", {0, 0, 0, 3601, 1}).prepend('[').append(']'));
    QTest::newRow("curve fade negative") << wrap("\"Duration\":1,\"Fps\":30", curveJson("Parameter", "P", {0, 0}, ",\"FadeInTime\":-1").prepend('[').append(']'));
}

void TestPuppetMotion::rejectsBadMotions()
{
    QFETCH(QByteArray, text);
    const MotionResult r = parseMotion(view(text));
    if (QTest::currentDataTag() == QByteArray("valid control")) {
        QVERIFY2(r.ok, r.error.c_str());
        return;
    }
    QVERIFY2(!r.ok, qPrintable(QString::fromLatin1(QTest::currentDataTag())));
    QVERIFY(!r.error.empty());
}

void TestPuppetMotion::limits()
{
    // curves: kMaxCurvesPerMotion fits, one more does not
    const auto many = [](int n) {
        QList<QByteArray> curves;
        const QByteArray c = curveJson("Parameter", "P", {0, 0});
        for (int i = 0; i < n; ++i)
            curves << c;
        return motionJson(curves);
    };
    QVERIFY(parseMotion(view(many(int(kMaxCurvesPerMotion)))).ok);
    QVERIFY(!parseMotion(view(many(int(kMaxCurvesPerMotion) + 1))).ok);
    // segments of one curve: counted as read
    const auto segments = [](qint64 n) {
        QByteArray s = "0,0";
        for (qint64 i = 0; i < n; ++i)
            s += ",0,0,0";
        return "{\"Version\":3,\"Meta\":{\"Duration\":1,\"Fps\":30},\"Curves\":[{\"Target\":\"Parameter\",\"Id\":\"P\",\"Segments\":[" + s + "]}]}";
    };
    QVERIFY(parseMotion(view(segments(kMaxSegmentsPerMotion))).ok);
    const MotionResult over = parseMotion(view(segments(kMaxSegmentsPerMotion + 1)));
    QVERIFY(!over.ok);
    QVERIFY(QString::fromStdString(over.error).contains(u"kMaxSegmentsPerMotion"_s));
    // segments spread over curves count together
    QByteArray two = "{\"Version\":3,\"Meta\":{\"Duration\":1,\"Fps\":30},\"Curves\":[";
    for (int c = 0; c < 2; ++c) {
        two += (c ? "," : "") + QByteArray("{\"Target\":\"Parameter\",\"Id\":\"P\",\"Segments\":[0,0");
        for (qint64 i = 0; i < kMaxSegmentsPerMotion / 2 + (c ? 1 : 0); ++i)
            two += ",0,0,0";
        two += "]}";
    }
    QVERIFY(!parseMotion(view(two + "]}")).ok);
    // the values of the file
    QByteArray huge = "{\"Version\":3,\"Meta\":{\"Duration\":1,\"Fps\":30},\"Curves\":[{\"Target\":\"Parameter\",\"Id\":\"P\",\"Segments\":[0,0";
    for (qint64 i = 0; i < kMaxMotionJsonValues; ++i)
        huge += ",0";
    huge += "]}]}";
    QVERIFY(!parseMotion(view(huge)).ok);
}

void TestPuppetMotion::goldenCurves()
{
    int checked = 0;
    for (const puppetgolden::Curve &g : puppetgolden::curves()) {
        const QByteArray text = motionJson({curveJson("Parameter", "P", g.segments)}, 3600);
        const MotionResult r = parseMotion(view(text));
        QVERIFY2(r.ok, g.name);
        QCOMPARE(g.times.size(), g.values.size());
        for (size_t i = 0; i < g.times.size(); ++i) {
            const double got = evaluateCurve(r.motion.curves[0], g.times[i]);
            QVERIFY2(std::abs(got - g.values[i]) < 1e-9, qPrintable(u"%1 at t=%2: got %3, expected %4"_s.arg(QString::fromLatin1(g.name)).arg(g.times[i]).arg(got).arg(g.values[i])));
            ++checked;
        }
    }
    QVERIFY(checked > 1000);
}

void TestPuppetMotion::handDerivedFromTheSpecificationWording()
{
    // Not from the script: values worked out by hand from what S1 says (docs/pets/puppets.md section 3.2) - a segment code is 0 linear, 1 Bezier, 2 stepped, 3 inverse stepped; a Bezier's inner
    // control points are at a third and two thirds of its time span, so time is linear in the Bezier parameter and the value at the middle of the span is (p0 + 3 p1 + 3 p2 + p3) / 8.
    const auto curve = [](const std::vector<double> &flat) { return parseMotion(view(motionJson({curveJson("Parameter", "P", flat)}, 10))).motion.curves[0]; };
    // Bezier from (0, 0) over 3 seconds with inner values 1 and 1 and end value 0: control times 1 and 2
    const Curve b = curve({0, 0, 1, 1, 1, 2, 1, 3, 0});
    QVERIFY(std::abs(evaluateCurve(b, 1.5) - (0 + 3 * 1 + 3 * 1 + 0) / 8.0) < 1e-12);
    // at a quarter of the span (s = 0.25): 0.421875 * 1 * 3 ... (1-s)^3 p0 + 3 (1-s)^2 s p1 + 3 (1-s) s^2 p2 + s^3 p3 with p = 0, 1, 1, 0
    const double s = 0.25;
    QVERIFY(std::abs(evaluateCurve(b, 0.75) - (3 * (1 - s) * (1 - s) * s + 3 * (1 - s) * s * s)) < 1e-12);
    // a Bezier whose inner values are on the line between its ends is that line
    const Curve line = curve({0, 0, 1, 1, 1, 2, 2, 3, 3});
    for (const double t : {0.0, 0.3, 1.5, 2.9, 3.0})
        QVERIFY(std::abs(evaluateCurve(line, t) - t) < 1e-9);
    // linear: the middle of (1, 2) -> (3, 6) is 4
    QCOMPARE(evaluateCurve(curve({1, 2, 0, 3, 6}), 2.0), 4.0);
    // stepped keeps the first value until the end of the segment, inverse stepped has the end value after its start (Q-F2: the value exactly at the start is the start's)
    const Curve st = curve({0, 5, 2, 1, 9});
    QCOMPARE(evaluateCurve(st, 0.999), 5.0);
    QCOMPARE(evaluateCurve(st, 1.0), 9.0);
    const Curve inv = curve({0, 5, 3, 1, 9});
    QCOMPARE(evaluateCurve(inv, 0.001), 9.0);
    QCOMPARE(evaluateCurve(inv, 0.0), 5.0);
    // Q-F3b (open): a curve's own fade multiplies the motion's; the specification does not say whether it replaces it. The case is pinned so a change is deliberate.
    const auto motion = parsed(motionJson({curveJson("Parameter", "P", {0, 10, 0, 4, 10}, ",\"FadeInTime\":2")}, 4.0));
    MotionPlayer player(motion, 1.0, 0.0);
    player.update(1.0); // the motion's fade (1 s) is complete, the curve's is half way
    ParameterSet p = oneParameter(0);
    PartSet parts;
    ModelValues model;
    player.apply(p, parts, model);
    QVERIFY(std::abs(p.value(0) - 10 * fadeEase(0.5)) < 1e-9);
}

void TestPuppetMotion::fadingMotionsAreBounded()
{
    MotionManager m;
    const auto motion = ramp(100.0, true);
    for (int i = 0; i < 1000; ++i) {
        QVERIFY(m.request({motion, Priority::Force, 0, 3600, "f"}));
        QVERIFY(m.fadingOut() <= size_t(kMaxFadingMotions));
    }
    QCOMPARE(m.fadingOut(), size_t(kMaxFadingMotions));
}

void TestPuppetMotion::curvesAtTheEdges()
{
    const Motion m = parseMotion(view(motionJson({curveJson("Parameter", "P", {1, 5, 0, 2, 7})}))).motion;
    const Curve &c = m.curves[0];
    QCOMPARE(evaluateCurve(c, -100), 5.0);
    QCOMPARE(evaluateCurve(c, 0), 5.0);
    QCOMPARE(evaluateCurve(c, 1), 5.0);
    QCOMPARE(evaluateCurve(c, 1.5), 6.0);
    QCOMPARE(evaluateCurve(c, 2), 7.0);
    QCOMPARE(evaluateCurve(c, 1e9), 7.0);
    QCOMPARE(evaluateCurve(c, std::numeric_limits<double>::quiet_NaN()), 5.0);
    QCOMPARE(evaluateCurve(c, std::numeric_limits<double>::infinity()), 7.0);
    QCOMPARE(evaluateCurve(c, -std::numeric_limits<double>::infinity()), 5.0);
    QCOMPARE(evaluateCurve(Curve{}, 1.0), 0.0);
    // a single point
    const Curve single = parseMotion(view(motionJson({curveJson("Parameter", "P", {0.5, 3})}))).motion.curves[0];
    QCOMPARE(evaluateCurve(single, 0), 3.0);
    QCOMPARE(evaluateCurve(single, 9), 3.0);
    // steps: the value changes at the end point
    const Curve step = parseMotion(view(motionJson({curveJson("Parameter", "P", {0, 1, 2, 1, 5, 3, 2, 9})}))).motion.curves[0];
    QCOMPARE(evaluateCurve(step, 0.999), 1.0);
    QCOMPARE(evaluateCurve(step, 1.0), 5.0);   // the next segment starts here
    QCOMPARE(evaluateCurve(step, 1.001), 9.0); // inverse stepped: at once
}

void TestPuppetMotion::easingIsMonotone()
{
    QCOMPARE(fadeEase(0), 0.0);
    QCOMPARE(fadeEase(1), 1.0);
    QCOMPARE(fadeEase(-5), 0.0);
    QCOMPARE(fadeEase(5), 1.0);
    QCOMPARE(fadeEase(std::numeric_limits<double>::quiet_NaN()), 0.0);
    QVERIFY(std::abs(fadeEase(0.5) - 0.5) < 1e-12);
    double last = -1;
    for (int i = 0; i <= 1000; ++i) {
        const double v = fadeEase(i / 1000.0);
        QVERIFY(v >= last);
        last = v;
    }
}

void TestPuppetMotion::playerFades()
{
    const auto motion = ramp(4.0, false);
    MotionPlayer player(motion, 1.0, 1.0);
    QCOMPARE(player.weight(), 0.0); // nothing has played yet
    double last = -1;
    for (int i = 0; i < 100; ++i) { // the fade in over one second is monotone
        player.update(0.01);
        const double w = player.weight();
        QVERIFY(w >= last - 1e-12);
        QVERIFY(w >= 0 && w <= 1);
        last = w;
    }
    QVERIFY(std::abs(player.weight() - 1.0) < 1e-9);
    // then full weight until the last second, where it fades out and the motion ends
    for (int i = 0; i < 100; ++i)
        player.update(0.01);
    QVERIFY(std::abs(player.weight() - 1.0) < 1e-9); // t = 2.0
    for (int i = 0; i < 100; ++i)
        player.update(0.01); // t = 3.0
    double prev = 2;
    while (!player.finished()) {
        player.update(0.01);
        QVERIFY(player.weight() <= prev + 1e-12);
        prev = player.weight();
    }
    QVERIFY(player.time() >= 4.0 - 1e-9);
    // zero fades: full weight at once and at the very end
    MotionPlayer sharp(ramp(2.0, false), 0.0, 0.0);
    sharp.update(0.5);
    QCOMPARE(sharp.weight(), 1.0);
    // a file's own fades are used when the caller gives none
    MotionPlayer fromFile(ramp(4.0, false, ",\"FadeInTime\":2,\"FadeOutTime\":0.5"), -1, -1);
    fromFile.update(1.0);
    QVERIFY(std::abs(fromFile.weight() - fadeEase(0.5)) < 1e-12);
    // the default is one second
    MotionPlayer defaulted(ramp(4.0, false), -1, -1);
    defaulted.update(0.5);
    QVERIFY(std::abs(defaulted.weight() - fadeEase(0.5)) < 1e-12);
}

void TestPuppetMotion::playerLoops()
{
    MotionPlayer player(ramp(2.0, true), 0.0, 0.0);
    ParameterSet p = oneParameter();
    PartSet parts;
    ModelValues model;
    double seen = 0;
    for (int i = 0; i < 1000; ++i) {
        player.update(0.01);
        QVERIFY(!player.finished());
        QVERIFY(player.time() >= 0 && player.time() < 2.0);
        p.resetToDefaults();
        player.apply(p, parts, model);
        seen = p.value(0);
        QVERIFY(std::abs(seen - player.time() / 2.0) < 1e-9); // the value is a function of the position, so a looped motion repeats
    }
    QVERIFY(player.weight() == 1.0); // a loop has no fade out
    // the loop override beats the file's flag in both directions
    MotionPlayer forced(ramp(1.0, false), 0, 0, true);
    forced.update(5.5);
    QVERIFY(!forced.finished());
    QVERIFY(std::abs(forced.time() - 0.5) < 1e-9);
    MotionPlayer unforced(ramp(1.0, true), 0, 0, false);
    unforced.update(5.5);
    QVERIFY(unforced.finished());
    // a motion of no length does not loop and ends at once
    const auto empty = parsed(motionJson({curveJson("Parameter", "P", {0, 0})}, 0.0, ",\"Loop\":true"));
    MotionPlayer none(empty, 0, 0);
    QVERIFY(none.finished());
    none.update(1);
    QCOMPARE(none.weight(), 0.0);
}

void TestPuppetMotion::playerStops()
{
    MotionPlayer player(ramp(10.0, true), 0.0, 0.5);
    player.update(1.0);
    QCOMPARE(player.weight(), 1.0);
    player.stop();
    QVERIFY(player.stopping());
    double prev = 2;
    int steps = 0;
    while (!player.finished() && steps++ < 1000) {
        player.update(0.01);
        QVERIFY(player.weight() <= prev + 1e-12);
        prev = player.weight();
    }
    QVERIFY(player.finished());
    QVERIFY(steps >= 49 && steps <= 51); // 0.5 s
    MotionPlayer instant(ramp(10.0, true), 0.0, 0.0);
    instant.stop();
    QVERIFY(instant.finished());
}

void TestPuppetMotion::playerIgnoresBadSteps()
{
    MotionPlayer player(ramp(2.0, false), 0.0, 0.0);
    player.update(-1);
    player.update(std::numeric_limits<double>::quiet_NaN());
    player.update(std::numeric_limits<double>::infinity() * -1);
    QCOMPARE(player.time(), 0.0);
    QVERIFY(!player.finished());
    player.update(1e300); // a huge step ends a plain motion
    QVERIFY(player.finished());
    MotionPlayer looped(ramp(2.0, true), 0, 0);
    looped.update(1e12); // and wraps a loop
    QVERIFY(looped.time() >= 0 && looped.time() < 2.0);
}

void TestPuppetMotion::applyBlendsCurves()
{
    const auto motion = parsed(motionJson({curveJson("Parameter", "P", {0, 10, 0, 2, 10}), curveJson("Parameter", "Missing", {0, 1}), curveJson("PartOpacity", "Arm", {0, 0}),
                                           curveJson("PartOpacity", "NoPart", {0, 0}), curveJson("Model", "Opacity", {0, 0.5}), curveJson("Model", "EyeBlink", {0, 0.25}),
                                           curveJson("Model", "LipSync", {0, 0.75}), curveJson("Model", "Unknown", {0, 9})}, 2.0));
    ParameterSet p = oneParameter(2);
    PartSet parts;
    parts.add("Arm");
    ModelValues model;
    MotionPlayer full(motion, 0, 0);
    full.update(0.5);
    full.apply(p, parts, model);
    QCOMPARE(p.value(0), 10.0);
    QCOMPARE(parts.opacity(0), 0.0);
    QCOMPARE(model.opacity, 0.5);
    QVERIFY(model.eyeBlink && std::abs(*model.eyeBlink - 0.25) < 1e-12);
    QVERIFY(model.lipSync && std::abs(*model.lipSync - 0.75) < 1e-12);
    // half weight moves halfway: fade in of 1 s, 0.5 s in => ease(0.5) = 0.5
    ParameterSet q = oneParameter(2);
    PartSet qp;
    qp.add("Arm");
    ModelValues qm;
    MotionPlayer half(motion, 1.0, 0);
    half.update(0.5);
    half.apply(q, qp, qm);
    QVERIFY(std::abs(q.value(0) - 6.0) < 1e-9);
    QVERIFY(std::abs(qp.opacity(0) - 0.5) < 1e-9);
    // a curve's own fade multiplies
    const auto faded = parsed(motionJson({curveJson("Parameter", "P", {0, 10, 0, 2, 10}, ",\"FadeInTime\":2")}, 2.0));
    ParameterSet r = oneParameter(0);
    PartSet rp;
    ModelValues rm;
    MotionPlayer own(faded, 0, 0);
    own.update(1.0);
    own.apply(r, rp, rm);
    QVERIFY(std::abs(r.value(0) - 5.0) < 1e-9);
    // a finished player changes nothing
    ParameterSet s = oneParameter(3);
    MotionPlayer done(ramp(1.0, false), 0, 0);
    done.update(5);
    PartSet sp;
    ModelValues sm;
    done.apply(s, sp, sm);
    QCOMPARE(s.value(0), 3.0);
}

void TestPuppetMotion::managerPriorities()
{
    MotionManager m;
    const auto idle = ramp(2.0, true);
    const auto normal = ramp(1.0, false);
    QVERIFY(!m.request({nullptr, Priority::Normal, 0, 0, "x"}));
    QVERIFY(!m.request({idle, Priority::None, 0, 0, "x"}));
    QVERIFY(m.request({idle, Priority::Idle, 0.1, 0.1, "idle"}));
    QCOMPARE(m.currentTag(), std::string("idle"));
    QVERIFY(!m.request({idle, Priority::Idle, 0, 0, "idle2"})); // idle never queues or replaces
    m.update(0.5);
    QVERIFY(m.request({normal, Priority::Normal, 0.1, 0.1, "tap"}));
    QCOMPARE(m.currentTag(), std::string("tap"));
    QCOMPARE(m.currentPriority(), Priority::Normal);
    QCOMPARE(m.fadingOut(), size_t(1)); // the idle one
    QCOMPARE(m.queued(), size_t(0));
    m.update(0.2);
    QCOMPARE(m.fadingOut(), size_t(0)); // its 0.1 s fade out is over
    QVERIFY(!m.request({idle, Priority::Idle, 0, 0, "idle3"})); // something plays
    // a second normal waits for the first
    QVERIFY(m.request({normal, Priority::Normal, 0.1, 0.1, "tap2"}));
    QCOMPARE(m.queued(), size_t(1));
    QCOMPARE(m.currentTag(), std::string("tap"));
    m.update(2.0); // tap ends
    QCOMPARE(m.currentTag(), std::string("tap2"));
    QCOMPARE(m.queued(), size_t(0));
    m.update(2.0);
    QVERIFY(!m.playing());
    QCOMPARE(m.currentPriority(), Priority::None);
    QVERIFY(m.currentTag().empty());
    // idle starts again when it is quiet
    QVERIFY(m.request({idle, Priority::Idle, 0, 0, "idle4"}));
}

void TestPuppetMotion::managerQueueAndForce()
{
    MotionManager m;
    const auto normal = ramp(1.0, false);
    QVERIFY(m.request({normal, Priority::Normal, 0, 0, "first"}));
    for (qint64 i = 0; i < kMaxMotionQueue; ++i)
        QVERIFY(m.request({normal, Priority::Normal, 0, 0.3, "q" + std::to_string(i)}));
    QCOMPARE(m.queued(), size_t(kMaxMotionQueue));
    QVERIFY(!m.request({normal, Priority::Normal, 0, 0, "over"}));
    QCOMPARE(m.queued(), size_t(kMaxMotionQueue));
    // the queue is served in order
    m.update(1.0);
    QCOMPARE(m.currentTag(), std::string("q0"));
    // force clears the queue, and the old current fades out beside the new one
    QVERIFY(m.request({normal, Priority::Force, 0.2, 0.2, "forced"}));
    QCOMPARE(m.queued(), size_t(0));
    QCOMPARE(m.currentTag(), std::string("forced"));
    QCOMPARE(m.fadingOut(), size_t(1));
    // a normal behind a forced waits; an idle is refused
    QVERIFY(m.request({normal, Priority::Normal, 0, 0, "later"}));
    QCOMPARE(m.currentTag(), std::string("forced"));
    QCOMPARE(m.queued(), size_t(1));
    // force over force
    QVERIFY(m.request({normal, Priority::Force, 0, 0, "forced2"}));
    QCOMPARE(m.currentTag(), std::string("forced2"));
    QCOMPARE(m.queued(), size_t(0));
    m.clear();
    QVERIFY(!m.playing());
    QCOMPARE(m.fadingOut(), size_t(0));
    // apply with several players writes in order, the current one last
    ParameterSet p = oneParameter(0);
    PartSet parts;
    ModelValues model;
    m.apply(p, parts, model);
    QCOMPARE(p.value(0), 0.0);
}

void TestPuppetMotion::managerStaysSaneUnderRandomRequests()
{
    QRandomGenerator rng(20261022);
    MotionManager m;
    const std::shared_ptr<const Motion> motions[] = {ramp(0.7, false), ramp(1.3, true), ramp(0.2, false), ramp(3.0, false)};
    ParameterSet p = oneParameter();
    PartSet parts;
    ModelValues model;
    for (int step = 0; step < 20000; ++step) {
        switch (rng.bounded(4)) {
        case 0:
        case 1: {
            const Priority priority = static_cast<Priority>(rng.bounded(4));
            m.request({motions[rng.bounded(4)], priority, rng.bounded(2) ? 0.0 : rng.generateDouble(), rng.generateDouble(), "t"});
            break;
        }
        case 2:
            m.update(rng.generateDouble() * 0.5);
            break;
        default:
            m.update(rng.bounded(10) == 0 ? 100.0 : 0.016);
            break;
        }
        QVERIFY(m.queued() <= size_t(kMaxMotionQueue));
        QVERIFY(m.fadingOut() < 64);
        if (!m.playing()) {
            QVERIFY(m.currentTag().empty());
            QCOMPARE(m.currentPriority(), Priority::None);
        }
        p.resetToDefaults();
        m.apply(p, parts, model);
        QVERIFY(p.value(0) >= -10 && p.value(0) <= 10);
        if (rng.bounded(997) == 0)
            m.clear();
    }
}

QTEST_GUILESS_MAIN(TestPuppetMotion)
#include "tst_puppetmotion.moc"
