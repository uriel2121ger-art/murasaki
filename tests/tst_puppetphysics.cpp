// Cubism physics (src/puppet/physics.*): the reader, the binding of rigs to a model, and the properties the format states and ours promises (docs/pets/puppets.md, section 15).
#include "puppet/physics.h"
#include "puppetlimits.h"

#include <QTest>

#include <cmath>
#include <type_traits>
#include <limits>

using namespace Qt::StringLiterals;
using namespace murasaki::puppet;

namespace {

std::string_view view(const QByteArray &b)
{
    return std::string_view(b.constData(), size_t(b.size()));
}

QByteArray vertexJson(double y, double mobility = 0.9, double delay = 0.8, double accel = 1.2, double radius = 3)
{
    return "{\"Position\":{\"X\":0,\"Y\":" + QByteArray::number(y) + "},\"Mobility\":" + QByteArray::number(mobility) + ",\"Delay\":" + QByteArray::number(delay)
        + ",\"Acceleration\":" + QByteArray::number(accel) + ",\"Radius\":" + QByteArray::number(radius) + "}";
}

QByteArray inputJson(const QByteArray &id, const QByteArray &type = "X", double weight = 100, bool reflect = false)
{
    return "{\"Source\":{\"Target\":\"Parameter\",\"Id\":\"" + id + "\"},\"Weight\":" + QByteArray::number(weight) + ",\"Type\":\"" + type + "\",\"Reflect\":" + (reflect ? "true" : "false") + "}";
}

QByteArray outputJson(const QByteArray &id, int vertex = 1, const QByteArray &type = "Angle", double scale = 1, double weight = 100, bool reflect = false)
{
    return "{\"Destination\":{\"Target\":\"Parameter\",\"Id\":\"" + id + "\"},\"VertexIndex\":" + QByteArray::number(vertex) + ",\"Scale\":" + QByteArray::number(scale)
        + ",\"Weight\":" + QByteArray::number(weight) + ",\"Type\":\"" + type + "\",\"Reflect\":" + (reflect ? "true" : "false") + "}";
}

QByteArray rigJson(const QByteArray &id, const QList<QByteArray> &inputs, const QList<QByteArray> &outputs, const QList<QByteArray> &vertices,
                   const QByteArray &norm = R"({"Position":{"Minimum":-10,"Default":0,"Maximum":10},"Angle":{"Minimum":-30,"Default":0,"Maximum":30}})")
{
    return "{\"Id\":\"" + id + "\",\"Input\":[" + inputs.join(',') + "],\"Output\":[" + outputs.join(',') + "],\"Vertices\":[" + vertices.join(',') + "],\"Normalization\":" + norm + "}";
}

QByteArray physicsJson(const QList<QByteArray> &rigs, const QByteArray &meta = R"("Fps":60,"EffectiveForces":{"Gravity":{"X":0,"Y":-1},"Wind":{"X":0,"Y":0}})", const QByteArray &version = "3")
{
    return "{\"Version\":" + version + ",\"Meta\":{" + meta + "},\"PhysicsSettings\":[" + rigs.join(',') + "]}";
}

QList<QByteArray> chain(int n)
{
    QList<QByteArray> v;
    for (int i = 0; i < n; ++i)
        v << vertexJson(-3.0 * i);
    return v;
}

// A hair rig: ParamAngleX (X input) drives HairSwing (angle of particle 1) and HairSway (X of particle 2).
QByteArray hair()
{
    return physicsJson({rigJson("Hair", {inputJson("ParamAngleX", "X")}, {outputJson("HairSwing", 1, "Angle"), outputJson("HairSway", 2, "X")}, chain(3))});
}

ParameterSet model()
{
    ParameterSet p;
    p.add("ParamAngleX", -30, 0, 30);
    p.add("HairSwing", -1, 0, 1);
    p.add("HairSway", -1, 0, 1);
    return p;
}

PhysicsSettings settings(const QByteArray &text)
{
    const PhysicsResult r = parsePhysics(view(text));
    if (!r.ok)
        qFatal("fixture physics does not parse: %s", r.error.c_str());
    return r.settings;
}

} // namespace

