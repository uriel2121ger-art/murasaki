// murasaki-scene-vk SceneScript API coverage: one case per documented entry of the public reference
// (docs.wallpaperengine.io, scenescript/reference), run in the QuickJS host against a fake scene. The table of entries is
// docs/scene-vk/scripts-api.tsv; every row must have a case here (everyDocumentedEntryHasACase).

#include "assets/vfs.h"
#include "puppetfixtures.h"
#include "render/scenescripts.h"
#include "scene/scene.h"
#include "scenevkfixtures.h"
#include "script/scripthost.h"
#include "support/meshwriter.h"
#include "videofixtures.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTest>

#include <cmath>
#include <map>

using namespace murasaki::scene;

namespace {

class FakeScene : public ScriptSceneBridge
{
public:
    std::map<std::pair<int, std::string>, ScriptValue> values;
    ScriptValue get(int layer, const std::string &p) override
    {
        const auto it = values.find({layer, p});
        return it == values.end() ? ScriptValue{} : it->second;
    }
    void set(int layer, const std::string &p, const ScriptValue &v) override { values[{layer, p}] = v; }
    int createLayer(const std::string &, bool) override { return 100; }
    int indexOf(int layer) override { return layer; }
    void sort(int, int) override {}
    int find(const std::string &name) override { return name == "zero" ? 0 : -1; }
    int findById(int id) override { return id == 77 ? 0 : -1; }
    // layer 0 is a solid 100 x 100 square from the origin; the local position is from its centre
    bool hitTest(int layer, double x, double y, double *lx, double *ly) override
    {
        *lx = x - 50;
        *ly = y - 50;
        return layer == 0 && x >= 0 && x <= 100 && y >= 0 && y <= 100;
    }
};

struct Case {
    QString id;       // "Vec2.length": the class (or module, event) and the member, as in the table
    QString expr;     // a JavaScript expression
    QString expected; // what it formats to: numbers rounded to 4 decimals, vectors and matrices as space separated components
};

// The shared helpers every expression is wrapped in.
const char *kFormat = R"JS(
const rnd = (x) => String(+x.toFixed(4));
const fmt = (r) => typeof r === 'number' ? rnd(r)
    : Array.isArray(r) ? r.map((x) => typeof x === 'number' ? rnd(x) : String(x)).join(' ')
    : r && typeof r === 'object' && r.m ? r.m.map(rnd).join(' ')
    : r && typeof r === 'object' && typeof r.toJSON === 'function' ? r.toJSON().map(rnd).join(' ')
    : String(r);
)JS";

QString evaluate(const QString &expr, QString *error = nullptr)
{
    FakeScene scene;
    ScriptHost host(scene);
    std::string err;
    ScriptTarget t;
    t.layer = 0;
    t.property = "text";
    t.name = "api";
    t.source = std::string(kFormat) + "export function update(v) { return fmt(" + expr.toStdString() + "); }";
    if (!host.add(t, ScriptValue::ofText(""), &err)) {
        if (error)
            *error = QString::fromStdString(err);
        return {};
    }
    ScriptFrame f;
    host.update(f);
    if (host.disabledCount() != 0 && error) {
        for (const std::string &m : host.messages())
            *error += QString::fromStdString(m) + "\n";
    }
    return QString::fromStdString(scene.get(0, "text").text);
}

