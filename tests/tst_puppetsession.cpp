// The session's wiring of motions, expressions, physics and pose (src/puppet/session.*): no GPU, a hand-made model, assets parsed from the formats' own JSON.
#include "puppet/assets.h"
#include "puppet/session.h"
#include "testutil.h"
#include "puppetfake.h"

#include <QTemporaryDir>
#include <QTest>

#include <cmath>

using namespace murasaki::puppet;
using namespace puppetfake;
using namespace Qt::StringLiterals;

namespace {

// X (0..2) places the square; ParamAngleX and the hair outputs are for the rigs; two parts, the first one's opacity is the square's.
std::unique_ptr<FakeModel> model()
{
    auto m = std::make_unique<FakeModel>();
    m->params.add("X", 0, 0, 2);
    m->params.add("ParamAngleX", -30, 0, 30);
    m->params.add("HairSwing", -1, 0, 1);
    m->params.add("HairSway", -1, 0, 1);
    m->partSet.add("Arm1");
    m->partSet.add("Arm2");
    m->shapes.push_back(FakeModel::quadShape());
    m->extent = {0, 0, 100, 100};
    m->fill = [](const ParameterSet &v, const PartSet &p, Frame &f) {
        DrawItem item = FakeModel::mesh(0, v.value(v.find("X")) * 20, 0, 10, 10);
        item.opacity = p.opacity(p.find("Arm1"));
        item.tint[0] = v.value(v.find("HairSwing"));
        item.tint[1] = p.opacity(p.find("Arm2"));
        f.items.push_back(item);
    };
    return m;
}

std::shared_ptr<const Motion> motionOf(const QByteArray &json)
{
    const MotionResult r = parseMotion(std::string_view(json.constData(), size_t(json.size())));
    return r.ok ? std::make_shared<const Motion>(r.motion) : nullptr;
}

// X goes 0 -> 2 in one second (linear)
std::shared_ptr<const Motion> rise(bool loop = false)
{
    return motionOf(QByteArray(R"({"Version":3,"Meta":{"Duration":1,"Fps":30,"Loop":)") + (loop ? "true" : "false") + R"(,"CurveCount":1,"TotalSegmentCount":1,"TotalPointCount":2},"Curves":[{"Target":"Parameter","Id":"X","Segments":[0,0,0,1,2]}]})");
}

std::shared_ptr<const Expression> expressionOf(const QByteArray &json)
{
    const ExpressionResult r = parseExpression(std::string_view(json.constData(), size_t(json.size())));
    return r.ok ? std::make_shared<const Expression>(r.expression) : nullptr;
}

SessionAssets assets()
{
    SessionAssets a;
    a.motionGroups["Idle"] = {{rise(true), 0, 0}};
    a.motionGroups["Tap"] = {{rise(), 0, 0}, {rise(), 0, 0}, {rise(), 0, 0}};
    a.expressions["wide"] = expressionOf(R"({"Type":"Live2D Expression","FadeInTime":0.5,"FadeOutTime":0.5,"Parameters":[{"Id":"X","Value":2,"Blend":"Overwrite"}]})");
    return a;
}

double xOf(Frame &f)
{
    return f.items.empty() ? -1 : f.items[0].positions[0].x / 20;
}

// The session cuts a step to PuppetSession::kMaxStep: time passes in small ones.
void run(PuppetSession &s, double seconds, double step = 0.05)
{
    for (double t = 0; t < seconds - 1e-9; t += step)
        s.step(step);
}

double evalX(PuppetSession &s)
{
    static double last = 0; // (an unchanged evaluation leaves the frame empty: the picture is the last one)
    Frame f;
    bool changed = false;
    s.evaluate(f, changed);
    if (changed)
        last = xOf(f);
    return last;
}

} // namespace

class TestPuppetSession : public QObject
{
    Q_OBJECT
private slots:
    void aMotionPlaysOnCommand();
    void badCommandsChangeNothing();
    void prioritiesDecideWhatPlays();
    void anExpressionFadesInAndOut();
    void physicsSwingsAndSettles();
    void aRigThatCannotBeBoundIsSkippedAndNamed();
    void poseShowsOneOfAGroup();
    void animatedFollowsWhatMoves();
    void theSameSeedPicksTheSameMotions();
    void aSessionWithoutAssetsRefusesTheCommands();
    void theAssetsOfAManifestAreLoadedAndTheBadOnesNamed();
    void aClickPlaysTheGroupOfTheHitAreaItFallsOn();
    void aClickCommandIsValidatedAndTaken();
    void aFileNamedManyTimesIsReadOnceAndTheTotalIsBudgeted();
};

