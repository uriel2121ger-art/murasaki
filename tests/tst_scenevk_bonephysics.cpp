// murasaki-scene-vk bone physics: the constraint JSON of a bone, the spring simulation behind the skinning matrices (rest, return
// to the pose, gravity, limits, wind and its seed) and the script impulses. Models are written by the tests (puppetfixtures.h).

#include "assets/vfs.h"
#include "puppet/bonephysics.h"
#include "puppet/mdl.h"
#include "puppetfixtures.h"
#include "render/puppetdraw.h"
#include "render/scenescripts.h"
#include "scene/scene.h"
#include "scenevkfixtures.h"
#include "script/scripthost.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <algorithm>
#include <cmath>

using namespace murasaki::scene;

namespace {

using namespace puppetfix;

constexpr double kFrame = 1.0 / 60.0;

// the editor's JSON for a bone that rotates (about z) or translates, as the library's models carry it
std::string springNote(bool rotation, bool translation, const std::string &extra = "")
{
    return std::string(R"({"a":"1 0 0","gd":"0 -1 0","ge":false,"ik":false,"la":false,"lamax":"0 0 3.1415","lamin":"0 0 -3.1415","lt":false,"ltmax":100,"m":20,"r":)")
        + (rotation ? "true" : "false") + R"(,"re":true,"rf":20,"ri":30,"rs":200,"s":0,"se":false,"t":)" + (translation ? "true" : "false")
        + R"(,"tf":20,"ti":30,"tm":200,"tp":"100 0 0","ts":200)" + extra + "}";
}

// `note` with the first `from` written as `to`
std::string swapped(std::string note, const std::string &from, const std::string &to)
{
    const size_t at = note.find(from);
    if (at != std::string::npos) {
        note.replace(at, from.size(), to);
    }
    return note;
}

// root, then a simulated child 10 units along x; one animation that holds both still
std::vector<uint8_t> hairModel(const std::string &note)
{
    return makeMdl({{"root", -1, 0, 0}, {"hair", 0, 10, 0, note}, {"tip", 1, 10, 0}},
                   {{7, "idle", "loop", 30.0f, 2, {{{frame(0, 0), frame(0, 0)}}, {{frame(10, 0), frame(10, 0)}}, {{frame(10, 0), frame(10, 0)}}}}});
}

std::optional<Puppet> load(const std::vector<uint8_t> &mdl)
{
    std::string error;
    auto puppet = parsePuppet(mdl, &error);
    if (!puppet) {
        qWarning("%s", error.c_str());
    }
    return puppet;
}

const std::vector<PuppetLayerState> kIdle{{7, 1.0f, 1.0f}};

// the angle about z that bone `bone`'s skinning matrix turns by
float turnOf(const std::vector<float> &bones, size_t bone)
{
    const float *m = bones.data() + bone * 16;
    return std::atan2(m[1], m[0]);
}

// runs `frames` frames of 1/60 s from `start`, returning the matrices of the last
std::vector<float> run(const Puppet &puppet, BonePhysicsState &state, const BoneWorld &world, int frames, double start = 0.0)
{
    std::vector<float> out;
    for (int f = 0; f <= frames; ++f) {
        puppetBoneMatrices(puppet, kIdle, start + f * kFrame, out, &state, &world);
    }
    return out;
}

} // namespace

