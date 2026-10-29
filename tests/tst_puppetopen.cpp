// The open back end (src/puppet/openformat.*, model.h): an Inochi2D puppet, INP version 1, read and evaluated. Every puppet here is written by the test; no real puppet is in the repository.
#include "puppet/check.h"
#include "puppet/openformat.h"
#include "puppetlimits.h"
#include "puppetnames.h"
#include "testutil.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>

using namespace Qt::StringLiterals;
using namespace murasaki::puppet;

namespace {

QJsonArray arr(std::initializer_list<double> v)
{
    QJsonArray a;
    for (const double d : v)
        a.append(d);
    return a;
}

QJsonObject transform(double tx = 0, double ty = 0, double rz = 0, double sx = 1, double sy = 1)
{
    return {{"trans", arr({tx, ty, 0})}, {"rot", arr({0, 0, rz})}, {"scale", arr({sx, sy})}};
}

QJsonObject meshOf(std::initializer_list<double> verts, std::initializer_list<double> indices, std::initializer_list<double> uvs = {})
{
    QJsonObject m{{"verts", arr(verts)}, {"indices", arr(indices)}, {"origin", arr({0, 0})}};
    if (uvs.size())
        m["uvs"] = arr(uvs);
    return m;
}

QJsonObject square(double x, double y, double s)
{
    return meshOf({x, y, x + s, y, x + s, y + s, x, y + s}, {0, 1, 2, 0, 2, 3}, {0, 0, 1, 0, 1, 1, 0, 1});
}

QJsonObject nodeOf(const QString &type, uint uuid, const QString &name, const QJsonObject &tr = transform(), QJsonArray children = {}, double z = 0)
{
    QJsonObject n{{"uuid", double(uuid)}, {"name", name}, {"type", type}, {"enabled", true}, {"zsort", z}, {"transform", tr}, {"lockToRoot", false}};
    if (!children.isEmpty())
        n["children"] = children;
    return n;
}

QJsonObject partOf(uint uuid, const QString &name, const QJsonObject &mesh, double z = 0, const QJsonObject &tr = transform(), const QJsonArray &children = {})
{
    QJsonObject n = nodeOf("Part", uuid, name, tr, children, z);
    n["mesh"] = mesh;
    n["textures"] = QJsonArray{0, 4294967295.0, 4294967295.0};
    n["blend_mode"] = "Normal";
    n["tint"] = arr({1, 1, 1});
    n["screenTint"] = arr({0, 0, 0});
    n["opacity"] = 1.0;
    n["mask_threshold"] = 0.5;
    return n;
}

QJsonObject binding(uint node, const QString &name, const QJsonArray &values, const QString &mode = u"Linear"_s)
{
    return {{"node", double(node)}, {"param_name", name}, {"values", values}, {"isSet", QJsonArray()}, {"interpolate_mode", mode}};
}

// 1D column helper: values[x][0]
QJsonArray column(std::initializer_list<double> v)
{
    QJsonArray out;
    for (const double d : v)
        out.append(QJsonArray{d});
    return out;
}

QJsonObject param1D(const QString &name, const QJsonArray &bindings, double mn = -1, double mx = 1, double def = 0, std::initializer_list<double> axis = {0, 0.5, 1})
{
    return {{"uuid", 1000.0}, {"name", name}, {"is_vec2", false}, {"min", arr({mn, 0})}, {"max", arr({mx, 1})}, {"defaults", arr({def, 0})},
            {"axis_points", QJsonArray{arr(axis), arr({0})}}, {"merge_mode", "Additive"}, {"bindings", bindings}};
}

QJsonObject puppetOf(const QJsonObject &root, const QJsonArray &params = {})
{
    QJsonObject p{{"meta", QJsonObject{{"name", "Test"}, {"version", "1.0-alpha"}, {"preservePixels", false}}}, {"nodes", root}};
    if (!params.isEmpty())
        p["param"] = params;
    return p;
}

QByteArray be32(quint32 v)
{
    QByteArray b;
    b += char(v >> 24);
    b += char(v >> 16);
    b += char(v >> 8);
    b += char(v);
    return b;
}

QByteArray inpOf(const QByteArray &json, const QList<QPair<int, QByteArray>> &textures = {{0, "png-bytes"}})
{
    QByteArray out("TRNSRTS\0", 8);
    out += be32(quint32(json.size())) + json;
    out += "TEX_SECT" + be32(quint32(textures.size()));
    for (const auto &t : textures)
        out += be32(quint32(t.second.size())) + char(t.first) + t.second;
    return out;
}

QByteArray inpOf(const QJsonObject &puppet, const QList<QPair<int, QByteArray>> &textures = {{0, "png-bytes"}})
{
    return inpOf(QJsonDocument(puppet).toJson(QJsonDocument::Compact), textures);
}

std::string_view view(const QByteArray &b)
{
    return std::string_view(b.constData(), size_t(b.size()));
}

std::unique_ptr<OpenModel> load(const QJsonObject &puppet)
{
    const QByteArray bytes = inpOf(puppet);
    OpenLoadResult r = OpenModel::load(view(bytes));
    if (!r.model)
        qFatal("fixture does not load: %s", r.error.c_str());
    return std::move(r.model);
}

Frame evaluate(OpenModel &m, ParameterSet values, PartSet parts = {})
{
    if (parts.size() == 0)
        parts = m.parts();
    Frame f;
    if (!m.evaluate(values, parts, f))
        qFatal("evaluate refused");
    return f;
}

Frame atRest(OpenModel &m)
{
    return evaluate(m, m.parameters());
}

bool near(Vec2 a, double x, double y, double eps = 1e-9)
{
    return std::abs(a.x - x) < eps && std::abs(a.y - y) < eps;
}

QString err(const QByteArray &bytes)
{
    const OpenLoadResult r = OpenModel::load(view(bytes));
    return r.model ? QString() : QString::fromStdString(r.error);
}

QJsonArray operator+(const QJsonArray &a, const QJsonArray &b)
{
    QJsonArray out = a;
    for (const QJsonValue &v : b)
        out.append(v);
    return out;
}

// the smallest puppet: a root with one square part
QJsonObject smallRoot(const QJsonArray &children = {})
{
    return nodeOf("Node", 1, "Root", transform(), QJsonArray{partOf(10, "A", square(0, 0, 100))} + children);
}

} // namespace

class TestPuppetOpen : public QObject
{
    Q_OBJECT
private slots:
    void readsAPuppet();
    void restPositionsAndOrder();
    void linearBindings();
    void interpolationModes();
    void twoDimensionalParameters();
    void transformsAndHierarchy();
    void opacityTintScale();
    void depthSort();
    void disabledNodes();
    void composites();
    void masks();
    void meshGroupDeformation();
    void meshGroupPropagation();
    void staticMeshGroup();
    void texturesAndContainer();
    void refusals_data();
    void refusals();
    void limits();
    void warnings();
    void evaluateChecksItsInput();
    void mutationFuzz();
    void loadsFiles();
    void deterministic();
    void hostileMeshGroups();
    void extremeNumbers();
    void texturesAreRangesOfTheFile();
    void textureHeadersAreReadWithoutDecoding();
    void theTextureBudgetIsCheckedBeforeDecoding();
    void theFileCanBeLetGo();
    void checkReadsThePuppet();
};

void TestPuppetOpen::readsAPuppet()
{
    auto m = load(puppetOf(smallRoot(), {param1D("Move", {binding(10, "transform.t.x", column({-10, 0, 10}))})}));
    QCOMPARE(m->name(), std::string("Test"));
    QCOMPARE(m->info().version, std::string("1.0-alpha"));
    QVERIFY(!m->info().preservePixels);
    QCOMPARE(m->info().textures.size(), size_t(1));
    QCOMPARE(std::string(m->textureBytes(0)), std::string("png-bytes"));
    QVERIFY(m->textureBytes(1).empty());
    QVERIFY(m->textureBytes(-1).empty());
    QCOMPARE(m->parameters().size(), size_t(1));
    QCOMPARE(m->parameters().at(0).id, std::string("Move"));
    QCOMPARE(m->parameters().at(0).min, -1.0);
    QCOMPARE(m->parameters().at(0).max, 1.0);
    QCOMPARE(m->parts().size(), size_t(1));
    QCOMPARE(m->parts().id(0), std::string("A"));
    QCOMPARE(m->drawableCount(), size_t(2)); // root and part
    QVERIFY(!m->drawableId(1).empty()); // a drawable is known by its node's name (the id a hit area names)
    QVERIFY(m->drawableId(-1).empty() && m->drawableId(99).empty());
    const Shape &s = m->shape(1);
    QCOMPARE(s.indices.size(), size_t(6));
    QCOMPARE(s.uvs.size(), size_t(4));
    QCOMPARE(s.uvs[2].x, 1.0);
    const Bounds b = m->bounds();
    QVERIFY(std::abs(b.minX) < 1e-9 && std::abs(b.maxX - 100) < 1e-9 && std::abs(b.maxY - 100) < 1e-9);
    QVERIFY(m->info().warnings.empty());
}

void TestPuppetOpen::restPositionsAndOrder()
{
    QJsonObject root = nodeOf("Node", 1, "Root", transform(), QJsonArray{partOf(10, "Back", square(0, 0, 10), 5), partOf(11, "Front", square(100, 0, 10), 1), partOf(12, "Mid", square(200, 0, 10), 3)});
    auto m = load(puppetOf(root));
    const Frame f = atRest(*m);
    // highest zsort first: Back (5), Mid (3), Front (1)
    QCOMPARE(f.items.size(), size_t(3));
    QCOMPARE(f.items[0].drawable, 1);
    QCOMPARE(f.items[1].drawable, 3);
    QCOMPARE(f.items[2].drawable, 2);
    QVERIFY(near(f.items[0].positions[0], 0, 0));
    QVERIFY(near(f.items[0].positions[2], 10, 10));
    QVERIFY(near(f.items[2].positions[1], 110, 0));
    QCOMPARE(f.items[0].kind, DrawKind::Mesh);
    QCOMPARE(f.items[0].texture, 0);
    QCOMPARE(f.items[0].blend, BlendMode::Normal);
    QCOMPARE(f.items[0].opacity, 1.0);
    // the origin of a mesh is subtracted
    QJsonObject shifted = square(0, 0, 10);
    shifted["origin"] = arr({5, 5});
    auto m2 = load(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{partOf(10, "A", shifted)})));
    QVERIFY(near(atRest(*m2).items[0].positions[0], -5, -5));
}

void TestPuppetOpen::linearBindings()
{
    auto m = load(puppetOf(smallRoot(), {param1D("Move", {binding(10, "transform.t.x", column({-10, 0, 10})), binding(10, "transform.t.y", column({0, 0, 40}))})}));
    const auto at = [&](double value) {
        ParameterSet v = m->parameters();
        v.set(0, value);
        return evaluate(*m, v).items[0].positions[0];
    };
    QVERIFY(near(at(-1), -10, 0));
    QVERIFY(near(at(0), 0, 0));
    QVERIFY(near(at(1), 10, 40));
    QVERIFY(near(at(0.5), 5, 20));     // normalised 0.75: half way between the second and third point
    QVERIFY(near(at(-0.5), -5, 0));    // 0.25
    QVERIFY(near(at(9), 10, 40));      // clamped to the range
    QVERIFY(near(at(-9), -10, 0));
}