class TestPuppetPhysics : public QObject
{
    Q_OBJECT
private slots:
    void readsAFile();
    void rejectsBadFiles_data();
    void rejectsBadFiles();
    void limits();
    void rigsWithMissingPartsAreSkipped();
    void atRestNothingMoves();
    void aMovedHeadSwingsTheHairAndSettles();
    void energyDecays();
    void outputsStayInRange();
    void weightScaleAndReflect();
    void tiltedHeadTiltsTheChain();
    void windPushesTheChain();
    void deterministic();
    void longStepsAreSplitAndSafe();
    void badStepsAreIgnored();
    void resetStartsWithoutAJolt();
    void randomInputsStaySane();
    void notCopyableAndBoundToItsParameters();
    void hugeNumbersAreRefused();
};

void TestPuppetPhysics::readsAFile()
{
    const QByteArray text = physicsJson({rigJson("Hair", {inputJson("A", "X", 60, true), inputJson("B", "Angle", 40)}, {outputJson("C", 1, "Angle", 1.5, 80, true)}, chain(3)),
                                         rigJson("Skirt", {}, {}, chain(2))},
                                        R"("Fps":30,"EffectiveForces":{"Gravity":{"X":0,"Y":-1},"Wind":{"X":0.2,"Y":0}},"PhysicsSettingCount":99)");
    const PhysicsResult r = parsePhysics(view(text));
    QVERIFY2(r.ok, r.error.c_str());
    QCOMPARE(r.settings.fps, 30.0);
    QCOMPARE(r.settings.wind.x, 0.2);
    QCOMPARE(r.settings.rigs.size(), size_t(2));
    const PhysicsRig &rig = r.settings.rigs[0];
    QCOMPARE(QString::fromStdString(rig.id), u"Hair"_s);
    QCOMPARE(rig.inputs.size(), size_t(2));
    QCOMPARE(rig.inputs[0].weight, 60.0);
    QVERIFY(rig.inputs[0].reflect);
    QCOMPARE(rig.inputs[1].type, IoType::Angle);
    QCOMPARE(rig.outputs[0].scale, 1.5);
    QCOMPARE(rig.outputs[0].vertex, 1);
    QCOMPARE(rig.vertices.size(), size_t(3));
    QCOMPARE(rig.vertices[1].position.y, -3.0);
    QCOMPARE(rig.position.max, 10.0);
    QCOMPARE(rig.angle.min, -30.0);
    // Fps defaults to 60
    QCOMPARE(parsePhysics(view(physicsJson({}, R"("EffectiveForces":{"Gravity":{"X":0,"Y":-1},"Wind":{"X":0,"Y":0}})"))).settings.fps, 60.0);
}