void TestPuppetSession::aMotionPlaysOnCommand()
{
    PuppetSession s(model(), 1, assets());
    s.setBreath(false);
    s.setBlink(false);
    QCOMPARE(s.motionGroupCount(), size_t(2));
    QCOMPARE(evalX(s), 0.0);
    std::string error;
    QVERIFY2(s.command(R"({"play":{"group":"Tap","index":1,"priority":"normal"}})", error), error.c_str());
    QVERIFY(s.motionPlaying());
    QCOMPARE(s.motionTag(), std::string("Tap/1"));
    run(s, 0.5);
    const double half = evalX(s);
    QVERIFY2(std::abs(half - 1.0) < 0.1, qPrintable(QString::number(half)));
    run(s, 0.45);
    QVERIFY2(evalX(s) > 1.8, "the motion reaches its end value");
    run(s, 2); // it ends, its fade out is 0, and the host's value (0) is back
    QVERIFY(!s.motionPlaying());
    QCOMPARE(evalX(s), 0.0);
    // the host's own value is what the motion starts from and returns to
    QVERIFY(s.command(R"({"set":{"X":0.5}})", error));
    QCOMPARE(evalX(s), 0.5);
    // "stop" ends a playing motion at once
    QVERIFY(s.command(R"({"play":{"group":"Idle","priority":"force"}})", error));
    run(s, 0.4);
    QVERIFY(s.motionPlaying());
    QVERIFY(s.command(R"({"stop":true})", error));
    QVERIFY(!s.motionPlaying());
    QCOMPARE(evalX(s), 0.5);
}

void TestPuppetSession::badCommandsChangeNothing()
{
    PuppetSession s(model(), 1, assets());
    std::string error;
    for (const char *bad : {R"({"play":{"group":"Nope"}})", R"({"play":{}})", R"({"play":"Tap"})", R"({"play":{"group":"Tap","index":3}})", R"({"play":{"group":"Tap","index":-1}})",
                            R"({"play":{"group":"Tap","index":0.5}})", R"({"play":{"group":"Tap","priority":"urgent"}})", R"({"expression":"nope"})", R"({"expression":3})",
                            R"({"set":{"X":1},"play":{"group":"Nope"}})", R"({"stop":"yes"})"}) {
        error.clear();
        QVERIFY2(!s.command(bad, error), bad);
        QVERIFY2(!error.empty(), bad);
        QVERIFY2(!s.motionPlaying(), bad);
        QVERIFY2(evalX(s) == 0.0, bad); // not even the valid "set" next to a refused play was applied
    }
}

void TestPuppetSession::prioritiesDecideWhatPlays()
{
    PuppetSession s(model(), 1, assets());
    s.setBreath(false);
    s.setBlink(false);
    std::string error;
    QVERIFY(s.command(R"({"play":{"group":"Tap","index":0}})", error));
    QCOMPARE(s.motionTag(), std::string("Tap/0"));
    // an idle behind a playing normal is refused, quietly
    QVERIFY(s.command(R"({"play":{"group":"Idle","priority":"idle"}})", error));
    QCOMPARE(s.motionTag(), std::string("Tap/0"));
    // a forced one takes over at once
    QVERIFY(s.command(R"({"play":{"group":"Tap","index":2,"priority":"force"}})", error));
    QCOMPARE(s.motionTag(), std::string("Tap/2"));
}

void TestPuppetSession::anExpressionFadesInAndOut()
{
    PuppetSession s(model(), 1, assets());
    s.setBreath(false);
    s.setBlink(false);
    std::string error;
    QVERIFY(s.command(R"({"expression":"wide"})", error));
    QVERIFY(s.animated());
    QCOMPARE(evalX(s), 0.0); // (no time has passed: weight 0)
    run(s, 0.25);
    const double half = evalX(s);
    QVERIFY2(half > 0.3 && half < 1.7, qPrintable(QString::number(half)));
    run(s, 0.35);
    QCOMPARE(evalX(s), 2.0);
    QVERIFY(s.command(R"({"expression":null})", error));
    run(s, 1.2);
    QCOMPARE(evalX(s), 0.0); // faded out, the layer dropped
}