QList<Case> cases()
{
    QList<Case> c;
    const auto add = [&](const char *id, const char *expr, const char *expected) { c.append({QString::fromLatin1(id), QString::fromLatin1(expr), QString::fromLatin1(expected)}); };
    // ---- Vec2 ----
    add("Vec2.x", "new Vec2(1, 2).x", "1");
    add("Vec2.y", "new Vec2(1, 2).y", "2");
    add("Vec2.constructor", "[new Vec2(1, 2), new Vec2(), new Vec2(3), new Vec2('4 5 6'), new Vec2(new Vec3(7, 8, 9))].join('|')", "1 2|0 0|3 3|4 5|7 8");
    add("Vec2.length", "new Vec2(3, 4).length()", "5");
    add("Vec2.lengthSqr", "new Vec2(3, 4).lengthSqr()", "25");
    add("Vec2.distance", "new Vec2(1, 1).distance(new Vec2(4, 5))", "5");
    add("Vec2.distanceSqr", "new Vec2(1, 1).distanceSqr(new Vec2(4, 5))", "25");
    add("Vec2.normalize", "new Vec2(0, 5).normalize()", "0 1");
    add("Vec2.copy", "(() => { const a = new Vec2(1, 2); const b = a.copy(); b.x = 9; return a + '|' + b; })()", "1 2|9 2");
    add("Vec2.equals", "new Vec2(1, 2).equals(new Vec2(1, 2.0000001))", "true");
    add("Vec2.isFinite", "(() => { const a = new Vec2(1, 2); const f = a.isFinite(); a.y = Infinity; return f + ' ' + a.isFinite(); })()", "true false");
    add("Vec2.negate", "new Vec2(1, -2).negate()", "-1 2");
    add("Vec2.add", "[new Vec2(1, 2).add(3), new Vec2(1, 2).add(new Vec2(3, 4))].join('|')", "4 5|4 6");
    add("Vec2.subtract", "[new Vec2(5, 5).subtract(1), new Vec2(5, 5).subtract(new Vec2(1, 2))].join('|')", "4 4|4 3");
    add("Vec2.multiply", "[new Vec2(1, 2).multiply(3), new Vec2(1, 2).multiply(new Vec2(3, 4))].join('|')", "3 6|3 8");
    add("Vec2.divide", "[new Vec2(6, 8).divide(2), new Vec2(6, 8).divide(new Vec2(3, 4))].join('|')", "3 4|2 2");
    add("Vec2.dot", "new Vec2(1, 2).dot(new Vec2(3, 4))", "11");
    add("Vec2.reflect", "new Vec2(1, -1).reflect(new Vec2(0, 1))", "1 1");
    add("Vec2.mix", "new Vec2(0, 0).mix(new Vec2(10, 20), 0.5)", "5 10");
    add("Vec2.min", "new Vec2(1, 5).min(new Vec2(3, 2))", "1 2");
    add("Vec2.max", "new Vec2(1, 5).max(new Vec2(3, 2))", "3 5");
    add("Vec2.clamp", "[new Vec2(-5, 5).clamp(0, 3), new Vec2(-5, 5).clamp(new Vec2(0, 1), new Vec2(3, 4))].join('|')", "0 3|0 4");
    add("Vec2.perpendicular", "new Vec2(1, 0).perpendicular()", "0 1");
    add("Vec2.abs", "new Vec2(-1, -2).abs()", "1 2");
    add("Vec2.sign", "new Vec2(-3, 4).sign()", "-1 1");
    add("Vec2.round", "new Vec2(1.4, 1.6).round()", "1 2");
    add("Vec2.floor", "new Vec2(1.9, -1.1).floor()", "1 -2");
    add("Vec2.ceil", "new Vec2(1.1, -1.9).ceil()", "2 -1");
    add("Vec2.fract", "new Vec2(1.25, -0.25).fract()", "0.25 0.75");
    add("Vec2.mod", "new Vec2(5, -1).mod(3)", "2 2");
    add("Vec2.step", "new Vec2(1, 3).step(2)", "0 1");
    add("Vec2.smoothStep", "new Vec2(0.5, 2).smoothStep(0, 1)", "0.5 1");
    add("Vec2.toString", "String(new Vec2(1, 2))", "1 2");
    add("Vec2.angle", "[new Vec2(1, 0).angle(), new Vec2(0, 1).angle(), new Vec2(-1, 0).angle(), new Vec2(0, -1).angle()].join(' ')", "0 90 180 -90");
    add("Vec2.angleBetween", "[new Vec2(1, 0).angleBetween(new Vec2(0, 1)), new Vec2(0, 1).angleBetween(new Vec2(1, 0)), new Vec2(1, 0).angleBetween(new Vec2(2, 0)), new Vec2(1, 0).angleBetween(new Vec2(0, 0))].join(' ')",
        "90 -90 0 0");
    add("Vec2.project", "[new Vec2(2, 3).project(new Vec2(4, 0)), new Vec2(2, 3).project(new Vec2(0, 0))].join('|')", "2 0|0 0");
    add("Vec2.rotate", "[new Vec2(1, 0).rotate(90), new Vec2(1, 0).rotate(180), new Vec2(0, 2).rotate(-90)].map(fmt).join('|')", "0 1|-1 0|2 0");
    add("IModelData.static", "[IModelData.POSITION, IModelData.NORMAL, IModelData.UV, IModelData.TANGENT_SIGNED, IModelData.COLOR].join(',')", "position,normal,uv,tangentsigned,color");
    add("Mat4.m", "fmt(Mat4.fromTranslation(new Vec3(1, 2, 3)).m)", "1 0 0 0 0 1 0 0 0 0 1 0 1 2 3 1");
    // ---- Vec3 ----
    add("Vec3.x", "new Vec3(1, 2, 3).x", "1");
    add("Vec3.y", "new Vec3(1, 2, 3).y", "2");
    add("Vec3.z", "new Vec3(1, 2, 3).z", "3");
    add("Vec3.constructor", "[new Vec3(1, 2, 3), new Vec3(), new Vec3(4), new Vec3(1, 2), new Vec3('5 6 7'), new Vec3(new Vec2(8, 9))].join('|')",
        "1 2 3|0 0 0|4 4 4|1 2 0|5 6 7|8 9 0");
    add("Vec3.fromSpherical", "[Vec3.fromSpherical(2, 90, 0), Vec3.fromSpherical(1, 0, 0)].join('|').split('|').map((s) => s.split(' ').map((n) => rnd(+n)).join(' ')).join('|')", "2 0 0|0 1 0");
    add("Vec3.toSpherical", "[new Vec3(0, 1, 0).toSpherical(), new Vec3(2, 0, 0).toSpherical()].map((v) => v.toJSON().map(rnd).join(' ')).join('|')", "1 0 0|2 90 0");
    add("Vec3.equals", "new Vec3(1, 2, 3).equals(new Vec3(1, 2, 3.0000001))", "true");
    add("Vec3.distance", "new Vec3(0, 0, 0).distance(new Vec3(1, 2, 2))", "3");
    add("Vec3.distanceSqr", "new Vec3(0, 0, 0).distanceSqr(new Vec3(1, 2, 2))", "9");
    add("Vec3.length", "new Vec3(2, 3, 6).length()", "7");
    add("Vec3.lengthSqr", "new Vec3(2, 3, 6).lengthSqr()", "49");
    add("Vec3.normalize", "new Vec3(0, 0, 4).normalize()", "0 0 1");
    add("Vec3.copy", "(() => { const a = new Vec3(1, 2, 3); const b = a.copy(); b.z = 0; return a + '|' + b; })()", "1 2 3|1 2 0");
    add("Vec3.isFinite", "(() => { const a = new Vec3(1, 2, 3); a.z = NaN; return a.isFinite(); })()", "false");
    add("Vec3.negate", "new Vec3(1, -2, 3).negate()", "-1 2 -3");
    add("Vec3.add", "[new Vec3(1, 2, 3).add(1), new Vec3(1, 2, 3).add(new Vec3(1, 1, 1)), new Vec3(1, 2, 3).add(new Vec2(1, 1))].join('|')", "2 3 4|2 3 4|2 3 3");
    add("Vec3.subtract", "new Vec3(3, 3, 3).subtract(new Vec3(1, 2, 3))", "2 1 0");
    add("Vec3.multiply", "[new Vec3(1, 2, 3).multiply(2), new Vec3(1, 2, 3).multiply(new Vec3(2, 2, 2))].join('|')", "2 4 6|2 4 6");
    add("Vec3.divide", "new Vec3(2, 4, 6).divide(2)", "1 2 3");
    add("Vec3.dot", "new Vec3(1, 2, 3).dot(new Vec3(4, 5, 6))", "32");
    add("Vec3.reflect", "new Vec3(1, -1, 0).reflect(new Vec3(0, 1, 0))", "1 1 0");
    add("Vec3.refract", "[new Vec3(0, -1, 0).refract(new Vec3(0, 1, 0), 1), new Vec3(1, 0, 0).refract(new Vec3(0, 1, 0), 2)].join('|')", "0 -1 0|0 0 0");
    add("Vec3.project", "new Vec3(1, 1, 0).project(new Vec3(2, 0, 0))", "1 0 0");
    add("Vec3.angleBetween", "new Vec3(1, 0, 0).angleBetween(new Vec3(0, 1, 0))", "90");
    add("Vec3.mix", "new Vec3(0, 0, 0).mix(new Vec3(2, 4, 6), 0.5)", "1 2 3");
    add("Vec3.min", "new Vec3(1, 5, 3).min(new Vec3(3, 2, 3))", "1 2 3");
    add("Vec3.max", "new Vec3(1, 5, 3).max(new Vec3(3, 2, 4))", "3 5 4");
    add("Vec3.cross", "new Vec3(1, 0, 0).cross(new Vec3(0, 1, 0))", "0 0 1");
    add("Vec3.clamp", "new Vec3(-1, 5, 2).clamp(0, 3)", "0 3 2");
    add("Vec3.abs", "new Vec3(-1, 2, -3).abs()", "1 2 3");
    add("Vec3.sign", "new Vec3(-1, 0, 3).sign()", "-1 0 1");
    add("Vec3.round", "new Vec3(0.4, 0.6, 1.5).round()", "0 1 2");
    add("Vec3.floor", "new Vec3(1.5, -1.5, 0.2).floor()", "1 -2 0");
    add("Vec3.ceil", "new Vec3(1.5, -1.5, 0.2).ceil()", "2 -1 1");
    add("Vec3.fract", "new Vec3(1.5, 2.25, -0.75).fract()", "0.5 0.25 0.25");
    add("Vec3.mod", "new Vec3(5, 6, 7).mod(new Vec3(2, 4, 8))", "1 2 7");
    add("Vec3.step", "new Vec3(1, 2, 3).step(new Vec3(2, 2, 2))", "0 1 1");
    add("Vec3.smoothStep", "new Vec3(0, 0.5, 1).smoothStep(0, 1)", "0 0.5 1");
    add("Vec3.toString", "String(new Vec3(1, 2, 3))", "1 2 3");
    // ---- Vec4 ----
    add("Vec4.x", "new Vec4(1, 2, 3, 4).x", "1");
    add("Vec4.y", "new Vec4(1, 2, 3, 4).y", "2");
    add("Vec4.z", "new Vec4(1, 2, 3, 4).z", "3");
    add("Vec4.w", "new Vec4(1, 2, 3, 4).w", "4");
    add("Vec4.constructor", "[new Vec4(1, 2, 3, 4), new Vec4(), new Vec4(5), new Vec4(new Vec2(1, 2), 3, 4), new Vec4(new Vec3(1, 2, 3), 4), new Vec4('1 2 3 4'), new Vec4(new Vec4(4, 3, 2, 1))].join('|')",
        "1 2 3 4|0 0 0 0|5 5 5 5|1 2 3 4|1 2 3 4|1 2 3 4|4 3 2 1");
    add("Vec4.length", "new Vec4(1, 1, 1, 1).length()", "2");
    add("Vec4.lengthSqr", "new Vec4(1, 2, 3, 4).lengthSqr()", "30");
    add("Vec4.distance", "new Vec4(0, 0, 0, 0).distance(new Vec4(2, 2, 2, 2))", "4");
    add("Vec4.distanceSqr", "new Vec4(0, 0, 0, 0).distanceSqr(new Vec4(1, 1, 1, 1))", "4");
    add("Vec4.normalize", "new Vec4(0, 0, 0, 3).normalize()", "0 0 0 1");
    add("Vec4.copy", "(() => { const a = new Vec4(1, 2, 3, 4); const b = a.copy(); b.w = 0; return a + '|' + b; })()", "1 2 3 4|1 2 3 0");
    add("Vec4.equals", "new Vec4(1, 2, 3, 4).equals(new Vec4(1, 2, 3, 4))", "true");
    add("Vec4.isFinite", "new Vec4(1, 2, 3, 4).isFinite()", "true");
    add("Vec4.negate", "new Vec4(1, -2, 3, -4).negate()", "-1 2 -3 4");
    add("Vec4.add", "[new Vec4(1, 2, 3, 4).add(1), new Vec4(1, 2, 3, 4).add(new Vec4(1, 1, 1, 1))].join('|')", "2 3 4 5|2 3 4 5");
    add("Vec4.subtract", "new Vec4(4, 4, 4, 4).subtract(new Vec4(1, 2, 3, 4))", "3 2 1 0");
    add("Vec4.multiply", "[new Vec4(1, 2, 3, 4).multiply(2), new Vec4(1, 2, 3, 4).multiply(new Vec4(1, 0, 1, 0))].join('|')", "2 4 6 8|1 0 3 0");
    add("Vec4.divide", "new Vec4(2, 4, 6, 8).divide(2)", "1 2 3 4");
    add("Vec4.dot", "new Vec4(1, 2, 3, 4).dot(new Vec4(1, 1, 1, 1))", "10");
    add("Vec4.reflect", "new Vec4(1, -1, 0, 0).reflect(new Vec4(0, 1, 0, 0))", "1 1 0 0");
    add("Vec4.project", "new Vec4(1, 1, 0, 0).project(new Vec4(2, 0, 0, 0))", "1 0 0 0");
    add("Vec4.mix", "[new Vec4(0, 0, 0, 0).mix(new Vec4(4, 4, 4, 4), 0.5), new Vec4(0, 0, 0, 0).mix(new Vec4(4, 4, 4, 4), new Vec4(0, 0.25, 0.5, 1))].join('|')", "2 2 2 2|0 1 2 4");
    add("Vec4.min", "new Vec4(1, 5, 3, 7).min(new Vec4(3, 2, 3, 8))", "1 2 3 7");
    add("Vec4.max", "new Vec4(1, 5, 3, 7).max(new Vec4(3, 2, 3, 8))", "3 5 3 8");
    add("Vec4.clamp", "new Vec4(-1, 5, 2, 9).clamp(0, 3)", "0 3 2 3");
    add("Vec4.abs", "new Vec4(-1, 2, -3, 4).abs()", "1 2 3 4");
    add("Vec4.sign", "new Vec4(-1, 0, 3, -4).sign()", "-1 0 1 -1");
    add("Vec4.round", "new Vec4(0.4, 0.6, 1.5, 2.4).round()", "0 1 2 2");
    add("Vec4.floor", "new Vec4(1.5, -1.5, 0.2, 3.9).floor()", "1 -2 0 3");
    add("Vec4.ceil", "new Vec4(1.5, -1.5, 0.2, 3.1).ceil()", "2 -1 1 4");
    add("Vec4.fract", "new Vec4(1.5, 2.25, -0.75, 3).fract()", "0.5 0.25 0.25 0");
    add("Vec4.mod", "new Vec4(5, 6, 7, 9).mod(4)", "1 2 3 1");
    add("Vec4.step", "new Vec4(1, 2, 3, 4).step(3)", "0 0 1 1");
    add("Vec4.smoothStep", "new Vec4(0, 0.5, 1, 2).smoothStep(0, 1)", "0 0.5 1 1");
    add("Vec4.toString", "String(new Vec4(1, 2, 3, 4))", "1 2 3 4");
    // ---- Mat3 ----
    add("Mat3.m", "new Mat3().m.length + ' ' + new Mat3().m[0] + ' ' + new Mat3().m[4] + ' ' + new Mat3().m[8]", "9 1 1 1");
    add("Mat3.constructor", "new Mat3()", "1 0 0 0 1 0 0 0 1");
    add("Mat3.identity", "Mat3.identity()", "1 0 0 0 1 0 0 0 1");
    add("Mat3.fromTranslation", "Mat3.fromTranslation(new Vec2(2, 3)).transformPoint(new Vec2(1, 1))", "3 4");
    add("Mat3.fromScale", "[Mat3.fromScale(2).transformPoint(new Vec2(1, 2)), Mat3.fromScale(new Vec2(2, 3)).transformPoint(new Vec2(1, 2))].join('|')", "2 4|2 6");
    add("Mat3.fromRotation", "Mat3.fromRotation(90).transformPoint(new Vec2(1, 0))", "0 1");
    add("Mat3.fromBasis", "Mat3.fromBasis(new Vec2(0, 1), new Vec2(-1, 0)).transformPoint(new Vec2(1, 0))", "0 1");
    add("Mat3.fromMat4", "Mat3.fromMat4(Mat4.fromScale(new Vec3(2, 3, 4)))", "2 0 0 0 3 0 0 0 4");
    add("Mat3.compose", "Mat3.compose(new Vec2(1, 2), 90, new Vec2(2, 2)).transformPoint(new Vec2(1, 0))", "1 4");
    add("Mat3.translation", "(() => { const m = new Mat3(); const r = m.translation(new Vec2(5, 6)); return [r === undefined, m.translation()].join('|'); })()", "true|5 6");
    add("Mat3.angle", "Mat3.fromRotation(30).angle()", "30");
    add("Mat3.add", "new Mat3().add(new Mat3())", "2 0 0 0 2 0 0 0 2");
    add("Mat3.subtract", "new Mat3().subtract(new Mat3())", "0 0 0 0 0 0 0 0 0");
    add("Mat3.multiply", "[Mat3.fromTranslation(new Vec2(1, 0)).multiply(Mat3.fromTranslation(new Vec2(0, 1))).transformPoint(new Vec2(0, 0)), Mat3.fromTranslation(new Vec2(1, 2)).multiply(new Vec3(1, 1, 1)), new Mat3().multiply(2)].join('|')",
        "1 1|2 3 1|2 0 0 0 2 0 0 0 2");
    add("Mat3.translate", "new Mat3().translate(new Vec2(1, 2)).transformPoint(new Vec2(0, 0))", "1 2");
    add("Mat3.rotate", "new Mat3().rotate(90).transformPoint(new Vec2(1, 0))", "0 1");
    add("Mat3.scale", "Mat3.fromTranslation(new Vec2(1, 1)).scale(2).transformPoint(new Vec2(1, 1))", "3 3");
    add("Mat3.transformPoint", "Mat3.fromTranslation(new Vec2(1, 2)).transformPoint(new Vec2(1, 1))", "2 3");
    add("Mat3.transformDirection", "Mat3.fromTranslation(new Vec2(1, 2)).transformDirection(new Vec2(1, 1))", "1 1");
    add("Mat3.transpose", "Mat3.fromTranslation(new Vec2(1, 2)).transpose()", "1 0 1 0 1 2 0 0 1");
    add("Mat3.determinant", "Mat3.fromScale(new Vec2(2, 3)).determinant()", "6");
    add("Mat3.inverse", "Mat3.fromTranslation(new Vec2(1, 2)).inverse().transformPoint(new Vec2(1, 2))", "0 0");
    add("Mat3.decompose", "(() => { const d = Mat3.compose(new Vec2(1, 2), 45, new Vec2(2, 3)).decompose(); return [d.translation.x, d.translation.y, d.rotation, d.scale.x, d.scale.y]; })()", "1 2 45 2 3");
    add("Mat3.copy", "(() => { const a = new Mat3(); const b = a.copy(); b.m[0] = 5; return a.m[0] + ' ' + b.m[0]; })()", "1 5");
    add("Mat3.equals", "[new Mat3().equals(new Mat3()), new Mat3().equals(Mat3.fromScale(2))].join(' ')", "true false");
    add("Mat3.toString", "String(Mat3.fromTranslation(new Vec2(1, 2)))", "1 0 0 0 1 0 1 2 1");
    // ---- Mat4 ----
    add("Mat4.constructor", "new Mat4()", "1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1");
    add("Mat4.identity", "Mat4.identity().determinant()", "1");
    add("Mat4.fromTranslation", "[Mat4.fromTranslation(new Vec3(1, 2, 3)).transformPoint(new Vec3(0, 0, 0)), Mat4.fromTranslation(new Vec2(1, 2)).translation()].join('|')", "1 2 3|1 2 0");
    add("Mat4.fromScale", "[Mat4.fromScale(2).transformPoint(new Vec3(1, 2, 3)), Mat4.fromScale(new Vec3(1, 2, 3)).transformPoint(new Vec3(1, 1, 1))].join('|')", "2 4 6|1 2 3");
    add("Mat4.fromRotation", "Mat4.fromRotation(90, new Vec3(0, 0, 1)).transformPoint(new Vec3(1, 0, 0))", "0 1 0");
    add("Mat4.fromEuler", "[Mat4.fromEuler(0, 0, 90).transformPoint(new Vec3(1, 0, 0)), Mat4.fromEuler(new Vec3(90, 0, 0)).transformPoint(new Vec3(0, 1, 0))].map((v) => fmt(v)).join('|')", "0 1 0|0 0 1");
    add("Mat4.fromBasis", "Mat4.fromBasis(new Vec3(0, 1, 0), new Vec3(-1, 0, 0), new Vec3(0, 0, 1)).transformPoint(new Vec3(1, 0, 0))", "0 1 0");
    add("Mat4.lookAt", "Mat4.lookAt(new Vec3(0, 0, 5), new Vec3(0, 0, 0), new Vec3(0, 1, 0)).transformPoint(new Vec3(0, 0, 0))", "0 0 -5");
    add("Mat4.compose", "Mat4.compose(new Vec3(1, 2, 3), new Vec3(0, 0, 90), new Vec3(2, 2, 2)).transformPoint(new Vec3(1, 0, 0))", "1 4 3");
    add("Mat4.translation", "(() => { const m = new Mat4(); const r = m.translation(new Vec3(5, 6, 7)); return [r === undefined, m.translation()].join('|'); })()", "true|5 6 7");
    add("Mat4.right", "Mat4.fromEuler(0, 0, 90).right()", "0 1 0");
    add("Mat4.up", "Mat4.fromEuler(0, 0, 90).up()", "-1 0 0");
    add("Mat4.forward", "Mat4.fromEuler(0, 0, 90).forward()", "0 0 1");
    add("Mat4.add", "new Mat4().add(new Mat4()).m[0]", "2");
    add("Mat4.subtract", "new Mat4().subtract(new Mat4()).m[5]", "0");
    add("Mat4.multiply", "[Mat4.fromTranslation(new Vec3(1, 0, 0)).multiply(Mat4.fromTranslation(new Vec3(0, 1, 0))).transformPoint(new Vec3(0, 0, 0)), Mat4.fromTranslation(new Vec3(1, 2, 3)).multiply(new Vec4(0, 0, 0, 1)), new Mat4().multiply(3).m[10]].join('|')",
        "1 1 0|1 2 3 1|3");
    add("Mat4.translate", "Mat4.fromScale(2).translate(new Vec3(1, 0, 0)).transformPoint(new Vec3(0, 0, 0))", "2 0 0");
    add("Mat4.rotate", "new Mat4().rotate(90, new Vec3(0, 0, 1)).transformPoint(new Vec3(1, 0, 0))", "0 1 0");
    add("Mat4.scale", "new Mat4().scale(new Vec3(1, 2, 3)).transformPoint(new Vec3(1, 1, 1))", "1 2 3");
    add("Mat4.transformPoint", "Mat4.fromTranslation(new Vec3(1, 2, 3)).transformPoint(new Vec3(1, 1, 1))", "2 3 4");
    add("Mat4.transformDirection", "Mat4.fromTranslation(new Vec3(1, 2, 3)).transformDirection(new Vec3(1, 1, 1))", "1 1 1");
    add("Mat4.transpose", "Mat4.fromTranslation(new Vec3(1, 2, 3)).transpose().m.slice(3, 4).concat(Mat4.fromTranslation(new Vec3(1, 2, 3)).transpose().m.slice(0, 3))", "1 1 0 0");
    add("Mat4.inverse", "(() => { const m = Mat4.compose(new Vec3(1, 2, 3), new Vec3(10, 20, 30), new Vec3(2, 3, 4)); return m.inverse().multiply(m).equals(new Mat4()); })()", "true");
    add("Mat4.determinant", "Mat4.fromScale(new Vec3(2, 3, 4)).determinant()", "24");
    add("Mat4.extractEuler", "Mat4.fromEuler(10, 20, 30).extractEuler()", "10 20 30");
    add("Mat4.normalMatrix", "Mat4.fromScale(2).normalMatrix()", "0.5 0 0 0 0.5 0 0 0 0.5");
    add("Mat4.decompose", "(() => { const d = Mat4.compose(new Vec3(1, 2, 3), new Vec3(10, 20, 30), new Vec3(2, 3, 4)).decompose(); return [d.translation, d.rotation, d.scale].join('|'); })().split('|').map((s) => s.split(' ').map((n) => rnd(+n)).join(' ')).join('|')",
        "1 2 3|10 20 30|2 3 4");
    add("Mat4.copy", "(() => { const a = new Mat4(); const b = a.copy(); b.m[0] = 5; return a.m[0] + ' ' + b.m[0]; })()", "1 5");
    add("Mat4.equals", "[new Mat4().equals(new Mat4()), new Mat4().equals(Mat4.fromScale(2))].join(' ')", "true false");
    add("Mat4.toString", "String(new Mat4())", "1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1");
    // ---- engine ----
    add("IEngine.AUDIO_RESOLUTION_16", "engine.AUDIO_RESOLUTION_16", "16");
    add("IEngine.AUDIO_RESOLUTION_32", "engine.AUDIO_RESOLUTION_32", "32");
    add("IEngine.AUDIO_RESOLUTION_64", "engine.AUDIO_RESOLUTION_64", "64");
    add("IEngine.isDesktopDevice", "engine.isDesktopDevice()", "true");
    add("IEngine.isMobileDevice", "engine.isMobileDevice()", "false");
    add("IEngine.isWallpaper", "engine.isWallpaper()", "true");
    add("IEngine.isScreensaver", "engine.isScreensaver()", "false");
    add("IEngine.isRunningInEditor", "engine.isRunningInEditor()", "false");
    add("IEngine.isPortrait", "engine.isPortrait()", "false");
    add("IEngine.isLandscape", "engine.isLandscape()", "true");
    add("IEngine.openUserShortcut", "engine.openUserShortcut('shortcut')", "false");
    add("IEngine.registerAudioBuffers", "[engine.registerAudioBuffers(engine.AUDIO_RESOLUTION_32).left.length, engine.registerAudioBuffers(16).right.length, engine.registerAudioBuffers(64).average.length].join(' ')", "32 16 64");
    add("AudioBuffers.left", "engine.registerAudioBuffers(16).left instanceof Float32Array", "true");
    add("AudioBuffers.right", "engine.registerAudioBuffers(16).right instanceof Float32Array", "true");
    add("AudioBuffers.average", "engine.registerAudioBuffers(16).average instanceof Float32Array", "true");
    add("IEngine.registerAsset", "(() => { const h = engine.registerAsset('models/x.json'); return typeof h + ' ' + (h !== null); })()", "object true");
    add("IEngine.screenResolution", "engine.screenResolution.x + ' ' + engine.screenResolution.y", "1920 1080");
    add("IEngine.canvasSize", "engine.canvasSize.x + ' ' + engine.canvasSize.y", "1920 1080");
    add("IEngine.userProperties", "typeof engine.userProperties", "object");
    add("IEngine.timeOfDay", "typeof engine.timeOfDay", "number");
    add("IEngine.frametime", "engine.frametime > 0", "true");
    add("IEngine.runtime", "typeof engine.runtime", "number");
    // ---- localStorage ----
    add("ILocalStorage.LOCATION_GLOBAL", "typeof localStorage.LOCATION_GLOBAL", "string");
    add("ILocalStorage.LOCATION_SCREEN", "typeof localStorage.LOCATION_SCREEN", "string");
    add("ILocalStorage.set", "(() => { localStorage.set('a', {x: [1, 2]}); return JSON.stringify(localStorage.get('a')); })()", "{\"x\":[1,2]}");
    add("ILocalStorage.get", "(() => { localStorage.set('n', 5, localStorage.LOCATION_GLOBAL); return [localStorage.get('n', localStorage.LOCATION_GLOBAL), localStorage.get('n'), localStorage.get('missing')].join('|'); })()", "5||");
    add("ILocalStorage.delete", "(() => { localStorage.set('k', 1); return [localStorage.delete('k'), localStorage.delete('k'), localStorage.get('k')].join('|'); })()", "true|false|");
    add("ILocalStorage.clear", "(() => { localStorage.set('a', 1); localStorage.set('b', 2); localStorage.set('c', 3, localStorage.LOCATION_GLOBAL); localStorage.clear(); return [localStorage.get('a'), localStorage.get('b'), localStorage.get('c', localStorage.LOCATION_GLOBAL)].join('|'); })()", "||3");
    add("IConsole.log", "(() => { console.log('hello', 1); return typeof console.log; })()", "function");
    add("IConsole.error", "(() => { console.error('bad'); return typeof console.error; })()", "function");
    add("AnimationEvent.name", "new AnimationEvent('hit', 3).name", "hit");
    add("AnimationEvent.frame", "new AnimationEvent('hit', 3).frame", "3");
    // ---- the math modules ----
    add("WEMath.deg2rad", "[WEMath.deg2rad * 180, WEMath.deg2rad(180)].map(rnd).join(' ')", "3.1416 3.1416");
    add("WEMath.rad2deg", "[WEMath.rad2deg * Math.PI, WEMath.rad2deg(Math.PI)].map(rnd).join(' ')", "180 180");
    add("WEMath.smoothStep", "[WEMath.smoothStep(0, 1, 0.5), WEMath.smoothStep(0, 10, 20), WEMath.smoothStep(0, 10, -5)].join(' ')", "0.5 1 0");
    add("WEMath.mix", "WEMath.mix(0, 10, 0.25)", "2.5");
    add("WEColor.rgb2hsv", "WEColor.rgb2hsv(new Vec3(1, 0, 0))", "0 1 1");
    add("WEColor.hsv2rgb", "WEColor.hsv2rgb(new Vec3(0, 1, 1))", "1 0 0");
    add("WEColor.normalizeColor", "WEColor.normalizeColor(new Vec3(255, 51, 0))", "1 0.2 0");
    add("WEColor.expandColor", "WEColor.expandColor(new Vec3(1, 0.2, 0))", "255 51 0");
    add("WEVector.angleVector2", "WEVector.angleVector2(90)", "0 1");
    add("WEVector.vectorAngle2", "WEVector.vectorAngle2(new Vec2(0, 1))", "90");
    return c;
}