void TestPuppetOpen::interpolationModes()
{
    const auto run = [&](const QString &mode, double value) {
        auto m = load(puppetOf(smallRoot(), {param1D("Move", {binding(10, "transform.t.x", column({0, 10, 40}), mode)})}));
        ParameterSet v = m->parameters();
        v.set(0, value);
        return evaluate(*m, v).items[0].positions[0].x;
    };
    // axis points 0, 0.5, 1 => value 0.5 is 0.75: segment 1, t = 0.5
    QVERIFY(std::abs(run("Linear", 0.5) - 25) < 1e-9);
    QVERIFY(std::abs(run("Nearest", 0.5) - 40) < 1e-9);  // t >= 0.5 takes the next point
    QVERIFY(std::abs(run("Nearest", 0.4) - 10) < 1e-9);  // 0.7: t = 0.4
    const double p0 = 0, p1 = 10, p2 = 40, p3 = 40, t = 0.5;
    const double cubic = 0.5 * (2 * p1 + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
    QVERIFY(std::abs(run("Cubic", 0.5) - cubic) < 1e-9);
    // a cubic passes through its points
    QVERIFY(std::abs(run("Cubic", 0) - 10) < 1e-9 + 10); // value 0 is normalised 0.5: exactly the middle point
    QVERIFY(std::abs(run("Cubic", 0) - 10) < 1e-9);
    QVERIFY(std::abs(run("Cubic", -1) - 0) < 1e-9);
    QVERIFY(std::abs(run("Cubic", 1) - 40) < 1e-9);
    // stepped and bezier run as nearest and linear, and say so
    auto m = load(puppetOf(smallRoot(), {param1D("Move", {binding(10, "transform.t.x", column({0, 10, 40}), "Stepped"), binding(10, "transform.t.y", column({0, 10, 40}), "Bezier")})}));
    QCOMPARE(m->info().warnings.size(), size_t(2));
    // a missing mode is linear (the file written by an editor always has one, a hand-written one may not)
    QJsonObject b = binding(10, "transform.t.x", column({0, 10, 40}));
    b.remove("interpolate_mode");
    auto m2 = load(puppetOf(smallRoot(), {param1D("Move", {b})}));
    ParameterSet v = m2->parameters();
    v.set(0, 0.5);
    QVERIFY(std::abs(evaluate(*m2, v).items[0].positions[0].x - 25) < 1e-9);
}

void TestPuppetOpen::twoDimensionalParameters()
{
    // a 2 x 2 grid on both axes; values [x][y]
    QJsonObject b = binding(10, "transform.t.x", QJsonArray{arr({0, 10}), arr({20, 40})});
    QJsonObject p{{"uuid", 1.0}, {"name", "Pos"}, {"is_vec2", true}, {"min", arr({-1, -1})}, {"max", arr({1, 1})}, {"defaults", arr({0, 0})}, {"axis_points", QJsonArray{arr({0, 1}), arr({0, 1})}},
                  {"bindings", QJsonArray{b}}};
    auto m = load(puppetOf(smallRoot(), {p}));
    QCOMPARE(m->parameters().size(), size_t(2));
    QCOMPARE(m->parameters().at(0).id, std::string("Pos.x"));
    QCOMPARE(m->parameters().at(1).id, std::string("Pos.y"));
    const auto at = [&](double x, double y) {
        ParameterSet v = m->parameters();
        v.set(0, x);
        v.set(1, y);
        return evaluate(*m, v).items[0].positions[0].x;
    };
    QVERIFY(std::abs(at(-1, -1) - 0) < 1e-9);
    QVERIFY(std::abs(at(-1, 1) - 10) < 1e-9);
    QVERIFY(std::abs(at(1, -1) - 20) < 1e-9);
    QVERIFY(std::abs(at(1, 1) - 40) < 1e-9);
    QVERIFY(std::abs(at(0, 0) - 17.5) < 1e-9);  // bilinear centre: (0 + 10 + 20 + 40) / 4
    QVERIFY(std::abs(at(0, -1) - 10) < 1e-9);
    // bicubic through four points per axis (here 3x3) is exact at the points
    QJsonArray cols;
    for (int x = 0; x < 3; ++x)
        cols.append(arr({x * 10.0, x * 10.0 + 1, x * 10.0 + 4}));
    QJsonObject bc = binding(10, "transform.t.x", cols, "Cubic");
    QJsonObject p3{{"uuid", 1.0}, {"name", "Pos"}, {"is_vec2", true}, {"min", arr({0, 0})}, {"max", arr({1, 1})}, {"defaults", arr({0, 0})}, {"axis_points", QJsonArray{arr({0, 0.5, 1}), arr({0, 0.5, 1})}},
                   {"bindings", QJsonArray{bc}}};
    auto m3 = load(puppetOf(smallRoot(), {p3}));
    ParameterSet v = m3->parameters();
    v.set(0, 0.5);
    v.set(1, 0.5);
    QVERIFY(std::abs(evaluate(*m3, v).items[0].positions[0].x - 11) < 1e-9);
    v.set(0, 1);
    v.set(1, 1);
    QVERIFY(std::abs(evaluate(*m3, v).items[0].positions[0].x - 24) < 1e-9);
}

void TestPuppetOpen::transformsAndHierarchy()
{
    // parent translated and scaled, child rotated a quarter turn
    QJsonObject child = partOf(11, "Child", meshOf({0, 0, 10, 0, 0, 10}, {0, 1, 2}), 0, transform(0, 0, M_PI / 2));
    QJsonObject parent = partOf(10, "Parent", square(0, 0, 10), 0, transform(100, 50, 0, 2, 2), QJsonArray{child});
    auto m = load(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{parent})));
    const Frame f = atRest(*m);
    QCOMPARE(f.items.size(), size_t(2));
    // parent: scale 2 then translate: (0,0) -> (100,50); (10,10) -> (120,70)
    const DrawItem *p = f.items[0].drawable == 1 ? &f.items[0] : &f.items[1];
    const DrawItem *c = f.items[0].drawable == 2 ? &f.items[0] : &f.items[1];
    QVERIFY(near(p->positions[2], 120, 70));
    // child: rotated 90 degrees about its own origin (x -> y), then in the parent's frame (scale 2, translate): (10,0) -> (0,10) -> (100, 70)
    QVERIFY(near(c->positions[0], 100, 50));
    QVERIFY(near(c->positions[1], 100, 70));
    QVERIFY(near(c->positions[2], 80, 50));
    // lockToRoot: the child ignores the parent's transform
    QJsonObject locked = child;
    locked["lockToRoot"] = true;
    parent["children"] = QJsonArray{locked};
    auto m2 = load(puppetOf(nodeOf("Node", 1, "Root", transform(5, 6), QJsonArray{parent})));
    const Frame f2 = atRest(*m2);
    const DrawItem *c2 = f2.items[0].drawable == 2 ? &f2.items[0] : &f2.items[1];
    QVERIFY(near(c2->positions[0], 5, 6));  // only the root's translation, then its own rotation
    QVERIFY(near(c2->positions[1], 5, 16));
    // rotation x and y are accepted (they foreshorten) and stay finite
    QJsonObject tilted = nodeOf("Part", 10, "T", QJsonObject{{"trans", arr({0, 0, 0})}, {"rot", arr({0.5, 0.25, 0.1})}, {"scale", arr({1, 1})}});
    tilted["mesh"] = square(0, 0, 10);
    auto m3 = load(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{tilted})));
    const Frame f3 = atRest(*m3);
    for (const Vec2 &v : f3.items[0].positions)
        QVERIFY(std::isfinite(v.x) && std::isfinite(v.y));
    // bindings on rotation and scale
    auto m4 = load(puppetOf(smallRoot(), {param1D("Spin", {binding(10, "transform.r.z", column({0, 0, M_PI / 2})), binding(10, "transform.s.x", column({1, 1, 3}))})}));
    ParameterSet v = m4->parameters();
    v.set(0, 1);
    const Frame f4 = evaluate(*m4, v);
    // scale x 3 then a quarter turn: (100, 0) -> (300, 0) -> (0, 300)
    QVERIFY(near(f4.items[0].positions[1], 0, 300, 1e-6));
}

void TestPuppetOpen::opacityTintScale()
{
    QJsonObject part = partOf(10, "A", square(0, 0, 10));
    part["opacity"] = 0.8;
    part["tint"] = arr({1, 0.5, 1});
    part["screenTint"] = arr({0, 0, 0.25});
    auto m = load(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{part}),
                           {param1D("P", {binding(10, "opacity", column({1, 1, 0.5})), binding(10, "tint.g", column({1, 1, 0.5})), binding(10, "screenTint.b", column({0, 0, 0.5}))})}));
    DrawItem d = atRest(*m).items[0];
    QVERIFY(std::abs(d.opacity - 0.8) < 1e-9);
    QVERIFY(std::abs(d.tint[1] - 0.5) < 1e-9);
    QVERIFY(std::abs(d.screenTint[2] - 0.25) < 1e-9);
    ParameterSet v = m->parameters();
    v.set(0, 1);
    d = evaluate(*m, v).items[0];
    QVERIFY(std::abs(d.opacity - 0.4) < 1e-9);       // multiplies
    QVERIFY(std::abs(d.tint[1] - 0.25) < 1e-9);      // multiplies
    QVERIFY(std::abs(d.tint[0] - 1.0) < 1e-9);
    QVERIFY(std::abs(d.screenTint[2] - 0.75) < 1e-9); // adds
    // clamped: a binding that pushes past 1
    auto big = load(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{part}), {param1D("P", {binding(10, "screenTint.b", column({5, 5, 5})), binding(10, "tint.r", column({9, 9, 9}))})}));
    d = atRest(*big).items[0];
    QCOMPARE(d.screenTint[2], 1.0);
    QCOMPARE(d.tint[0], 1.0);
    // the host's part opacity multiplies
    PartSet parts = m->parts();
    parts.set(0, 0.5);
    d = evaluate(*m, m->parameters(), parts).items[0];
    QVERIFY(std::abs(d.opacity - 0.4) < 1e-9);
    // two parameters on the same property combine (scale multiplies, translation adds)
    QJsonObject p1 = param1D("A", {binding(10, "transform.t.x", column({0, 0, 10})), binding(10, "transform.s.x", column({1, 1, 2}))});
    QJsonObject p2 = param1D("B", {binding(10, "transform.t.x", column({0, 0, 5})), binding(10, "transform.s.x", column({1, 1, 3}))});
    auto two = load(puppetOf(smallRoot(), {p1, p2}));
    ParameterSet tv = two->parameters();
    tv.set(0, 1);
    tv.set(1, 1);
    QVERIFY(near(evaluate(*two, tv).items[0].positions[1], 15 + 600, 0, 1e-6));
}

void TestPuppetOpen::depthSort()
{
    // z adds along the chain: a child of a z=10 parent is behind a z=5 sibling even with z=0 of its own
    QJsonObject deep = partOf(12, "Deep", square(0, 0, 10), 0);
    QJsonObject parent = partOf(11, "P", square(20, 0, 10), 10, transform(), QJsonArray{deep});
    QJsonObject sibling = partOf(13, "S", square(40, 0, 10), 5);
    auto m = load(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{sibling, parent})));
    Frame f = atRest(*m);
    QCOMPARE(f.items.size(), size_t(3));
    QCOMPARE(f.items[0].drawable, 2); // parent (10)
    QCOMPARE(f.items[1].drawable, 3); // deep (10 + 0): ties keep the tree order, parent first
    QCOMPARE(f.items[2].drawable, 1); // sibling (5), in front
    // a parameter on zSort reorders
    auto m2 = load(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{partOf(10, "A", square(0, 0, 1), 0), partOf(11, "B", square(0, 0, 1), 1)}),
                            {param1D("Z", {binding(10, "zSort", column({0, 0, 5}))})}));
    f = atRest(*m2);
    QCOMPARE(f.items[0].drawable, 2); // B (1) behind A (0)
    ParameterSet v = m2->parameters();
    v.set(0, 1);
    f = evaluate(*m2, v);
    QCOMPARE(f.items[0].drawable, 1); // A now 5: drawn first, behind
    // ties are stable
    auto m3 = load(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{partOf(10, "A", square(0, 0, 1)), partOf(11, "B", square(0, 0, 1)), partOf(12, "C", square(0, 0, 1))})));
    f = atRest(*m3);
    QCOMPARE(f.items[0].drawable, 1);
    QCOMPARE(f.items[1].drawable, 2);
    QCOMPARE(f.items[2].drawable, 3);
}