namespace {

SessionAssets hair()
{
    SessionAssets a;
    const QByteArray json = R"({"Version":3,"Meta":{"Fps":60,"EffectiveForces":{"Gravity":{"X":0,"Y":-1},"Wind":{"X":0,"Y":0}}},"PhysicsSettings":[{"Id":"Hair",
        "Input":[{"Source":{"Target":"Parameter","Id":"ParamAngleX"},"Weight":100,"Type":"X","Reflect":false}],
        "Output":[{"Destination":{"Target":"Parameter","Id":"HairSwing"},"VertexIndex":1,"Scale":1,"Weight":100,"Type":"Angle","Reflect":false}],
        "Vertices":[{"Position":{"X":0,"Y":0},"Mobility":0.9,"Delay":0.8,"Acceleration":1.2,"Radius":0},{"Position":{"X":0,"Y":-3},"Mobility":0.9,"Delay":0.8,"Acceleration":1.2,"Radius":3},{"Position":{"X":0,"Y":-6},"Mobility":0.9,"Delay":0.8,"Acceleration":1.2,"Radius":3}],
        "Normalization":{"Position":{"Minimum":-10,"Default":0,"Maximum":10},"Angle":{"Minimum":-30,"Default":0,"Maximum":30}}}]})";
    const PhysicsResult r = parsePhysics(std::string_view(json.constData(), size_t(json.size())));
    if (r.ok)
        a.physics = r.settings;
    return a;
}

double swing(PuppetSession &s)
{
    Frame f;
    bool changed = false;
    s.evaluate(f, changed);
    return f.items.empty() ? 0 : f.items[0].tint[0];
}

} // namespace

void TestPuppetSession::physicsSwingsAndSettles()
{
    PuppetSession s(model(), 1, hair());
    s.setBreath(false);
    s.setBlink(false);
    QVERIFY(s.hasPhysics());
    QVERIFY(s.physicsSkipped().empty());
    QCOMPARE(swing(s), 0.0);
    QVERIFY(!s.animated());
    std::string error;
    QVERIFY(s.command(R"({"set":{"ParamAngleX":30}})", error));
    double peak = 0;
    bool animatedWhileSwinging = false;
    for (int i = 0; i < 120; ++i) {
        s.step(1.0 / 60);
        peak = std::max(peak, std::abs(swing(s)));
        animatedWhileSwinging = animatedWhileSwinging || s.animated();
    }
    QVERIFY2(peak > 0.05, qPrintable(QString::number(peak)));
    QVERIFY(animatedWhileSwinging);
    // the head comes back and the hair settles: the session stops being animated once it has
    QVERIFY(s.command(R"({"set":{"ParamAngleX":0}})", error));
    bool settled = false;
    for (int i = 0; i < 60 * 20 && !settled; ++i) {
        s.step(1.0 / 60);
        swing(s);
        settled = !s.animated();
    }
    QVERIFY2(settled, "the rigs come to rest");
    QVERIFY(std::abs(swing(s)) < 0.05);
    // a long step does not explode
    s.step(100);
    QVERIFY(std::isfinite(swing(s)));
}

void TestPuppetSession::aRigThatCannotBeBoundIsSkippedAndNamed()
{
    auto m = std::make_unique<FakeModel>();
    m->params.add("X", 0, 0, 2); // no ParamAngleX, no HairSwing
    m->shapes.push_back(FakeModel::quadShape());
    m->fill = [](const ParameterSet &, const PartSet &, Frame &f) { f.items.push_back(FakeModel::mesh(0, 0, 0, 10, 10)); };
    PuppetSession s(std::move(m), 1, hair());
    QVERIFY(s.hasPhysics());
    QCOMPARE(s.physicsSkipped().size(), size_t(1));
    QVERIFY(QString::fromStdString(s.physicsSkipped()[0]).contains(u"Hair"_s));
    QVERIFY(!s.animated());
}