class TstSceneVkBonePhysics : public QObject
{
    Q_OBJECT

private slots:
    void aConstraintIsRead()
    {
        const auto c = parseBoneConstraint(springNote(true, false, R"(,"rax":false,"ray":false,"raz":true)"));
        QVERIFY(c.has_value());
        QVERIFY(c->rotation);
        QVERIFY(!c->translation);
        QCOMPARE(c->rotationStiffness, 200.0f);
        QCOMPARE(c->rotationDamping, 20.0f);
        QVERIFY(c->rotationAxes[2] && !c->rotationAxes[0] && !c->rotationAxes[1]);
        QVERIFY(!c->gravity);
        QCOMPARE(c->gravityDirection.y, -1.0f);
        QVERIFY(c->simulated());
        QVERIFY(!c->ik);

        const auto full = parseBoneConstraint(R"({"r":false,"t":true,"ts":300,"tf":5,"ge":true,"gd":"0.6 -0.8 0","la":true,"lamin":"0 0 -0.5","lamax":"0 0 0.25","lt":true,"ltmax":12,"ik":true})");
        QVERIFY(full.has_value());
        QVERIFY(full->translation && !full->rotation);
        QCOMPARE(full->translationStiffness, 300.0f);
        QCOMPARE(full->translationDamping, 5.0f);
        QVERIFY(full->gravity);
        QVERIFY(std::fabs(full->gravityDirection.x - 0.6f) < 1e-5f);
        QVERIFY(full->limitAngles);
        QCOMPARE(full->angleMin.z, -0.5f);
        QCOMPARE(full->angleMax.z, 0.25f);
        QVERIFY(full->limitTranslation);
        QCOMPARE(full->translationLimit, 12.0f);
        QVERIFY(full->ik);
        QVERIFY(!full->ikControlPoint); // `ikcp` absent
        const auto chain = parseBoneConstraint(R"({"ik":true,"ikd":2,"ikcp":true,"ikcpn":1,"r":true})");
        QVERIFY(chain.has_value());
        QVERIFY(chain->ik && chain->ikControlPoint);
        QCOMPARE(chain->ikDepth, 2);
        const auto flat = parseBoneConstraint(R"({"ik":false,"ikd":2,"ikcp":true,"r":true})");
        QVERIFY(flat.has_value());
        QVERIFY(!flat->ikControlPoint && flat->ikDepth == 0); // only a chain's end carries them
    }

    void aConstraintWithNothingToSimulateIsNotSimulated()
    {
        // the 1371 plain bones of the library: no flags at all
        const auto plain = parseBoneConstraint(R"j({"a":null,"lamax":null,"lamin":null,"rax":null,"ray":null,"raz":null,"s":null,"tm":100.0,"tp":"-nan(ind) -nan(ind) -nan(ind)"})j");
        QVERIFY(plain.has_value());
        QVERIFY(!plain->simulated());
        QVERIFY(!parseBoneConstraint("").has_value());
        QVERIFY(!parseBoneConstraint("not json").has_value());
        QVERIFY(!parseBoneConstraint("[1,2]").has_value());
        QVERIFY(!parseBoneConstraint("{\"r\":tru").has_value());
    }

    void hostileValuesAreClamped()
    {
        const auto c = parseBoneConstraint(R"({"r":true,"rs":1e300,"rf":-5,"ts":"x","gd":"0 0 0","a":"nan nan nan","la":true,"lamin":"0 0 -1e30","lamax":"0 0 1e30","lt":true,"ltmax":1e30})");
        QVERIFY(c.has_value());
        QCOMPARE(c->rotationStiffness, kMaxBoneStiffness);
        QCOMPARE(c->rotationDamping, 0.0f);
        QCOMPARE(c->translationStiffness, 200.0f); // a string where a number goes: the default
        QCOMPARE(c->gravityDirection.y, -1.0f);    // a zero vector: the default
        QCOMPARE(c->axis.x, 1.0f);
        QVERIFY(c->angleMin.z >= -6.3f && c->angleMax.z <= 6.3f);
        QVERIFY(c->translationLimit <= 1e4f);
    }

    void theReaderCountsAndKeepsTheSpringBones()
    {
        const auto puppet = load(hairModel(springNote(true, false, R"(,"ik":false)")));
        QVERIFY(puppet.has_value());
        QCOMPARE(puppet->simulatedBones, size_t{1});
        QCOMPARE(puppet->ikBones, size_t{0});
        QVERIFY(puppet->bones[1].constraint.rotation);
        QVERIFY(!puppet->bones[0].constraint.simulated());
        QCOMPARE(puppet->bones[1].constraint.length, 10.0f); // the distance to its child
        const auto ik = load(hairModel(R"({"r":false,"t":false,"ik":true})"));
        QVERIFY(ik.has_value());
        QCOMPARE(ik->simulatedBones, size_t{0});
        QCOMPARE(ik->ikBones, size_t{1});
        // a note that is not JSON leaves the model readable
        const auto broken = load(hairModel("{oops"));
        QVERIFY(broken.has_value());
        QCOMPARE(broken->simulatedBones, size_t{0});
    }