void TestPuppetOpen::disabledNodes()
{
    QJsonObject off = partOf(10, "Off", square(0, 0, 1));
    off["enabled"] = false;
    QJsonObject parentOff = partOf(11, "ParentOff", square(0, 0, 1), 0, transform(), QJsonArray{partOf(12, "ChildOfOff", square(0, 0, 1))});
    parentOff["enabled"] = false;
    auto m = load(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{off, parentOff, partOf(13, "On", square(0, 0, 1))})));
    const Frame f = atRest(*m);
    QCOMPARE(f.items.size(), size_t(1));
    QCOMPARE(f.items[0].drawable, 4);
    // a part with no triangles is not drawn
    auto m2 = load(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{partOf(10, "Empty", meshOf({}, {}))})));
    QCOMPARE(atRest(*m2).items.size(), size_t(0));
}

void TestPuppetOpen::composites()
{
    QJsonObject comp = nodeOf("Composite", 20, "C", transform(), QJsonArray{partOf(21, "Back", square(0, 0, 10), 3), partOf(22, "Front", square(5, 5, 10), 1)}, 2);
    comp["blend_mode"] = "Multiply";
    comp["opacity"] = 0.5;
    comp["tint"] = arr({1, 1, 0.5});
    comp["screenTint"] = arr({0, 0, 0});
    comp["mask_threshold"] = 0.5;
    comp["propagate_meshgroup"] = false;
    QJsonObject outside = partOf(30, "Outside", square(0, 0, 10), 5);
    auto m = load(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{comp, outside}), {param1D("O", {binding(20, "opacity", column({1, 1, 0.5}))})}));
    Frame f = atRest(*m);
    // outside (5) first, then the group (2): begin, back (3), front (1), end
    QCOMPARE(f.items.size(), size_t(5));
    QCOMPARE(f.items[0].drawable, 4);
    QCOMPARE(f.items[1].kind, DrawKind::BeginGroup);
    QCOMPARE(f.items[1].drawable, 1);
    QCOMPARE(f.items[2].drawable, 2);
    QCOMPARE(f.items[3].drawable, 3);
    QCOMPARE(f.items[4].kind, DrawKind::EndGroup);
    QCOMPARE(f.items[4].drawable, 1);
    QCOMPARE(f.items[4].blend, BlendMode::Multiply);
    QVERIFY(std::abs(f.items[4].opacity - 0.5) < 1e-9);
    QVERIFY(std::abs(f.items[4].tint[2] - 0.5) < 1e-9);
    QVERIFY(f.items[4].positions.empty());
    ParameterSet v = m->parameters();
    v.set(0, 1);
    f = evaluate(*m, v);
    QVERIFY(std::abs(f.items[4].opacity - 0.25) < 1e-9);
    QCOMPARE(m->parts().size(), size_t(4)); // the composite is a part too
    // a disabled composite hides its parts; a disabled part in a composite is skipped
    comp["enabled"] = false;
    auto m2 = load(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{comp})));
    QCOMPARE(atRest(*m2).items.size(), size_t(0));
    // a composite inside a composite is flattened into the outer one
    QJsonObject inner = nodeOf("Composite", 40, "Inner", transform(), QJsonArray{partOf(41, "InnerPart", square(0, 0, 1), 0)});
    QJsonObject outer = nodeOf("Composite", 30, "Outer", transform(), QJsonArray{partOf(31, "OuterPart", square(0, 0, 1), 1), inner});
    auto m3 = load(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{outer})));
    const Frame f3 = atRest(*m3);
    int begins = 0, meshes = 0;
    for (const DrawItem &i : f3.items) {
        begins += i.kind == DrawKind::BeginGroup;
        meshes += i.kind == DrawKind::Mesh;
    }
    QCOMPARE(begins, 1);
    QCOMPARE(meshes, 2);
    QCOMPARE(m3->info().warnings.size(), size_t(1)); // the flattening is said
}

void TestPuppetOpen::masks()
{
    QJsonObject source = partOf(10, "Source", square(0, 0, 10));
    source["mask_threshold"] = 0.25;
    QJsonObject masked = partOf(11, "Masked", square(0, 0, 10), 1);
    masked["masks"] = QJsonArray{QJsonObject{{"source", 10}, {"mode", "Mask"}}, QJsonObject{{"source", 12}, {"mode", "DodgeMask"}}};
    QJsonObject hidden = partOf(12, "Hidden", square(2, 2, 3));
    hidden["enabled"] = false;
    auto m = load(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{source, masked, hidden})));
    QVERIFY(m->info().warnings.empty());
    const Frame f = atRest(*m);
    const DrawItem *item = nullptr;
    for (const DrawItem &i : f.items)
        if (i.drawable == 2)
            item = &i;
    QVERIFY(item);
    QCOMPARE(item->masks.size(), size_t(2));
    QCOMPARE(item->masks[0].source, 1);
    QVERIFY(!item->masks[0].dodge);
    QCOMPARE(item->masks[1].source, 3);
    QVERIFY(item->masks[1].dodge);
    // the sources' geometry is there even for the hidden one, with their own thresholds
    QCOMPARE(f.maskSources.size(), size_t(2));
    bool sawHidden = false;
    for (const DrawItem &s : f.maskSources) {
        QVERIFY(s.masks.empty());
        if (s.drawable == 3) {
            sawHidden = true;
            QVERIFY(near(s.positions[0], 2, 2));
        }
        if (s.drawable == 1)
            QVERIFY(std::abs(s.maskThreshold - 0.25) < 1e-9);
    }
    QVERIFY(sawHidden);
    // a source that is not a part (a composite, a missing uuid) is dropped, with a warning
    QJsonObject comp = nodeOf("Composite", 20, "C", transform(), QJsonArray{partOf(21, "In", square(0, 0, 1))});
    QJsonObject bad = partOf(30, "Bad", square(0, 0, 10));
    bad["masks"] = QJsonArray{QJsonObject{{"source", 20}, {"mode", "Mask"}}, QJsonObject{{"source", 999}, {"mode", "Mask"}}};
    auto m2 = load(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{comp, bad})));
    QCOMPARE(m2->info().warnings.size(), size_t(2));
    const Frame f2 = atRest(*m2);
    for (const DrawItem &i : f2.items)
        QVERIFY(i.masks.empty());
    // the old form: masked_by and one mask_mode
    QJsonObject legacy = partOf(11, "Legacy", square(0, 0, 10));
    legacy["masked_by"] = arr({10});
    legacy["mask_mode"] = "DodgeMask";
    auto m3 = load(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{source, legacy})));
    const Frame f3 = atRest(*m3);
    for (const DrawItem &i : f3.items)
        if (i.drawable == 2) {
            QCOMPARE(i.masks.size(), size_t(1));
            QVERIFY(i.masks[0].dodge);
        }
}

namespace {

// A mesh group of two triangles over a 100 x 100 square (corners 0..3), with a child part whose points lie in it and outside it.
QJsonObject groupPuppet(bool dynamic, const QJsonObject &groupTransform = transform(), const QJsonArray &moreChildren = {}, const QJsonArray &deformCell = {})
{
    QJsonObject child = partOf(31, "Child", meshOf({25, 25, 75, 25, 50, 75, 200, 200, -50, 10}, {0, 1, 2, 3, 4, 0}));
    QJsonObject group = nodeOf("MeshGroup", 30, "G", groupTransform, QJsonArray{child} + moreChildren);
    group["mesh"] = meshOf({0, 0, 100, 0, 100, 100, 0, 100}, {0, 1, 2, 0, 2, 3});
    group["dynamic_deformation"] = dynamic;
    group["translate_children"] = false;
    Q_UNUSED(deformCell);
    return nodeOf("Node", 1, "Root", transform(), QJsonArray{group});
}

QJsonArray groupDeform(std::initializer_list<double> offsets)
{
    // the bend parameter has two points: rest and bent; cell [1][0] holds the offsets of the four corners
    QJsonArray rest, bent;
    for (int i = 0; i < 4; ++i)
        rest.append(arr({0, 0}));
    auto it = offsets.begin();
    for (int i = 0; i < 4; ++i, it += 2)
        bent.append(arr({*it, *(it + 1)}));
    return QJsonArray{param1D("Bend", {binding(30, "deform", QJsonArray{QJsonArray{rest}, QJsonArray{bent}})}, 0, 1, 0, {0, 1})};
}

} // namespace

void TestPuppetOpen::meshGroupDeformation()
{
    // moving the right-hand corners 1 and 2 by +20 in x stretches the square; a child point inside follows through barycentric coordinates
    auto m = load(puppetOf(groupPuppet(true), groupDeform({0, 0, 20, 0, 20, 0, 0, 0})));
    Frame f = atRest(*m);
    QVERIFY(near(f.items[0].positions[0], 25, 25)); // at rest nothing moves
    ParameterSet v = m->parameters();
    v.set(0, 1);
    f = evaluate(*m, v);
    const DrawItem &c = f.items[0];
    // triangles (0,1,2) and (0,2,3); the point (25,25) is in the first (below the diagonal x > y): u = (x - y)/100 ... compute independently
    const auto warp = [](double x, double y) { // exact: inside triangle (0,0)-(100,0)-(100,100) when y <= x, else (0,0)-(100,100)-(0,100)
        if (y <= x) {
            const double u = (x - y) / 100.0, w = y / 100.0; // x = 100u + 100w, y = 100w
            return Vec2{u * 120 + w * 120, w * 100};         // corners: (0,0), (120,0), (120,100)
        }
        const double u = x / 100.0, w = (y - x) / 100.0; // (0,0)-(100,100)-(0,100): p = u*(100,100) + w*(0,100)
        return Vec2{u * 120, u * 100 + w * 100};          // corners: (0,0), (120,100), (0,100)
    };
    for (int k = 0; k < 3; ++k) {
        const double x = k == 0 ? 25 : k == 1 ? 75 : 50, y = k == 0 ? 25 : k == 1 ? 25 : 75;
        const Vec2 e = warp(x, y);
        QVERIFY2(near(c.positions[size_t(k)], e.x, e.y, 1e-9), qPrintable(u"point %1: %2,%3 expected %4,%5"_s.arg(k).arg(c.positions[size_t(k)].x).arg(c.positions[size_t(k)].y).arg(e.x).arg(e.y)));
    }
    // points outside every triangle are untouched
    QVERIFY(near(c.positions[3], 200, 200));
    QVERIFY(near(c.positions[4], -50, 10));
    // a half way parameter moves half way
    v.set(0, 0.5);
    f = evaluate(*m, v);
    const Vec2 half = warp(25, 25);
    QVERIFY(near(f.items[0].positions[0], 25 + (half.x - 25) / 2 * 1.0, 25 + (half.y - 25) / 2, 0.6)); // linear in the offsets, the warp is affine in each triangle
    // a group that is itself translated and rotated carries the child with it (dynamic: the group's matrix is the child's)
    auto m2 = load(puppetOf(groupPuppet(true, transform(500, 300, M_PI / 2)), groupDeform({0, 0, 20, 0, 20, 0, 0, 0})));
    ParameterSet v2 = m2->parameters();
    v2.set(0, 1);
    const DrawItem c2 = evaluate(*m2, v2).items[0];
    const Vec2 w = warp(25, 25); // in group space; world = rotate 90 degrees (x,y) -> (-y,x), then translate
    QVERIFY(near(c2.positions[0], 500 - w.y, 300 + w.x, 1e-6));
}