void TestPuppetPhysics::rejectsBadFiles_data()
{
    QTest::addColumn<QByteArray>("text");
    const QByteArray forces = R"("EffectiveForces":{"Gravity":{"X":0,"Y":-1},"Wind":{"X":0,"Y":0}})";
    const auto rig = [](const QByteArray &body) { return QByteArray("{\"Version\":3,\"Meta\":{\"Fps\":60,\"EffectiveForces\":{\"Gravity\":{\"X\":0,\"Y\":-1},\"Wind\":{\"X\":0,\"Y\":0}}},\"PhysicsSettings\":[") + body + "]}"; };
    const QByteArray norm = R"("Normalization":{"Position":{"Minimum":-10,"Default":0,"Maximum":10},"Angle":{"Minimum":-30,"Default":0,"Maximum":30}})";
    const auto setting = [&](const QByteArray &in, const QByteArray &out, const QByteArray &vs, const QByteArray &n = QByteArray()) {
        return rig("{\"Id\":\"R\",\"Input\":" + in + ",\"Output\":" + out + ",\"Vertices\":" + vs + "," + (n.isEmpty() ? norm : n) + "}");
    };
    const QByteArray v = "[" + vertexJson(0) + "," + vertexJson(-3) + "]";
    QTest::newRow("control") << setting("[]", "[]", v);
    QTest::newRow("not json") << QByteArray("x");
    QTest::newRow("array") << QByteArray("[]");
    QTest::newRow("version 2") << physicsJson({}, "\"EffectiveForces\":{\"Gravity\":{\"X\":0,\"Y\":-1},\"Wind\":{\"X\":0,\"Y\":0}}", "2");
    QTest::newRow("no meta") << QByteArray(R"({"Version":3,"PhysicsSettings":[]})");
    QTest::newRow("no forces") << QByteArray(R"({"Version":3,"Meta":{"Fps":60},"PhysicsSettings":[]})");
    QTest::newRow("gravity missing y") << QByteArray(R"({"Version":3,"Meta":{"EffectiveForces":{"Gravity":{"X":0},"Wind":{"X":0,"Y":0}}},"PhysicsSettings":[]})");
    QTest::newRow("wind string") << QByteArray(R"({"Version":3,"Meta":{"EffectiveForces":{"Gravity":{"X":0,"Y":0},"Wind":{"X":"0","Y":0}}},"PhysicsSettings":[]})");
    QTest::newRow("fps zero") << physicsJson({}, "\"Fps\":0," + forces);
    QTest::newRow("fps huge") << physicsJson({}, "\"Fps\":1000," + forces);
    QTest::newRow("fps string") << physicsJson({}, "\"Fps\":\"60\"," + forces);
    QTest::newRow("no settings") << QByteArray(R"({"Version":3,"Meta":{"EffectiveForces":{"Gravity":{"X":0,"Y":0},"Wind":{"X":0,"Y":0}}}})");
    QTest::newRow("settings object") << QByteArray(R"({"Version":3,"Meta":{"EffectiveForces":{"Gravity":{"X":0,"Y":0},"Wind":{"X":0,"Y":0}}},"PhysicsSettings":{}})");
    QTest::newRow("setting number") << rig("1");
    QTest::newRow("no id") << rig(R"({"Input":[],"Output":[],"Vertices":[],)" + norm + "}");
    QTest::newRow("no vertices list") << rig(R"({"Id":"R","Input":[],"Output":[],)" + norm + "}");
    QTest::newRow("input without source") << setting(R"([{"Weight":100,"Type":"X","Reflect":false}])", "[]", v);
    QTest::newRow("input target other") << setting(R"([{"Source":{"Target":"Part","Id":"A"},"Weight":100,"Type":"X","Reflect":false}])", "[]", v);
    QTest::newRow("input weight 101") << setting("[" + inputJson("A", "X", 101) + "]", "[]", v);
    QTest::newRow("input weight negative") << setting("[" + inputJson("A", "X", -1) + "]", "[]", v);
    QTest::newRow("input type") << setting("[" + inputJson("A", "Y") + "]", "[]", v);
    QTest::newRow("input no reflect") << setting(R"([{"Source":{"Target":"Parameter","Id":"A"},"Weight":100,"Type":"X"}])", "[]", v);
    QTest::newRow("input reflect number") << setting(R"([{"Source":{"Target":"Parameter","Id":"A"},"Weight":100,"Type":"X","Reflect":1}])", "[]", v);
    QTest::newRow("input empty id") << setting("[" + inputJson("") + "]", "[]", v);
    QTest::newRow("output index fraction") << setting("[]", "[" + outputJson("B").replace("\"VertexIndex\":1", "\"VertexIndex\":1.5") + "]", v);
    QTest::newRow("output index negative") << setting("[]", "[" + outputJson("B", -1) + "]", v);
    QTest::newRow("output weight 200") << setting("[]", "[" + outputJson("B", 1, "Angle", 1, 200) + "]", v);
    QTest::newRow("output no scale") << setting("[]", R"([{"Destination":{"Target":"Parameter","Id":"B"},"VertexIndex":1,"Weight":100,"Type":"Angle","Reflect":false}])", v);
    QTest::newRow("vertex no position") << setting("[]", "[]", R"([{"Mobility":1,"Delay":1,"Acceleration":1,"Radius":1}])");
    QTest::newRow("vertex mobility 2") << setting("[]", "[]", "[" + vertexJson(0, 2) + "]");
    QTest::newRow("vertex delay negative") << setting("[]", "[]", "[" + vertexJson(0, 1, -1) + "]");
    QTest::newRow("vertex delay huge") << setting("[]", "[]", "[" + vertexJson(0, 1, 101) + "]");
    QTest::newRow("vertex radius huge") << setting("[]", "[]", "[" + vertexJson(0, 1, 1, 1, 1001) + "]");
    QTest::newRow("vertex radius negative") << setting("[]", "[]", "[" + vertexJson(0, 1, 1, 1, -1) + "]");
    QTest::newRow("no normalization") << rig(R"({"Id":"R","Input":[],"Output":[],"Vertices":[]})");
    QTest::newRow("normalization order") << setting("[]", "[]", v, R"("Normalization":{"Position":{"Minimum":5,"Default":0,"Maximum":10},"Angle":{"Minimum":-30,"Default":0,"Maximum":30}})");
    QTest::newRow("normalization missing angle") << setting("[]", "[]", v, R"("Normalization":{"Position":{"Minimum":-10,"Default":0,"Maximum":10}})");
    QTest::newRow("repeated key") << QByteArray(R"({"Version":3,"Version":3,"Meta":{},"PhysicsSettings":[]})");
}