    void withoutForcesTheBonesStayOnTheAnimatedPose()
    {
        const auto puppet = load(hairModel(springNote(true, true)));
        QVERIFY(puppet.has_value());
        BonePhysicsState state(puppet->bones.size(), 1);
        const BoneWorld world; // gravity is the bone's flag, which is off; no wind
        const auto reference = puppetBoneMatrices(*puppet, kIdle, 0.0);
        for (int f = 0; f < 240; ++f) {
            std::vector<float> out;
            puppetBoneMatrices(*puppet, kIdle, f * kFrame, out, &state, &world);
            for (size_t i = 0; i < out.size(); ++i) {
                QVERIFY2(std::fabs(out[i] - reference[i]) < 1e-5f, qPrintable(QStringLiteral("frame %1 value %2").arg(f).arg(i)));
            }
        }
    }

    void aKickedBoneSwingsAndReturnsToThePose()
    {
        const auto puppet = load(hairModel(springNote(true, false)));
        QVERIFY(puppet.has_value());
        BonePhysicsState state(puppet->bones.size(), 1);
        const BoneWorld world;
        run(*puppet, state, world, 2);
        state.applyImpulse(1, {0, 0, 0}, {0, 0, 6});
        float largest = 0.0f;
        std::vector<float> out;
        for (int f = 3; f < 600; ++f) {
            puppetBoneMatrices(*puppet, kIdle, f * kFrame, out, &state, &world);
            largest = std::max(largest, std::fabs(turnOf(out, 1)));
            if (f == 10) {
                QVERIFY(std::fabs(turnOf(out, 1)) > 0.05f); // it moved
            }
        }
        QVERIFY(largest > 0.1f);
        QVERIFY(largest < 1.0f);                      // a spring, not a runaway
        QVERIFY(std::fabs(turnOf(out, 1)) < 1e-3f);   // ten seconds later it is back
        QVERIFY(std::fabs(turnOf(out, 0)) < 1e-6f);   // the root, which does not simulate, never moved
    }

    void aTranslatingBoneTakesALinearImpulseAndReturns()
    {
        const auto puppet = load(hairModel(springNote(false, true)));
        QVERIFY(puppet.has_value());
        BonePhysicsState state(puppet->bones.size(), 1);
        const BoneWorld world;
        run(*puppet, state, world, 2);
        state.applyImpulse(1, {0, 50, 0}, {0, 0, 0});
        std::vector<float> out;
        float farthest = 0.0f;
        for (int f = 3; f < 600; ++f) {
            puppetBoneMatrices(*puppet, kIdle, f * kFrame, out, &state, &world);
            farthest = std::max(farthest, std::fabs(out[16 + 13]));
        }
        QVERIFY(farthest > 0.5f);
        QVERIFY(std::fabs(out[16 + 13]) < 1e-3f);
    }

    void resetPutsABoneBackAtRest()
    {
        const auto puppet = load(hairModel(springNote(true, false)));
        QVERIFY(puppet.has_value());
        BonePhysicsState state(puppet->bones.size(), 1);
        const BoneWorld world;
        run(*puppet, state, world, 2);
        state.applyImpulse(1, {0, 0, 0}, {0, 0, 40});
        std::vector<float> out;
        for (int f = 3; f < 8; ++f) {
            puppetBoneMatrices(*puppet, kIdle, f * kFrame, out, &state, &world);
        }
        QVERIFY(std::fabs(turnOf(out, 1)) > 0.05f);
        state.reset();
        puppetBoneMatrices(*puppet, kIdle, 8 * kFrame, out, &state, &world);
        QVERIFY(std::fabs(turnOf(out, 1)) < 1e-6f);
        // and a reset of another bone leaves this one alone
        state.applyImpulse(1, {0, 0, 0}, {0, 0, 40});
        state.reset(0);
        for (int f = 9; f < 14; ++f) {
            puppetBoneMatrices(*puppet, kIdle, f * kFrame, out, &state, &world);
        }
        QVERIFY(std::fabs(turnOf(out, 1)) > 0.05f);
    }

    void impulsesThatAreNotNumbersOrNotBonesChangeNothing()
    {
        const auto puppet = load(hairModel(springNote(true, false)));
        QVERIFY(puppet.has_value());
        BonePhysicsState state(puppet->bones.size(), 1);
        const BoneWorld world;
        run(*puppet, state, world, 2);
        const float nan = std::nanf("");
        state.applyImpulse(1, {nan, 0, 0}, {0, 0, 0});
        state.applyImpulse(99, {1, 0, 0}, {0, 0, 1});
        state.applyImpulse(1, {0, 0, 0}, {0, 0, 1e30f}); // clamped, still finite
        const auto out = run(*puppet, state, world, 200, 3 * kFrame);
        for (const float v : out) {
            QVERIFY(std::isfinite(v));
        }
    }