// ---- the scene entries: a real Scene behind the real SceneScripts ----

// The world the scene cases run in. Draw order: a, b (a child of a), p (particles), out (the text the case writes its answer
// to), label (a text object with every ITextLayer property), pup (a puppet with two animations and two bones). Not drawn: music
// (a sound object) and driver (the group whose `visible` property carries the case's script).
struct World {
    scenevkfix::Item item;
    QString error;
    std::optional<Scene> scene;
    Vfs vfs{Vfs::forItem("/nonexistent", {})};
    PropertyStore props;
    ScriptFlags flags;
    std::unique_ptr<SceneScripts> scripts;

    // `video`: a layer "vid" shows a video texture (drawn after pup). `custom`: the driver's whole script, instead of the one that
    // writes the expression to `out`.
    bool build(const QString &expr, bool video = false, const QString &custom = {})
    {
        using namespace scenevkfix;
        const TexSpec tex = simpleRgba(8, 8);
        if (video) {
            const std::vector<uint8_t> clip = videofix::clipTex();
            item.file(QStringLiteral("materials/vid.tex"), QByteArray(reinterpret_cast<const char *>(clip.data()), static_cast<qsizetype>(clip.size())));
            item.image(QStringLiteral("vid"));
        }
        item.tex(QStringLiteral("a"), tex);
        item.image(QStringLiteral("a"));
        item.tex(QStringLiteral("b"), tex);
        item.image(QStringLiteral("b"));
        item.tex(QStringLiteral("red"), tex);
        item.file(QStringLiteral("effects/invert/effect.json"), R"({"passes":[{"material":"materials/effects/invert.json"}]})");
        item.file(QStringLiteral("materials/effects/invert.json"), R"({"passes":[{"shader":"effects/invert","blending":"normal","textures":[null]}]})");
        item.file(QStringLiteral("shaders/genericparticle.vert"), "void main() {}\n");
        item.file(QStringLiteral("shaders/genericparticle.frag"), "void main() {}\n");
        item.tex(QStringLiteral("particle/dot"), tex);
        item.file(QStringLiteral("materials/presets/dot.json"), R"({"passes":[{"blending":"additive","shader":"genericparticle","textures":["particle/dot"]}]})");
        item.file(QStringLiteral("particles/p.json"), R"({"material":"materials/presets/dot.json","maxcount":10,"emitter":[{"name":"sphererandom","rate":5,"distancemax":10}]})");
        item.file(QStringLiteral("sounds/tone.mp3"), "not really an mp3");
        const auto mdl = puppetfix::makeMdl({{"root", -1, 0, 0}, {"child", 0, 5, 0}},
                                            {{7, "slide", "loop", 30.0f, 2, {{{puppetfix::frame(20, 0), puppetfix::frame(20, 0)}}}},
                                             {9, "spin", "loop", 15.0f, 30, {{{puppetfix::frame(0, 0), puppetfix::frame(0, 5)}}}}});
        item.file(QStringLiteral("models/pup.mdl"), QByteArray(reinterpret_cast<const char *>(mdl.data()), static_cast<qsizetype>(mdl.size())));
        item.file(QStringLiteral("models/pup.json"), R"({"material":"materials/pup.json","puppet":"models/pup.mdl"})");
        item.file(QStringLiteral("materials/pup.json"), R"({"passes":[{"shader":"puppetshader","blending":"translucent","textures":["red"]}]})");

        const QString source = !custom.isEmpty() ? QString::fromUtf8(kFormat) + custom
            : QString::fromUtf8(kFormat) + QStringLiteral("export function update(v) { thisScene.getLayer('out').text = fmt(") + expr + QStringLiteral("); return v; }");
        const QJsonObject driver{{"id", 7}, {"name", "driver"}, {"visible", QJsonObject{{"script", source}, {"value", true}}}};
        const QByteArray objects =
            R"({"id":1,"name":"a","image":"models/a.json","origin":"960 540 0","size":"400 300","solid":true,"effects":[{"file":"effects/invert/effect.json","name":"invert","id":9,"passes":[{}]}],)"
            R"("alpha":{"value":1,"animation":{"c0":[{"frame":0,"value":0},{"frame":30,"value":1}],"options":{"fps":30,"length":30,"mode":"single","name":"fade","startpaused":true}}}},)"
            R"({"id":2,"name":"b","image":"models/b.json","parent":1,"origin":"100 0 0","size":"50 50"},)"
            R"({"id":3,"name":"p","particle":"particles/p.json","origin":"100 100 0"},)"
            R"({"id":4,"name":"out","text":"","pointsize":20,"font":"fonts/x.ttf","origin":"10 10 0"},)"
            R"({"id":8,"name":"label","text":"hello","pointsize":24,"font":"fonts/x.ttf","color":"1 0 0","alpha":0.5,"horizontalalign":"left","verticalalign":"top","padding":4,"maxwidth":300,"origin":"10 50 0"},)"
            R"({"id":5,"name":"pup","image":"models/pup.json","origin":"500 500 0","size":"40 40","animationlayers":[{"animation":7}]},)"
            R"({"id":6,"name":"music","sound":["sounds/tone.mp3"],"playbackmode":"loop","volume":1.0},)"
            + (video ? QByteArray(R"({"id":10,"name":"vid","image":"models/vid.json","origin":"100 100 0","size":"64 48"},)") : QByteArray())
            + QJsonDocument(driver).toJson(QJsonDocument::Compact);
        item.file(QStringLiteral("scene.json"), scenevkfix::sceneJson(objects));
        vfs = Vfs::forItem(item.root().toStdString(), {});
        std::string err;
        scene = loadScene(vfs, "scene.json", &err);
        if (!scene) {
            error = QString::fromStdString(err);
            return false;
        }
        scripts = std::make_unique<SceneScripts>(*scene, vfs, props, flags);
        scripts->start();
        if (scripts->failed() != 0) {
            error = QString::fromStdString(scripts->failures().front());
            return false;
        }
        return true;
    }

    // One frame; the text object `out` holds what the case's expression evaluated to.
    QString run()
    {
        ScriptFrame f;
        f.canvasWidth = 1920;
        f.canvasHeight = 1080;
        scripts->update(f);
        for (const ImageLayer &l : scene->layers) {
            if (l.text && scene->nodes[l.node].name == "out") {
                return QString::fromStdString(l.text->value);
            }
        }
        return {};
    }
    QString log() const
    {
        QStringList lines;
        for (const std::string &m : scripts->messages())
            lines << QString::fromStdString(m);
        return lines.join('\n');
    }
};

QString evaluateInScene(const QString &expr, QString *error = nullptr)
{
    World w;
    if (!w.build(expr)) {
        if (error)
            *error = w.error;
        return {};
    }
    const QString out = w.run();
    if (error)
        *error = w.log();
    return out;
}