void TestPuppetPhysics::rejectsBadFiles()
{
    QFETCH(QByteArray, text);
    const PhysicsResult r = parsePhysics(view(text));
    if (QTest::currentDataTag() == QByteArray("control")) {
        QVERIFY2(r.ok, r.error.c_str());
        return;
    }
    QVERIFY2(!r.ok, QTest::currentDataTag());
    QVERIFY(!r.error.empty());
}

void TestPuppetPhysics::limits()
{
    const auto rigs = [](int n) {
        QList<QByteArray> list;
        for (int i = 0; i < n; ++i)
            list << rigJson("R" + QByteArray::number(i), {}, {}, chain(2));
        return physicsJson(list);
    };
    QVERIFY(parsePhysics(view(rigs(int(kMaxPhysicsRigs)))).ok);
    QVERIFY(!parsePhysics(view(rigs(int(kMaxPhysicsRigs) + 1))).ok);
    QVERIFY(parsePhysics(view(physicsJson({rigJson("R", {}, {}, chain(int(kMaxPhysicsVerticesPerRig)))}))).ok);
    QVERIFY(!parsePhysics(view(physicsJson({rigJson("R", {}, {}, chain(int(kMaxPhysicsVerticesPerRig) + 1))}))).ok);
    QList<QByteArray> ins, outs;
    for (int i = 0; i <= int(kMaxPhysicsIoPerRig); ++i) {
        ins << inputJson("A");
        outs << outputJson("B");
    }
    QList<QByteArray> insOk = ins.mid(0, int(kMaxPhysicsIoPerRig));
    QList<QByteArray> outsOk = outs.mid(0, int(kMaxPhysicsIoPerRig));
    QVERIFY(parsePhysics(view(physicsJson({rigJson("R", insOk, outsOk, chain(2))}))).ok);
    QVERIFY(!parsePhysics(view(physicsJson({rigJson("R", ins, {}, chain(2))}))).ok);
    QVERIFY(!parsePhysics(view(physicsJson({rigJson("R", {}, outs, chain(2))}))).ok);
    // totals: 64 rigs of 32 vertices is exactly kMaxPhysicsVertices, one more vertex in a 65th is over
    QList<QByteArray> many;
    for (int i = 0; i < int(kMaxPhysicsVertices / kMaxPhysicsVerticesPerRig); ++i)
        many << rigJson("R" + QByteArray::number(i), {}, {}, chain(int(kMaxPhysicsVerticesPerRig)));
    QVERIFY(parsePhysics(view(physicsJson(many))).ok);
    many << rigJson("Rx", {}, {}, chain(1));
    QVERIFY(!parsePhysics(view(physicsJson(many))).ok);
    // total inputs: 32 rigs of 32 inputs = 1024
    QList<QByteArray> inRigs;
    for (int i = 0; i < int(kMaxPhysicsInputs / kMaxPhysicsIoPerRig); ++i)
        inRigs << rigJson("R" + QByteArray::number(i), insOk, {}, chain(2));
    QVERIFY(parsePhysics(view(physicsJson(inRigs))).ok);
    inRigs << rigJson("Rx", {inputJson("A")}, {}, chain(2));
    QVERIFY(!parsePhysics(view(physicsJson(inRigs))).ok);
    QList<QByteArray> outRigs;
    for (int i = 0; i < int(kMaxPhysicsOutputs / kMaxPhysicsIoPerRig); ++i)
        outRigs << rigJson("R" + QByteArray::number(i), {}, outsOk, chain(2));
    QVERIFY(parsePhysics(view(physicsJson(outRigs))).ok);
    outRigs << rigJson("Rx", {}, {outputJson("B")}, chain(2));
    QVERIFY(!parsePhysics(view(physicsJson(outRigs))).ok);
}