    void gravityBendsAFlaggedBoneAndItSettles()
    {
        const auto puppet = load(hairModel(springNote(true, false, "")));
        const auto flagged = load(hairModel(swapped(springNote(true, false), R"("ge":false)", R"("ge":true)")));
        QVERIFY(puppet.has_value() && flagged.has_value());
        const BoneWorld world; // gravity (0, -1, 0) at strength 1
        BonePhysicsState free(puppet->bones.size(), 1), pulled(flagged->bones.size(), 1);
        const auto a = run(*puppet, free, world, 600);
        const auto b = run(*flagged, pulled, world, 600);
        QVERIFY(std::fabs(turnOf(a, 1)) < 1e-4f);
        QVERIFY(turnOf(b, 1) < -0.05f);  // the bone points along +x; gravity pulls it down, a clockwise (negative) turn
        QVERIFY(turnOf(b, 1) > -1.5f);
        const auto settled = run(*flagged, pulled, world, 120, 10.0 + kFrame);
        QVERIFY(std::fabs(turnOf(settled, 1) - turnOf(b, 1)) < 1e-3f);
        // twice the strength bends it further
        BoneWorld strong;
        strong.gravityStrength = 2.0f;
        BonePhysicsState pulledMore(flagged->bones.size(), 1);
        QVERIFY(turnOf(run(*flagged, pulledMore, strong, 600), 1) < turnOf(b, 1) - 0.05f);
    }

    void anAngleLimitHoldsTheBone()
    {
        std::string note = swapped(springNote(true, false), R"("la":false)", R"("la":true)");
        note = swapped(note, R"("lamax":"0 0 3.1415")", R"("lamax":"0 0 0.1")");
        note = swapped(note, R"("lamin":"0 0 -3.1415")", R"("lamin":"0 0 -0.1")");
        const auto limited = load(hairModel(note));
        QVERIFY(limited.has_value());
        QVERIFY(limited->bones[1].constraint.limitAngles);
        BonePhysicsState state(limited->bones.size(), 1);
        const BoneWorld world;
        run(*limited, state, world, 2);
        state.applyImpulse(1, {0, 0, 0}, {0, 0, 500});
        std::vector<float> out;
        float largest = 0.0f;
        for (int f = 3; f < 300; ++f) {
            puppetBoneMatrices(*limited, kIdle, f * kFrame, out, &state, &world);
            largest = std::max(largest, std::fabs(turnOf(out, 1)));
        }
        QVERIFY(largest > 0.09f);     // it reached the stop
        QVERIFY(largest < 0.1001f);   // and never went past
    }

    void aTranslationLimitHoldsTheBone()
    {
        std::string note = swapped(springNote(false, true), R"("lt":false)", R"("lt":true)");
        note = swapped(note, R"("ltmax":100)", R"("ltmax":3)");
        const auto puppet = load(hairModel(note));
        QVERIFY(puppet.has_value());
        BonePhysicsState state(puppet->bones.size(), 1);
        const BoneWorld world;
        run(*puppet, state, world, 2);
        state.applyImpulse(1, {0, 400, 0}, {0, 0, 0});
        std::vector<float> out;
        float farthest = 0.0f;
        for (int f = 3; f < 300; ++f) {
            puppetBoneMatrices(*puppet, kIdle, f * kFrame, out, &state, &world);
            farthest = std::max(farthest, std::fabs(out[16 + 13]));
        }
        QVERIFY(farthest > 2.9f);
        QVERIFY(farthest < 3.001f);
    }