QList<Case> sceneCases()
{
    QList<Case> c;
    const auto add = [&](const char *id, const char *expr, const char *expected) { c.append({QString::fromLatin1(id), QString::fromLatin1(expr), QString::fromLatin1(expected)}); };
    const QString A = QStringLiteral("thisScene.getLayer('a')");
    // ---- IScene ----
    add("IScene.getLayer", "[thisScene.getLayer('a').name, thisScene.getLayer('missing') === null, thisScene.getLayer(0).name, thisScene.getLayer(99) === null].join(' ')", "a true a true");
    add("IScene.getLayerByID",
        "[thisScene.getLayerByID('1').name, thisScene.getLayerByID(2).name, thisScene.getLayerByID('3').name, thisScene.getLayerByID('99') === null, thisScene.getLayerByID('a') === null, "
        "thisScene.getLayerByID('1') === thisScene.getLayer('a'), thisScene.getLayerByID() === null, (thisScene.destroyLayer('b'), thisScene.getLayerByID('2') === null)].join(' ')",
        "a b p true true true true true");
    add("IScene.getAnimation", "[thisScene.getAnimation('fade').name, thisScene.getAnimation('nope'), thisScene.getAnimation()].map(String).join(' ')", "fade null null");
    add("IScene.getLayerCount", "thisScene.getLayerCount()", "6");
    add("IScene.enumerateLayers", "thisScene.enumerateLayers().map((l) => l.name).join(',')", "a,b,p,out,label,pup");
    add("IScene.destroyLayer", "[thisScene.destroyLayer('b'), thisScene.getLayerCount(), thisScene.destroyLayer('b'), thisScene.getLayer('b') === null].join(' ')", "true 5 false true");
    add("IScene.createLayer", "[thisScene.createLayer('models/a.json').name, thisScene.createLayer({text: 'hi', pointsize: 20}) !== null, thisScene.createLayer(engine.registerAsset('models/b.json')) !== null, thisScene.getLayerCount()].join('|')", "script layer|true|true|9");
    add("IScene.createLayer.description",
        "(() => { const d = thisScene.createLayer({image: 'models/a.json', name: 'dot', origin: new Vec3(5, 6, 0), size: new Vec2(3, 4), scale: new Vec2(2, 1), alpha: 0.5, color: new Vec3(1, 0, 0), visible: false}); "
        "return [d.name, d.origin, d.size, d.scale, d.alpha, d.color, d.visible].join('|'); })()",
        "dot|5 6 0|3 4|2 1 1|0.5|1 0 0|false");
    add("IScene.getInitialLayerConfig.image", "thisScene.getInitialLayerConfig('a').image", "models/a.json");
    add("IScene.sortLayer", "[thisScene.sortLayer('a', 5), thisScene.getLayerIndex('a'), thisScene.enumerateLayers().map((l) => l.name).join(',')].join('|')", "true|5|b,p,out,label,pup,a");
    add("IScene.getLayerIndex", "[thisScene.getLayerIndex('p'), thisScene.getLayerIndex(thisScene.getLayer('pup'))].join(' ')", "2 5");
    add("IScene.getInitialLayerConfig", "(() => { thisScene.getLayer('a').origin = new Vec3(1, 2, 3); const c = thisScene.getInitialLayerConfig('a'); return [c.name, c.origin, c.solid].join(' '); })()", "a 960 540 0 true");
    add("IScene.getCameraTransforms", "(() => { const t = thisScene.getCameraTransforms(); return [t.eye, t.center, t.up, t.zoom].join('|'); })()", "0 0 0|0 0 -1|0 1 0|1");
    add("IScene.setCameraTransforms", "(() => { const t = thisScene.getCameraTransforms(); t.eye = new Vec3(1, 2, 3); t.zoom = 2; thisScene.setCameraTransforms(t); const u = thisScene.getCameraTransforms(); return u.eye + '|' + u.zoom; })()", "1 2 3|2");
    add("CameraTransforms.eye", "thisScene.getCameraTransforms().eye instanceof Vec3", "true");
    add("CameraTransforms.center", "thisScene.getCameraTransforms().center instanceof Vec3", "true");
    add("CameraTransforms.up", "thisScene.getCameraTransforms().up instanceof Vec3", "true");
    add("CameraTransforms.zoom", "thisScene.getCameraTransforms().zoom", "1");
    add("IScene.createModelData", "typeof thisScene.createModelData({})", "object");
    add("IScene.destroyModelData", "thisScene.destroyModelData(thisScene.createModelData({}))", "undefined");
    add("IModelData.POSITION", "thisScene.createModelData({}).POSITION", "position");
    add("IModelData.NORMAL", "thisScene.createModelData({}).NORMAL", "normal");
    add("IModelData.UV", "thisScene.createModelData({}).UV", "uv");
    add("IModelData.TANGENT_SIGNED", "thisScene.createModelData({}).TANGENT_SIGNED", "tangentsigned");
    add("IModelData.COLOR", "thisScene.createModelData({}).COLOR", "color");
    add("IModelData.applyData", "(() => { thisScene.createModelData({}).applyData([]); return 'ok'; })()", "ok");
    add("IModelData.replaceData", "(() => { thisScene.createModelData({}).replaceData({}); return 'ok'; })()", "ok");
    add("IScene.bloom", "[thisScene.bloom, (thisScene.bloom = true, thisScene.bloom)].join(' ')", "false true");
    add("IScene.bloomstrength", "[thisScene.bloomstrength, (thisScene.bloomstrength = 3, thisScene.bloomstrength)].join(' ')", "2 3");
    add("IScene.bloomthreshold", "[thisScene.bloomthreshold, (thisScene.bloomthreshold = 0.5, thisScene.bloomthreshold)].map(rnd).join(' ')", "0.65 0.5");
    add("IScene.clearenabled", "[thisScene.clearenabled, (thisScene.clearenabled = false, thisScene.clearenabled)].join(' ')", "true false");
    add("IScene.clearcolor", "[fmt(thisScene.clearcolor), (thisScene.clearcolor = new Vec3(1, 0, 0), fmt(thisScene.clearcolor))].join('|')", "0.1 0.2 0.3|1 0 0");
    add("IScene.ambientcolor", "[fmt(thisScene.ambientcolor), (thisScene.ambientcolor = new Vec3(1, 1, 0), fmt(thisScene.ambientcolor))].join('|')", "0.2 0.2 0.2|1 1 0");
    add("IScene.skylightcolor", "[fmt(thisScene.skylightcolor), (thisScene.skylightcolor = new Vec3(0, 1, 0), fmt(thisScene.skylightcolor))].join('|')", "0.3 0.3 0.3|0 1 0");
    add("IScene.fov", "[thisScene.fov, (thisScene.fov = 60, thisScene.fov)].join(' ')", "50 60");
    add("IScene.nearz", "[rnd(thisScene.nearz), (thisScene.nearz = 1, thisScene.nearz)].join(' ')", "0.01 1");
    add("IScene.farz", "[thisScene.farz, (thisScene.farz = 500, thisScene.farz)].join(' ')", "10000 500");
    add("IScene.camerafade", "[thisScene.camerafade, (thisScene.camerafade = true, thisScene.camerafade)].join(' ')", "false true");
    add("IScene.camerashake", "[thisScene.camerashake, (thisScene.camerashake = true, thisScene.camerashake)].join(' ')", "false true");
    add("IScene.camerashakespeed", "[thisScene.camerashakespeed, (thisScene.camerashakespeed = 5, thisScene.camerashakespeed)].join(' ')", "3 5");
    add("IScene.camerashakeamplitude", "[thisScene.camerashakeamplitude, (thisScene.camerashakeamplitude = 2, thisScene.camerashakeamplitude)].join(' ')", "0.5 2");
    add("IScene.camerashakeroughness", "[thisScene.camerashakeroughness, (thisScene.camerashakeroughness = 0.25, thisScene.camerashakeroughness)].join(' ')", "1 0.25");
    add("IScene.cameraparallax", "[thisScene.cameraparallax, (thisScene.cameraparallax = true, thisScene.cameraparallax)].join(' ')", "false true");
    add("IScene.cameraparallaxamount", "[thisScene.cameraparallaxamount, (thisScene.cameraparallaxamount = 2, thisScene.cameraparallaxamount)].join(' ')", "1 2");
    add("IScene.cameraparallaxdelay", "[thisScene.cameraparallaxdelay, (thisScene.cameraparallaxdelay = 1, thisScene.cameraparallaxdelay)].join(' ')", "0 1");
    add("IScene.cameraparallaxmouseinfluence", "[thisScene.cameraparallaxmouseinfluence, (thisScene.cameraparallaxmouseinfluence = 0.5, thisScene.cameraparallaxmouseinfluence)].join(' ')", "1 0.5");
    // ---- ILayer ----
    add("ILayer.origin", "fmt(thisScene.getLayer('a').origin)", "960 540 0");
    add("ILayer.angles", "fmt(thisScene.getLayer('a').angles)", "0 0 0");
    add("ILayer.scale", "fmt(thisScene.getLayer('a').scale)", "1 1 1");
    add("ILayer.name", "thisScene.getLayer('a').name", "a");
    add("ILayer.visible", "[thisScene.getLayer('a').visible, (thisScene.getLayer('a').visible = false, thisScene.getLayer('a').visible)].join(' ')", "true false");
    add("ILayer.parallaxDepth", "fmt(thisScene.getLayer('a').parallaxDepth)", "1 1");
    add("ILayer.getTransformMatrix", "[thisScene.getLayer('a').getTransformMatrix().translation(), thisScene.getLayer('b').getTransformMatrix().translation()].map(fmt).join('|')", "960 540 0|1060 540 0");
    add("ILayer.lookAt",
        "(() => { const l = thisScene.getLayer('a'); l.lookAt(new Vec3(960, 543, 4)); const tilted = fmt(Mat4.fromEuler(l.angles).forward()); l.lookAt(new Vec3(1960, 540, 0)); return tilted + '|' + fmt(l.angles); })()",
        "0 0.6 0.8|0 90 0");
    add("ILayer.lookAtYaw",
        "(() => { const l = thisScene.getLayer('a'); l.lookAtYaw(new Vec3(960, 543, 4)); const flat = fmt(Mat4.fromEuler(l.angles).forward()); l.lookAtYaw(new Vec3(1960, 1540, 0)); return flat + '|' + fmt(l.angles); })()",
        "0 0 1|0 90 0");
    add("ILayer.getAnimation", "[thisScene.getLayer('a').getAnimation('fade').name, thisScene.getLayer('a').getAnimation() !== null, thisScene.getLayer('a').getAnimation('x'), thisScene.getLayer('b').getAnimation()].map(String).join(' ')", "fade true null null");
    add("IThisPropertyObject.getAnimation", "[thisObject.getAnimation(), thisObject.getAnimation('fade'), thisObject.name].map(String).join(' ')", "null null driver"); // driver's visibility has no keyframes
    {
        // keyframes on a's alpha, started paused; the case's update runs at time 0
        const char *F = "thisScene.getLayer('a').getAnimation('fade')";
        const auto anim = [&](const char *id, const std::string &body, const char *expected) {
            c.append({QString::fromLatin1(id), QString::fromStdString("(() => { const a = " + std::string(F) + "; " + body + " })()"), QString::fromLatin1(expected)});
        };
        anim("IAnimation.fps", "return a.fps;", "30");
        anim("IAnimation.frameCount", "return a.frameCount;", "30");
        anim("IAnimation.duration", "return a.duration;", "1");
        anim("IAnimation.name", "return a.name;", "fade");
        anim("IAnimation.rate", "const r = a.rate; a.rate = 2; return [r, a.rate].join(' ');", "1 2");
        anim("IAnimation.play", "const was = a.isPlaying(); a.play(); return [was, a.isPlaying()].join(' ');", "false true");
        anim("IAnimation.isPlaying", "a.play(); a.rate = 0; const still = a.isPlaying(); a.rate = 1; return [still, a.isPlaying()].join(' ');", "false true");
        anim("IAnimation.pause", "a.play(); a.setFrame(12); a.pause(); return [a.isPlaying(), a.getFrame()].join(' ');", "false 12");
        anim("IAnimation.stop", "a.play(); a.setFrame(12); a.stop(); return [a.isPlaying(), a.getFrame()].join(' ');", "false 0");
        anim("IAnimation.getFrame", "return a.getFrame();", "0");
        anim("IAnimation.setFrame", "a.setFrame(20); const f = a.getFrame(); a.setFrame(99); return [f, a.getFrame()].join(' ');", "20 30");
    }
    add("ILayer.rotateObjectSpace", "(() => { const l = thisScene.getLayer('a'); l.rotateObjectSpace(new Vec3(0, 0, 90)); return fmt(l.angles); })()", "0 0 90");
    add("ILayer.getParent", "[thisScene.getLayer('b').getParent().name, thisScene.getLayer('a').getParent()].join(' ')", "a ");
    add("ILayer.getChildren", "thisScene.getLayer('a').getChildren().map((l) => l.name).join(',')", "b");
    add("ILayer.setParent", "(() => { const b = thisScene.getLayer('b'); const r = []; b.setParent(null, true); r.push(b.getParent(), fmt(b.origin)); b.setParent(thisScene.getLayer('a')); r.push(b.getParent().name); return r.join('|'); })()", "|1060 540 0|a");
    add("ILayer.getAttachmentIndex", "thisScene.getLayer('a').getAttachmentIndex('x')", "-1");
    add("ILayer.getAttachmentMatrix", "fmt(thisScene.getLayer('a').getAttachmentMatrix('x'))", "1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1");
    add("ILayer.getAttachmentOrigin", "fmt(thisScene.getLayer('a').getAttachmentOrigin('x'))", "0 0 0");
    add("ILayer.getAttachmentAngles", "fmt(thisScene.getLayer('a').getAttachmentAngles('x'))", "0 0 0");
    // ---- IImageLayer and IEffectLayer ----
    for (const char *cls : {"IImageLayer", "IEffectLayer"}) {
        const QByteArray k(cls);
        const auto addBoth = [&](const char *member, const QString &expr, const char *expected) {
            c.append({QString::fromLatin1(k) + QLatin1Char('.') + QString::fromLatin1(member), expr, QString::fromLatin1(expected)});
        };
        addBoth("size", QStringLiteral("fmt(") + A + QStringLiteral(".size)"), "400 300");
        addBoth("perspective", QStringLiteral("[") + A + QStringLiteral(".perspective, (") + A + QStringLiteral(".perspective = true, ") + A + QStringLiteral(".perspective)].join(' ')"), "false true");
        addBoth("solid", QStringLiteral("[") + A + QStringLiteral(".solid, (") + A + QStringLiteral(".solid = false, ") + A + QStringLiteral(".solid)].join(' ')"), "true false");
        addBoth("getEffect", QStringLiteral("[") + A + QStringLiteral(".getEffect(0).name, ") + A + QStringLiteral(".getEffect('invert') !== null, ") + A + QStringLiteral(".getEffect(5) === null, ") + A + QStringLiteral(".getEffect('nope') === null].join(' ')"), "invert true true true");
        addBoth("getEffectCount", A + QStringLiteral(".getEffectCount()"), "1");
    }
    add("IEffectLayer.transformAttachmentToTexture", "fmt(thisScene.getLayer('a').transformAttachmentToTexture('a', 'x'))", "1 0 0 0 1 0 0 0 1");
    add("IImageLayer.alpha", "(() => { const l = thisScene.getLayer('a'); l.alpha = 0.25; return l.alpha; })()", "0.25");
    add("IImageLayer.color", "(() => { const l = thisScene.getLayer('a'); l.color = new Vec3(1, 0, 0); return fmt(l.color); })()", "1 0 0");
    add("IImageLayer.alignment", "(() => { const l = thisScene.getLayer('a'); const was = l.alignment; l.alignment = 'topleft'; return [was, l.alignment].join(' '); })()", "center topleft");
    add("ICamera.fov", "(() => { const l = thisScene.getLayer('a'); l.fov = 70; return l.fov; })()", "70");
    add("ICamera.zoom", "(() => { const l = thisScene.getLayer('a'); l.zoom = 2; return l.zoom; })()", "2");
    add("IObject.getAnimation", "[thisScene.getLayer('a').getAnimation('fade').name, thisScene.getLayer('b').getAnimation('fade')].map(String).join(' ')", "fade null");
    add("IImageLayer.getTextureAnimation", "typeof thisScene.getLayer('a').getTextureAnimation()", "object");
    // the texture animation object (it keeps its own state: the engine's sprite sheets run on its clock)
    const QString TA = QStringLiteral("thisScene.getLayer('a').getTextureAnimation()");
    c.append({QStringLiteral("ITextureAnimation.frameCount"), TA + QStringLiteral(".frameCount"), QStringLiteral("1")});
    c.append({QStringLiteral("ITextureAnimation.duration"), TA + QStringLiteral(".duration"), QStringLiteral("0")});
    c.append({QStringLiteral("ITextureAnimation.rate"), QStringLiteral("(() => { const t = ") + TA + QStringLiteral("; const a = t.rate; t.rate = 2; return a + ' ' + t.rate; })()"), QStringLiteral("1 2")});
    c.append({QStringLiteral("ITextureAnimation.play"), QStringLiteral("(() => { const t = ") + TA + QStringLiteral("; t.stop(); t.play(); return t.isPlaying(); })()"), QStringLiteral("true")});
    c.append({QStringLiteral("ITextureAnimation.stop"), QStringLiteral("(() => { const t = ") + TA + QStringLiteral("; t.setFrame(3); t.stop(); return t.isPlaying() + ' ' + t.getFrame(); })()"), QStringLiteral("false 0")});
    c.append({QStringLiteral("ITextureAnimation.pause"), QStringLiteral("(() => { const t = ") + TA + QStringLiteral("; t.pause(); return t.isPlaying(); })()"), QStringLiteral("false")});
    c.append({QStringLiteral("ITextureAnimation.isPlaying"), TA + QStringLiteral(".isPlaying()"), QStringLiteral("true")});
    c.append({QStringLiteral("ITextureAnimation.getFrame"), TA + QStringLiteral(".getFrame()"), QStringLiteral("0")});
    c.append({QStringLiteral("ITextureAnimation.setFrame"), QStringLiteral("(() => { const t = ") + TA + QStringLiteral("; t.setFrame(4); return t.getFrame(); })()"), QStringLiteral("4")});
    c.append({QStringLiteral("ITextureAnimation.join"), QStringLiteral("(() => { ") + TA + QStringLiteral(".join(); return 'ok'; })()"), QStringLiteral("ok")});
    add("IImageLayer.getVideoTexture", "thisScene.getLayer('a').getVideoTexture()", "null");
    add("IImageLayer.getAnimationLayerCount", "[thisScene.getLayer('pup').getAnimationLayerCount(), thisScene.getLayer('a').getAnimationLayerCount()].join(' ')", "1 0");
    add("IImageLayer.getAnimationLayer", "[thisScene.getLayer('pup').getAnimationLayer(0).name, thisScene.getLayer('pup').getAnimationLayer('slide') !== null, thisScene.getLayer('pup').getAnimationLayer('x') === null].join(' ')", "slide true true");
    add("IImageLayer.createAnimationLayer", "(() => { const p = thisScene.getLayer('pup'); const l = p.createAnimationLayer('spin'); return [l.name, p.getAnimationLayerCount(), p.createAnimationLayer('nothing') === null].join(' '); })()", "spin 2 true");
    add("IImageLayer.destroyAnimationLayer", "(() => { const p = thisScene.getLayer('pup'); const l = p.createAnimationLayer('spin'); return [p.destroyAnimationLayer(l), p.getAnimationLayerCount()].join(' '); })()", "true 1");
    add("IImageLayer.playSingleAnimation", "(() => { const p = thisScene.getLayer('pup'); const l = p.playSingleAnimation('spin', {rate: 2}); return [l.name, l.rate].join(' '); })()", "spin 2");
    add("IImageLayer.getBoneCount", "thisScene.getLayer('pup').getBoneCount()", "2");
    add("IImageLayer.getBoneIndex", "[thisScene.getLayer('pup').getBoneIndex('child'), thisScene.getLayer('pup').getBoneIndex('nope')].join(' ')", "1 -1");
    add("IImageLayer.getBoneParentIndex", "[thisScene.getLayer('pup').getBoneParentIndex('child'), thisScene.getLayer('pup').getBoneParentIndex(0)].join(' ')", "0 -1");
    add("IImageLayer.getBoneTransform", "fmt(thisScene.getLayer('pup').getBoneTransform(0))", "1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1");
    add("IImageLayer.setBoneTransform", "thisScene.getLayer('pup').setBoneTransform(0, new Mat4())", "undefined");
    add("IImageLayer.getLocalBoneTransform", "fmt(thisScene.getLayer('pup').getLocalBoneTransform(1))", "1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1");
    add("IImageLayer.setLocalBoneTransform", "thisScene.getLayer('pup').setLocalBoneTransform(1, new Mat4())", "undefined");
    add("IImageLayer.getLocalBoneAngles", "fmt(thisScene.getLayer('pup').getLocalBoneAngles(1))", "0 0 0");
    add("IImageLayer.setLocalBoneAngles", "thisScene.getLayer('pup').setLocalBoneAngles(1, new Vec3(1, 2, 3))", "undefined");
    add("IImageLayer.getLocalBoneOrigin", "fmt(thisScene.getLayer('pup').getLocalBoneOrigin(1))", "0 0 0");
    add("IImageLayer.setLocalBoneOrigin", "thisScene.getLayer('pup').setLocalBoneOrigin(1, new Vec3(1, 2, 3))", "undefined");
    add("IImageLayer.applyBonePhysicsImpulse", "thisScene.getLayer('pup').applyBonePhysicsImpulse(0, new Vec3(1, 0, 0), new Vec3(0, 0, 0))", "undefined");
    add("IImageLayer.resetBonePhysicsSimulation", "thisScene.getLayer('pup').resetBonePhysicsSimulation()", "undefined");
    add("IImageLayer.getBlendShapeIndex", "thisScene.getLayer('pup').getBlendShapeIndex('smile')", "-1");
    add("IImageLayer.getBlendShapeWeight", "thisScene.getLayer('pup').getBlendShapeWeight(0)", "0");
    add("IImageLayer.setBlendShapeWeight", "thisScene.getLayer('pup').setBlendShapeWeight(0, 1)", "undefined");
    // ---- IAnimationLayer (the first animation layer of the puppet: "slide", 30 fps, 2 frames) ----
    const QString L = QStringLiteral("thisScene.getLayer('pup').getAnimationLayer(0)");
    const auto addL = [&](const char *member, const QString &expr, const char *expected) { c.append({QStringLiteral("IAnimationLayer.") + QString::fromLatin1(member), expr, QString::fromLatin1(expected)}); };
    addL("fps", L + QStringLiteral(".fps"), "30");
    addL("frameCount", L + QStringLiteral(".frameCount"), "2");
    addL("duration", QStringLiteral("rnd(") + L + QStringLiteral(".duration)"), "0.0667");
    addL("name", L + QStringLiteral(".name"), "slide");
    addL("rate", QStringLiteral("[") + L + QStringLiteral(".rate, (") + L + QStringLiteral(".rate = 2, ") + L + QStringLiteral(".rate)].join(' ')"), "1 2");
    addL("blend", QStringLiteral("[") + L + QStringLiteral(".blend, (") + L + QStringLiteral(".blend = 0.5, ") + L + QStringLiteral(".blend)].join(' ')"), "1 0.5");
    addL("visible", QStringLiteral("[") + L + QStringLiteral(".visible, (") + L + QStringLiteral(".visible = false, ") + L + QStringLiteral(".visible)].join(' ')"), "true false");
    addL("play", QStringLiteral("(() => { const l = ") + L + QStringLiteral("; l.stop(); l.play(); return l.isPlaying(); })()"), "true");
    addL("stop", QStringLiteral("(() => { const l = ") + L + QStringLiteral("; l.stop(); return l.isPlaying(); })()"), "false");
    addL("pause", QStringLiteral("(() => { const l = ") + L + QStringLiteral("; l.pause(); const a = l.isPlaying(); l.play(); return a + ' ' + l.isPlaying(); })()"), "false true");
    addL("isPlaying", L + QStringLiteral(".isPlaying()"), "true");
    addL("getFrame", L + QStringLiteral(".getFrame()"), "0");
    addL("setFrame", QStringLiteral("(() => { const l = ") + L + QStringLiteral("; l.setFrame(1); return l.getFrame(); })()"), "1");
    addL("addEndedCallback", QStringLiteral("(() => { ") + L + QStringLiteral(".addEndedCallback(() => {}); return 'ok'; })()"), "ok");
    // ---- IEffect and IMaterial ----
    const QString E = A + QStringLiteral(".getEffect(0)");
    const auto addE = [&](const char *member, const QString &expr, const char *expected) { c.append({QStringLiteral("IEffect.") + QString::fromLatin1(member), expr, QString::fromLatin1(expected)}); };
    addE("visible", QStringLiteral("[") + E + QStringLiteral(".visible, (") + E + QStringLiteral(".visible = false, ") + E + QStringLiteral(".visible)].join(' ')"), "true false");
    addE("name", E + QStringLiteral(".name"), "invert");
    addE("getMaterialCount", E + QStringLiteral(".getMaterialCount()"), "1");
    addE("getMaterial", QStringLiteral("(() => { const m = ") + E + QStringLiteral(".getMaterial(0); m.amount = 0.25; return [typeof m, m.amount, ") + E + QStringLiteral(".getMaterial(3) === null].join(' '); })()"), "object 0.25 true");
    addE("setMaterialProperty", QStringLiteral("(() => { ") + E + QStringLiteral(".setMaterialProperty('amount', 0.5); return ") + E + QStringLiteral(".getMaterial(0).amount; })()"), "0.5");
    addE("executeMaterialFunction", QStringLiteral("(() => { ") + E + QStringLiteral(".executeMaterialFunction('reset'); return 'ok'; })()"), "ok");
    // ---- ISoundLayer ----
    const QString S = QStringLiteral("thisScene.getLayer('music')");
    const auto addS = [&](const char *member, const QString &expr, const char *expected) { c.append({QStringLiteral("ISoundLayer.") + QString::fromLatin1(member), expr, QString::fromLatin1(expected)}); };
    addS("volume", QStringLiteral("[") + S + QStringLiteral(".volume, (") + S + QStringLiteral(".volume = 0.5, ") + S + QStringLiteral(".volume)].join(' ')"), "1 0.5");
    addS("isPlaying", S + QStringLiteral(".isPlaying()"), "true");
    addS("play", QStringLiteral("(() => { const s = ") + S + QStringLiteral("; s.stop(); s.play(); return s.isPlaying(); })()"), "true");
    addS("stop", QStringLiteral("(() => { const s = ") + S + QStringLiteral("; s.stop(); return s.isPlaying(); })()"), "false");
    addS("pause", QStringLiteral("(() => { const s = ") + S + QStringLiteral("; s.pause(); return s.isPlaying(); })()"), "false");
    // ---- IParticleSystem and its instance ----
    const QString P = QStringLiteral("thisScene.getLayer('p')");
    const auto addP = [&](const char *cls, const char *member, const QString &expr, const char *expected) {
        c.append({QString::fromLatin1(cls) + QLatin1Char('.') + QString::fromLatin1(member), expr, QString::fromLatin1(expected)});
    };
    addP("IParticleSystem", "instance", QStringLiteral("typeof ") + P + QStringLiteral(".instance"), "object");
    addP("IParticleSystem", "play", QStringLiteral("(() => { const p = ") + P + QStringLiteral("; p.stop(); p.play(); return p.isPlaying(); })()"), "true");
    addP("IParticleSystem", "pause", QStringLiteral("(() => { const p = ") + P + QStringLiteral("; p.pause(); return p.isPlaying(); })()"), "false");
    addP("IParticleSystem", "stop", QStringLiteral("(() => { const p = ") + P + QStringLiteral("; p.stop(); return p.isPlaying(); })()"), "false");
    addP("IParticleSystem", "isPlaying", P + QStringLiteral(".isPlaying()"), "true");
    addP("IParticleSystem", "emitParticles", QStringLiteral("(() => { ") + P + QStringLiteral(".emitParticles(5); ") + P + QStringLiteral(".emitParticles(); return 'ok'; })()"), "ok");
    for (const char *n : {"alpha", "size", "count", "speed", "lifetime", "rate"}) {
        addP("IParticleSystemInstance", n, QStringLiteral("(() => { const i = ") + P + QStringLiteral(".instance; const a = i.") + QString::fromLatin1(n) + QStringLiteral("; i.") + QString::fromLatin1(n)
                 + QStringLiteral(" = 3; return a + ' ' + i.") + QString::fromLatin1(n) + QStringLiteral("; })()"), "1 3");
    }
    addP("IParticleSystemInstance", "colorn", QStringLiteral("(() => { const i = ") + P + QStringLiteral(".instance; const a = fmt(i.colorn); i.colorn = new Vec3(1, 0, 0); return a + '|' + fmt(i.colorn); })()"), "1 1 1|1 0 0");
    for (int n = 0; n < 8; ++n) {
        const QString m = QStringLiteral("controlpoint%1").arg(n);
        c.append({QStringLiteral("IParticleSystemInstance.") + m,
                  QStringLiteral("(() => { const i = ") + P + QStringLiteral(".instance; const a = fmt(i.") + m + QStringLiteral("); i.") + m + QStringLiteral(" = new Vec3(1, 2, %1); return a + '|' + fmt(i.").arg(n) + m + QStringLiteral("); })()"),
                  QStringLiteral("0 0 0|1 2 %1").arg(n)});
    }
    // ---- ITextLayer (the object `label`) ----
    const QString T = QStringLiteral("thisScene.getLayer('label')");
    const auto addT = [&](const char *member, const QString &expr, const char *expected) { c.append({QStringLiteral("ITextLayer.") + QString::fromLatin1(member), expr, QString::fromLatin1(expected)}); };
    const auto roundTrip = [&](const char *member, const char *set, const char *first, const char *second, bool vec = false) {
        const QString m = QString::fromLatin1(member);
        const QString f = vec ? QStringLiteral("fmt(") + T + QStringLiteral(".") + m + QStringLiteral(")") : T + QStringLiteral(".") + m;
        addT(member, QStringLiteral("[") + f + QStringLiteral(", (") + T + QStringLiteral(".") + m + QStringLiteral(" = ") + QString::fromLatin1(set) + QStringLiteral(", ") + f + QStringLiteral(")].join('|')"),
             (QString::fromLatin1(first) + QLatin1Char('|') + QString::fromLatin1(second)).toLatin1().constData());
    };
    roundTrip("text", "'bye'", "hello", "bye");
    roundTrip("color", "new Vec3(0, 1, 0)", "1 0 0", "0 1 0", true);
    roundTrip("alpha", "1", "0.5", "1");
    roundTrip("opaquebackground", "true", "false", "true");
    roundTrip("backgroundcolor", "new Vec3(1, 1, 1)", "0 0 0", "1 1 1", true);
    roundTrip("pointsize", "40", "24", "40");
    roundTrip("font", "'fonts/y.ttf'", "fonts/x.ttf", "fonts/y.ttf");
    roundTrip("padding", "8", "4", "8");
    roundTrip("horizontalalign", "'right'", "left", "right");
    roundTrip("verticalalign", "'bottom'", "top", "bottom");
    roundTrip("anchor", "'topleft'", "center", "topleft");
    roundTrip("limitrows", "true", "false", "true");
    roundTrip("maxrows", "3", "0", "3");
    roundTrip("limitwidth", "false", "true", "false");
    roundTrip("maxwidth", "500", "300", "500");
    return c;
}

} // namespace