void TestPuppetPhysics::rigsWithMissingPartsAreSkipped()
{
    ParameterSet p = model();
    const QByteArray text = physicsJson({rigJson("Good", {inputJson("ParamAngleX")}, {outputJson("HairSwing")}, chain(3)),
                                         rigJson("NoInput", {inputJson("Nope")}, {outputJson("HairSwing")}, chain(3)),
                                         rigJson("NoOutput", {inputJson("ParamAngleX")}, {outputJson("Nope")}, chain(3)),
                                         rigJson("VertexZero", {inputJson("ParamAngleX")}, {outputJson("HairSwing", 0)}, chain(3)),
                                         rigJson("VertexPast", {inputJson("ParamAngleX")}, {outputJson("HairSwing", 3)}, chain(3)),
                                         rigJson("OneVertex", {inputJson("ParamAngleX")}, {}, chain(1)),
                                         rigJson("Empty", {}, {}, chain(2))});
    Physics physics(settings(text), p);
    QCOMPARE(physics.activeRigs(), size_t(2)); // Good, Empty
    QCOMPARE(physics.skipped().size(), size_t(5));
    for (const std::string &s : physics.skipped())
        QVERIFY(s.rfind("puppet: physics rig skipped: ", 0) == 0);
    QVERIFY(QString::fromStdString(physics.skipped()[0]).contains(u"NoInput"_s));
    QVERIFY(QString::fromStdString(physics.skipped()[0]).contains(u"Nope"_s));
    // the good rig still works
    physics.update(p, 1.0 / 60); // the rigs start at rest wherever the head is
    p.setById("ParamAngleX", 30);
    double peak = 0;
    for (int i = 0; i < 30; ++i) {
        p.setById("HairSwing", 0);
        physics.update(p, 1.0 / 60);
        peak = std::max(peak, std::abs(p.value(p.find("HairSwing"))));
    }
    QVERIFY(peak > 1e-3);
}

void TestPuppetPhysics::atRestNothingMoves()
{
    ParameterSet p = model();
    Physics physics(settings(hair()), p);
    for (int i = 0; i < 10000; ++i) {
        p.resetToDefaults();
        physics.update(p, 1.0 / 60);
        QCOMPARE(p.value(1), 0.0);
        QCOMPARE(p.value(2), 0.0);
    }
    QVERIFY(physics.energy() < 1e-20);
}

void TestPuppetPhysics::aMovedHeadSwingsTheHairAndSettles()
{
    ParameterSet p = model();
    Physics physics(settings(hair()), p);
    for (int i = 0; i < 60; ++i) {
        p.resetToDefaults();
        physics.update(p, 1.0 / 60);
    }
    // the head turns and stays turned: the hair swings, then hangs from the new place again
    double peak = 0;
    double lastSway = 0;
    for (int i = 0; i < 1200; ++i) {
        p.resetToDefaults();
        p.setById("ParamAngleX", 30);
        physics.update(p, 1.0 / 60);
        peak = std::max(peak, std::abs(p.value(2)));
        lastSway = p.value(2);
    }
    QVERIFY(peak > 0.05);            // it moved
    QVERIFY(std::abs(lastSway) < 1e-3); // and came to rest
    QVERIFY(physics.energy() < 1e-6);
    QVERIFY(std::abs(p.value(1)) < 1e-3);
}