    void windMovesTheBonesAndTheSeedMakesItReproducible()
    {
        const auto puppet = load(hairModel(springNote(false, true)));
        QVERIFY(puppet.has_value());
        BoneWorld wind;
        wind.windEnabled = true;
        wind.windDirection = {1, 0, 0};
        wind.windStrength = 4000.0f;
        const auto trace = [&](uint32_t seed) {
            BonePhysicsState state(puppet->bones.size(), seed);
            std::vector<float> samples, out;
            for (int f = 0; f < 300; ++f) {
                puppetBoneMatrices(*puppet, kIdle, f * kFrame, out, &state, &wind);
                samples.push_back(out[16 + 12]);
            }
            return samples;
        };
        const auto a = trace(5), b = trace(5), c = trace(6);
        QCOMPARE(a, b); // bit for bit
        QVERIFY(a != c);
        QVERIFY(*std::max_element(a.begin(), a.end()) > 1.0f); // the wind pushed it along +x
        // no wind: nothing moves
        BoneWorld calm = wind;
        calm.windEnabled = false;
        BonePhysicsState still(puppet->bones.size(), 5);
        std::vector<float> out;
        for (int f = 0; f < 120; ++f) {
            puppetBoneMatrices(*puppet, kIdle, f * kFrame, out, &still, &calm);
        }
        QVERIFY(std::fabs(out[16 + 12]) < 1e-5f);
    }

    void aStalledClockIsCaughtUpInPartAndAClockThatGoesBackIsNot()
    {
        BonePhysicsState state(3, 1);
        state.beginFrame(0.0);
        QCOMPARE(state.steps(), 0);
        state.beginFrame(kFrame * 1.5);
        QCOMPARE(state.steps(), 1);
        state.beginFrame(kFrame * 1.5);
        QCOMPARE(state.steps(), 0);
        state.beginFrame(1000.0); // a stall of minutes: at most the catch-up
        QCOMPARE(state.steps(), kBonePhysicsMaxSteps);
        state.beginFrame(1.0); // back in time
        QCOMPARE(state.steps(), 0);
        state.beginFrame(std::nan(""));
        QCOMPARE(state.steps(), 0);
    }

    void extremeSpringConstantsStayBoundedAndConverge()
    {
        for (const auto &[rs, rf] : {std::pair{"100000", "10000"}, std::pair{"100000", "0"}, std::pair{"50000", "500"}, std::pair{"0", "10000"}}) {
            std::string note = springNote(true, true);
            note = swapped(note, R"("rs":200)", std::string(R"("rs":)") + rs);
            note = swapped(note, R"("rf":20)", std::string(R"("rf":)") + rf);
            note = swapped(note, R"("ts":200)", std::string(R"("ts":)") + rs);
            note = swapped(note, R"("tf":20)", std::string(R"("tf":)") + rf);
            const auto puppet = load(hairModel(note));
            QVERIFY(puppet.has_value());
            BonePhysicsState state(puppet->bones.size(), 1);
            const BoneWorld world;
            run(*puppet, state, world, 2);
            state.applyImpulse(1, {30, 30, 0}, {0, 0, 20});
            std::vector<float> out;
            float largest = 0.0f;
            for (int f = 3; f < 1500; ++f) {
                puppetBoneMatrices(*puppet, kIdle, f * kFrame, out, &state, &world);
                for (const float v : out) {
                    QVERIFY(std::isfinite(v));
                }
                largest = std::max({largest, std::fabs(turnOf(out, 1)), std::fabs(out[16 + 13])});
            }
            QVERIFY2(largest < 100.0f, rs);
            if (std::string(rs) != "0" && std::string(rf) != "0") { // no stiffness: nothing brings it back; no damping: it rings for ever (bounded)
                QVERIFY2(std::fabs(turnOf(out, 1)) < 1e-2f && std::fabs(out[16 + 13]) < 1e-2f, rs);
            }
        }
    }

    void aSpringlessBoneUnderGravityDoesNotDriftWithoutEnd()
    {
        std::string note = swapped(springNote(false, true), R"("ts":200)", R"("ts":0)");
        note = swapped(note, R"("tf":20)", R"("tf":0)");
        note = swapped(note, R"("ge":false)", R"("ge":true)");
        const auto puppet = load(hairModel(note));
        QVERIFY(puppet.has_value());
        QCOMPARE(puppet->bones[1].constraint.translationStiffness, 0.0f);
        BonePhysicsState state(puppet->bones.size(), 1);
        BoneWorld world;
        world.gravityStrength = 1000.0f;
        const auto out = run(*puppet, state, world, 3000);
        QVERIFY(std::fabs(out[16 + 13]) <= 1e4f + 1.0f);
        QVERIFY(std::fabs(out[16 + 13]) > 9000.0f); // it did fall, as far as the bound
    }