void TestPuppetOpen::meshGroupPropagation()
{
    // a nested mesh group stops the outer one; a composite carries only when it says so; a plain node is passed through
    const auto childCount = [](const QJsonObject &root, const QJsonArray &params) {
        auto m = load(puppetOf(root, params));
        ParameterSet v = m->parameters();
        v.set(0, 1);
        return evaluate(*m, v);
    };
    QJsonObject inner = partOf(41, "InnerChild", meshOf({25, 25, 75, 25, 50, 75}, {0, 1, 2}));
    QJsonObject nestedGroup = nodeOf("MeshGroup", 40, "Inner", transform(), QJsonArray{inner});
    nestedGroup["mesh"] = meshOf({0, 0, 100, 0, 100, 100, 0, 100}, {0, 1, 2, 0, 2, 3});
    nestedGroup["dynamic_deformation"] = true;
    QJsonObject plain = nodeOf("Node", 50, "Plain", transform(), QJsonArray{partOf(51, "ViaPlain", meshOf({25, 25, 75, 25, 50, 75}, {0, 1, 2}))});
    QJsonObject compOff = nodeOf("Composite", 60, "CompOff", transform(), QJsonArray{partOf(61, "InOff", meshOf({25, 25, 75, 25, 50, 75}, {0, 1, 2}))});
    QJsonObject compOn = nodeOf("Composite", 70, "CompOn", transform(), QJsonArray{partOf(71, "InOn", meshOf({25, 25, 75, 25, 50, 75}, {0, 1, 2}))});
    compOn["propagate_meshgroup"] = true;
    const QJsonObject root = groupPuppet(true, transform(), QJsonArray{nestedGroup, plain, compOff, compOn});
    const Frame f = childCount(root, groupDeform({0, 0, 20, 0, 20, 0, 0, 0}));
    const auto find = [&](const QString &) { return 0; };
    Q_UNUSED(find);
    // drawable indices in tree order: 0 root, 1 G, 2 Child, 3 Inner, 4 InnerChild, 5 Plain, 6 ViaPlain, 7 CompOff, 8 InOff, 9 CompOn, 10 InOn
    const auto moved = [&](int drawable) {
        for (const DrawItem &i : f.items)
            if (i.drawable == drawable && i.kind == DrawKind::Mesh)
                return std::abs(i.positions[1].x - 75) > 1e-6;
        return false;
    };
    QVERIFY(moved(2));    // the child
    QVERIFY(!moved(4));   // under the nested group: only that group, which is not deformed
    QVERIFY(moved(6));    // through a plain node
    QVERIFY(!moved(8));   // composite that does not propagate
    QVERIFY(moved(10));   // composite that does
}

void TestPuppetOpen::staticMeshGroup()
{
    // with identity transforms the static mode (displacement added to the child's own deformation) and the dynamic one agree on where the child's points end up
    ParameterSet v;
    Vec2 stat, dyn;
    for (const bool dynamic : {false, true}) {
        auto m = load(puppetOf(groupPuppet(dynamic), groupDeform({0, 0, 20, 0, 20, 0, 0, 0})));
        v = m->parameters();
        v.set(0, 1);
        (dynamic ? dyn : stat) = evaluate(*m, v).items[0].positions[0];
    }
    QVERIFY(near(stat, dyn.x, dyn.y, 1e-9));
    // with a scaled child the static displacement is in the child's local space: the result is still the warped place
    QJsonObject root = groupPuppet(false);
    QJsonObject group = root["children"].toArray()[0].toObject();
    QJsonObject child = group["children"].toArray()[0].toObject();
    child["transform"] = transform(10, 0, 0, 0.5, 0.5);
    group["children"] = QJsonArray{child};
    root["children"] = QJsonArray{group};
    auto m = load(puppetOf(root, groupDeform({0, 0, 20, 0, 20, 0, 0, 0})));
    ParameterSet v2 = m->parameters();
    v2.set(0, 1);
    const Vec2 got = evaluate(*m, v2).items[0].positions[0];
    // the child's point (25,25) sits at 0.5 * 25 + 10 = 22.5, 12.5 in the group's space; it is in the lower triangle (y <= x); its warp is x' = 1.2 x
    QVERIFY(near(got, 22.5 * 1.2, 12.5, 1e-9));
}

void TestPuppetOpen::texturesAndContainer()
{
    QJsonObject a = partOf(10, "A", square(0, 0, 1));
    a["textures"] = QJsonArray{1, 4294967295.0, 4294967295.0};
    QJsonObject b = partOf(11, "B", square(0, 0, 1));
    b["textures"] = QJsonArray{7, 4294967295.0, 4294967295.0}; // not in the file
    QJsonObject c = partOf(12, "C", square(0, 0, 1));
    c["textures"] = QJsonArray{4294967295.0, 4294967295.0, 4294967295.0};
    const QByteArray bytes = inpOf(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{a, b, c})), {{0, "zero"}, {1, "one"}});
    OpenLoadResult r = OpenModel::load(view(bytes));
    QVERIFY2(r.model, r.error.c_str());
    QCOMPARE(r.model->info().textures.size(), size_t(2));
    QCOMPARE(r.model->info().textures[1].encoding, 1);
    QCOMPARE(r.model->info().warnings.size(), size_t(1));
    const Frame f = atRest(*r.model);
    int textures[3] = {};
    for (const DrawItem &i : f.items)
        textures[i.drawable - 1] = i.texture;
    QCOMPARE(textures[0], 1);
    QCOMPARE(textures[1], -1);
    QCOMPARE(textures[2], -1);
    // no texture section at all is a puppet without textures
    QByteArray json = QJsonDocument(puppetOf(smallRoot())).toJson(QJsonDocument::Compact);
    QByteArray bare("TRNSRTS\0", 8);
    bare += be32(quint32(json.size())) + json;
    r = OpenModel::load(view(bare));
    QVERIFY2(r.model, r.error.c_str());
    QCOMPARE(r.model->info().textures.size(), size_t(0));
    // an extension section after the textures is not read
    r = OpenModel::load(view(inpOf(json) + "EXT_SECT" + be32(1) + QByteArray(100, 'x')));
    QVERIFY2(r.model, r.error.c_str());
}