void TestPuppetSession::poseShowsOneOfAGroup()
{
    SessionAssets a;
    const QByteArray json = R"({"Type":"Live2D Pose","FadeInTime":0.5,"Groups":[[{"Id":"Arm1","Link":[]},{"Id":"Arm2","Link":[]}]]})";
    const PoseResult r = parsePose(std::string_view(json.constData(), size_t(json.size())));
    QVERIFY2(r.ok, r.error.c_str());
    a.pose = r.pose;
    PuppetSession s(model(), 1, a);
    s.setBreath(false);
    s.setBlink(false);
    Frame f, latest;
    bool changed = false;
    for (int i = 0; i < 40; ++i) {
        s.step(0.05);
        QVERIFY(s.evaluate(f, changed));
        if (changed)
            latest = f;
    }
    f = latest;
    // both parts started opaque: the first stays, the other is faded out by the pose
    QVERIFY2(f.items[0].opacity > 0.99, qPrintable(QString::number(f.items[0].opacity)));
    QVERIFY2(f.items[0].tint[1] < 0.05, qPrintable(QString::number(f.items[0].tint[1])));
}

void TestPuppetSession::animatedFollowsWhatMoves()
{
    PuppetSession s(model(), 1, assets());
    s.setBreath(false);
    s.setBlink(false);
    QVERIFY(!s.animated());
    std::string error;
    QVERIFY(s.command(R"({"play":{"group":"Tap","index":0}})", error));
    QVERIFY(s.animated());
    run(s, 6); // the motion ends; its tail passes
    QVERIFY(!s.animated());
    // an idle loop keeps it animated for as long as it plays
    QVERIFY(s.command(R"({"play":{"group":"Idle","priority":"idle"}})", error));
    run(s, 5);
    QVERIFY(s.motionPlaying());
    QVERIFY(s.animated());
}

void TestPuppetSession::theSameSeedPicksTheSameMotions()
{
    const auto picks = [](uint64_t seed) {
        PuppetSession s(model(), seed, assets());
        std::string error, tags;
        for (int i = 0; i < 12; ++i) {
            s.command(R"({"play":{"group":"Tap","priority":"force"}})", error);
            tags += s.motionTag() + ";";
        }
        return tags;
    };
    QCOMPARE(picks(5), picks(5));
    QVERIFY(picks(5) != picks(6));
    const std::string many = picks(7);
    QVERIFY(many.find("Tap/0") != std::string::npos && many.find("Tap/1") != std::string::npos && many.find("Tap/2") != std::string::npos);
}

void TestPuppetSession::aSessionWithoutAssetsRefusesTheCommands()
{
    PuppetSession s(model(), 1);
    std::string error;
    QVERIFY(!s.command(R"({"play":{"group":"Idle"}})", error));
    QVERIFY(!s.command(R"({"expression":"wide"})", error));
    QVERIFY(s.command(R"({"expression":null,"stop":true})", error)); // (nothing to clear is fine)
    QCOMPARE(s.motionGroupCount(), size_t(0));
    QVERIFY(!s.hasPhysics());
}

namespace {

// A body (0,0)-(100,100) and a head (30,0)-(70,30), the head's hit area "TapHead"; groups TapHead, TapBody and Tap. `headOnTop`: the head is drawn after the body.
struct ClickRig {
    PuppetSession session;
    Frame frame;
    ViewTransform view;
    explicit ClickRig(bool headOnTop)
        : session(clickModel(headOnTop), 1, clickAssets())
    {
        bool changed = false;
        session.evaluate(frame, changed);
        view = fitBounds(session.model().bounds(), 100, 100);
    }
    static std::unique_ptr<FakeModel> clickModel(bool headOnTop)
    {
        auto m = std::make_unique<FakeModel>();
        m->params.add("X", 0, 0, 2);
        m->shapes = {FakeModel::quadShape(), FakeModel::quadShape()};
        m->ids = {"body", "head"};
        m->extent = {0, 0, 100, 100};
        m->fill = [headOnTop](const ParameterSet &, const PartSet &, Frame &f) {
            const DrawItem body = FakeModel::mesh(0, 0, 0, 100, 100), head = FakeModel::mesh(1, 30, 0, 40, 30);
            if (headOnTop)
                f.items = {body, head};
            else
                f.items = {head, body}; // the body covers the head
        };
        return m;
    }
    static SessionAssets clickAssets()
    {
        SessionAssets a;
        const MotionAsset motion{rise(), 0, 0};
        a.motionGroups["TapHead"] = {motion};
        a.motionGroups["TapBody"] = {motion, motion};
        a.motionGroups["Tap"] = {motion};
        a.hitAreas = {{"head", "TapHead"}};
        return a;
    }
    std::string click(double u, double v, std::vector<std::string> fallback = {}) { return session.applyClick({u, v, std::move(fallback)}, frame, view, 100, 100); }
};

} // namespace