void TestPuppetPhysics::energyDecays()
{
    ParameterSet p = model();
    Physics physics(settings(hair()), p);
    for (int i = 0; i < 5; ++i) {
        p.resetToDefaults();
        physics.update(p, 1.0 / 60);
    }
    for (int i = 0; i < 20; ++i) { // a shake: the head moves back and forth
        p.resetToDefaults();
        p.setById("ParamAngleX", (i % 4 < 2) ? 25 : -25);
        physics.update(p, 1.0 / 60);
    }
    double window[8] = {};
    for (int w = 0; w < 8; ++w) {
        for (int i = 0; i < 30; ++i) {
            p.resetToDefaults();
            p.setById("ParamAngleX", -25);
            physics.update(p, 1.0 / 60);
            window[w] = std::max(window[w], physics.energy());
        }
    }
    // the largest energy of each half second does not grow (after the head has stopped moving)
    for (int w = 2; w < 8; ++w)
        QVERIFY2(window[w] <= window[w - 1] * 1.0001 + 1e-12, qPrintable(u"window %1: %2 > %3"_s.arg(w).arg(window[w]).arg(window[w - 1])));
    QVERIFY(window[7] < window[1]);
}

void TestPuppetPhysics::outputsStayInRange()
{
    ParameterSet p = model();
    const QByteArray text = physicsJson({rigJson("Hair", {inputJson("ParamAngleX", "X")}, {outputJson("HairSwing", 1, "Angle", 1000), outputJson("HairSway", 2, "X", -1000)}, chain(3))});
    Physics physics(settings(text), p);
    for (int i = 0; i < 2000; ++i) {
        p.resetToDefaults();
        p.setById("ParamAngleX", (i / 7) % 2 ? 30 : -30);
        physics.update(p, 1.0 / 60);
        QVERIFY(p.value(1) >= -1 && p.value(1) <= 1);
        QVERIFY(p.value(2) >= -1 && p.value(2) <= 1);
        QVERIFY(std::isfinite(physics.energy()));
    }
}

void TestPuppetPhysics::weightScaleAndReflect()
{
    const auto run = [](const QByteArray &output) {
        ParameterSet p = model();
        Physics physics(settings(physicsJson({rigJson("Hair", {inputJson("ParamAngleX")}, {output}, chain(3))})), p);
        double peak = 0;
        for (int i = 0; i < 40; ++i) {
            p.resetToDefaults();
            p.setById("ParamAngleX", i < 10 ? 0 : 30);
            physics.update(p, 1.0 / 60);
            if (std::abs(p.value(2)) > std::abs(peak))
                peak = p.value(2);
        }
        return peak;
    };
    const double base = run(outputJson("HairSway", 2, "X", 0.5, 100, false));
    QVERIFY(std::abs(base) > 1e-3);
    QVERIFY(std::abs(run(outputJson("HairSway", 2, "X", 0.5, 50, false)) - base / 2) < 1e-9);  // weight is a percentage
    QVERIFY(std::abs(run(outputJson("HairSway", 2, "X", 0.25, 100, false)) - base / 2) < 1e-9); // scale multiplies
    QVERIFY(std::abs(run(outputJson("HairSway", 2, "X", 0.5, 100, true)) + base) < 1e-9);       // reflect flips the sign
    QCOMPARE(run(outputJson("HairSway", 2, "X", 0.5, 0, false)), 0.0);                            // weight 0 writes nothing
}

void TestPuppetPhysics::tiltedHeadTiltsTheChain()
{
    // an angle input of the head's tilt: the chain hangs along the tilted direction, so the angle output settles at the tilt
    ParameterSet p;
    p.add("ParamAngleZ", -30, 0, 30);
    p.add("HairAngle", -1, 0, 1);
    const QByteArray text = physicsJson({rigJson("Hair", {inputJson("ParamAngleZ", "Angle")}, {outputJson("HairAngle", 1, "Angle")}, chain(3))});
    Physics physics(settings(text), p);
    for (int i = 0; i < 600; ++i) {
        p.resetToDefaults();
        p.setById("ParamAngleZ", 30); // the rig's angle range is -30..30: all the way
        physics.update(p, 1.0 / 60);
    }
    QVERIFY(std::abs(p.value(1) - 1.0) < 1e-3);
    for (int i = 0; i < 600; ++i) {
        p.resetToDefaults();
        p.setById("ParamAngleZ", -15);
        physics.update(p, 1.0 / 60);
    }
    QVERIFY(std::abs(p.value(1) - -0.5) < 1e-3);
}