class TstSceneVkScriptApi : public QObject
{
    Q_OBJECT

private slots:
    void aVideoTextureIsThePlayerOfItsLayer()
    {
        // a layer without a video texture has none (as WE: null); the one with a video gets one object, steering its player
        World w;
        const QString script = QStringLiteral(
            "let n = 0, t = null, log = '';\n"
            "export function update(v) {\n"
            "  const out = thisScene.getLayer('out');\n"
            "  const vid = thisScene.getLayer('vid');\n"
            "  const vt = vid.getVideoTexture();\n"
            "  if (!t) {\n"
            "    t = vt;\n"
            "    log += [String(thisScene.getLayer('a').getVideoTexture()), vt ===vid.getVideoTexture(), vt.rate, vt.loop, vt.isPlaying(), vt.duration, vt.getCurrentTime()].join(' ') + '|';\n"
            "    vt.rate = 2; vt.loop = false; vt.pause();\n"
            "    log += [vt.rate, vt.loop, vt.isPlaying()].join(' ') + '|';\n"
            "    vt.play(); vt.setCurrentTime(1.5);\n"
            "    log += [vt.isPlaying(), vt.getCurrentTime()].join(' ') + '|';\n"
            "    vt.stop();\n"
            "    log += [vt.isPlaying(), vt.getCurrentTime()].join(' ') + '|';\n"
            "    vt.addEndedCallback(() => { ++n; });\n"
            "    vt.addEndedCallback(7);\n"
            "  }\n"
            "  out.text = log + n;\n"
            "  return v;\n"
            "}\n");
        QVERIFY2(w.build({}, true, script), qPrintable(w.error));
        QCOMPARE(w.run(), QStringLiteral("null true 1 true true 0 0|2 false false|true 1.5|false 0|0"));
        QVERIFY(w.scene->videos.count("vid"));
        const VideoControl &c = w.scene->videos.at("vid");
        QCOMPARE(c.rate, 2.0f);
        QVERIFY(!c.loop);
        QVERIFY(!c.playing);
        QVERIFY(c.seek && *c.seek == 0.0); // the stop: the renderer takes it
        // what the renderer reports reaches the script; each time the video went round the callback is called, in the next frame
        w.scene->videos["vid"].duration = 4.0;
        w.scene->videos["vid"].ended = 2;
        QCOMPARE(w.run(), QStringLiteral("null true 1 true true 0 0|2 false false|true 1.5|false 0|2"));
        w.scene->videos["vid"].ended = 3;
        QCOMPARE(w.run(), QStringLiteral("null true 1 true true 0 0|2 false false|true 1.5|false 0|3"));
    }