void TestPuppetOpen::refusals_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<QString>("contains");
    const auto withMesh = [&](const QJsonObject &mesh) { return inpOf(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{partOf(10, "A", mesh)}))); };
    const QByteArray good = inpOf(puppetOf(smallRoot()));
    QTest::newRow("empty") << QByteArray() << u"INP"_s;
    QTest::newRow("short") << QByteArray("TRNSRTS\0", 8) << u"INP"_s;
    QTest::newRow("magic version 2") << (QByteArray("TRNSRTS2") + QByteArray(40, 'x')) << u"INP"_s;
    QTest::newRow("magic wrong") << (QByteArray("NOTANINP") + QByteArray(40, 'x')) << u"INP"_s;
    QTest::newRow("json length past the end") << (QByteArray("TRNSRTS\0", 8) + be32(1000) + "{}") << u"past the end"_s;
    QTest::newRow("json length huge") << (QByteArray("TRNSRTS\0", 8) + be32(0xFFFFFFFFu) + "{}") << u"kMaxInpJsonBytes"_s;
    QTest::newRow("json broken") << (QByteArray("TRNSRTS\0", 8) + be32(2) + "{x") << u"strict JSON"_s;
    QTest::newRow("json array") << inpOf(QByteArray("[]")) << u"not an object"_s;
    QTest::newRow("json repeated key") << inpOf(QByteArray(R"({"nodes":{},"nodes":{}})")) << u"strict JSON"_s;
    QTest::newRow("no nodes") << inpOf(QByteArray(R"({"meta":{}})")) << u"node tree"_s;
    QTest::newRow("nodes array") << inpOf(QByteArray(R"({"nodes":[]})")) << u"node tree"_s;
    QTest::newRow("root unknown type") << inpOf(QByteArray(R"({"nodes":{"uuid":1,"type":"Bogus","name":"R"}})")) << u"root"_s;
    QTest::newRow("node without type") << inpOf(QByteArray(R"({"nodes":{"uuid":1,"name":"R"}})")) << u"type"_s;
    QTest::newRow("node without uuid") << inpOf(QByteArray(R"({"nodes":{"type":"Node","name":"R"}})")) << u"uuid"_s;
    QTest::newRow("uuid fraction") << inpOf(QByteArray(R"({"nodes":{"uuid":1.5,"type":"Node"}})")) << u"uuid"_s;
    QTest::newRow("uuid negative") << inpOf(QByteArray(R"({"nodes":{"uuid":-1,"type":"Node"}})")) << u"uuid"_s;
    QTest::newRow("uuid huge") << inpOf(QByteArray(R"({"nodes":{"uuid":4294967296,"type":"Node"}})")) << u"uuid"_s;
    QTest::newRow("uuid string") << inpOf(QByteArray(R"({"nodes":{"uuid":"1","type":"Node"}})")) << u"uuid"_s;
    QTest::newRow("enabled number") << inpOf(QByteArray(R"({"nodes":{"uuid":1,"type":"Node","enabled":1}})")) << u"enabled"_s;
    QTest::newRow("zsort string") << inpOf(QByteArray(R"({"nodes":{"uuid":1,"type":"Node","zsort":"1"}})")) << u"zsort"_s;
    QTest::newRow("transform array") << inpOf(QByteArray(R"({"nodes":{"uuid":1,"type":"Node","transform":[]}})")) << u"transform"_s;
    QTest::newRow("trans short") << inpOf(QByteArray(R"({"nodes":{"uuid":1,"type":"Node","transform":{"trans":[1,2]}}})")) << u"trans"_s;
    QTest::newRow("children object") << inpOf(QByteArray(R"({"nodes":{"uuid":1,"type":"Node","children":{}}})")) << u"children"_s;
    QTest::newRow("duplicate uuid") << inpOf(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{partOf(10, "A", square(0, 0, 1)), partOf(10, "B", square(0, 0, 1))}))) << u"uuid"_s;
    QTest::newRow("part without mesh") << inpOf(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{nodeOf("Part", 10, "A")}))) << u"mesh"_s;
    QTest::newRow("odd verts") << withMesh(meshOf({0, 0, 1}, {})) << u"odd"_s;
    QTest::newRow("indices not triangles") << withMesh(meshOf({0, 0, 1, 0, 1, 1}, {0, 1})) << u"triangles"_s;
    QTest::newRow("index past") << withMesh(meshOf({0, 0, 1, 0, 1, 1}, {0, 1, 3})) << u"outside"_s;
    QTest::newRow("index negative") << withMesh(meshOf({0, 0, 1, 0, 1, 1}, {0, 1, -1})) << u"outside"_s;
    QTest::newRow("index fraction") << withMesh(meshOf({0, 0, 1, 0, 1, 1}, {0, 1, 1.5})) << u"outside"_s;
    QTest::newRow("index 65536") << withMesh(meshOf({0, 0, 1, 0, 1, 1}, {0, 1, 65536})) << u"outside"_s;
    QTest::newRow("uvs short") << withMesh(meshOf({0, 0, 1, 0, 1, 1}, {0, 1, 2}, {0, 0})) << u"uvs"_s;
    QTest::newRow("origin short") << withMesh(QJsonObject{{"verts", arr({0, 0, 1, 0, 1, 1})}, {"indices", arr({0, 1, 2})}, {"origin", arr({1})}}) << u"origin"_s;
    QTest::newRow("verts strings") << inpOf(QByteArray(R"({"nodes":{"uuid":1,"type":"Node","children":[{"uuid":2,"type":"Part","mesh":{"verts":["a"],"indices":[]}}]}})")) << u"verts"_s;
    QTest::newRow("unknown blend") << [&] {
        QJsonObject p = partOf(10, "A", square(0, 0, 1));
        p["blend_mode"] = "Hologram";
        return inpOf(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{p})));
    }() << u"blend"_s;
    QTest::newRow("bad mask mode") << [&] {
        QJsonObject p = partOf(10, "A", square(0, 0, 1));
        p["masks"] = QJsonArray{QJsonObject{{"source", 10}, {"mode", "Bogus"}}};
        return inpOf(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{p})));
    }() << u"mask"_s;
    QTest::newRow("mask without source") << [&] {
        QJsonObject p = partOf(10, "A", square(0, 0, 1));
        p["masks"] = QJsonArray{QJsonObject{{"mode", "Mask"}}};
        return inpOf(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{p})));
    }() << u"mask"_s;
    QTest::newRow("tint short") << [&] {
        QJsonObject p = partOf(10, "A", square(0, 0, 1));
        p["tint"] = arr({1, 1});
        return inpOf(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{p})));
    }() << u"tint"_s;
    QTest::newRow("opacity string") << [&] {
        QJsonObject p = partOf(10, "A", square(0, 0, 1));
        p["opacity"] = "1";
        return inpOf(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{p})));
    }() << u"opacity"_s;
    QTest::newRow("textures string") << [&] {
        QJsonObject p = partOf(10, "A", square(0, 0, 1));
        p["textures"] = "0";
        return inpOf(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{p})));
    }() << u"textures"_s;
    QTest::newRow("texture index fraction") << [&] {
        QJsonObject p = partOf(10, "A", square(0, 0, 1));
        p["textures"] = arr({0.5});
        return inpOf(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{p})));
    }() << u"texture"_s;
    QTest::newRow("physics model") << [&] {
        QJsonObject p = nodeOf("SimplePhysics", 10, "Ph");
        p["model_type"] = "string_theory";
        return inpOf(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{p})));
    }() << u"SimplePhysics"_s;
    QTest::newRow("composite propagate number") << [&] {
        QJsonObject p = nodeOf("Composite", 10, "C");
        p["propagate_meshgroup"] = 1;
        return inpOf(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{p})));
    }() << u"propagate"_s;
    const auto withParam = [&](const QJsonObject &p) { return inpOf(puppetOf(smallRoot(), QJsonArray{p})); };
    QTest::newRow("param array") << inpOf(QByteArray(R"({"nodes":{"uuid":1,"type":"Node"},"param":{}})")) << u"param"_s;
    QTest::newRow("param without name") << inpOf(QByteArray(R"({"nodes":{"uuid":1,"type":"Node"},"param":[{"uuid":1}]})")) << u"name"_s;
    QTest::newRow("param default outside") << withParam(param1D("P", {}, -1, 1, 5)) << u"range"_s;
    QTest::newRow("param min over max") << withParam(param1D("P", {}, 2, 1, 1.5)) << u"range"_s;
    QTest::newRow("axis one point") << withParam(param1D("P", {}, -1, 1, 0, {0.5})) << u"axis_points"_s;
    QTest::newRow("axis decreasing") << withParam(param1D("P", {}, -1, 1, 0, {0, 1, 0.5})) << u"axis_points"_s;
    QTest::newRow("axis repeated") << withParam(param1D("P", {}, -1, 1, 0, {0, 0.5, 0.5, 1})) << u"axis_points"_s;
    QTest::newRow("axis outside 0..1") << withParam(param1D("P", {}, -1, 1, 0, {0, 2})) << u"axis_points"_s;
    QTest::newRow("axis one list") << inpOf(QByteArray(R"({"nodes":{"uuid":1,"type":"Node"},"param":[{"name":"P","axis_points":[[0,1]]}]})")) << u"axis_points"_s;
    QTest::newRow("is_vec2 string") << inpOf(QByteArray(R"({"nodes":{"uuid":1,"type":"Node"},"param":[{"name":"P","is_vec2":"yes"}]})")) << u"is_vec2"_s;
    QTest::newRow("bindings object") << inpOf(QByteArray(R"({"nodes":{"uuid":1,"type":"Node"},"param":[{"name":"P","bindings":{}}]})")) << u"bindings"_s;
    QTest::newRow("binding without node") << withParam(param1D("P", {QJsonObject{{"param_name", "zSort"}}})) << u"node"_s;
    QTest::newRow("binding values one column") << withParam(param1D("P", {binding(10, "zSort", column({1, 2}))})) << u"values"_s;
    QTest::newRow("binding values row") << withParam(param1D("P", {binding(10, "zSort", QJsonArray{arr({1, 2}), arr({1, 2}), arr({1, 2})})})) << u"row"_s;
    QTest::newRow("binding value string") << withParam(param1D("P", {binding(10, "zSort", QJsonArray{QJsonArray{"a"}, QJsonArray{"b"}, QJsonArray{"c"}})})) << u"number"_s;
    QTest::newRow("binding mode unknown") << withParam(param1D("P", {binding(10, "zSort", column({1, 2, 3}), "Wavy")})) << u"interpolate_mode"_s;
    QTest::newRow("binding mode number") << inpOf(QByteArray(R"({"nodes":{"uuid":1,"type":"Node","children":[{"uuid":2,"type":"Node"}]},"param":[{"name":"P","bindings":[{"node":2,"param_name":"zSort","interpolate_mode":1,"values":[[1],[2]]}]}]})")) << u"interpolate_mode"_s;
    QTest::newRow("binding isSet number") << inpOf(QByteArray(R"({"nodes":{"uuid":1,"type":"Node","children":[{"uuid":2,"type":"Node"}]},"param":[{"name":"P","bindings":[{"node":2,"param_name":"zSort","isSet":5,"values":[[1],[2]]}]}]})")) << u"isSet"_s;
    QTest::newRow("deform offset one number") << inpOf(puppetOf(smallRoot(), QJsonArray{param1D("P", {binding(10, "deform", QJsonArray{QJsonArray{QJsonArray{arr({1}), arr({1, 1}), arr({1, 1}), arr({1, 1})}}, QJsonArray{QJsonArray{arr({1, 1}), arr({1, 1}), arr({1, 1}), arr({1, 1})}}, QJsonArray{QJsonArray{arr({1, 1}), arr({1, 1}), arr({1, 1}), arr({1, 1})}}})}, -1, 1, 0)})) << u"offset"_s;
    QTest::newRow("deform holds numbers") << withParam(param1D("P", {binding(10, "deform", column({1, 2, 3}))})) << u"deform"_s;
    // textures
    QTest::newRow("texture cut") << (good.left(good.size() - 4)) << u"texture"_s;
    QTest::newRow("texture bc7") << inpOf(puppetOf(smallRoot()), {{2, "x"}}) << u"BC7"_s;
    QTest::newRow("texture unknown encoding") << inpOf(puppetOf(smallRoot()), {{9, "x"}}) << u"encoding"_s;
    QTest::newRow("texture section cut") << (good.left(good.indexOf("TEX_SECT") + 10)) << u"cut"_s;
    QTest::newRow("texture length huge") << [&] {
        QByteArray json = QJsonDocument(puppetOf(smallRoot())).toJson(QJsonDocument::Compact);
        return QByteArray("TRNSRTS\0", 8) + be32(quint32(json.size())) + json + "TEX_SECT" + be32(1) + be32(0xFFFFFFFFu) + char(0) + "x";
    }() << u"texture"_s;
}

void TestPuppetOpen::refusals()
{
    QFETCH(QByteArray, bytes);
    QFETCH(QString, contains);
    const OpenLoadResult r = OpenModel::load(view(bytes));
    QVERIFY2(!r.model, QTest::currentDataTag());
    QVERIFY(!r.error.empty());
    QVERIFY2(QString::fromStdString(r.error).contains(contains, Qt::CaseInsensitive), qPrintable(QString::fromStdString(r.error)));
}

void TestPuppetOpen::limits()
{
    const auto ok = [](const QByteArray &b) { return err(b).isEmpty(); };
    // nodes: a chain of plain nodes
    const auto chain = [](int depth) {
        QJsonObject n = nodeOf("Node", uint(depth + 1), "leaf");
        for (int i = depth - 1; i >= 0; --i)
            n = nodeOf("Node", uint(i + 1), "n", transform(), QJsonArray{n});
        return inpOf(puppetOf(n));
    };
    QVERIFY(ok(chain(int(kMaxDeformerDepth))));
    QVERIFY(!ok(chain(int(kMaxDeformerDepth) + 2)));
    QVERIFY(!ok(chain(2000)));
    const auto flat = [](int count) {
        QJsonArray kids;
        for (int i = 1; i < count; ++i)
            kids.append(nodeOf("Node", uint(i + 1), "n"));
        return inpOf(puppetOf(nodeOf("Node", 1, "root", transform(), kids)));
    };
    QVERIFY(ok(flat(int(kMaxNodes))));
    QVERIFY(err(flat(int(kMaxNodes) + 1)).contains(u"kMaxNodes"_s));
    // drawables
    const auto parts = [](int count) {
        QJsonArray kids;
        for (int i = 0; i < count; ++i)
            kids.append(partOf(uint(i + 2), "p" + QString::number(i), meshOf({0, 0, 1, 0, 1, 1}, {0, 1, 2})));
        return inpOf(puppetOf(nodeOf("Node", 1, "root", transform(), kids)));
    };
    QVERIFY(ok(parts(int(kMaxParts)))); // 512 parts, 512 drawables <= 2048
    QVERIFY(err(parts(int(kMaxParts) + 1)).contains(u"kMaxParts"_s));
    // vertices of a mesh
    const auto mesh = [](int vertices, int indices) {
        QJsonArray v, i;
        for (int k = 0; k < vertices * 2; ++k)
            v.append(0);
        for (int k = 0; k < indices; ++k)
            i.append(0);
        return inpOf(puppetOf(nodeOf("Node", 1, "root", transform(), QJsonArray{partOf(2, "A", QJsonObject{{"verts", v}, {"indices", i}})})));
    };
    QVERIFY(ok(mesh(int(kMaxVerticesPerMesh), 3)));
    QVERIFY(err(mesh(int(kMaxVerticesPerMesh) + 1, 3)).contains(u"kMaxVerticesPerMesh"_s));
    QVERIFY(ok(mesh(3, int(kMaxIndicesPerMesh))));
    QVERIFY(err(mesh(3, int(kMaxIndicesPerMesh) + 3)).contains(u"kMaxIndicesPerMesh"_s));
    // total vertices: 16 meshes of 32768 is the limit
    const auto many = [](int count) {
        QJsonArray v;
        for (int k = 0; k < int(kMaxVerticesPerMesh) * 2; ++k)
            v.append(0);
        QJsonArray kids;
        for (int i = 0; i < count; ++i)
            kids.append(partOf(uint(i + 2), "p" + QString::number(i), QJsonObject{{"verts", v}, {"indices", arr({0, 1, 2})}}));
        return inpOf(puppetOf(nodeOf("Node", 1, "root", transform(), kids)));
    };
    QVERIFY(ok(many(int(kMaxVerticesTotal / kMaxVerticesPerMesh))));
    QVERIFY(err(many(int(kMaxVerticesTotal / kMaxVerticesPerMesh) + 1)).contains(u"kMaxVerticesTotal"_s));
    // masks
    QJsonObject masked = partOf(10, "A", square(0, 0, 1));
    QJsonArray list;
    for (int i = 0; i < int(kMaxMasksPerPart); ++i)
        list.append(QJsonObject{{"source", 11}, {"mode", "Mask"}});
    masked["masks"] = list;
    const QJsonObject src = partOf(11, "S", square(0, 0, 1));
    QVERIFY(ok(inpOf(puppetOf(nodeOf("Node", 1, "root", transform(), QJsonArray{masked, src})))));
    list.append(QJsonObject{{"source", 11}, {"mode", "Mask"}});
    masked["masks"] = list;
    QVERIFY(err(inpOf(puppetOf(nodeOf("Node", 1, "root", transform(), QJsonArray{masked, src})))).contains(u"kMaxMasksPerPart"_s));
    // axis points
    QJsonArray axisOk, axisBad;
    for (int i = 0; i < int(kMaxAxisPoints); ++i)
        axisOk.append(double(i) / double(kMaxAxisPoints));
    axisBad = axisOk;
    axisBad.append(1.0);
    const auto withAxis = [&](const QJsonArray &axis) {
        QJsonObject p{{"name", "P"}, {"axis_points", QJsonArray{axis, arr({0})}}};
        return inpOf(puppetOf(smallRoot(), QJsonArray{p}));
    };
    QVERIFY(ok(withAxis(axisOk)));
    QVERIFY(!ok(withAxis(axisBad)));
    // parameters: kMaxParameters ones fit, one more does not
    const auto params = [&](int count) {
        QJsonArray ps;
        for (int i = 0; i < count; ++i)
            ps.append(QJsonObject{{"name", "p" + QString::number(i)}});
        return inpOf(puppetOf(smallRoot(), ps));
    };
    QVERIFY(ok(params(int(kMaxParameters))));
    QVERIFY(!ok(params(int(kMaxParameters) + 1)));
    // bindings: all on the one part; kMaxBindings fit
    const auto bindings = [&](int count) {
        QJsonArray bs;
        for (int i = 0; i < count; ++i)
            bs.append(binding(10, "zSort", column({0, 0, 0})));
        return inpOf(puppetOf(smallRoot(), QJsonArray{param1D("P", bs)}));
    };
    QVERIFY(ok(bindings(int(kMaxBindings))));
    QVERIFY(err(bindings(int(kMaxBindings) + 1)).contains(u"kMaxBindings"_s));
    // textures
    QList<QPair<int, QByteArray>> ts;
    for (int i = 0; i < int(kMaxTextures); ++i)
        ts.append({0, "x"});
    QVERIFY(ok(inpOf(puppetOf(smallRoot()), ts)));
    ts.append({0, "x"});
    QVERIFY(err(inpOf(puppetOf(smallRoot()), ts)).contains(u"kMaxTextures"_s));
    // the whole file
    QVERIFY(err(QByteArray(int(kMaxInpBytes) + 1, 'x')).contains(u"kMaxInpBytes"_s));
}