void TestPuppetSession::aClickPlaysTheGroupOfTheHitAreaItFallsOn()
{
    for (const bool headOnTop : {true, false}) {
        ClickRig rig(headOnTop);
        QVERIFY(rig.frame.items.size() == 2);
        QCOMPARE(rig.click(0.5, 0.1, {"TapBody", "Tap"}), std::string("area:TapHead")); // on the head
        QVERIFY(rig.session.motionPlaying());
        QVERIFY(rig.session.motionTag().rfind("TapHead/", 0) == 0);
        QCOMPARE(rig.session.lastClick(), std::string("area:TapHead"));
        // a hit area is tested where its drawable is drawn, whatever is drawn over it (Cubism does so): with the body over the head the same point still hits the head
    }
    ClickRig rig(true);
    QCOMPARE(rig.click(0.5, 0.7, {"TapBody", "Tap"}), std::string("fallback:TapBody"));      // the body: no hit area, the first fallback the model has
    QVERIFY(rig.session.motionTag().rfind("TapBody/", 0) == 0);
    QCOMPARE(rig.click(0.5, 0.7, {"Nope", "Tap"}), std::string("fallback:Tap"));              // the first one it has
    QCOMPARE(rig.click(0.5, 0.7, {"Nope"}), std::string());                                    // none: nothing plays
    QCOMPARE(rig.click(0.5, 0.7), std::string());
    QCOMPARE(rig.session.lastClick(), std::string());
    // the corners of the head and a point beside it
    QCOMPARE(rig.click(0.31, 0.01, {}), std::string("area:TapHead"));
    QCOMPARE(rig.click(0.9, 0.1, {}), std::string());
    // a click on a head that is not drawn (invisible) is not a click on it
    rig.frame.items[1].opacity = 0;
    QCOMPARE(rig.click(0.5, 0.1, {}), std::string());
    // a model without the group of the area plays nothing for it
    PuppetSession plain(ClickRig::clickModel(true), 1, SessionAssets{});
    QCOMPARE(plain.applyClick({0.5, 0.1, {"Tap"}}, rig.frame, rig.view, 100, 100), std::string());
    // numbers that are not a picture do not crash it
    QCOMPARE(rig.session.applyClick({0.5, 0.1, {}}, rig.frame, ViewTransform{0, 0, 0, 0}, 100, 100), std::string());
    QCOMPARE(rig.session.applyClick({0.5, 0.1, {}}, rig.frame, rig.view, 0, 0), std::string());
    // a head collapsed to a point, or with positions that are not numbers, covers nothing (not every point)
    ClickRig flat(true);
    for (auto &position : flat.frame.items[1].positions)
        position = {50, 10};
    QCOMPARE(flat.click(0.5, 0.1, {}), std::string());
    QCOMPARE(flat.click(0.9, 0.9, {}), std::string());
    for (auto &position : flat.frame.items[1].positions)
        position = {std::nan(""), std::nan("")};
    QCOMPARE(flat.click(0.5, 0.1, {}), std::string());
    // a hit area with no id does not match the drawables that have none
    auto noIds = ClickRig::clickModel(true);
    noIds->ids = {"", ""};
    SessionAssets assets = ClickRig::clickAssets();
    assets.hitAreas = {{"", "TapHead"}};
    PuppetSession anonymous(std::move(noIds), 1, std::move(assets));
    Frame shown;
    bool changed = false;
    anonymous.evaluate(shown, changed);
    QCOMPARE(anonymous.applyClick({0.5, 0.1, {}}, shown, rig.view, 100, 100), std::string());
}