void TestPuppetPhysics::windPushesTheChain()
{
    ParameterSet p = model();
    const QByteArray text = physicsJson({rigJson("Hair", {inputJson("ParamAngleX")}, {outputJson("HairSway", 1, "X")}, chain(2))},
                                        R"("Fps":60,"EffectiveForces":{"Gravity":{"X":0,"Y":-1},"Wind":{"X":0.5,"Y":0}})");
    Physics physics(settings(text), p);
    for (int i = 0; i < 600; ++i) {
        p.resetToDefaults();
        physics.update(p, 1.0 / 60);
    }
    QVERIFY(p.value(2) > 0.05); // blown to the +x side, and held there
}

void TestPuppetPhysics::deterministic()
{
    ParameterSet a = model(), b = model();
    Physics pa(settings(hair()), a), pb(settings(hair()), b);
    for (int i = 0; i < 10000; ++i) {
        const double input = 30 * std::sin(i * 0.05);
        a.resetToDefaults();
        b.resetToDefaults();
        a.setById("ParamAngleX", input);
        b.setById("ParamAngleX", input);
        pa.update(a, 1.0 / 60);
        pb.update(b, 1.0 / 60);
        QCOMPARE(a.value(1), b.value(1));
        QCOMPARE(a.value(2), b.value(2));
    }
    QCOMPARE(pa.energy(), pb.energy());
}

void TestPuppetPhysics::longStepsAreSplitAndSafe()
{
    // six frames in one update equal six updates (a constant input, so the interpolation of inputs does not differ)
    const double h = 1.0 / 60;
    ParameterSet a = model(), b = model();
    Physics pa(settings(hair()), a), pb(settings(hair()), b);
    for (int i = 0; i < 20; ++i) {
        a.resetToDefaults();
        b.resetToDefaults();
        a.setById("ParamAngleX", 20);
        b.setById("ParamAngleX", 20);
        pa.update(a, h + 1e-9);
        pb.update(b, h + 1e-9);
    }
    a.resetToDefaults();
    b.resetToDefaults();
    a.setById("ParamAngleX", 20);
    b.setById("ParamAngleX", 20);
    pa.update(a, 6 * h + 6e-9);
    for (int i = 0; i < 6; ++i) {
        b.resetToDefaults();
        b.setById("ParamAngleX", 20);
        pb.update(b, h + 1e-9);
    }
    QVERIFY(std::abs(a.value(2) - b.value(2)) < 1e-9);
    QVERIFY(std::abs(pa.energy() - pb.energy()) < 1e-9);
    // an hour at once does not explode: at most kMaxPhysicsStepsPerUpdate steps are simulated
    ParameterSet c = model();
    Physics pc(settings(hair()), c);
    for (const double dt : {3600.0, 1e12, 1e300}) {
        c.resetToDefaults();
        c.setById("ParamAngleX", 30);
        pc.update(c, dt);
        QVERIFY(std::isfinite(pc.energy()));
        QVERIFY(c.value(2) >= -1 && c.value(2) <= 1);
    }
    // a lower Fps simulates fewer steps for the same time, and still behaves
    ParameterSet d = model();
    Physics pd(settings(physicsJson({rigJson("Hair", {inputJson("ParamAngleX")}, {outputJson("HairSway", 2, "X")}, chain(3))}, R"("Fps":10,"EffectiveForces":{"Gravity":{"X":0,"Y":-1},"Wind":{"X":0,"Y":0}})")), d);
    for (int i = 0; i < 1000; ++i) {
        d.resetToDefaults();
        d.setById("ParamAngleX", (i / 20) % 2 ? 30 : -30);
        pd.update(d, 1.0 / 60);
        QVERIFY(std::isfinite(pd.energy()) && pd.energy() < 1e6);
    }
}

void TestPuppetPhysics::badStepsAreIgnored()
{
    ParameterSet p = model();
    Physics physics(settings(hair()), p);
    p.setById("ParamAngleX", 30);
    physics.update(p, -1);
    physics.update(p, std::numeric_limits<double>::quiet_NaN());
    physics.update(p, std::numeric_limits<double>::infinity());
    QCOMPARE(p.value(1), 0.0);
    physics.update(p, 0.0);
    QVERIFY(std::isfinite(p.value(1)));
}