void TestPuppetOpen::warnings()
{
    // an unknown node type is skipped with its subtree
    QJsonObject odd = nodeOf("Shapes", 20, "Odd", transform(), QJsonArray{partOf(21, "Under", square(0, 0, 1))});
    auto m = load(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{odd, partOf(10, "A", square(0, 0, 1))}), {param1D("P", {binding(999, "zSort", column({1, 2, 3})), binding(10, "no.such.thing", column({1, 2, 3})),
                                                                                                                    binding(10, "emissionStrength", column({1, 2, 3})), binding(10, "alphaThreshold", column({1, 2, 3}))})}));
    QCOMPARE(m->parts().size(), size_t(1));
    QCOMPARE(atRest(*m).items.size(), size_t(1));
    // warnings: the skipped node, the missing node and the unknown name (emission and alphaThreshold are accepted and ignored)
    QCOMPARE(m->info().warnings.size(), size_t(3));
    // a binding of the wrong kind of node (opacity on a plain node) is dropped
    auto m2 = load(puppetOf(smallRoot(), {param1D("P", {binding(1, "opacity", column({1, 2, 3}))})}));
    QCOMPARE(m2->info().warnings.size(), size_t(1));
    // a deform binding with the wrong number of offsets is dropped
    QJsonArray cell{arr({0, 0})};
    auto m3 = load(puppetOf(smallRoot(), QJsonArray{param1D("P", {binding(10, "deform", QJsonArray{QJsonArray{cell}, QJsonArray{cell}, QJsonArray{cell}})})}));
    QCOMPARE(m3->info().warnings.size(), size_t(1));
    // a SimplePhysics node is counted
    auto m4 = load(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{nodeOf("SimplePhysics", 20, "Ph")})));
    QCOMPARE(m4->info().physicsNodes, 1);
    // duplicate parameter and part names get a suffix
    auto m5 = load(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{partOf(10, "Same", square(0, 0, 1)), partOf(11, "Same", square(0, 0, 1))}), {param1D("P", {}), param1D("P", {})}));
    QCOMPARE(m5->parts().size(), size_t(2));
    QVERIFY(m5->parts().id(0) != m5->parts().id(1));
    QCOMPARE(m5->parameters().size(), size_t(2));
    QVERIFY(m5->parameters().at(0).id != m5->parameters().at(1).id);
}

void TestPuppetOpen::evaluateChecksItsInput()
{
    auto m = load(puppetOf(smallRoot(), {param1D("P", {})}));
    Frame f;
    f.items.resize(3);
    ParameterSet wrong;
    wrong.add("Other", 0, 0, 1);
    QVERIFY(!m->evaluate(wrong, m->parts(), f));
    QCOMPARE(f.items.size(), size_t(3)); // untouched
    ParameterSet empty;
    QVERIFY(!m->evaluate(empty, m->parts(), f));
    PartSet noParts;
    QVERIFY(!m->evaluate(m->parameters(), noParts, f));
    QVERIFY(m->evaluate(m->parameters(), m->parts(), f));
    QCOMPARE(f.items.size(), size_t(1));
    // the shape of a drawable that does not exist is empty
    QVERIFY(m->shape(-1).indices.empty());
    QVERIFY(m->shape(999).indices.empty());
}

void TestPuppetOpen::mutationFuzz()
{
    // a puppet with everything in it, bytes mutated at random with a fixed seed: never a crash, and what loads evaluates to finite numbers
    QJsonObject comp = nodeOf("Composite", 20, "C", transform(), QJsonArray{partOf(21, "In", square(0, 0, 10), 1)});
    QJsonObject masked = partOf(12, "M", square(0, 0, 10));
    masked["masks"] = QJsonArray{QJsonObject{{"source", 10}, {"mode", "Mask"}}};
    QJsonObject group = nodeOf("MeshGroup", 30, "G", transform(1, 2), QJsonArray{partOf(31, "Child", meshOf({25, 25, 75, 25, 50, 75}, {0, 1, 2}))});
    group["mesh"] = meshOf({0, 0, 100, 0, 100, 100, 0, 100}, {0, 1, 2, 0, 2, 3});
    group["dynamic_deformation"] = true;
    const QJsonObject root = nodeOf("Node", 1, "Root", transform(), QJsonArray{partOf(10, "A", square(0, 0, 100)), masked, comp, group});
    const QJsonArray params{param1D("Move", {binding(10, "transform.t.x", column({-10, 0, 10}), "Cubic"), binding(30, "deform", QJsonArray{QJsonArray{QJsonArray{arr({0, 0}), arr({0, 0}), arr({0, 0}), arr({0, 0})}}, QJsonArray{QJsonArray{arr({1, 1}), arr({2, 2}), arr({3, 3}), arr({4, 4})}}, QJsonArray{QJsonArray{arr({5, 5}), arr({6, 6}), arr({7, 7}), arr({8, 8})}}})})};
    const QByteArray base = QJsonDocument(puppetOf(root, params)).toJson(QJsonDocument::Compact);
    quint32 state = 31337;
    const auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    int loaded = 0;
    for (int round = 0; round < 3000; ++round) {
        QByteArray text = base;
        for (int e = 0, edits = 1 + int(next() % 4); e < edits && !text.isEmpty(); ++e) {
            const int at = int(next() % quint32(text.size()));
            switch (next() % 4) {
            case 0: text[at] = char(next() & 0xFF); break;
            case 1: text.remove(at, 1 + int(next() % 6)); break;
            case 2: text.insert(at, QByteArray(1, "0123456789-.,[]{}\":"[next() % 20])); break;
            default: text.truncate(at); break;
            }
        }
        OpenLoadResult r = OpenModel::load(view(inpOf(text)));
        if (!r.model) {
            QVERIFY(!r.error.empty());
            continue;
        }
        ++loaded;
        ParameterSet v = r.model->parameters();
        for (size_t i = 0; i < v.size(); ++i)
            v.set(int(i), double(next() % 2001) / 1000.0 - 1.0);
        Frame f;
        QVERIFY(r.model->evaluate(v, r.model->parts(), f));
        for (const DrawItem &item : f.items)
            for (const Vec2 &p : item.positions)
                QVERIFY(std::isfinite(p.x) && std::isfinite(p.y));
    }
    QVERIFY(loaded > 50);
}

void TestPuppetOpen::loadsFiles()
{
    QTemporaryDir tmp;
    const std::string d = tmp.path().toStdString();
    testutil::writeFile(tmp.path() + u"/p.inp"_s, inpOf(puppetOf(smallRoot())));
    OpenLoadResult r = OpenModel::loadFile(d, "p.inp");
    QVERIFY2(r.model, r.error.c_str());
    // a link as the last component
    QVERIFY(QFile::link(tmp.path() + u"/p.inp"_s, tmp.path() + u"/link.inp"_s));
    QVERIFY(!OpenModel::loadFile(d, "link.inp").model);
    // a link on the way: sub -> a folder that holds a good file
    testutil::writeFile(tmp.path() + u"/elsewhere/q.inp"_s, inpOf(puppetOf(smallRoot())));
    QVERIFY(QDir().mkpath(tmp.path() + u"/item"_s));
    QVERIFY(QFile::link(tmp.path() + u"/elsewhere"_s, tmp.path() + u"/item/sub"_s));
    QVERIFY(!OpenModel::loadFile((tmp.path() + u"/item"_s).toStdString(), "sub/q.inp").model);
    QVERIFY(OpenModel::loadFile((tmp.path() + u"/elsewhere"_s).toStdString(), "q.inp").model);
    // names that leave the folder, a missing file, a folder, a pipe
    QVERIFY(!OpenModel::loadFile((tmp.path() + u"/item"_s).toStdString(), "../p.inp").model);
    QVERIFY(!OpenModel::loadFile(d, "/etc/hostname").model);
    QVERIFY(!OpenModel::loadFile(d, "missing.inp").model);
    QVERIFY(!OpenModel::loadFile(d, "elsewhere").model);
    QVERIFY(::mkfifo(qPrintable(tmp.path() + u"/pipe.inp"_s), 0600) == 0);
    QVERIFY(!OpenModel::loadFile(d, "pipe.inp").model);
    // the size
    QFile big(tmp.path() + u"/big.inp"_s);
    QVERIFY(big.open(QIODevice::WriteOnly));
    QVERIFY(big.resize(kMaxInpBytes + 1));
    big.close();
    r = OpenModel::loadFile(d, "big.inp");
    QVERIFY(!r.model);
    QVERIFY(QString::fromStdString(r.error).contains(u"kMaxInpBytes"_s));
}