    void windIsTurnedIntoTheLayersMeshSpace()
    {
        BoneWorld world;
        world.windDirection = {1, 0, 0};
        // a layer turned a quarter turn about z: scene +x is the mesh's -y
        const BoneWorld turned = boneWorldInLayer(world, rotationZ(1.57079633f));
        QVERIFY(std::fabs(turned.windDirection.x) < 1e-5f);
        QVERIFY(std::fabs(turned.windDirection.y + 1.0f) < 1e-5f);
        // a mirrored layer flips it, and a scale does not change its direction
        const BoneWorld mirrored = boneWorldInLayer(world, scaling({-2, 2, 1}));
        QVERIFY(std::fabs(mirrored.windDirection.x + 1.0f) < 1e-5f);
    }

    void aStateForAnotherSkeletonIsIgnored()
    {
        const auto puppet = load(hairModel(springNote(true, false)));
        QVERIFY(puppet.has_value());
        BonePhysicsState wrong(7, 1);
        std::vector<float> out;
        puppetBoneMatrices(*puppet, kIdle, 0.0, out, &wrong, nullptr);
        QCOMPARE(out, puppetBoneMatrices(*puppet, kIdle, 0.0));
    }

    void manyBonesCostALinearAmount()
    {
        std::vector<BoneSpec> bones{{"root", -1, 0, 0}};
        std::vector<TrackSpec> tracks{{{frame(0, 0), frame(0, 0)}}};
        for (int i = 1; i < 128; ++i) {
            bones.push_back({"b" + std::to_string(i), i - 1, 5, 0, springNote(true, true)});
            tracks.push_back({{frame(5, 0), frame(5, 0)}});
        }
        const auto puppet = load(makeMdl(bones, {{7, "idle", "loop", 30.0f, 2, tracks}}));
        QVERIFY(puppet.has_value());
        QCOMPARE(puppet->simulatedBones, size_t{127});
        BonePhysicsState state(puppet->bones.size(), 1);
        const BoneWorld world;
        run(*puppet, state, world, 2);
        state.applyImpulse(1, {0, 0, 0}, {0, 0, 3});
        const auto out = run(*puppet, state, world, 600, 3 * kFrame);
        for (const float v : out) {
            QVERIFY(std::isfinite(v));
        }
    }