void TestPuppetSession::aClickCommandIsValidatedAndTaken()
{
    ClickRig rig(true);
    std::string error;
    QVERIFY(!rig.session.takeClick());
    for (const char *bad : {R"({"click":1})", R"({"click":{}})", R"({"click":{"u":0.5}})", R"({"click":{"u":2,"v":0.5}})", R"({"click":{"u":-0.1,"v":0.5}})", R"({"click":{"u":"a","v":0.5}})",
                            R"({"click":{"u":0.5,"v":0.5,"fallback":"Tap"}})", R"({"click":{"u":0.5,"v":0.5,"fallback":[1]}})", R"({"click":{"u":0.5,"v":0.5,"fallback":["a","b","c","d","e"]}})"}) {
        QVERIFY2(!rig.session.command(bad, error), bad);
        QVERIFY2(!rig.session.takeClick(), bad); // refused: nothing is left to apply
    }
    QVERIFY(rig.session.command(R"({"click":{"u":0.25,"v":0.75,"fallback":["TapBody","Tap"]}})", error));
    const auto click = rig.session.takeClick();
    QVERIFY(click);
    QCOMPARE(click->u, 0.25);
    QCOMPARE(click->v, 0.75);
    QCOMPARE(click->fallback, (std::vector<std::string>{"TapBody", "Tap"}));
    QVERIFY(!rig.session.takeClick()); // taken once
    // a refused command next to a click leaves no click behind
    QVERIFY(!rig.session.command(R"({"click":{"u":0.5,"v":0.5},"play":{"group":"Nope"}})", error));
    QVERIFY(!rig.session.takeClick());
}

void TestPuppetSession::theAssetsOfAManifestAreLoadedAndTheBadOnesNamed()
{
    QTemporaryDir dir;
    const QString d = dir.path();
    const QByteArray motion = R"({"Version":3,"Meta":{"Duration":1,"Fps":30,"Loop":false,"CurveCount":1,"TotalSegmentCount":1,"TotalPointCount":2},"Curves":[{"Target":"Parameter","Id":"X","Segments":[0,0,0,1,2]}]})";
    testutil::writeFile(d + u"/m.model3.json"_s, R"({"Version":3,"FileReferences":{"Moc":"m.moc3","Textures":["t0.png"],"Physics":"p.physics3.json","Pose":"m.pose3.json",
        "Expressions":[{"Name":"wide","File":"wide.exp3.json"},{"Name":"broken","File":"broken.exp3.json"}],
        "Motions":{"Tap":[{"File":"a.motion3.json","FadeInTime":0.2,"FadeOutTime":0.3},{"File":"missing.motion3.json"},{"File":"bad.motion3.json"}],"Gone":[{"File":"bad.motion3.json"}]}},
        "HitAreas":[{"Id":"HitAreaBody","Name":"TapBody"}]})");
    testutil::writeFile(d + u"/missing.motion3.json"_s, motion); // (there when the manifest is checked, gone when it is read: the item changed under us)
    testutil::writeFile(d + u"/m.moc3"_s, "MOC3");
    testutil::writeFile(d + u"/t0.png"_s, "png");
    testutil::writeFile(d + u"/a.motion3.json"_s, motion);
    testutil::writeFile(d + u"/bad.motion3.json"_s, "{not json");
    testutil::writeFile(d + u"/wide.exp3.json"_s, R"({"Type":"Live2D Expression","Parameters":[{"Id":"X","Value":2,"Blend":"Overwrite"}]})");
    testutil::writeFile(d + u"/broken.exp3.json"_s, "[1,2");
    testutil::writeFile(d + u"/p.physics3.json"_s, "nope");
    testutil::writeFile(d + u"/m.pose3.json"_s, R"({"Type":"Live2D Pose","Groups":[[{"Id":"Arm1","Link":[]},{"Id":"Arm2","Link":[]}]]})");
    const ManifestResult manifest = loadManifest(d.toStdString());
    QVERIFY2(manifest.ok, manifest.reasons.empty() ? "" : manifest.reasons[0].text().c_str());
    QFile::remove(d + u"/missing.motion3.json"_s);
    const AssetsResult loaded = loadSessionAssets(manifest.manifest);
    QCOMPARE(loaded.assets.motionGroups.size(), size_t(1));          // "Gone" had nothing readable: no group
    QCOMPARE(loaded.assets.motionGroups.at("Tap").size(), size_t(1)); // the good motion of three
    QCOMPARE(loaded.assets.motionGroups.at("Tap")[0].fadeIn, 0.2);
    QCOMPARE(loaded.assets.motionGroups.at("Tap")[0].fadeOut, 0.3);
    QCOMPARE(loaded.assets.expressions.size(), size_t(1));
    QVERIFY(loaded.assets.pose.has_value());
    QCOMPARE(loaded.assets.hitAreas.size(), size_t(1)); // the manifest's hit areas come along
    QCOMPARE(loaded.assets.hitAreas[0].drawableId, std::string("HitAreaBody"));
    QCOMPARE(loaded.assets.hitAreas[0].name, std::string("TapBody"));
    QVERIFY(!loaded.assets.physics.has_value());
    // what was left out is named, once per file, never a refusal of the item
    QStringList named;
    for (const Problem &p : loaded.degraded)
        named << QString::fromStdString(p.text());
    for (const QString &expected : {u"puppet: motion skipped: missing.motion3.json"_s, u"puppet: motion skipped: bad.motion3.json"_s, u"puppet: motion skipped: broken.exp3.json"_s,
                                    u"puppet: physics rig skipped: p.physics3.json"_s})
        QVERIFY2(named.contains(expected), qPrintable(expected + u" in "_s + named.join(u"; "_s)));
    // and a session takes it
    PuppetSession s(model(), 1, loaded.assets);
    std::string error;
    QVERIFY2(s.command(R"({"play":{"group":"Tap"}})", error), error.c_str());
    QVERIFY(s.command(R"({"expression":"wide"})", error));
    QVERIFY(!s.command(R"({"play":{"group":"Gone"}})", error));
}