    void entries_data()
    {
        QTest::addColumn<QString>("expr");
        QTest::addColumn<QString>("expected");
        for (const Case &c : cases())
            QTest::newRow(qPrintable(c.id)) << c.expr << c.expected;
    }
    void entries()
    {
        QFETCH(QString, expr);
        QFETCH(QString, expected);
        QString error;
        const QString got = evaluate(expr, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(got, expected);
    }


    void timersFireOnTheClockAndCanBeCancelled()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        ScriptTarget t;
        t.layer = 0;
        t.name = "timers";
        t.source = R"JS(
            let n = 0, m = 0, stop;
            export function init() { engine.setTimeout(() => { n += 1; }, 100); stop = engine.setInterval(() => { m += 1; if (m === 3) stop(); }, 50); }
            export function update() { thisLayer.alpha = n * 10 + m; }
        )JS";
        QVERIFY2(host.add(t, ScriptValue{}, &error), error.c_str());
        for (const double time : {0.03, 0.06, 0.11, 0.2, 0.3, 0.5}) {
            ScriptFrame f;
            f.time = time;
            host.update(f);
        }
        QCOMPARE(scene.get(0, "alpha").number, 13.0); // the timeout once, the interval until it cancelled itself after three
    }

    void inputIsInSceneUnitsAndScreenPixels()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        ScriptTarget t;
        t.layer = 0;
        t.name = "input";
        t.source = "export function update() { thisLayer.text = [input.cursorWorldPosition, input.cursorScreenPosition, input.cursorLeftDown].join('|'); }";
        QVERIFY2(host.add(t, ScriptValue{}, &error), error.c_str());
        ScriptFrame f;
        f.canvasWidth = 1920;
        f.canvasHeight = 1080;
        f.screenWidth = 960;
        f.screenHeight = 540;
        f.cursorX = 960;
        f.cursorY = 540;
        host.update(f);
        QCOMPARE(QString::fromStdString(scene.get(0, "text").text), QStringLiteral("960 540 0|480 270|false"));
        f.cursorDown = true;
        host.update(f);
        QVERIFY(QString::fromStdString(scene.get(0, "text").text).endsWith(QStringLiteral("|true")));
    }

    void localStorageIsSharedByEveryScript()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        ScriptTarget a;
        a.layer = 0;
        a.name = "writer";
        a.source = "export function init() { localStorage.set('s', {n: 1}); }";
        ScriptTarget b;
        b.layer = 0;
        b.name = "reader";
        b.source = "export function update() { thisLayer.text = JSON.stringify(localStorage.get('s')); }";
        QVERIFY2(host.add(a, ScriptValue{}, &error), error.c_str());
        QVERIFY2(host.add(b, ScriptValue{}, &error), error.c_str());
        host.update(ScriptFrame{});
        QCOMPARE(QString::fromStdString(scene.get(0, "text").text), QStringLiteral("{\"n\":1}"));
    }

    void consoleWritesToTheHostLog()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        ScriptTarget t;
        t.layer = 0;
        t.name = "log";
        t.source = "export function init() { console.log('hello', 1); console.error('bad'); }";
        QVERIFY2(host.add(t, ScriptValue{}, &error), error.c_str());
        QStringList lines;
        for (const std::string &m : host.messages())
            lines << QString::fromStdString(m);
        QVERIFY2(lines.join('\n').contains(QStringLiteral("hello 1")), qPrintable(lines.join('\n')));
        QVERIFY(lines.join('\n').contains(QStringLiteral("bad")));
    }

    void aScriptWithAMediaHookSaysTheSceneUsesTheMedia()
    {
        FakeScene scene;
        std::string error;
        ScriptHost host(scene);
        ScriptTarget plain;
        plain.layer = 0;
        plain.name = "plain";
        plain.source = "export function update() {}";
        QVERIFY2(host.add(plain, ScriptValue{}, &error), error.c_str());
        QVERIFY(!host.wantsMedia());
        ScriptTarget listener = plain;
        listener.layer = 1;
        listener.name = "listener";
        listener.source = "export function mediaTimelineChanged(e) {}";
        QVERIFY2(host.add(listener, ScriptValue{}, &error), error.c_str());
        QVERIFY(host.wantsMedia());
    }

    void mediaEventsFollowThePlayer()
    {
        FakeScene scene;
        std::string error;
        ScriptHost host(scene);
        ScriptTarget t;
        t.layer = 0;
        t.name = "media";
        t.source = R"JS(
            const log = [];
            export function update() { thisLayer.text = log.join(','); }
            export function mediaStatusChanged(e) { log.push('status:' + e.enabled); }
            export function mediaPlaybackChanged(e) { log.push('playback:' + e.state); }
            export function mediaPropertiesChanged(e) { log.push('properties:' + [e.title, e.artist, e.albumTitle, e.albumArtist, e.subTitle, e.contentType, e.genres.join('+')].join('/')); }
            export function mediaThumbnailChanged(e) { log.push('thumbnail:' + e.hasThumbnail + ':' + (e.primaryColor instanceof Vec3) + ':' + e.textColor.x); }
            export function mediaTimelineChanged(e) { log.push('timeline:' + e.position + '/' + e.duration); }
        )JS";
        QVERIFY2(host.add(t, ScriptValue{}, &error), error.c_str());
        ScriptFrame f;
        f.time = 1.0;
        host.update(f);
        const auto text = [&] { return QString::fromStdString(scene.get(0, "text").text); };
        QCOMPARE(text(), QStringLiteral("status:false,playback:0")); // nothing plays: told once
        MediaState playing;
        playing.enabled = true;
        playing.playback = MediaPlayback::Playing;
        playing.title = "Song \"One\"";
        playing.artist = "Artist";
        playing.albumTitle = "Album";
        playing.genres = {"rock", "pop"};
        playing.position = 10;
        playing.duration = 200;
        host.setMedia(playing, 1.0);
        host.update(f);
        QCOMPARE(text(), QStringLiteral("status:false,playback:0,status:true,playback:1,properties:Song \"One\"/Artist/Album////rock+pop,timeline:10/200"));
        MediaColors colors;
        colors.text = {0, 0, 0};
        host.setMediaThumbnail(true, colors);
        f.time = 1.5;
        host.update(f);
        QVERIFY2(text().endsWith(QStringLiteral(",thumbnail:true:true:0")), qPrintable(text()));
        f.time = 2.2; // a second after the report: the timeline moves with the playback
        host.update(f);
        QVERIFY2(text().endsWith(QStringLiteral(",timeline:11.2/200")), qPrintable(text()));
        // paused: no more timeline ticks, the new state is told
        playing.playback = MediaPlayback::Paused;
        playing.position = 30;
        host.setMedia(playing, 2.2);
        f.time = 5.0;
        host.update(f);
        QVERIFY2(text().endsWith(QStringLiteral(",playback:2,timeline:30/200")), qPrintable(text()));
        // the player goes
        host.setMedia(MediaState{}, 5.0);
        f.time = 6.0;
        host.update(f);
        QVERIFY2(text().endsWith(QStringLiteral(",status:false,playback:0")), qPrintable(text()));
        // a script that starts now is told the present state, not the history
        ScriptTarget late = t;
        late.layer = 1;
        host.setMedia(playing, 6.0);
        QVERIFY2(host.add(late, ScriptValue{}, &error), error.c_str());
        f.time = 6.1;
        host.update(f);
        QCOMPARE(QString::fromStdString(scene.get(1, "text").text),
                 QStringLiteral("status:true,playback:2,properties:Song \"One\"/Artist/Album////rock+pop,thumbnail:true:true:0,timeline:30/200"));
        QCOMPARE(host.disabledCount(), size_t{0});
    }

    void eventFunctionsAreCalled()
    {
        FakeScene scene;
        std::string error;
        {
            ScriptHost host(scene);
            ScriptTarget t;
            t.layer = 0;
            t.name = "events";
            t.source = R"JS(
                const log = [];
                export function init() { log.push('init'); }
                export function update() { thisLayer.text = log.join(','); }
                export function destroy() { thisLayer.alpha = 0.125; }
                export function resizeScreen(size) { log.push('resize:' + size.x + 'x' + size.y + ':' + (size instanceof Vec2)); }
                export function applyUserProperties(p) { log.push('user:' + Object.keys(p).sort().join('+') + ':' + (p.tint instanceof Vec3) + ':' + engine.userProperties.speed); }
                export function applyGeneralSettings(g) { log.push('lang:' + g.language); }
                export function cursorEnter(e) { log.push('enter:' + e.worldPosition.x + ',' + e.localPosition.x + ':' + (e.worldPosition instanceof Vec3)); }
                export function cursorLeave(e) { log.push('leave'); }
                export function cursorMove(e) { log.push('move'); }
                export function cursorDown(e) { log.push('down'); }
                export function cursorUp(e) { log.push('up'); }
                export function cursorClick(e) { log.push('click'); }
                export function mediaStatusChanged(e) { log.push('status:' + e.enabled); }
                export function mediaPlaybackChanged(e) { log.push('playback:' + e.state); }
                export function mediaPropertiesChanged(e) { log.push('properties:' + [e.title, e.artist, e.contentType].join('/')); }
                export function mediaThumbnailChanged(e) { log.push('thumbnail:' + e.hasThumbnail + ':' + (e.primaryColor instanceof Vec3)); }
                export function mediaTimelineChanged(e) { log.push('timeline:' + e.position + '/' + e.duration); }
            )JS";
            QVERIFY2(host.add(t, ScriptValue{}, &error), error.c_str());
            std::map<std::string, ScriptValue> all{{"tint", ScriptValue::ofVector({1, 0, 0})}, {"speed", ScriptValue::ofNumber(3)}};
            host.applyUserProperties({{"tint", all["tint"]}}, all);
            host.applyGeneralSettings({{"language", ScriptValue::ofText("es-es")}});
            ScriptFrame f;
            f.screenWidth = 1920;
            f.screenHeight = 1080;
            f.cursorX = 300;
            f.cursorY = 300;
            host.update(f);   // the first frame sets the screen size silently
            f.screenWidth = 2560;
            f.screenHeight = 1440;
            f.cursorX = 20;
            f.cursorY = 50;
            host.update(f);   // a new size, and the cursor enters the square (20 from its left edge: -30 from its centre)
            f.cursorX = 60;
            host.update(f);   // moves inside
            f.cursorDown = true;
            host.update(f);   // presses
            f.cursorDown = false;
            host.update(f);   // releases: up, then click
            f.cursorX = 500;
            host.update(f);   // leaves
            QCOMPARE(QString::fromStdString(scene.get(0, "text").text),
                     QStringLiteral("init,status:false,playback:0,user:tint:true:3,lang:es-es,"
                                    "resize:2560x1440:true,enter:20,-30:true,move,move,down,up,click,leave"));
        }
        QCOMPARE(scene.get(0, "alpha").number, 0.125); // destroy ran when the host went away
    }

    void sceneEntries_data()
    {
        QTest::addColumn<QString>("expr");
        QTest::addColumn<QString>("expected");
        for (const Case &c : sceneCases())
            QTest::newRow(qPrintable(c.id)) << c.expr << c.expected;
    }
    void sceneEntries()
    {
        QFETCH(QString, expr);
        QFETCH(QString, expected);
        QString log;
        const QString got = evaluateInScene(expr, &log);
        QVERIFY2(!got.isEmpty() || expected.isEmpty(), qPrintable(log));
        QCOMPARE(got, expected);
    }

    void aScriptOnAnAnimationLayersVisibilityHasTheLayerAsItsObject()
    {
        // 3822822401 and 3808922316: `'addEndedCallback' in thisObject ? thisObject : thisObject.getAnimation()`, then play() and
        // setFrame(frameCount * percentage), and the visibility handed back as it was
        scenevkfix::Item item;
        item.tex(QStringLiteral("red"), scenevkfix::simpleRgba(8, 8));
        const auto mdl = puppetfix::makeMdl({{"root", -1, 0, 0}, {"child", 0, 5, 0}},
                                            {{7, "slide", "loop", 30.0f, 2, {{{puppetfix::frame(20, 0), puppetfix::frame(20, 0)}}}},
                                             {9, "spin", "loop", 15.0f, 30, {{{puppetfix::frame(0, 0), puppetfix::frame(0, 5)}}}}});
        item.file(QStringLiteral("models/pup.mdl"), QByteArray(reinterpret_cast<const char *>(mdl.data()), static_cast<qsizetype>(mdl.size())));
        item.file(QStringLiteral("models/pup.json"), R"({"material":"materials/pup.json","puppet":"models/pup.mdl"})");
        item.file(QStringLiteral("materials/pup.json"), R"({"passes":[{"shader":"puppetshader","blending":"translucent","textures":["red"]}]})");
        const QString script = QStringLiteral("export var scriptProperties = createScriptProperties().addSlider({name: 'percentage', value: 1, min: 0, max: 1}).finish();\n"
                                              "export function init(value) {\n  const ani = 'addEndedCallback' in thisObject ? thisObject : thisObject.getAnimation();\n"
                                              "  ani.play();\n  ani.setFrame(ani.frameCount * scriptProperties.percentage);\n  return value;\n}\n");
        const QJsonObject visible{{"script", script}, {"scriptproperties", QJsonObject{{"percentage", 0.5}}}, {"value", true}};
        const QJsonObject pup{{"id", 5}, {"name", "pup"}, {"image", "models/pup.json"}, {"origin", "500 500 0"}, {"size", "40 40"},
                              {"animationlayers", QJsonArray{QJsonObject{{"animation", 7}}, QJsonObject{{"animation", 9}, {"visible", visible}}}}};
        item.file(QStringLiteral("scene.json"), scenevkfix::sceneJson(QJsonDocument(pup).toJson(QJsonDocument::Compact)));
        const Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        std::string err;
        auto scene = loadScene(vfs, "scene.json", &err);
        QVERIFY2(scene.has_value(), err.c_str());
        QVERIFY2(scene->unsupported.empty(), scene->unsupported.empty() ? "" : scene->unsupported[0].c_str());
        QCOMPARE(scene->scripts.size(), size_t{1});
        QCOMPARE(scene->scripts[0].property, std::string("alayer.1.visible"));
        PropertyStore props;
        ScriptFlags flags;
        SceneScripts scripts(*scene, vfs, props, flags);
        scripts.start();
        QVERIFY2(scripts.failed() == 0, scripts.failures().empty() ? "" : scripts.failures().front().c_str());
        const AnimationLayer &spin = scene->layers[0].animationLayers[1];
        QCOMPARE(spin.frameOffset, 15.0f); // half of the 30 frames
        QVERIFY(resolve(spin.visible, props));
        QCOMPARE(scene->layers[0].animationLayers[0].frameOffset, 0.0f);
    }

    void keyframeAnimationsPlayOnTheirOwnClock()
    {
        // the media widgets: a property script whose property has keyframes started paused plays them from a hook (2915841260's
        // `thisObject.getAnimation().play()`); a linked track (`options.parent`) plays with it
        scenevkfix::Item item;
        item.tex(QStringLiteral("a"), scenevkfix::simpleRgba(8, 8));
        item.image(QStringLiteral("a"));
        const QString script = QStringLiteral("export function cursorClick() {}\nexport function init(value) { shared.fade = thisObject.getAnimation(); return value; }\n");
        const QJsonObject keys{{"c0", QJsonArray{QJsonObject{{"frame", 0}, {"value", 0}}, QJsonObject{{"frame", 30}, {"value", 1}}}},
                               {"options", QJsonObject{{"fps", 30}, {"length", 30}, {"mode", "single"}, {"startpaused", true}}}};
        const QJsonObject linked{{"c0", QJsonArray{QJsonObject{{"frame", 0}, {"value", 0}}, QJsonObject{{"frame", 30}, {"value", 300}}}},
                                 {"c1", QJsonArray{QJsonObject{{"frame", 0}, {"value", 0}}, QJsonObject{{"frame", 30}, {"value", 0}}}},
                                 {"c2", QJsonArray{QJsonObject{{"frame", 0}, {"value", 0}}, QJsonObject{{"frame", 30}, {"value", 0}}}},
                                 {"options", QJsonObject{{"fps", 30}, {"length", 30}, {"mode", "single"}, {"startpaused", true}, {"parent", QJsonObject{{"key", "alpha"}}}}}};
        const QJsonObject a{{"id", 1}, {"name", "a"}, {"image", "models/a.json"}, {"origin", QJsonObject{{"value", "0 0 0"}, {"animation", linked}}},
                            {"size", "10 10"}, {"alpha", QJsonObject{{"value", 1}, {"script", script}, {"animation", keys}}}};
        const QString driver = QStringLiteral("let n = 0; export function update(v) { if (++n === 2) shared.fade.play(); if (n === 5) shared.fade.pause(); return v; }\n");
        const QJsonObject d{{"id", 2}, {"name", "driver"}, {"visible", QJsonObject{{"script", driver}, {"value", true}}}};
        item.file(QStringLiteral("scene.json"), scenevkfix::sceneJson(QJsonDocument(a).toJson(QJsonDocument::Compact) + "," + QJsonDocument(d).toJson(QJsonDocument::Compact)));
        const Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        std::string err;
        auto scene = loadScene(vfs, "scene.json", &err);
        QVERIFY2(scene.has_value(), err.c_str());
        QCOMPARE(scene->animations.size(), size_t{2}); // a property with a script keeps its keyframes
        PropertyStore props;
        ScriptFlags flags;
        SceneScripts scripts(*scene, vfs, props, flags);
        scripts.start();
        QVERIFY2(scripts.failed() == 0, scripts.failures().empty() ? "" : scripts.failures().front().c_str());
        const PropertyAnimation &alpha = scene->animations[1].target == PropertyAnimation::Target::Alpha ? scene->animations[1] : scene->animations[0];
        const PropertyAnimation &origin = &alpha == &scene->animations[0] ? scene->animations[1] : scene->animations[0];
        QCOMPARE(sampleAnimation(alpha, 0, 10.0, 1.0f), 0.0f); // started paused: frame 0 whatever the scene clock says
        QCOMPARE(alpha.endSeconds(), 0.0);
        ScriptFrame f;
        const auto frames = [&](int from, int to) { // frames at 0, 0.5 (play), 1, 1.5, 2 (pause, 1.5 s after the play: past the end)
            for (int i = from; i < to; ++i) {
                f.time = 0.5 * i;
                scripts.update(f);
            }
        };
        frames(0, 3);
        QVERIFY(flags.animationsChanged);
        QCOMPARE(sampleAnimation(alpha, 0, 1.0, -1.0f), 0.5f);     // half a second after the play: frame 15 of 30
        QCOMPARE(sampleAnimation(origin, 0, 1.0, -1.0f), 150.0f); // the linked track plays with it
        frames(3, 5);
        QCOMPARE(sampleAnimation(alpha, 0, 9.0, -1.0f), 1.0f);     // paused at the end, it holds there
        QCOMPARE(alpha.endSeconds(), 2.0);
    }

    void aLightsKeyframesAreLinkedByTheirFileKeys()
    {
        // a script on a light's intensity starts its paused keyframes from a hook; a track of the node's origin that names `intensity` as its
        // parent plays with it (the keyframes of a light value are known by the file's key, the script's property is "light.intensity")
        scenevkfix::Item item;
        const QString script = QStringLiteral("export function init(value) { shared.glow = thisObject.getAnimation(); return value; }\n");
        const QJsonObject keys{{"c0", QJsonArray{QJsonObject{{"frame", 0}, {"value", 0}}, QJsonObject{{"frame", 30}, {"value", 1}}}},
                               {"options", QJsonObject{{"fps", 30}, {"length", 30}, {"mode", "single"}, {"startpaused", true}}}};
        const QJsonObject linked{{"c0", QJsonArray{QJsonObject{{"frame", 0}, {"value", 0}}, QJsonObject{{"frame", 30}, {"value", 300}}}},
                                 {"c1", QJsonArray{QJsonObject{{"frame", 0}, {"value", 0}}, QJsonObject{{"frame", 30}, {"value", 0}}}},
                                 {"c2", QJsonArray{QJsonObject{{"frame", 0}, {"value", 0}}, QJsonObject{{"frame", 30}, {"value", 0}}}},
                                 {"options", QJsonObject{{"fps", 30}, {"length", 30}, {"mode", "single"}, {"startpaused", true}, {"parent", QJsonObject{{"key", "intensity"}}}}}};
        const QJsonObject light{{"id", 1}, {"name", "lamp"}, {"light", "lpoint"}, {"origin", QJsonObject{{"value", "0 0 0"}, {"animation", linked}}},
                                {"intensity", QJsonObject{{"value", 1}, {"script", script}, {"animation", keys}}}};
        const QString driver = QStringLiteral("let n = 0; export function update(v) { if (++n === 2) shared.glow.play(); return v; }\n");
        const QJsonObject d{{"id", 2}, {"name", "driver"}, {"visible", QJsonObject{{"script", driver}, {"value", true}}}};
        item.file(QStringLiteral("scene.json"), scenevkfix::sceneJson(QJsonDocument(light).toJson(QJsonDocument::Compact) + "," + QJsonDocument(d).toJson(QJsonDocument::Compact)));
        const Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        std::string err;
        auto scene = loadScene(vfs, "scene.json", &err);
        QVERIFY2(scene.has_value(), err.c_str());
        QCOMPARE(scene->animations.size(), size_t{2});
        PropertyStore props;
        ScriptFlags flags;
        SceneScripts scripts(*scene, vfs, props, flags);
        scripts.start();
        QVERIFY2(scripts.failed() == 0, scripts.failures().empty() ? "" : scripts.failures().front().c_str());
        const PropertyAnimation &glow = scene->animations[0].target == PropertyAnimation::Target::LightIntensity ? scene->animations[0] : scene->animations[1];
        const PropertyAnimation &origin = &glow == &scene->animations[0] ? scene->animations[1] : scene->animations[0];
        ScriptFrame f;
        for (int i = 0; i < 3; ++i) {
            f.time = 0.5 * i;
            scripts.update(f);
        }
        QCOMPARE(sampleAnimation(glow, 0, 1.0, -1.0f), 0.5f);
        QCOMPARE(sampleAnimation(origin, 0, 1.0, -1.0f), 150.0f); // the linked track plays with it
    }

    void aPausedAnimationWhoseScriptDoesNotRunPlays()
    {
        // the script that would play it does not compile, throws in its init, or is stopped later: the animation runs as without it
        const auto playingAfter = [](const char *script, int frames) {
            scenevkfix::Item item;
            item.tex(QStringLiteral("a"), scenevkfix::simpleRgba(8, 8));
            item.image(QStringLiteral("a"));
            const QJsonObject keys{{"c0", QJsonArray{QJsonObject{{"frame", 0}, {"value", 0}}, QJsonObject{{"frame", 30}, {"value", 1}}}},
                                   {"options", QJsonObject{{"fps", 30}, {"length", 30}, {"mode", "single"}, {"startpaused", true}}}};
            const QJsonObject a{{"id", 1}, {"name", "a"}, {"image", "models/a.json"}, {"size", "10 10"},
                                {"alpha", QJsonObject{{"value", 1}, {"script", QString::fromLatin1(script)}, {"animation", keys}}}};
            item.file(QStringLiteral("scene.json"), scenevkfix::sceneJson(QJsonDocument(a).toJson(QJsonDocument::Compact)));
            const Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
            std::string err;
            auto scene = loadScene(vfs, "scene.json", &err);
            PropertyStore props;
            ScriptFlags flags;
            SceneScripts scripts(*scene, vfs, props, flags);
            scripts.start();
            ScriptFrame f;
            for (int i = 0; i < frames; ++i)
                scripts.update(f);
            return scene->animations.at(0).playback.playing;
        };
        QVERIFY(!playingAfter("export function init(v) { return v; }", 3)); // a script that runs keeps it paused
        QVERIFY(playingAfter("export function init(v { }", 0));
        QVERIFY(playingAfter("export function init(v) { throw new Error('no'); }", 0));
        QVERIFY(playingAfter("export function update(v) { throw new Error('later'); }", 1));
    }

    void aSkinnedMeshHasTheAnimationLayersOfAPuppet()
    {
        // IModelLayer: a 3D mesh whose model has bones and animations plays them through `animationlayers`, which scripts read and change
        scenevkfix::Item item;
        item.tex(QStringLiteral("models/box/skin"), scenevkfix::simpleRgba(4, 4));
        item.file(QStringLiteral("materials/models/box/skin.json"), R"({"passes":[{"shader":"generic4","blending":"normal","textures":["models/box/skin"]}]})");
        meshgen::Model m;
        m.parts.push_back(meshgen::box({-1, -1, -1}, {1, 1, 1}, "materials/models/box/skin.json"));
        m.skinned = true;
        m.bones = {{"root", -1, {}}, {"arm", 0, {}}};
        mdlgen::Animation wave;
        wave.id = 4;
        wave.name = "wave";
        wave.length = 10;
        wave.tracks = {std::vector<mdlgen::Pose>(11), std::vector<mdlgen::Pose>(11)};
        mdlgen::Animation spin = wave;
        spin.id = 5;
        spin.name = "spin";
        m.animations = {wave, spin};
        const auto mdl = meshgen::write(m);
        item.file(QStringLiteral("models/box/box.mdl"), QByteArray(reinterpret_cast<const char *>(mdl.data()), static_cast<qsizetype>(mdl.size())));
        const QString script = QString::fromUtf8(kFormat) + QStringLiteral(
            "export function init() {\n"
            "  const box = thisScene.getLayer('box');\n"
            "  const r = [box.getAnimationLayerCount(), box.getAnimationLayer(0).name, box.getBoneCount(), box.getBoneIndex('arm')];\n"
            "  const added = box.playSingleAnimation('spin', {rate: 2});\n"
            "  r.push(added.name, added.rate, box.getAnimationLayerCount());\n"
            "  added.setFrame(5); r.push(rnd(added.getFrame()));\n"
            "  r.push(box.destroyAnimationLayer(added), box.getAnimationLayerCount(), box.createAnimationLayer('nothing') === null);\n"
            "  console.log('answer ' + r.join(' '));\n"
            "}\n");
        const QJsonObject box{{"id", 2}, {"name", "box"}, {"model", "models/box/box.mdl"}, {"origin", "0 0 0"},
                              {"animationlayers", QJsonArray{QJsonObject{{"animation", 4}}}}};
        const QJsonObject driver{{"id", 3}, {"name", "driver"}, {"visible", QJsonObject{{"script", script}, {"value", true}}}};
        item.file(QStringLiteral("scene.json"), scenevkfix::sceneJson(QJsonDocument(box).toJson(QJsonDocument::Compact) + "," + QJsonDocument(driver).toJson(QJsonDocument::Compact)));
        const Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        std::string err;
        auto scene = loadScene(vfs, "scene.json", &err);
        QVERIFY2(scene.has_value(), err.c_str());
        QVERIFY2(scene->unsupported.empty(), scene->unsupported.empty() ? "" : scene->unsupported[0].c_str());
        QVERIFY(scene->meshes.size() == 1 && scene->meshes[0].skinning);
        PropertyStore props;
        ScriptFlags flags;
        SceneScripts scripts(*scene, vfs, props, flags);
        scripts.start();
        QVERIFY2(scripts.failed() == 0, scripts.failures().empty() ? "" : scripts.failures().front().c_str());
        QCOMPARE(scene->meshes[0].animationLayers.size(), size_t{1});
        QString log;
        for (const std::string &line : scripts.messages())
            log += QString::fromStdString(line) + '\n';
        QVERIFY2(log.contains(QStringLiteral("answer 1 wave 2 1 spin 2 2 5 true 1 true")), qPrintable(log));
    }

    void everyModuleIsEvaluatedBeforeAnyInit()
    {
        // 3521337568: the inits of a puppet's animation layers call shared.offsetedStartAni, which a library script of an object further
        // down the file defines at its top level; and a script on an animation layer's rate (3448290956) drives the rate
        scenevkfix::Item item;
        item.tex(QStringLiteral("red"), scenevkfix::simpleRgba(8, 8));
        const auto mdl = puppetfix::makeMdl({{"root", -1, 0, 0}, {"child", 0, 5, 0}},
                                            {{9, "spin", "loop", 15.0f, 30, {{{puppetfix::frame(0, 0), puppetfix::frame(0, 5)}}}}});
        item.file(QStringLiteral("models/pup.mdl"), QByteArray(reinterpret_cast<const char *>(mdl.data()), static_cast<qsizetype>(mdl.size())));
        item.file(QStringLiteral("models/pup.json"), R"({"material":"materials/pup.json","puppet":"models/pup.mdl"})");
        item.file(QStringLiteral("materials/pup.json"), R"({"passes":[{"shader":"puppetshader","blending":"translucent","textures":["red"]}]})");
        const QString user = QStringLiteral("export function init(value) { shared.startAt(thisObject, 0.5); return value; }\n");
        const QString library = QStringLiteral("function startAt(ani, part) { ani.play(); ani.setFrame(ani.frameCount * part); }\nshared.startAt = startAt;\n"
                                               "export function update(value) { return value; }\n");
        const QString rate = QStringLiteral("export function update(value) { return 3; }\n");
        const QJsonObject pup{{"id", 5}, {"name", "pup"}, {"image", "models/pup.json"}, {"origin", "500 500 0"}, {"size", "40 40"},
                              {"animationlayers", QJsonArray{QJsonObject{{"animation", 9},
                                                                         {"visible", QJsonObject{{"script", user}, {"value", true}}},
                                                                         {"rate", QJsonObject{{"script", rate}, {"value", 1.0}}}}}}};
        const QJsonObject lib{{"id", 6}, {"name", "library"}, {"visible", QJsonObject{{"script", library}, {"value", true}}}};
        item.file(QStringLiteral("scene.json"), scenevkfix::sceneJson(QJsonDocument(pup).toJson(QJsonDocument::Compact) + ","
                                                                       + QJsonDocument(lib).toJson(QJsonDocument::Compact)));
        const Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        std::string err;
        auto scene = loadScene(vfs, "scene.json", &err);
        QVERIFY2(scene.has_value(), err.c_str());
        QVERIFY2(scene->unsupported.empty(), scene->unsupported.empty() ? "" : scene->unsupported[0].c_str());
        PropertyStore props;
        ScriptFlags flags;
        SceneScripts scripts(*scene, vfs, props, flags);
        scripts.start();
        QVERIFY2(scripts.failed() == 0, scripts.failures().empty() ? "" : scripts.failures().front().c_str());
        QCOMPARE(scene->layers[0].animationLayers[0].frameOffset, 15.0f);
        ScriptFrame frame;
        scripts.update(frame);
        QCOMPARE(resolve(scene->layers[0].animationLayers[0].rate, props), 3.0f);
    }

    // ---- the layers a script makes: limits, quotas and what a description may hold ----
    void scriptLayerChurnIsBoundedBecauseDestroyedLayersStayInTheScene()
    {
        World w;
        QVERIFY2(w.build(QStringLiteral("0")), qPrintable(w.error));
        const size_t before = w.scene->layers.size();
        // thousands of create-and-destroy cycles (a script that rebuilds something every frame): a destroyed layer is only hidden, so without
        // a limit the scene would hold them all
        int made = 0;
        for (int i = 0; i < 6000; ++i) {
            const int layer = w.scripts->createLayer("models/a.json", false);
            if (layer >= 0) {
                ++made;
                QVERIFY(w.scripts->destroy(layer));
            }
        }
        QCOMPARE(made, 4096); // the lifetime allowance, then -1 without more layers
        QCOMPARE(w.scene->layers.size(), before + 4096);
        QCOMPARE(w.scripts->createLayer("models/a.json", false), -1);
    }

    // The limit counts the layers that are alive: 600 made and destroyed one after the other never reach it. (In init, whose budget is a
    // second: 700 layers in one frame's 50 ms is too close to call on a busy machine.)
    void theLayerLimitCountsLiveLayers()
    {
        World w;
        QVERIFY2(w.build({}, false, QStringLiteral("let result = '';\n"
            "export function init() { result = String((() => { for (let i = 0; i < 600; ++i) { const l = thisScene.createLayer({image: 'models/a.json'}); "
            "if (!l) return 'null at ' + i; thisScene.destroyLayer(l); } "
            "let alive = 0; for (let i = 0; i < 100; ++i) if (thisScene.createLayer({image: 'models/a.json'})) ++alive; return alive; })()); }\n"
            "export function update(v) { thisScene.getLayer('out').text = result; return v; }\n")), qPrintable(w.error));
        QCOMPARE(w.run(), QStringLiteral("100"));
    }

    void aLayerLimitIsNotPassedAndDestroyingMakesRoomAgain()
    {
        World w;
        // 300 attempts: 256 succeed (a script's allowance); ten destroyed; ten more attempts all succeed
        QVERIFY2(w.build(QStringLiteral("(() => { const alive = []; for (let i = 0; i < 300; ++i) { const l = thisScene.createLayer({image: 'models/a.json'}); if (l) alive.push(l); } "
                                        "const first = alive.length; for (let i = 0; i < 10; ++i) thisScene.destroyLayer(alive.pop()); "
                                        "let again = 0; for (let i = 0; i < 10; ++i) if (thisScene.createLayer({image: 'models/a.json'})) ++again; return first + '|' + again; })()")),
                 qPrintable(w.error));
        QCOMPARE(w.run(), QStringLiteral("256|10"));
    }

    void aLayerThatCouldNotBeMadeCostsNothing()
    {
        World w;
        QVERIFY2(w.build(QStringLiteral("(() => { for (let i = 0; i < 300; ++i) thisScene.createLayer('models/missing.json'); "
                                        "let made = 0; for (let i = 0; i < 300; ++i) if (thisScene.createLayer({image: 'models/a.json'})) ++made; return made; })()")),
                 qPrintable(w.error));
        QCOMPARE(w.run(), QStringLiteral("256"));
    }

    void imagePathsOfADescriptionDoNotEscapeTheItem()
    {
        World w;
        QVERIFY2(w.build(QStringLiteral(R"js((() => [thisScene.createLayer({image: '../x'}), thisScene.createLayer({image: '/etc/passwd'}), thisScene.createLayer({image: 'a\\..\\b'}), )js"
                                        R"js(thisScene.createLayer('../x'), thisScene.createLayer('/etc/passwd')].map((l) => l === null).join())())js")),
                 qPrintable(w.error));
        QCOMPARE(w.run(), QStringLiteral("true,true,true,true,true"));
    }

    void aDescriptionHoldsNoNonFiniteNumberAndKeepsToSaneRanges()
    {
        World w;
        QVERIFY2(w.build(QStringLiteral("(() => { const d = thisScene.createLayer({image: 'models/a.json', origin: new Vec3(1e30, -1e30, 5), size: 'nan 5', scale: new Vec2(1e30, 2), "
                                        "alpha: 7, color: 'nan 1 1', brightness: 1e30}); const e = thisScene.createLayer({image: 'models/a.json', size: new Vec2(1e30, 1e30), origin: 'nan nan nan', angles: 'inf 0 0'}); "
                                        "return [d.origin, d.size.x === 0 || d.size.x > 0, d.scale, d.alpha, d.color, d.brightness, e.size.x < 70000, e.origin].join('|'); })()")),
                 qPrintable(w.error));
        const QString out = w.run();
        // origin held to a million, size left as the model has it, scale held, alpha to 1, a colour with a nan left alone (white), brightness held
        QCOMPARE(out, QStringLiteral("1000000 -1000000 5|true|10000 2 1|1|1 1 1|100|true|0 0 0"));
    }

    void everyDocumentedEntryHasACase()
    {
        QFile table(QStringLiteral(SCENE_VK_DOCS_DIR "/scripts-api.tsv"));
        QVERIFY2(table.open(QIODevice::ReadOnly), "docs/scene-vk/scripts-api.tsv");
        QSet<QString> have;
        for (const Case &c : cases())
            have.insert(c.id);
        for (const Case &c : sceneCases())
            have.insert(c.id);
        QStringList missing;
        for (const QByteArray &line : table.readAll().split('\n')) {
            const QList<QByteArray> cols = line.split('\t');
            if (cols.size() < 3 || cols[0].startsWith('#'))
                continue;
            const QString id = QString::fromUtf8(cols[0]) + QLatin1Char('.') + QString::fromUtf8(cols[1]);
            if (cols[2] != "todo" && cols[2] != "na" && !have.contains(id) && !otherCases().contains(id))
                missing << id;
        }
        QVERIFY2(missing.isEmpty(), qPrintable(missing.join(' ')));
    }