void TestPuppetPhysics::resetStartsWithoutAJolt()
{
    ParameterSet p = model();
    Physics physics(settings(hair()), p);
    for (int i = 0; i < 100; ++i) {
        p.resetToDefaults();
        p.setById("ParamAngleX", (i % 10) * 3);
        physics.update(p, 1.0 / 60);
    }
    physics.reset();
    p.resetToDefaults();
    p.setById("ParamAngleX", 30); // the head is far from where it was
    physics.update(p, 1.0 / 60);
    QVERIFY(physics.energy() < 1e-20);
    QCOMPARE(p.value(2), 0.0);
}

void TestPuppetPhysics::randomInputsStaySane()
{
    ParameterSet p = model();
    Physics physics(settings(hair()), p);
    quint32 state = 99;
    for (int i = 0; i < 20000; ++i) {
        state = state * 1664525u + 1013904223u;
        p.resetToDefaults();
        p.setById("ParamAngleX", (double(state >> 8) / double(1 << 24)) * 80 - 40);
        physics.update(p, (state & 3) == 0 ? 0.2 : 1.0 / 60);
        QVERIFY(std::isfinite(physics.energy()));
        QVERIFY(p.value(1) >= -1 && p.value(1) <= 1);
        QVERIFY(p.value(2) >= -1 && p.value(2) <= 1);
    }
}

void TestPuppetPhysics::notCopyableAndBoundToItsParameters()
{
    static_assert(!std::is_copy_constructible_v<Physics>);
    static_assert(!std::is_copy_assignable_v<Physics>);
    ParameterSet p = model();
    Physics physics(settings(hair()), p);
    Physics moved(std::move(physics)); // a move keeps the rigs' pointers valid
    for (int i = 0; i < 30; ++i) {
        p.resetToDefaults();
        p.setById("ParamAngleX", i < 10 ? 0 : 30);
        moved.update(p, 1.0 / 60);
    }
    QVERIFY(moved.energy() > 0);
    // another parameter set than the one it was bound to: nothing happens
    ParameterSet other;
    other.add("x", 0, 0, 1);
    const double energy = moved.energy();
    moved.update(other, 1.0);
    QCOMPARE(moved.energy(), energy);
    QCOMPARE(other.value(0), 0.0);
    // the bounds checks of the sets
    QVERIFY(p.at(99).id.empty());
    QCOMPARE(p.value(99), 0.0);
    QCOMPARE(p.value(-1), 0.0);
    PartSet parts;
    QVERIFY(parts.id(3).empty());
    QCOMPARE(parts.opacity(3), 0.0);
}

void TestPuppetPhysics::hugeNumbersAreRefused()
{
    const auto text = [](const QByteArray &position, const QByteArray &scale) {
        return physicsJson({rigJson("R", {inputJson("A")}, {outputJson("B", 1, "Angle", scale.toDouble())}, {QByteArray("{\"Position\":{\"X\":0,\"Y\":") + position + ",\"Mobility\":1,\"Delay\":1,\"Acceleration\":1,\"Radius\":3}"})});
    };
    Q_UNUSED(text);
    QByteArray bigVertex = R"({"Position":{"X":1e300,"Y":0},"Mobility":1,"Delay":1,"Acceleration":1,"Radius":3})";
    QVERIFY(!parsePhysics(view(physicsJson({rigJson("R", {}, {}, {bigVertex, bigVertex})}))).ok);
    QVERIFY(!parsePhysics(view(physicsJson({rigJson("R", {}, {outputJson("B", 1, "Angle", 1e9)}, chain(2))}))).ok);
    QVERIFY(!parsePhysics(view(physicsJson({rigJson("R", {}, {}, chain(2), R"({"Position":{"Minimum":-1e9,"Default":0,"Maximum":10},"Angle":{"Minimum":-30,"Default":0,"Maximum":30}})")}))).ok);
    QVERIFY(parsePhysics(view(physicsJson({rigJson("R", {}, {outputJson("B", 1, "Angle", 1e7)}, chain(2))}))).ok);
}

QTEST_GUILESS_MAIN(TestPuppetPhysics)
#include "tst_puppetphysics.moc"