namespace {

// Runs `work` in a forked child and returns by how many kB its resident memory peaked above what it started with: the child begins with the parent's current resident set as its own peak, so what
// other tests left behind in this process (a high-water mark that VmHWM would keep for ever) does not hide or fake anything. -1 when it could not be measured.
long peakGrowthKb(const std::function<void()> &work)
{
    int fds[2];
    if (::pipe(fds) != 0)
        return -1;
    const pid_t pid = ::fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        ::close(fds[0]);
        const auto residentKb = [] {
            std::ifstream status("/proc/self/status");
            std::string line;
            while (std::getline(status, line))
                if (line.rfind("VmRSS:", 0) == 0)
                    return std::stol(line.substr(6));
            return 0L;
        };
        const long base = residentKb();
        work();
        rusage usage{};
        ::getrusage(RUSAGE_SELF, &usage);
        const long growth = std::max(0L, usage.ru_maxrss - base);
        const std::string text = std::to_string(growth);
        if (::write(fds[1], text.data(), text.size()) < 0)
            ::_exit(2);
        ::_exit(0);
    }
    ::close(fds[1]);
    char buffer[64] = {};
    const ssize_t n = ::read(fds[0], buffer, sizeof buffer - 1);
    ::close(fds[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    return n > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? std::atol(buffer) : -1;
}

QByteArray groupWithMesh(const QJsonObject &mesh)
{
    QJsonObject child = partOf(31, "Child", meshOf({25, 25, 75, 25, 50, 75}, {0, 1, 2}));
    QJsonObject group = nodeOf("MeshGroup", 30, "G", transform(), QJsonArray{child});
    group["mesh"] = mesh;
    group["dynamic_deformation"] = true;
    return inpOf(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{group})));
}

} // namespace

void TestPuppetOpen::hostileMeshGroups()
{
    // 3 far apart vertices and 65 536 copies of the one triangle: kept once, so loading and drawing stay cheap
    QJsonArray verts{0, 0, 1e6, 0, 0, 1e6};
    QJsonArray indices;
    for (int i = 0; i < 65536; ++i)
        for (const int v : {0, 1, 2})
            indices.append(v);
    const auto hostile = [&] {
        OpenLoadResult loaded = OpenModel::load(view(groupWithMesh(QJsonObject{{"verts", verts}, {"indices", indices}})));
        if (!loaded.model)
            ::_exit(3);
        ParameterSet values = loaded.model->parameters();
        Frame frame;
        for (int i = 0; i < 20; ++i)
            if (!loaded.model->evaluate(values, loaded.model->parts(), frame))
                ::_exit(3);
    };
    // the measure itself works: a child that touches 300 MB reports about that much
    const long control = peakGrowthKb([] {
        std::vector<char> big(300u << 20);
        std::fill(big.begin(), big.end(), char(1));
        asm volatile("" : : "r"(big.data()) : "memory");
    });
    QVERIFY2(control > 250 * 1024, qPrintable(QString::number(control)));
    // and the hostile model (65 536 copies of one triangle, as JSON, loaded and drawn 20 times) stays small
    const long growth = peakGrowthKb(hostile);
    QVERIFY2(growth >= 0 && growth < 100 * 1024, qPrintable(QString::number(growth)));
    // the time is a multiple of a plain mesh group's, not a number of seconds
    QElapsedTimer timer;
    timer.start();
    OpenLoadResult r = OpenModel::load(view(groupWithMesh(QJsonObject{{"verts", verts}, {"indices", indices}})));
    QVERIFY2(r.model, r.error.c_str());
    ParameterSet v = r.model->parameters();
    Frame f;
    for (int i = 0; i < 20; ++i)
        QVERIFY(r.model->evaluate(v, r.model->parts(), f));
    const qint64 hostileMs = timer.elapsed();
    timer.restart();
    OpenLoadResult plain = OpenModel::load(view(groupWithMesh(QJsonObject{{"verts", QJsonArray{0, 0, 1e6, 0, 0, 1e6}}, {"indices", QJsonArray{0, 1, 2}}})));
    QVERIFY(plain.model);
    ParameterSet pv = plain.model->parameters();
    for (int i = 0; i < 20; ++i)
        QVERIFY(plain.model->evaluate(pv, plain.model->parts(), f));
    const qint64 plainMs = timer.elapsed();
    qInfo("hostile mesh group: %lld ms, plain: %lld ms, +%ld kB", static_cast<long long>(hostileMs), static_cast<long long>(plainMs), growth);
    // (the 65 536 triangles are parsed from JSON, which the plain one does not have to: allow for that, not for the evaluations)
    QVERIFY2(hostileMs < 300 + 50 * plainMs, qPrintable(QString::number(hostileMs) + " vs " + QString::number(plainMs)));
    // more distinct triangles than kMaxWarpTriangles: refused
    QJsonArray ring, ringIdx;
    ring.append(0);
    ring.append(0);
    const int n = int(kMaxWarpTriangles) + 2;
    for (int i = 1; i <= n; ++i) {
        const double a = 6.2831853 * i / (n + 1);
        ring.append(std::cos(a) * 1000);
        ring.append(std::sin(a) * 1000);
    }
    for (int i = 1; i <= int(kMaxWarpTriangles) + 1; ++i)
        for (const int k : {0, i, i + 1})
            ringIdx.append(k);
    r = OpenModel::load(view(groupWithMesh(QJsonObject{{"verts", ring}, {"indices", ringIdx}})));
    QVERIFY(!r.model);
    QVERIFY2(QString::fromStdString(r.error).contains(u"kMaxWarpTriangles"_s), r.error.c_str());
    // and a lattice of exactly kMaxWarpTriangles small triangles is accepted
    QJsonArray lattice, latticeIdx;
    const int cols = 128, rows = 64;
    for (int y = 0; y <= rows; ++y)
        for (int x = 0; x <= cols; ++x) {
            lattice.append(x * 10.0);
            lattice.append(y * 10.0);
        }
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x) {
            const int a = y * (cols + 1) + x, b = a + 1, c = a + cols + 1, d = c + 1;
            for (const int k : {a, b, c, b, d, c})
                latticeIdx.append(k);
        }
    QCOMPARE(latticeIdx.size(), int(kMaxWarpTriangles) * 3);
    timer.restart();
    r = OpenModel::load(view(groupWithMesh(QJsonObject{{"verts", lattice}, {"indices", latticeIdx}})));
    QVERIFY2(r.model, r.error.c_str());
    QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
    // thin triangles whose boxes all cover the whole mesh: the grid entries are capped
    QJsonArray thin{0, 0, 1e6, 1e6};
    QJsonArray thinIdx;
    for (int i = 1; i <= 1100; ++i) {
        thin.append(i * 100.0);
        thin.append(i * 100.0 + 1);
        for (const int k : {0, 1, i + 1})
            thinIdx.append(k);
    }
    r = OpenModel::load(view(groupWithMesh(QJsonObject{{"verts", thin}, {"indices", thinIdx}})));
    QVERIFY(!r.model);
    QVERIFY2(QString::fromStdString(r.error).contains(u"kMaxWarpCellEntries"_s), r.error.c_str());
    // many mesh groups share the one budget
    QJsonArray kids;
    for (int g = 0; g < 40; ++g) {
        QJsonObject group = nodeOf("MeshGroup", uint(100 + g), "G" + QString::number(g));
        QJsonArray tv{0, 0, 1e6, 1e6}, ti;
        for (int i = 1; i <= 300; ++i) {
            tv.append(i * 100.0);
            tv.append(i * 100.0 + 1);
            for (const int k : {0, 1, i + 1})
                ti.append(k);
        }
        group["mesh"] = QJsonObject{{"verts", tv}, {"indices", ti}};
        kids.append(group);
    }
    r = OpenModel::load(view(inpOf(puppetOf(nodeOf("Node", 1, "Root", transform(), kids)))));
    QVERIFY(!r.model);
}

void TestPuppetOpen::extremeNumbers()
{
    // anything over kMaxCoordinate is refused where it is read
    for (const double big : {1e8, 1e300}) {
        QJsonObject p = partOf(10, "A", square(0, 0, 1), 0, transform(0, 0, 0, big, 1));
        QVERIFY(!err(inpOf(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{p})))).isEmpty());
        QJsonObject q = partOf(10, "A", square(0, 0, big));
        QVERIFY(!err(inpOf(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{q})))).isEmpty());
        QJsonObject z = partOf(10, "A", square(0, 0, 1), big);
        QVERIFY(!err(inpOf(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{z})))).isEmpty());
        QVERIFY(!err(inpOf(puppetOf(smallRoot(), {param1D("P", {}, -big, big, 0)}))).isEmpty());
        QVERIFY(!err(inpOf(puppetOf(smallRoot(), {param1D("P", {binding(10, "zSort", column({0, big, 0}))})}))).isEmpty());
    }
    // the largest allowed values, chained: the numbers overflow on the way, what comes out is finite
    QJsonObject n = partOf(10 + 64, "leaf", square(0, 0, 1), 1e7, transform(1e7, 1e7, 0, 1e7, 1e7));
    for (int i = 63; i >= 1; --i)
        n = partOf(uint(10 + i), "n", square(0, 0, 1), 1e7, transform(1e7, 1e7, 0, 1e7, 1e7), QJsonArray{n});
    auto m = load(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{n}), {param1D("P", {binding(11, "transform.s.x", column({1e7, 1e7, 1e7})), binding(11, "zSort", column({1e7, 1e7, 1e7}))}, -1e7, 1e7, 0)}));
    ParameterSet v = m->parameters();
    v.set(0, 1e7);
    const Frame f = evaluate(*m, v);
    QVERIFY(!f.items.empty());
    // within kMaxOutputCoordinate (1e12): a double beyond a float's range is not converted by the renderer
    bool reachedTheBound = false;
    for (const DrawItem &i : f.items)
        for (const Vec2 &p : i.positions) {
            QVERIFY(std::isfinite(p.x) && std::isfinite(p.y));
            QVERIFY(std::abs(p.x) <= 1e12 && std::abs(p.y) <= 1e12);
            reachedTheBound = reachedTheBound || std::abs(p.x) == 1e12 || std::abs(p.y) == 1e12;
        }
    QVERIFY(reachedTheBound); // (the chain does overshoot: the clamp is what holds it)
    QVERIFY(std::isfinite(m->bounds().maxX));
    QVERIFY(std::abs(m->bounds().maxX) <= 1e12 && std::abs(m->bounds().minX) <= 1e12);
    // a parameter range as wide as allowed normalises to something finite
    auto wide = load(puppetOf(smallRoot(), {param1D("P", {binding(10, "transform.t.x", column({1, 2, 3}))}, -1e7, 1e7, 0)}));
    ParameterSet w = wide->parameters();
    w.set(0, 1e7);
    QVERIFY(std::isfinite(evaluate(*wide, w).items[0].positions[0].x));
}