void TestPuppetSession::aFileNamedManyTimesIsReadOnceAndTheTotalIsBudgeted()
{
    QTemporaryDir dir;
    const QString d = dir.path();
    const QByteArray motion = R"({"Version":3,"Meta":{"Duration":1,"Fps":30,"Loop":false,"CurveCount":1,"TotalSegmentCount":1,"TotalPointCount":2},"Curves":[{"Target":"Parameter","Id":"X","Segments":[0,0,0,1,2]}]})";
    QByteArray refs;
    for (int i = 0; i < 40; ++i)
        refs += (i ? "," : "") + QByteArray(R"({"File":"same.motion3.json"})");
    QByteArray others;
    for (int i = 0; i < 6; ++i) {
        testutil::writeFile(d + u"/m%1.motion3.json"_s.arg(i), motion);
        others += QByteArray(i ? "," : "") + R"({"File":"m)" + QByteArray::number(i) + R"(.motion3.json"})";
    }
    testutil::writeFile(d + u"/same.motion3.json"_s, motion);
    testutil::writeFile(d + u"/m.model3.json"_s, R"({"Version":3,"FileReferences":{"Moc":"m.moc3","Textures":["t0.png"],"Motions":{"Same":[)" + refs + R"(],"Many":[)" + others + R"(]}}})");
    testutil::writeFile(d + u"/m.moc3"_s, "MOC3");
    testutil::writeFile(d + u"/t0.png"_s, "png");
    const ManifestResult manifest = loadManifest(d.toStdString());
    QVERIFY2(manifest.ok, manifest.reasons.empty() ? "" : manifest.reasons[0].text().c_str());
    // the same file forty times: one object, read once (its size is all the budget there is)
    const AssetsResult once = loadSessionAssets(manifest.manifest, static_cast<std::int64_t>(motion.size()) + 1);
    QCOMPARE(once.assets.motionGroups.at("Same").size(), size_t(40));
    for (const MotionAsset &asset : once.assets.motionGroups.at("Same"))
        QCOMPARE(asset.motion.get(), once.assets.motionGroups.at("Same")[0].motion.get());
    // six files with room for two: the rest are left out, and the budget is named once
    const AssetsResult limited = loadSessionAssets(manifest.manifest, static_cast<std::int64_t>(motion.size()) * 2 + 1);
    QVERIFY(limited.assets.motionGroups.at("Many").size() < 6);
    QVERIFY(limited.assets.motionGroups.at("Many").size() >= 2);
    int named = 0;
    for (const Problem &p : limited.degraded)
        named += p.text() == "puppet: too large: kMaxTotalAssetBytes";
    QCOMPARE(named, 1);
    // with the real budget everything is read
    QCOMPARE(loadSessionAssets(manifest.manifest).assets.motionGroups.at("Many").size(), size_t(6));
}

QTEST_GUILESS_MAIN(TestPuppetSession)
#include "tst_puppetsession.moc"