private:
    // entries exercised by the slots of this file below (those that need more than an expression)
    static QSet<QString> otherCases()
    {
        return {"IModelData.static", "IComponent.init", "IComponent.update", "IComponent.destroy", "IComponent.resizeScreen", "IComponent.applyUserProperties",
                "IComponent.applyGeneralSettings", "IComponent.cursorEnter", "IComponent.cursorLeave", "IComponent.cursorMove", "IComponent.cursorDown",
                "IComponent.cursorUp", "IComponent.cursorClick", "IComponent.mediaPlaybackChanged", "IComponent.mediaStatusChanged",
                "IComponent.mediaPropertiesChanged", "IComponent.mediaThumbnailChanged", "IComponent.mediaTimelineChanged", // the event.* slots
                "IEngine.setTimeout", "IEngine.setInterval", "IInput.cursorWorldPosition", "IInput.cursorScreenPosition", "IInput.cursorLeftDown",
                "event.init", "event.update", "event.destroy", "event.resizeScreen", "event.applyUserProperties", "event.applyGeneralSettings",
                "event.cursorEnter", "event.cursorLeave", "event.cursorMove", "event.cursorDown", "event.cursorUp", "event.cursorClick",
                "event.mediaPlaybackChanged", "event.mediaStatusChanged", "event.mediaPropertiesChanged", "event.mediaThumbnailChanged",
                "event.mediaTimelineChanged", "CursorEvent.worldPosition", "CursorEvent.localPosition", "CursorEvent.hitBox",
                "MediaPlaybackEvent.state", "MediaStatusEvent.enabled", "MediaPropertiesEvent.title", "MediaPropertiesEvent.artist",
                "MediaPropertiesEvent.contentType", "MediaPropertiesEvent.albumTitle", "MediaPropertiesEvent.subTitle",
                "MediaPropertiesEvent.albumArtist", "MediaPropertiesEvent.genres", "MediaThumbnailEvent.hasThumbnail",
                "MediaThumbnailEvent.primaryColor", "MediaThumbnailEvent.secondaryColor", "MediaThumbnailEvent.tertiaryColor",
                "MediaThumbnailEvent.textColor", "MediaThumbnailEvent.highContrastColor", "MediaTimelineEvent.position", "MediaTimelineEvent.duration",
                // aSkinnedMeshHasTheAnimationLayersOfAPuppet
                "IModelLayer.playSingleAnimation", "IModelLayer.getAnimationLayerCount", "IModelLayer.getAnimationLayer", "IModelLayer.createAnimationLayer",
                "IModelLayer.destroyAnimationLayer",
                // aVideoTextureIsThePlayerOfItsLayer
                "IVideoTexture.duration", "IVideoTexture.rate", "IVideoTexture.loop", "IVideoTexture.play", "IVideoTexture.pause",
                "IVideoTexture.stop", "IVideoTexture.isPlaying", "IVideoTexture.getCurrentTime", "IVideoTexture.setCurrentTime",
                "IVideoTexture.addEndedCallback"};
    }
};

QTEST_GUILESS_MAIN(TstSceneVkScriptApi)
#include "tst_scenevk_scriptapi.moc"