    void scriptsImpulseAndResetTheSimulation()
    {
        using namespace scenevkfix;
        Item item;
        item.tex(QStringLiteral("red"), simpleRgba(8, 8));
        const auto mdl = hairModel(springNote(true, false));
        item.file(QStringLiteral("models/pup.mdl"), QByteArray(reinterpret_cast<const char *>(mdl.data()), static_cast<qsizetype>(mdl.size())));
        item.file(QStringLiteral("models/pup.json"), R"({"material":"materials/pup.json","puppet":"models/pup.mdl"})");
        item.file(QStringLiteral("materials/pup.json"), R"({"passes":[{"shader":"puppetshader","blending":"translucent","textures":["red"]}]})");
        const QString source = QStringLiteral(
            "let n = 0; export function update(v) { const p = thisScene.getLayer('pup'); n++; "
            "if (n === 1) p.applyBonePhysicsImpulse('hair', new Vec3(0, 2, 0), new Vec3(0, 0, 3)); "
            "if (n === 2) p.resetBonePhysicsSimulation(); "
            "if (n === 4) p.applyBonePhysicsImpulse(1, new Vec3(0, 0, 0), new Vec3(0, 0, 4)); "
            "if (n === 5) { p.resetBonePhysicsSimulation('nope'); p.resetBonePhysicsSimulation(NaN); p.resetBonePhysicsSimulation(99); } "
            "if (n === 6) p.resetBonePhysicsSimulation(1); "
            "if (n === 3) { p.applyBonePhysicsImpulse(0, new Vec3(1, 0, 0), new Vec3(0, 0, 1)); p.applyBonePhysicsImpulse(99, new Vec3(1, 0, 0), new Vec3(0, 0, 1)); p.applyBonePhysicsImpulse('nope', new Vec3(1, 0, 0), new Vec3(0, 0, 1)); } "
            "return v; }");
        const QJsonObject driver{{"id", 7}, {"name", "driver"}, {"visible", QJsonObject{{"script", source}, {"value", true}}}};
        const QByteArray objects = R"({"id":5,"name":"pup","image":"models/pup.json","origin":"500 500 0","size":"40 40"},)" + QJsonDocument(driver).toJson(QJsonDocument::Compact);
        item.file(QStringLiteral("scene.json"), sceneJson(objects));
        Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        std::string err;
        auto scene = loadScene(vfs, "scene.json", &err);
        QVERIFY2(scene.has_value(), err.c_str());
        ImageLayer *pup = nullptr;
        for (ImageLayer &l : scene->layers) {
            if (scene->nodes[l.node].name == "pup") {
                pup = &l;
            }
        }
        QVERIFY(pup && pup->puppet && pup->bonePhysics);
        QVERIFY(puppetSkinned(*pup)); // skinned without animation layers: its spring bones sway about the bind pose
        PropertyStore props;
        ScriptFlags flags;
        SceneScripts scripts(*scene, vfs, props, flags);
        scripts.start();
        QCOMPARE(scripts.failed(), 0);
        ScriptFrame f;
        f.canvasWidth = 1920;
        f.canvasHeight = 1080;
        scripts.update(f);
        QCOMPARE(pup->bonePhysics->bone(1).impulseLinear.y, 2.0f);
        QCOMPARE(pup->bonePhysics->bone(1).impulseAngular.z, 3.0f);
        scripts.update(f); // the reset
        QCOMPARE(pup->bonePhysics->bone(1).impulseAngular.z, 0.0f);
        scripts.update(f);
        QCOMPARE(pup->bonePhysics->bone(0).impulseAngular.z, 1.0f); // the root takes it too (it is stored; a bone that does not simulate ignores it)
        scripts.update(f);
        QCOMPARE(pup->bonePhysics->bone(1).impulseAngular.z, 4.0f);
        scripts.update(f); // a reset of a bone that does not exist, NaN or out of range touches nothing
        QCOMPARE(pup->bonePhysics->bone(1).impulseAngular.z, 4.0f);
        QCOMPARE(pup->bonePhysics->bone(0).impulseAngular.z, 1.0f);
        scripts.update(f); // a reset of one bone leaves the others
        QCOMPARE(pup->bonePhysics->bone(1).impulseAngular.z, 0.0f);
        QCOMPARE(pup->bonePhysics->bone(0).impulseAngular.z, 1.0f);
    }

    void checkReportsWhatIsNotEvaluated()
    {
        using namespace scenevkfix;
        Item item;
        item.tex(QStringLiteral("red"), simpleRgba(8, 8));
        const auto mdl = hairModel(R"({"r":false,"t":false,"ik":true})");
        item.file(QStringLiteral("models/pup.mdl"), QByteArray(reinterpret_cast<const char *>(mdl.data()), static_cast<qsizetype>(mdl.size())));
        item.file(QStringLiteral("models/pup.json"), R"({"material":"materials/pup.json","puppet":"models/pup.mdl"})");
        item.file(QStringLiteral("materials/pup.json"), R"({"passes":[{"shader":"puppetshader","blending":"translucent","textures":["red"]}]})");
        item.file(QStringLiteral("scene.json"), sceneJson(R"({"id":5,"name":"pup","image":"models/pup.json","origin":"500 500 0","size":"40 40"})", R"(,"windenabled":true,"windstrength":300.0,"winddirection":"1 0 0")"));
        Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        std::string err;
        const auto scene = loadScene(vfs, "scene.json", &err);
        QVERIFY2(scene.has_value(), err.c_str());
        QVERIFY(std::find(scene->degraded.begin(), scene->degraded.end(), "puppet bone IK (no goal in the data)") != scene->degraded.end());
        QVERIFY(std::find(scene->degraded.begin(), scene->degraded.end(), "scene wind (bones only)") != scene->degraded.end());
        QVERIFY(scene->boneWorld.windEnabled);
        QCOMPARE(scene->boneWorld.windStrength, 300.0f);
        QCOMPARE(scene->boneWorld.windDirection.x, 1.0f);
        QCOMPARE(scene->boneWorld.gravityStrength, 1.0f);
    }
};

QTEST_APPLESS_MAIN(TstSceneVkBonePhysics)
#include "tst_scenevk_bonephysics.moc"