void TestPuppetOpen::texturesAreRangesOfTheFile()
{
    const QByteArray bytes = inpOf(puppetOf(smallRoot()), {{0, "first"}, {1, "second!"}});
    OpenLoadResult r = OpenModel::load(view(bytes));
    QVERIFY2(r.model, r.error.c_str());
    QCOMPARE(std::string(r.model->textureBytes(0)), std::string("first"));
    QCOMPARE(std::string(r.model->textureBytes(1)), std::string("second!"));
    // taking the bytes over keeps them valid
    QByteArray copy = bytes;
    r = OpenModel::load(std::string(copy.constData(), size_t(copy.size())));
    QVERIFY(r.model);
    QCOMPARE(std::string(r.model->textureBytes(1)), std::string("second!"));
    // the total of the textures is capped
    QList<QPair<int, QByteArray>> big;
    for (int i = 0; i < 5; ++i)
        big.append({0, QByteArray(int(kMaxTextureFileBytes), 'x')});
    const QString e = err(inpOf(puppetOf(smallRoot()), big));
    QVERIFY2(e.contains(u"kMaxTotalTextureBytes"_s), qPrintable(e));
    // leftovers are said, an EXT_SECT is not
    const QByteArray plain = inpOf(puppetOf(smallRoot()));
    r = OpenModel::load(view(plain + "garbage after"));
    QVERIFY(r.model);
    QCOMPARE(r.model->info().warnings.size(), size_t(1));
    r = OpenModel::load(view(plain + "EXT_SECT" + be32(0)));
    QVERIFY(r.model);
    QVERIFY(r.model->info().warnings.empty());
}

void TestPuppetOpen::deterministic()
{
    auto m = load(puppetOf(groupPuppet(true), groupDeform({0, 0, 20, 0, 20, 0, 0, 0})));
    ParameterSet v = m->parameters();
    v.set(0, 0.37);
    const Frame a = evaluate(*m, v);
    const Frame b = evaluate(*m, v);
    QCOMPARE(a.items.size(), b.items.size());
    for (size_t i = 0; i < a.items.size(); ++i) {
        QCOMPARE(a.items[i].positions.size(), b.items[i].positions.size());
        for (size_t k = 0; k < a.items[i].positions.size(); ++k) {
            QCOMPARE(a.items[i].positions[k].x, b.items[i].positions[k].x);
            QCOMPARE(a.items[i].positions[k].y, b.items[i].positions[k].y);
        }
    }
    // evaluating another value in between does not leave anything behind (offsets are rebuilt every frame)
    ParameterSet other = m->parameters();
    other.set(0, 1);
    evaluate(*m, other);
    const Frame c = evaluate(*m, v);
    QCOMPARE(c.items[0].positions[0].x, a.items[0].positions[0].x);
}

namespace {

// The first bytes of a PNG that says it is w x h: signature, the IHDR chunk's length and name, the sizes. Nothing after (it can not be decoded; only its header is read).
QByteArray pngHeader(quint32 w, quint32 h)
{
    return QByteArray("\x89PNG\r\n\x1a\n", 8) + be32(13) + "IHDR" + be32(w) + be32(h) + QByteArray(5, '\0');
}

QByteArray tgaHeader(quint16 w, quint16 h)
{
    QByteArray b(18, '\0');
    b[2] = 2;
    b[12] = char(w & 0xFF);
    b[13] = char(w >> 8);
    b[14] = char(h & 0xFF);
    b[15] = char(h >> 8);
    b[16] = 32;
    return b;
}

} // namespace

void TestPuppetOpen::textureHeadersAreReadWithoutDecoding()
{
    const QByteArray bytes = inpOf(puppetOf(smallRoot()), {{0, pngHeader(300, 200)}, {1, tgaHeader(64, 32)}, {0, "not a png at all, but long enough to read"}, {1, "short"}});
    OpenLoadResult r = OpenModel::load(view(bytes));
    QVERIFY2(r.model, r.error.c_str());
    uint32_t w = 0, h = 0;
    QVERIFY(r.model->textureSize(0, w, h));
    QCOMPARE(w, 300u);
    QCOMPARE(h, 200u);
    QVERIFY(r.model->textureSize(1, w, h));
    QCOMPARE(w, 64u);
    QCOMPARE(h, 32u);
    QVERIFY(!r.model->textureSize(2, w, h)); // no PNG signature
    QVERIFY(!r.model->textureSize(3, w, h)); // too short for a TGA header
    QVERIFY(!r.model->textureSize(-1, w, h));
    QVERIFY(!r.model->textureSize(4, w, h));
    // huge claims are read as they are (the budget is another function's)
    const QByteArray big = inpOf(puppetOf(smallRoot()), {{0, pngHeader(4294967295u, 4294967295u)}});
    r = OpenModel::load(view(big));
    QVERIFY(r.model);
    QVERIFY(r.model->textureSize(0, w, h));
    QCOMPARE(w, 4294967295u);
}

void TestPuppetOpen::theTextureBudgetIsCheckedBeforeDecoding()
{
    std::string error;
    // 16 textures of 8192 x 4096 (each under the per-texture limit, 33.5 M pixels): 16 x 33.5 M pixels is over the total of 67 M, and none of them is decodable (header only), so a refusal here
    // is a refusal before any decode
    QList<QPair<int, QByteArray>> many;
    for (int i = 0; i < 16; ++i)
        many.append({0, pngHeader(8192, 4096)});
    OpenLoadResult r = OpenModel::load(view(inpOf(puppetOf(smallRoot()), many)));
    QVERIFY2(r.model, r.error.c_str());
    QVERIFY(!textureBudgetOk(*r.model, error));
    QVERIFY2(error.find("kMaxTotalTexturePixels") != std::string::npos, error.c_str());
    // the same two textures are fine, and so is the boundary
    r = OpenModel::load(view(inpOf(puppetOf(smallRoot()), {{0, pngHeader(8192, 4096)}, {0, pngHeader(8192, 4096)}})));
    QVERIFY(r.model);
    QVERIFY(textureBudgetOk(*r.model, error));
    r = OpenModel::load(view(inpOf(puppetOf(smallRoot()), {{0, pngHeader(8192, 4096)}, {0, pngHeader(8192, 4096)}, {0, pngHeader(1, 1)}})));
    QVERIFY(r.model);
    QVERIFY(!textureBudgetOk(*r.model, error)); // one pixel over 2 x 33554432 = 67108864
    // one texture over the per-texture limit, an empty one, and one whose header is not readable
    r = OpenModel::load(view(inpOf(puppetOf(smallRoot()), {{0, pngHeader(8192, 8192)}})));
    QVERIFY(r.model);
    QVERIFY(!textureBudgetOk(*r.model, error));
    QVERIFY2(error.find("kMaxTexturePixels") != std::string::npos, error.c_str());
    r = OpenModel::load(view(inpOf(puppetOf(smallRoot()), {{0, pngHeader(0, 100)}})));
    QVERIFY(r.model);
    QVERIFY(!textureBudgetOk(*r.model, error));
    r = OpenModel::load(view(inpOf(puppetOf(smallRoot()), {{0, "junk"}})));
    QVERIFY(r.model);
    QVERIFY(!textureBudgetOk(*r.model, error));
    QVERIFY2(error.find("header") != std::string::npos, error.c_str());
    // sizes that would overflow a 32 bit product are products in 64 bits
    r = OpenModel::load(view(inpOf(puppetOf(smallRoot()), {{0, pngHeader(4294967295u, 4294967295u)}})));
    QVERIFY(r.model);
    QVERIFY(!textureBudgetOk(*r.model, error));
}

void TestPuppetOpen::theFileCanBeLetGo()
{
    const QByteArray bytes = inpOf(puppetOf(smallRoot()), {{0, pngHeader(4, 4)}, {1, tgaHeader(2, 2)}});
    OpenLoadResult r = OpenModel::load(std::string(bytes.constData(), size_t(bytes.size())));
    QVERIFY(r.model);
    QVERIFY(r.model->fileKept());
    QVERIFY(!r.model->textureBytes(0).empty());
    r.model->releaseFile();
    QVERIFY(!r.model->fileKept());
    // nothing reads the bytes again, and nothing breaks: the ranges now point at nothing
    uint32_t w = 0, h = 0;
    QVERIFY(r.model->textureBytes(0).empty());
    QVERIFY(r.model->textureBytes(1).empty());
    QVERIFY(!r.model->textureSize(0, w, h));
    QCOMPARE(r.model->info().textures.size(), size_t(2)); // what the file said stays
    // and the model still evaluates
    ParameterSet values = r.model->parameters();
    Frame frame;
    QVERIFY(r.model->evaluate(values, r.model->parts(), frame));
    QVERIFY(!frame.items.empty());
    r.model->releaseFile(); // twice is fine
}

void TestPuppetOpen::checkReadsThePuppet()
{
    QTemporaryDir tmp;
    const QString d = tmp.path();
    QJsonObject root = nodeOf("Node", 1, "Root", transform(), QJsonArray{partOf(10, "A", square(0, 0, 10)), nodeOf("SimplePhysics", 20, "Ph")});
    testutil::writeFile(d + u"/ok/p.inp"_s, inpOf(puppetOf(root, {param1D("P", {}), param1D("Q", {})}), {{0, "a"}, {1, "b"}}));
    ManifestResult r = checkItem((d + u"/ok"_s).toStdString());
    QVERIFY(r.ok);
    QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(checkJson(r, "1"))).object();
    QCOMPARE(o.value(u"ok"_s).toBool(), true);
    QCOMPARE(o.value(u"format"_s).toString(), u"open"_s);
    QCOMPARE(o.value(u"modelVersion"_s).toInt(), 1);
    QCOMPARE(o.value(u"parameters"_s).toInt(), 2);
    QCOMPARE(o.value(u"drawables"_s).toInt(), 1);
    QCOMPARE(o.value(u"textures"_s).toInt(), 2);
    QCOMPARE(o.value(u"physics"_s).toInt(), 1);
    QVERIFY(o.value(u"motions"_s).isNull());
    QCOMPARE(o.value(u"reasons"_s).toArray().size(), 0);
    // a container that looks right and holds nonsense, a limit, and BC7
    const auto reason = [&](const QString &name, const QByteArray &bytes) {
        testutil::writeFile(d + u"/" + name + u"/p.inp"_s, bytes);
        const ManifestResult bad = checkItem((d + u"/" + name).toStdString());
        if (bad.ok || bad.reasons.empty())
            return QString();
        return QString::fromStdString(std::string(bad.reasons[0].name));
    };
    QCOMPARE(reason(u"junk"_s, inpOf(QByteArray("{not json"))), QString::fromStdString(std::string(kManifestInvalid)));
    QCOMPARE(reason(u"bc7"_s, inpOf(puppetOf(smallRoot()), {{2, "x"}})), QString::fromStdString(std::string(kUnsupportedFormat)));
    QJsonObject over = nodeOf("Node", 1, "Root", transform(), QJsonArray{partOf(10, "A", meshOf({0, 0, 1, 0, 1, 1}, {0, 1, 3}))});
    QCOMPARE(reason(u"index"_s, inpOf(puppetOf(over))), QString::fromStdString(std::string(kManifestInvalid)));
    QJsonArray v;
    for (int k = 0; k < int(kMaxVerticesPerMesh) * 2 + 2; ++k)
        v.append(0);
    QCOMPARE(reason(u"large"_s, inpOf(puppetOf(nodeOf("Node", 1, "Root", transform(), QJsonArray{partOf(10, "A", QJsonObject{{"verts", v}, {"indices", arr({0, 1, 2})}})})))),
             QString::fromStdString(std::string(kTooLarge)));
    // version 2 passes the manifest (signature only) and is refused by the deep check
    QCOMPARE(reason(u"v2"_s, QByteArray("TRNSRTS2") + QByteArray(40, 'x')), QString::fromStdString(std::string(kUnsupportedFormat)));
#ifdef MURASAKI_PUPPET_BIN
    QProcess p;
    p.start(QStringLiteral(MURASAKI_PUPPET_BIN), {u"--check"_s, d + u"/ok"_s});
    QVERIFY(p.waitForFinished(30000));
    o = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
    QCOMPARE(o.value(u"parameters"_s).toInt(), 2);
#endif
}

QTEST_GUILESS_MAIN(TestPuppetOpen)
#include "tst_puppetopen.moc"
