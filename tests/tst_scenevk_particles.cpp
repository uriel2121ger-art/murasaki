// murasaki-scene-vk particles: definitions and the CPU simulation, on synthetic systems with a fixed seed. No GPU.

#include "particles/definition.h"
#include "particles/system.h"
#include "scenevkfixtures.h"

#include <QTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <limits>
#include <memory>
#include <optional>

using namespace Qt::StringLiterals;
using namespace murasaki::scene;
using namespace scenevkfix;
using Col = ParticleColumn;

namespace {

// A system file: `body` is the JSON between the braces.
ParticleDefinition define(const QByteArray &body, Item &item, std::string *error = nullptr)
{
    item.file(u"materials/presets/ember.json"_s,
              R"({"passes":[{"blending":"additive","shader":"genericparticle","textures":["particle/halo_2"]}]})");
    item.file(u"particles/t.json"_s, "{\"material\":\"materials/presets/ember.json\"," + body + "}");
    const Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
    auto def = loadParticleDefinition(vfs, "particles/t.json", error);
    if (!def) {
        qFatal("loadParticleDefinition: %s", error ? error->c_str() : "");
    }
    return *def;
}

const QByteArray kLong = R"("initializer":[{"name":"lifetimerandom","min":100,"max":100},{"name":"sizerandom","min":10,"max":10}])";

float maxOf(const ParticleSystem &s, Col c)
{
    float m = -1e30f;
    for (float v : s.column(c))
        m = std::max(m, v);
    return m;
}
float minOf(const ParticleSystem &s, Col c)
{
    float m = 1e30f;
    for (float v : s.column(c))
        m = std::min(m, v);
    return m;
}

// A system with its own definition (a system keeps a reference to it).
struct Sim {
    std::unique_ptr<ParticleDefinition> def;
    std::unique_ptr<ParticleSystem> s;
    ParticleSystem *operator->() const { return s.get(); }
    ParticleSystem &operator*() const { return *s; }
};

// One particle at `origin` with velocity `velocity`, living 100 s, and these operators after a movement operator (no gravity).
Sim single(Item &item, const QByteArray &origin, const QByteArray &velocity, const QByteArray &operators, const QByteArray &extraInit = {},
                  const QByteArray &top = {})
{
    Sim sim;
    sim.def = std::make_unique<ParticleDefinition>(
        define(top + "\"maxcount\":1,\"emitter\":[{\"name\":\"sphererandom\",\"instantaneous\":1,\"distancemax\":0,\"origin\":\"" + origin
                   + "\"}],\"initializer\":[{\"name\":\"lifetimerandom\",\"min\":100,\"max\":100},{\"name\":\"velocityrandom\",\"min\":\"" + velocity
                   + "\",\"max\":\"" + velocity + "\"}" + extraInit + "],\"operator\":[{\"name\":\"movement\"}"
                   + (operators.isEmpty() ? QByteArray() : "," + operators) + "]",
               item));
    sim.s = std::make_unique<ParticleSystem>(*sim.def, ParticleOverrides{}, 1);
    return sim;
}

void run(ParticleSystem &s, double seconds)
{
    for (int i = 0; i < static_cast<int>(std::lround(seconds * 60.0)); ++i)
        s.advance(1.0 / 60.0);
}

} // namespace

class TstSceneVkParticles : public QObject
{
    Q_OBJECT

private slots:
    void definitionsReadValuesAndDefaults()
    {
        Item item;
        const auto d = define(R"("maxcount":50,"starttime":3,"flags":4,"emitter":[{"name":"boxrandom","rate":20,"distancemax":"100 50 0","directions":"1 0.5 0"}],)"
                              R"("initializer":[{"name":"sizerandom","min":3,"max":{"user":"big","value":9}}],)"
                              R"("operator":[{"name":"movement","gravity":"0 -300 0","drag":0.5}],"controlpoint":[{"id":0,"flags":1,"offset":"1 2 3"}])",
                              item);
        QCOMPARE(d.maxCount, 50u);
        QCOMPARE(d.startTime, 3.0f);
        QCOMPARE(d.flags, 4u);
        QCOMPARE(d.emitters.size(), size_t{1});
        QCOMPARE(d.emitters[0].number("rate", 0), 20.0f);
        const Vec3 dm = d.emitters[0].vector("distancemax", {});
        QCOMPARE(dm.y, 50.0f);
        QCOMPARE(d.emitters[0].vector("distancemin", {7, 7, 7}).x, 7.0f); // default when absent
        QCOMPARE(d.operators[0].vector("gravity", {}).y, -300.0f);
        QCOMPARE(d.renderers.size(), size_t{1}); // a plain sprite when the file names none
        QCOMPARE(d.renderers[0].name, std::string("sprite"));
        QCOMPARE(d.controlPoints.size(), size_t{1});
        QCOMPARE(d.controlPoints[0].flags, 1u);
        QCOMPARE(d.controlPoints[0].offset.z, 3.0f);
        QVERIFY(d.unsupported.empty());

        // a bound value reads the user property
        PropertyStore props;
        QCOMPARE(d.initializers[0].number("max", 0, &props), 9.0f);
        props.set("big", 40.0);
        QCOMPARE(d.initializers[0].number("max", 0, &props), 40.0f);
    }

    void unsupportedFeaturesAreNamed()
    {
        Item item;
        const auto d = define(R"("emitter":[{"name":"spiralthing"}],"initializer":[{"name":"mapsequencefoo"}],)"
                              R"("operator":[{"name":"sparkle"},{"name":"movement","gravity":{"script":"x"}}],"renderer":[{"name":"spriteburst"}])",
                              item);
        const auto has = [&](const char *r) { return std::find(d.unsupported.begin(), d.unsupported.end(), r) != d.unsupported.end(); };
        QVERIFY(has("particles: emitter spiralthing"));
        QVERIFY(has("particles: initializer mapsequencefoo"));
        QVERIFY(has("particles: operator sparkle"));
        QVERIFY(has("particles: renderer spriteburst"));
        QVERIFY(has("SceneScript (particle movement.gravity): M4"));
    }

    void childrenAreLoadedAndTheirReasonsReported()
    {
        Item item;
        item.file(u"particles/child.json"_s, R"({"material":"materials/presets/ember.json","operator":[{"name":"sparkle"}]})");
        const auto d = define(R"("children":[{"name":"particles/child.json","origin":"5 6 0"},{"name":"particles/child.json","type":"eventspawn"},{"name":"particles/child.json","type":"mystery"}])", item);
        QCOMPARE(d.children.size(), size_t{3});
        QCOMPARE(d.children[0].origin.x, 5.0f);
        QVERIFY(d.children[0].definition);
        const auto has = [&](const char *r) { return std::find(d.unsupported.begin(), d.unsupported.end(), r) != d.unsupported.end(); };
        QVERIFY(has("particles: operator sparkle")); // from the child
        QVERIFY(!has("particles: child eventspawn")); // the event children are drawn
        QVERIFY(has("particles: child mystery"));

        // a child that is missing is an unsupported reason, not a failure
        const auto missing = define(R"("children":[{"name":"particles/none.json"}])", item);
        QVERIFY(!missing.unsupported.empty());
    }

    // Audio-reactive emitters: the emission rate follows the spectrum. Particles emitted in 10 s at a base rate of 60/s.
    static size_t emittedWithAudio(const QByteArray &emitterExtra, const std::array<float, 16> &left, const std::array<float, 16> &right)
    {
        Item item;
        const auto d = define(R"("maxcount":100000,"emitter":[{"name":"boxrandom","rate":60)" + emitterExtra + R"(}],)" + kLong, item);
        ParticleSystem s(d, {}, 1);
        s.setAudio(left, right);
        for (int i = 0; i < 600; ++i)
            s.advance(1.0 / 60.0);
        return s.count();
    }
    static std::array<float, 16> level(float v)
    {
        std::array<float, 16> a;
        a.fill(v);
        return a;
    }

    void audioModeScalesTheEmissionRate()
    {
        const auto silent = level(0.0f), loud = level(1.0f);
        const auto near = [](size_t got, double want) { return std::abs(static_cast<double>(got) - want) <= 2.0; };
        // mode 3, default bounds 0.8..1: silence emits at 80 % of the rate, a full spectrum at 100 %
        QVERIFY(near(emittedWithAudio(R"(,"audioprocessingmode":3)", silent, silent), 480));
        QVERIFY(near(emittedWithAudio(R"(,"audioprocessingmode":3)", loud, loud), 600));
        // mode 0 (and a file without the keys) ignores the spectrum
        QVERIFY(near(emittedWithAudio(R"(,"audioprocessingmode":0)", silent, silent), 600));
        QVERIFY(near(emittedWithAudio(R"(,"audioprocessingmode":0)", loud, loud), 600));
        QVERIFY(near(emittedWithAudio("", silent, silent), 600));
    }

    void audioBoundsAndExponentShapeTheResponse()
    {
        const auto silent = level(0.0f), half = level(0.5f);
        const auto near = [](size_t got, double want) { return std::abs(static_cast<double>(got) - want) <= 2.0; };
        // bounds "0 1": silence emits nothing; level 0.5 with the default exponent 2 gives 0.25
        QVERIFY(near(emittedWithAudio(R"(,"audioprocessingmode":3,"audioprocessingbounds":"0 1")", silent, silent), 0));
        QVERIFY(near(emittedWithAudio(R"(,"audioprocessingmode":3,"audioprocessingbounds":"0 1")", half, half), 150));
        // exponent 1: linear
        QVERIFY(near(emittedWithAudio(R"(,"audioprocessingmode":3,"audioprocessingbounds":"0 1","audioprocessingexponent":1)", half, half), 300));
        // bounds "0.5 1.5" at level 0.5, exponent 1: 1.0
        QVERIFY(near(emittedWithAudio(R"(,"audioprocessingmode":3,"audioprocessingbounds":"0.5 1.5","audioprocessingexponent":1)", half, half), 600));
    }

    void audioChannelsAndBandsSelectTheLevel()
    {
        const auto silent = level(0.0f), loud = level(1.0f);
        const auto near = [](size_t got, double want) { return std::abs(static_cast<double>(got) - want) <= 2.0; };
        const QByteArray zeroOne = R"(,"audioprocessingbounds":"0 1","audioprocessingexponent":1)";
        // left loud, right silent: mode 1 sees all of it, mode 2 nothing, mode 3 half
        QVERIFY(near(emittedWithAudio(R"(,"audioprocessingmode":1)" + zeroOne, loud, silent), 600));
        QVERIFY(near(emittedWithAudio(R"(,"audioprocessingmode":2)" + zeroOne, loud, silent), 0));
        QVERIFY(near(emittedWithAudio(R"(,"audioprocessingmode":3)" + zeroOne, loud, silent), 300));
        // only bands 0 and 1 loud (the default range is 0..1); a range further up sees silence, swapped ends are put right
        auto low = level(0.0f);
        low[0] = low[1] = 1.0f;
        QVERIFY(near(emittedWithAudio(R"(,"audioprocessingmode":3)" + zeroOne, low, low), 600));
        QVERIFY(near(emittedWithAudio(R"(,"audioprocessingmode":3,"audioprocessingfrequencystart":4,"audioprocessingfrequencyend":8)" + zeroOne, low, low), 0));
        QVERIFY(near(emittedWithAudio(R"(,"audioprocessingmode":3,"audioprocessingfrequencystart":1,"audioprocessingfrequencyend":0)" + zeroOne, low, low), 600));
        // the mean of bands 0..3 with two of them loud: half; ends above 15 are clamped to the last band
        QVERIFY(near(emittedWithAudio(R"(,"audioprocessingmode":3,"audioprocessingfrequencystart":0,"audioprocessingfrequencyend":3)" + zeroOne, low, low), 300));
        QVERIFY(near(emittedWithAudio(R"(,"audioprocessingmode":3,"audioprocessingfrequencystart":14,"audioprocessingfrequencyend":99)" + zeroOne, loud, loud), 600));
    }

    void audioEmittersAreDetectedAndUnknownModesDegrade()
    {
        Item item;
        const auto on = define(R"("emitter":[{"name":"boxrandom","audioprocessingmode":3}])", item);
        QVERIFY(particleUsesAudio(on));
        QVERIFY(on.degraded.empty());
        const auto off = define(R"("emitter":[{"name":"boxrandom","audioprocessingmode":0}])", item);
        QVERIFY(!particleUsesAudio(off));
        const auto odd = define(R"("emitter":[{"name":"boxrandom","audioprocessingmode":7}],"operator":[{"name":"capvelocity","audioprocessingmode":3},{"name":"vortex","audioprocessingmode":7}])", item);
        const auto has = [&](const char *r) { return std::find(odd.degraded.begin(), odd.degraded.end(), r) != odd.degraded.end(); };
        QVERIFY(has("particle emitter audio mode 7"));
        QVERIFY(has("particle operator audio mode 7"));
        // a negative mode is off, not an error
        const auto negative = define(R"("emitter":[{"name":"boxrandom","audioprocessingmode":-1}])", item);
        QVERIFY(negative.degraded.empty());
        QVERIFY(!particleUsesAudio(negative));
        QVERIFY(has("particle operator audio response")); // the capvelocity: only vortex and turbulence follow the spectrum
        // a vortex or turbulence with audio makes the engine capture it; either alone is not degraded
        const auto vortex = define(R"("operator":[{"name":"vortex","audioprocessingmode":3}])", item);
        QVERIFY(particleUsesAudio(vortex) && vortex.degraded.empty());
        const auto turb = define(R"("operator":[{"name":"turbulence","audioprocessingmode":1}])", item);
        QVERIFY(particleUsesAudio(turb) && turb.degraded.empty());
    }

    // Operators with audio response: their strength follows the spectrum like an emitter's rate does.
    void audioScalesAVortexAndATurbulence()
    {
        Item item;
        const auto speedOf = [&](const QByteArray &op, float lv) {
            const QByteArray json = R"("maxcount":4,"emitter":[{"name":"sphererandom","instantaneous":1,"distancemax":0,"origin":"25 0 0"}],)"
                R"("initializer":[{"name":"lifetimerandom","min":5,"max":5}],"operator":[)" + op + "]";
            const auto def = define(json, item);
            ParticleSystem s(def, {}, 1);
            s.setAudio(level(lv), level(lv));
            s.advance(0.1);
            return s.count() == 1 ? std::hypot(s.column(Col::VX)[0], s.column(Col::VY)[0]) : -1.0f;
        };
        const QByteArray vortex = R"({"name":"vortex","distanceouter":50,"speedinner":100,"speedouter":100)";
        const QByteArray turbulence = R"({"name":"turbulence","speedmin":1000,"speedmax":1000,"scale":0.01)";
        const QByteArray audio = R"(,"audioprocessingmode":3,"audioprocessingbounds":"0.2 1","audioprocessingexponent":1})";
        // bounds 0.2..1, exponent 1: silence 0.2 of the strength, a full spectrum all of it, half 0.6
        const float base = speedOf(vortex + "}", 0.0f);
        QVERIFY(base > 5.0f);
        QVERIFY(std::abs(speedOf(vortex + audio, 0.0f) - 0.2f * base) < 0.05f * base);
        QVERIFY(std::abs(speedOf(vortex + audio, 1.0f) - base) < 0.05f * base);
        QVERIFY(std::abs(speedOf(vortex + audio, 0.5f) - 0.6f * base) < 0.05f * base);
        QVERIFY(std::abs(speedOf(vortex + R"(,"audioprocessingmode":0})", 0.0f) - base) < 0.05f * base); // mode 0: off
        const float tBase = speedOf(turbulence + "}", 0.0f);
        QVERIFY(tBase > 5.0f);
        QVERIFY(std::abs(speedOf(turbulence + audio, 0.0f) - 0.2f * tBase) < 0.05f * tBase);
        QVERIFY(std::abs(speedOf(turbulence + audio, 1.0f) - tBase) < 0.05f * tBase);
    }

    // A sprite sheet on several images is drawn from the first one only (the particle shader lays frames out on one image).
    void aSheetOnSeveralImagesIsReportedAsDegraded()
    {
        const auto degradedWith = [&](TexSpec s, bool pageSheet) {
            Item item;
            s.version = 3;
            s.texW = s.imgW = 4;
            s.texH = s.imgH = 2;
            const Bytes first = rgbaPattern(4, 2);
            s.images = {{MipSpec{4, 2, first, false}}};
            s.sheetVersion = 3;
            s.flags = TexFlags::IsGif | TexFlags::ClampUVs;
            s.frames = {{0, 0, 4, 0, 0, 2}};
            s.frameImages = {0};
            if (pageSheet) {
                s.images.push_back({MipSpec{2, 2, Bytes(16, 0x55), true}});
                s.frames.push_back({0, 0, 2, 0, 0, 2});
                s.frameImages.push_back(1);
            }
            item.tex(u"sheet"_s, s);
            item.file(u"materials/sheetmat.json"_s, R"({"passes":[{"blending":"additive","shader":"genericparticle","textures":["sheet"]}]})");
            item.file(u"particles/t.json"_s, R"({"material":"materials/sheetmat.json","emitter":[{"name":"boxrandom"}]})");
            const Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
            const auto def = loadParticleDefinition(vfs, "particles/t.json", nullptr);
            return def && std::find(def->degraded.begin(), def->degraded.end(), "particle sprite sheet pages") != def->degraded.end();
        };
        QVERIFY(degradedWith({}, true));
        QVERIFY(!degradedWith({}, false));
    }

    void rendererOrientationValues()
    {
        Item item;
        const auto of = [&](const char *renderer) { return particleOrientation(define(QByteArray(R"("renderer":[)") + renderer + "]", item)); };
        const auto same = [](Vec3 a, Vec3 b) { return std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z) < 1e-5f; };

        // screen facing (the default, "screen", unknown words) leaves the axes alone and keeps the combo at 0
        for (const char *r : {R"({"name":"sprite"})", R"({"name":"sprite","orientation":"screen","axis":"0 45 0"})", R"({"name":"sprite","orientation":"banana"})"}) {
            const auto o = of(r);
            QCOMPARE(o.mode, 0);
            QVERIFY(same(o.right, {1, 0, 0}) && same(o.up, {0, 1, 0}) && same(o.forward, {0, 0, 1}));
        }
        // upright / fixed: the axis is the sprite's facing; "0 0 1" is the screen-facing basis
        {
            const auto o = of(R"({"name":"sprite","orientation":"upright","axis":"0 0 1"})");
            QCOMPARE(o.mode, 1);
            QVERIFY(same(o.right, {1, 0, 0}) && same(o.up, {0, 1, 0}) && same(o.forward, {0, 0, 1}));
        }
        // no axis, or all zero, is WE's default A = (0, 1, 0): the sprite lies in the xz plane (edge-on to a flat 2D scene)
        for (const char *r : {R"({"name":"sprite","orientation":"upright"})", R"({"name":"sprite","orientation":"upright","axis":"0 0 0"})",
                              R"({"name":"sprite","orientation":"fixed","axis":"0 0 0"})"}) {
            const auto o = of(r);
            QVERIFY(o.mode == 1 || o.mode == 2);
            QVERIFY(same(o.right, {1, 0, 0}) && same(o.up, {0, 0, -1}) && same(o.forward, {0, 1, 0}));
        }
        QCOMPARE(of(R"({"name":"sprite","orientation":"fixed"})").mode, 2);
        // axis along Y: B falls back to x, C = A x B
        const auto flat = of(R"({"name":"sprite","orientation":"upright","axis":"0 1 0"})");
        QVERIFY(same(flat.forward, {0, 1, 0}) && same(flat.right, {1, 0, 0}) && same(flat.up, {0, 0, -1}));
        // "1 1 1": A normalised, B = (0,1,0) x A normalised, C = A x B; the basis is orthonormal and right handed
        const auto diag = of(R"({"name":"ropetrail","orientation":"upright","axis":"1 1 1"})");
        const float k = 1.0f / std::sqrt(3.0f);
        QVERIFY(same(diag.forward, {k, k, k}));
        QVERIFY(same(diag.right, {k * std::sqrt(1.5f), 0.0f, -k * std::sqrt(1.5f)}));
        QVERIFY(std::abs(diag.right.x * diag.up.x + diag.right.y * diag.up.y + diag.right.z * diag.up.z) < 1e-5f);
        QVERIFY(std::abs(diag.up.x * diag.forward.x + diag.up.y * diag.forward.y + diag.up.z * diag.forward.z) < 1e-5f);
        // the combo the rope shader reads follows the mode
        const auto d = define(R"("renderer":[{"name":"ropetrail","orientation":"fixed"}])", item);
        QCOMPARE(particleMaterialPass(d, false).combos.at("ORIENTATION"), 2);
        QVERIFY(particleMaterialPass(define(R"("renderer":[{"name":"sprite"}])", item), false).combos.count("ORIENTATION") == 0);
    }

    void rendererReasonsAreNamed()
    {
        Item item;
        const auto has = [](const ParticleDefinition &d, const char *r) { return std::find(d.degraded.begin(), d.degraded.end(), r) != d.degraded.end(); };
        QVERIFY(has(define(R"("renderer":[{"name":"sprite","flags":1}])", item), "particle renderer flags"));
        QVERIFY(!has(define(R"("renderer":[{"name":"sprite","flags":0}])", item), "particle renderer flags"));
        // renderers that ask for different orientations cannot both be honoured: one pass draws them
        const auto mixed = define(R"("renderer":[{"name":"sprite","orientation":"upright","axis":"0 1 0"},{"name":"rope"}])", item);
        QVERIFY(has(mixed, "particle renderers with different orientations"));
        QCOMPARE(particleOrientation(mixed).mode, 1);
        QVERIFY(has(define(R"("renderer":[{"name":"sprite","orientation":"upright"},{"name":"rope","orientation":"upright","axis":"0 0 1"}])", item),
                    "particle renderers with different orientations")); // no axis is (0, 1, 0) now
        QVERIFY(!has(define(R"("renderer":[{"name":"sprite","orientation":"upright"},{"name":"rope","orientation":"upright","axis":"0 1 0"}])", item),
                     "particle renderers with different orientations"));
        // a script on a renderer member is refused like those of emitters and operators
        const auto scripted = define(R"("renderer":[{"name":"sprite","length":{"script":"return 1;","value":1}}])", item);
        QVERIFY(std::any_of(scripted.unsupported.begin(), scripted.unsupported.end(),
                            [](const std::string &r) { return r.rfind("SceneScript (particle sprite.", 0) == 0; }));
    }

    void rateGivesTheExpectedCountAndLifetimeBoundsIt()
    {
        Item item;
        const auto d = define(R"("maxcount":10000,"emitter":[{"name":"sphererandom","rate":100,"distancemax":10}],)"
                              R"("initializer":[{"name":"lifetimerandom","min":0.5,"max":0.5}])",
                              item);
        ParticleSystem s(d, {}, 1);
        s.advance(0.1);
        s.advance(0.1); // clamped catch-up: the steps are fixed whatever the calls
        QVERIFY(s.count() >= 18 && s.count() <= 22); // 12 steps of 1/60 s at 100/s
        for (int i = 0; i < 30; ++i)
            s.advance(1.0 / 60.0);
        const size_t steady = s.count();
        QVERIFY2(steady >= 48 && steady <= 52, qPrintable(QString::number(steady))); // 100/s * 0.5 s
        for (int i = 0; i < 120; ++i)
            s.advance(1.0 / 60.0);
        QVERIFY(s.count() >= 48 && s.count() <= 52); // and it stays there
    }

    void maxcountAndTheCountOverrideCapThePool()
    {
        Item item;
        const auto d = define(R"("maxcount":20,"emitter":[{"name":"boxrandom","rate":1000}],)" + kLong, item);
        ParticleSystem a(d, {}, 1);
        for (int i = 0; i < 60; ++i)
            a.advance(1.0 / 60.0);
        QCOMPARE(a.count(), size_t{20});
        ParticleOverrides ov;
        ov.count = 2.5f;
        ParticleSystem b(d, ov, 1);
        for (int i = 0; i < 60; ++i)
            b.advance(1.0 / 60.0);
        QCOMPARE(b.count(), size_t{50});
    }

    void instantaneousEmitsOnceAndDelayAndDurationBoundEmission()
    {
        Item item;
        const auto burst = define(R"("maxcount":500,"emitter":[{"name":"sphererandom","instantaneous":30}],)" + kLong, item);
        ParticleSystem a(burst, {}, 1);
        for (int i = 0; i < 120; ++i)
            a.advance(1.0 / 60.0);
        QCOMPARE(a.count(), size_t{30});

        const auto timed = define(R"("maxcount":5000,"emitter":[{"name":"sphererandom","rate":60,"delay":0.5,"duration":0.5}],)" + kLong, item);
        ParticleSystem b(timed, {}, 1);
        for (int i = 0; i < 25; ++i)
            b.advance(1.0 / 60.0);
        QCOMPARE(b.count(), size_t{0}); // still in the delay
        for (int i = 0; i < 200; ++i)
            b.advance(1.0 / 60.0);
        QVERIFY2(b.count() >= 28 && b.count() <= 32, qPrintable(QString::number(b.count()))); // 0.5 s at 60/s, then it stops
    }

    void boxAndSphereEmittersStayInsideTheirVolumes()
    {
        Item item;
        const auto box = define(R"("maxcount":3000,"emitter":[{"name":"boxrandom","rate":3000,"distancemin":"10 0 0","distancemax":"100 50 0","origin":"1000 0 0"}],)" + kLong, item);
        ParticleSystem b(box, {}, 7);
        b.advance(0.1);
        QVERIFY(b.count() > 100);
        // x = 1000 +- [10, 100]; y = +- [0, 50] scaled by directions (default 1 1 0)
        for (size_t i = 0; i < b.count(); ++i) {
            const float dx = std::fabs(b.column(Col::X)[i] - 1000.0f);
            QVERIFY(dx >= 9.9f && dx <= 100.1f);
            QVERIFY(std::fabs(b.column(Col::Y)[i]) <= 50.1f);
            QCOMPARE(b.column(Col::Z)[i], 0.0f);
        }
        QVERIFY(minOf(b, Col::X) < 950.0f && maxOf(b, Col::X) > 1050.0f); // both sides used

        const auto sphere = define(R"("maxcount":3000,"emitter":[{"name":"sphererandom","rate":3000,"distancemin":20,"distancemax":60}],)" + kLong, item);
        ParticleSystem s(sphere, {}, 7);
        s.advance(0.1);
        QVERIFY(s.count() > 100);
        for (size_t i = 0; i < s.count(); ++i) {
            const float r = std::hypot(s.column(Col::X)[i], s.column(Col::Y)[i]);
            QVERIFY2(r >= 19.9f && r <= 60.1f, qPrintable(QString::number(r)));
        }
        // sign 1 on x keeps every particle on the right
        const auto right = define(R"("maxcount":3000,"emitter":[{"name":"sphererandom","rate":3000,"distancemax":60,"sign":"1 0 0"}],)" + kLong, item);
        ParticleSystem r(right, {}, 7);
        r.advance(0.1);
        QVERIFY(minOf(r, Col::X) >= 0.0f);
        QVERIFY(minOf(r, Col::Y) < 0.0f);
    }

    void movementIntegratesGravityDragAndVelocity()
    {
        Item item;
        const auto def = define(R"("maxcount":1,"emitter":[{"name":"sphererandom","instantaneous":1,"distancemax":0}],"initializer":[{"name":"lifetimerandom","min":100,"max":100},)"
                                R"({"name":"velocityrandom","min":"100 0 0","max":"100 0 0"}],"operator":[{"name":"movement","gravity":"0 -100 0"}])",
                                item);
        ParticleSystem s(def, {}, 1);
        for (int i = 0; i < 60; ++i)
            s.advance(1.0 / 60.0);
        QCOMPARE(s.count(), size_t{1});
        QVERIFY(std::fabs(s.column(Col::X)[0] - 100.0f) < 1.0f); // 100 u/s for 1 s
        QVERIFY2(std::fabs(s.column(Col::Y)[0] + 49.2f) < 1.5f, qPrintable(QString::number(s.column(Col::Y)[0]))); // -g t^2 / 2 minus half a step; the first step moves before gravity

        // drag takes the speed down: 1 - 2/s over a second is gone almost completely
        const auto dragged = define(R"("maxcount":1,"emitter":[{"name":"sphererandom","instantaneous":1,"distancemax":0}],"initializer":[{"name":"lifetimerandom","min":100,"max":100},)"
                                    R"({"name":"velocityrandom","min":"100 0 0","max":"100 0 0"}],"operator":[{"name":"movement","drag":3}])",
                                    item);
        ParticleSystem d(dragged, {}, 1);
        for (int i = 0; i < 120; ++i)
            d.advance(1.0 / 60.0);
        QVERIFY(std::fabs(d.column(Col::VX)[0]) < 1.0f);
        QVERIFY(d.column(Col::X)[0] > 20.0f && d.column(Col::X)[0] < 40.0f); // v0 / drag = 33
    }

    void fadesAndChangesFollowTheLifetime()
    {
        Item item;
        const auto def = define(R"("maxcount":1,"emitter":[{"name":"sphererandom","instantaneous":1,"distancemax":0}],"initializer":[{"name":"lifetimerandom","min":2,"max":2},)"
                                R"({"name":"sizerandom","min":100,"max":100}],"operator":[{"name":"alphafade","fadeintime":0.25,"fadeouttime":0.75},)"
                                R"({"name":"sizechange","starttime":0.5,"endtime":1,"startvalue":1,"endvalue":0.5}])",
                                item);
        ParticleSystem s(def, {}, 1);
        const auto at = [&](double seconds) {
            ParticleSystem t(def, {}, 1);
            for (int i = 0; i < static_cast<int>(seconds * 60); ++i)
                t.advance(1.0 / 60.0);
            return std::pair<float, float>{t.column(Col::Alpha)[0], t.column(Col::Size)[0]};
        };
        QVERIFY(std::fabs(at(0.25).first - 0.5f) < 0.05f);   // fading in: 12 % of the life of a 25 % fade
        QVERIFY(std::fabs(at(1.0).first - 1.0f) < 0.02f);    // fully visible mid life
        QVERIFY(at(1.9).first < 0.35f);                      // fading out
        QVERIFY(std::fabs(at(0.5).second - 50.0f) < 1.0f);   // size is 100 / 2 = 50 until half the life
        QVERIFY(std::fabs(at(1.5).second - 50.0f * 0.75f) < 1.5f); // then shrinks towards half
    }

    void oscillatorsAndColourChangeKeepWithinTheirRanges()
    {
        Item item;
        const auto def = define(R"("maxcount":200,"emitter":[{"name":"sphererandom","rate":2000,"distancemax":5}],"initializer":[{"name":"lifetimerandom","min":50,"max":50},)"
                                R"({"name":"colorrandom","min":"255 255 255","max":"255 255 255"}],"operator":[{"name":"oscillatealpha","scalemin":0.25,"scalemax":0.75,"frequencymin":2,"frequencymax":6},)"
                                R"({"name":"colorchange","starttime":0,"endtime":1,"startvalue":"1 1 1","endvalue":"0 0 0"}])",
                                item);
        ParticleSystem s(def, {}, 3);
        for (int i = 0; i < 120; ++i)
            s.advance(1.0 / 60.0);
        QVERIFY(s.count() > 150);
        QVERIFY(minOf(s, Col::Alpha) >= 0.24f && maxOf(s, Col::Alpha) <= 0.76f);
        QVERIFY(maxOf(s, Col::Alpha) - minOf(s, Col::Alpha) > 0.2f); // particles are at different phases
        QVERIFY(maxOf(s, Col::R) < 1.0f && minOf(s, Col::R) >= 0.0f);
        QVERIFY(maxOf(s, Col::R) < 1.0f); // colours were normalised from 0..255 and fade to black over life
    }

    void overridesScaleSizeAlphaLifetimeSpeedAndColour()
    {
        Item item;
        const auto def = define(R"("maxcount":1,"emitter":[{"name":"sphererandom","instantaneous":1,"distancemax":0}],"initializer":[{"name":"lifetimerandom","min":4,"max":4},)"
                                R"({"name":"sizerandom","min":40,"max":40},{"name":"alpharandom","min":0.8,"max":0.8},{"name":"colorrandom","min":"255 255 255","max":"255 255 255"},)"
                                R"({"name":"velocityrandom","min":"10 0 0","max":"10 0 0"}])",
                                item);
        ParticleOverrides ov;
        ov.size = 0.5f;
        ov.alpha = 0.5f;
        ov.lifetime = 2.0f;
        ov.speed = 3.0f;
        ov.color = {1.0f, 0.5f, 0.0f};
        ParticleSystem s(def, ov, 1);
        s.advance(1.0 / 60.0);
        QCOMPARE(s.count(), size_t{1});
        QVERIFY(std::fabs(s.column(Col::Size)[0] - 40.0f * 0.5f / 2.0f) < 1e-3f);
        QVERIFY(std::fabs(s.column(Col::Alpha)[0] - 0.4f) < 1e-3f);
        QCOMPARE(s.column(Col::Lifetime)[0], 8.0f); // 4 s times the lifetime multiplier 2
    }

    void sameSeedSameSystemDifferentSeedDifferentSystem()
    {
        Item item;
        const auto def = define(R"("maxcount":300,"emitter":[{"name":"boxrandom","rate":200}],"initializer":[{"name":"lifetimerandom","min":1,"max":3},{"name":"velocityrandom"},)"
                                R"({"name":"turbulentvelocityrandom"}],"operator":[{"name":"movement"},{"name":"turbulence"}])",
                                item);
        const auto run = [&](uint32_t seed) {
            ParticleSystem s(def, {}, seed);
            for (int i = 0; i < 90; ++i)
                s.advance(1.0 / 60.0);
            return std::vector<float>(s.column(Col::X));
        };
        QCOMPARE(run(5), run(5));
        QVERIFY(run(5) != run(6));
    }

    void warmUpFillsASystemThatStartedBeforeTheWallpaper()
    {
        Item item;
        const auto def = define(R"("maxcount":1000,"starttime":4,"emitter":[{"name":"sphererandom","rate":50}],"initializer":[{"name":"lifetimerandom","min":3,"max":3}])", item);
        ParticleSystem cold(def, {}, 1);
        QCOMPARE(cold.count(), size_t{0});
        ParticleSystem warm(def, {}, 1);
        warm.warmUp();
        QVERIFY2(warm.count() >= 140 && warm.count() <= 160, qPrintable(QString::number(warm.count()))); // 50/s * 3 s
        QVERIFY(warm.time() >= 3.9);
    }

    // ---------------- hostile input ----------------

    void numbersFromUserPropertiesAreSanitised()
    {
        Item item;
        const auto d = define(R"("maxcount":200,"emitter":[{"name":"boxrandom","rate":{"user":"rate","value":50},"instantaneous":{"user":"burst","value":0},)"
                              R"("distancemax":{"user":"spread","value":"10 10 0"}}],)"
                              R"("initializer":[{"name":"lifetimerandom","min":{"user":"lo","value":1},"max":{"user":"hi","value":2}},)"
                              R"({"name":"sizerandom","min":{"user":"lo","value":1},"max":{"user":"hi","value":2}},)"
                              R"({"name":"alpharandom","min":{"user":"lo","value":1},"max":{"user":"hi","value":2}}],)"
                              R"("operator":[{"name":"movement"}])",
                              item);
        const double nan = std::nan("");
        const double inf = std::numeric_limits<double>::infinity();
        for (const double bad : {nan, inf, -inf, 1e300, -1e300, -5.0}) {
            PropertyStore props;
            props.set("rate", bad);
            props.set("burst", bad);
            props.set("spread", bad);
            props.set("lo", bad);
            props.set("hi", -bad);
            ParticleSystem s(d, {}, 3, &props);
            for (int i = 0; i < 120; ++i)
                s.advance(1.0 / 60.0);
            QVERIFY(s.count() <= 200);
            for (const Col c : {Col::X, Col::Y, Col::Alpha, Col::Size, Col::Lifetime}) {
                for (float v : s.column(c))
                    QVERIFY2(std::isfinite(v), qPrintable(QString::number(bad)));
            }
        }
        // integer() saturates instead of overflowing
        QCOMPARE(d.emitters[0].integer("instantaneous", 0), 0);
        PropertyStore huge;
        huge.set("burst", 1e300);
        QVERIFY(d.emitters[0].integer("instantaneous", 0) == 0);
        QCOMPARE(d.emitters[0].number("rate", 1.0f, &huge), 50.0f);
        huge.set("rate", std::nan(""));
        QCOMPARE(d.emitters[0].number("rate", 7.0f, &huge), 7.0f);
    }

    void overridesAreSanitised()
    {
        Item item;
        const auto d = define(R"("maxcount":100,"emitter":[{"name":"boxrandom","rate":500}],)" + kLong, item);
        ParticleOverrides ov;
        ov.count = std::nanf("");
        ov.rate = std::numeric_limits<float>::infinity();
        ov.size = -3.0f;
        ov.alpha = 1e30f;
        ov.color = {std::nanf(""), 1e30f, -1.0f};
        ParticleSystem s(d, ov, 1);
        for (int i = 0; i < 60; ++i)
            s.advance(1.0 / 60.0);
        QVERIFY(s.count() > 0 && s.count() <= 100);
        for (const Col c : {Col::R, Col::G, Col::B, Col::Alpha, Col::Size}) {
            for (float v : s.column(c))
                QVERIFY(std::isfinite(v) && v >= 0.0f);
        }
        ov.count = 1e30f; // the pool never passes the global cap
        ParticleSystem big(d, ov, 1);
        QVERIFY(big.capacity() <= ParticleSystem::kGlobalCap);
    }

    void emitterClocksAdvanceWhileThePoolIsFull()
    {
        Item item;
        // five particles of one second, an emitter that runs for half a second: it fills the pool at once, and its
        // duration must run out while the pool is full, so it stays finished when the particles die
        const auto d = define(R"("maxcount":5,"emitter":[{"name":"boxrandom","rate":1000,"duration":0.5}],)"
                              R"("initializer":[{"name":"lifetimerandom","min":1,"max":1}])",
                              item);
        ParticleSystem s(d, {}, 1);
        for (int i = 0; i < 60; ++i)
            s.advance(1.0 / 60.0);
        QCOMPARE(s.count(), size_t{5});
        for (int i = 0; i < 120; ++i)
            s.advance(1.0 / 60.0);
        QCOMPARE(s.count(), size_t{0});
        QVERIFY(!s.active());
    }

    void warmUpWorkIsBounded()
    {
        Item item;
        const auto d = define(R"("maxcount":1000000,"starttime":3600,"emitter":[{"name":"boxrandom","rate":1000000}],)"
                              R"("initializer":[{"name":"lifetimerandom","min":100,"max":100}])",
                              item);
        ParticleSystem s(d, {}, 1);
        s.warmUp();
        // a dense system stops early: far less than the two minutes a sparse one would simulate
        QVERIFY2(s.time() < 30.0, qPrintable(QString::number(s.time())));
        QVERIFY(s.count() <= ParticleSystem::kGlobalCap);
    }

    void trailHistoryHasABudget()
    {
        Item item;
        const auto d = define(R"("maxcount":1000000,"emitter":[{"name":"boxrandom","rate":100}],)" + kLong, item);
        ParticleSystem s(d, {}, 1);
        s.enableTrails(64, 0.1f);
        QVERIFY(static_cast<size_t>(s.trailPoints()) * s.capacity() <= ParticleSystem::kMaxTrailStates);
        QVERIFY(s.trailPoints() >= 1);
        ParticleSystem small(define(R"("maxcount":50)", item), {}, 1);
        small.enableTrails(64, 0.1f);
        QCOMPARE(small.trailPoints(), 64);
    }

    void childFanOutIsBounded()
    {
        Item item;
        item.file(u"particles/leaf.json"_s, R"({"material":"materials/presets/ember.json"})");
        // 300 copies of one child: past the system limit, refused with a reason, and loaded once
        QByteArray many = R"("children":[)";
        for (int i = 0; i < 300; ++i)
            many += QByteArray(i ? "," : "") + R"({"name":"particles/leaf.json"})";
        many += "]";
        const auto wide = define(many, item);
        QVERIFY(wide.systemCount <= kMaxParticleSystems);
        const auto has = [](const ParticleDefinition &def, const char *r) { return std::find(def.unsupported.begin(), def.unsupported.end(), r) != def.unsupported.end(); };
        QVERIFY(has(wide, "particles: more than 256 systems in one object"));
        // ten children of ten children of ten children: 1000 systems, refused without parsing a thousand files
        item.file(u"particles/l2.json"_s, R"({"material":"materials/presets/ember.json","children":[)"
                  + [] { QByteArray k; for (int i = 0; i < 10; ++i) k += QByteArray(i ? "," : "") + R"({"name":"particles/leaf.json"})"; return k; }() + "]}");
        item.file(u"particles/l3.json"_s, R"({"material":"materials/presets/ember.json","children":[)"
                  + [] { QByteArray k; for (int i = 0; i < 10; ++i) k += QByteArray(i ? "," : "") + R"({"name":"particles/l2.json"})"; return k; }() + "]}");
        const auto deep = define(R"("children":[)" + [] { QByteArray k; for (int i = 0; i < 10; ++i) k += QByteArray(i ? "," : "") + R"({"name":"particles/l3.json"})"; return k; }() + "]", item);
        QVERIFY(has(deep, "particles: more than 256 systems in one object"));
        QVERIFY(deep.systemCount <= kMaxParticleSystems);
        // a file that lists itself stops at the depth limit and is refused
        item.file(u"particles/self.json"_s, R"({"material":"materials/presets/ember.json","children":[{"name":"particles/self.json"}]})");
        const auto self = define(R"("children":[{"name":"particles/self.json"}])", item);
        QVERIFY(!self.unsupported.empty());
        // a few children are fine
        item.file(u"particles/ok.json"_s, R"({"material":"materials/presets/ember.json"})");
        const auto few = define(R"("children":[{"name":"particles/ok.json"},{"name":"particles/ok.json"}])", item);
        QCOMPARE(few.systemCount, size_t{3});
        QVERIFY(few.unsupported.empty());
    }

    void followerOfAFollowerIsRefused()
    {
        Item item;
        item.file(u"particles/inner.json"_s, R"({"material":"materials/presets/ember.json"})");
        item.file(u"particles/mid.json"_s, R"({"material":"materials/presets/ember.json","children":[{"name":"particles/inner.json","type":"eventfollow"}]})");
        const auto d = define(R"("children":[{"name":"particles/mid.json","type":"eventfollow"}])", item);
        QVERIFY(std::find(d.unsupported.begin(), d.unsupported.end(), "particles: nested eventfollow") != d.unsupported.end());
        // a follower with static children is not the nested case
        item.file(u"particles/mid2.json"_s, R"({"material":"materials/presets/ember.json","children":[{"name":"particles/inner.json"}]})");
        const auto ok = define(R"("children":[{"name":"particles/mid2.json","type":"eventfollow"}])", item);
        QVERIFY(std::find(ok.unsupported.begin(), ok.unsupported.end(), "particles: nested eventfollow") == ok.unsupported.end());
    }

    void controlPointsFollowThePointerAndAttractParticles()
    {
        Item item;
        const auto def = define(R"("maxcount":50,"controlpoint":[{"id":0,"flags":1,"offset":"0 0 0"},{"id":1,"flags":2,"offset":"100 200 0"}],)"
                                R"("emitter":[{"name":"sphererandom","rate":2000,"distancemax":1,"controlpoint":0}],"initializer":[{"name":"lifetimerandom","min":5,"max":5}],)"
                                R"("operator":[{"name":"movement"},{"name":"controlpointattract","controlpoint":0,"scale":1000,"threshold":5000}])",
                                item);
        ParticleSystem s(def, {}, 1);
        // system at (500, 300) in the scene: the matrix takes scene coordinates into the system's
        const Mat4 toSystem = translation({-500.0f, -300.0f, 0.0f});
        s.updateControlPoints({800.0f, 400.0f}, true, toSystem);
        QCOMPARE(s.controlPoints()[0].x, 300.0f); // follows the pointer
        QCOMPARE(s.controlPoints()[0].y, 100.0f);
        QCOMPARE(s.controlPoints()[1].x, -400.0f); // scene-space offset, seen from the system
        QCOMPARE(s.controlPoints()[1].y, -100.0f);
        s.advance(0.05);
        QVERIFY(s.count() > 0);
        // spawned around the pointer's control point
        QVERIFY(std::fabs(s.column(Col::X)[0] - 300.0f) < 5.0f);
        // the pointer moves: the attract pulls the particles towards it
        s.updateControlPoints({800.0f, 1000.0f}, true, toSystem);
        const float before = s.column(Col::Y)[0];
        for (int i = 0; i < 20; ++i)
            s.advance(1.0 / 60.0);
        QVERIFY(s.column(Col::Y)[0] > before + 30.0f); // 1000 u/s^2 for a third of a second: about 55
        // an unknown pointer leaves the control point where it was
        s.updateControlPoints({0.0f, 0.0f}, false, toSystem);
        QCOMPARE(s.controlPoints()[0].y, 700.0f);
    }

    void aLongPrewarmFillsASlowEmitterToItsSteadyState()
    {
        // the library's line particles: half a particle a second living 120 s, started 120 s in: about 60 are alive at once, not a few
        Item item;
        const auto def = define(R"("maxcount":500,"starttime":120,"emitter":[{"name":"boxrandom","rate":0.5,"distancemax":"4000 0 500"}],)"
                                R"("initializer":[{"name":"lifetimerandom","min":120,"max":120},{"name":"velocityrandom","min":"0 100 0","max":"0 100 0"}])", item);
        double total = 0;
        for (unsigned seed = 1; seed <= 8; ++seed) {
            ParticleSystem s(def, {}, seed);
            s.warmUp(); // (the renderer does this when it makes the system)
            total += static_cast<double>(s.count());
        }
        QVERIFY2(total / 8 > 40 && total / 8 < 80, qPrintable(QString::number(total / 8)));
    }

    void aSpriteWithoutAPictureIsTheWhiteOne()
    {
        Item item;
        item.file(u"materials/presets/vapor.json"_s, R"({"passes":[{"blending":"translucent","shader":"genericparticle","textures":[null]}]})");
        item.file(u"particles/v.json"_s, R"({"material":"materials/presets/vapor.json","emitter":[{"name":"boxrandom","rate":1}]})");
        const Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        std::string error;
        const auto def = loadParticleDefinition(vfs, "particles/v.json", &error);
        QVERIFY2(def, error.c_str());
        QVERIFY(def->unsupported.empty());
        QCOMPARE(def->materialPass.textures[0], std::string("util/white"));
    }

    void hsvColourRandomDrawsSaturatedHues()
    {
        // the conversion
        const auto near = [](Vec3 a, Vec3 b) { return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f && std::fabs(a.z - b.z) < 1e-4f; };
        QVERIFY(near(hsvToRgb(0.0f, 1.0f, 1.0f), {1, 0, 0}));
        QVERIFY(near(hsvToRgb(1.0f / 3.0f, 1.0f, 1.0f), {0, 1, 0}));
        QVERIFY(near(hsvToRgb(2.0f / 3.0f, 1.0f, 1.0f), {0, 0, 1}));
        QVERIFY(near(hsvToRgb(1.5f, 1.0f, 0.5f), {0, 0.5f, 0.5f})); // the hue wraps
        QVERIFY(near(hsvToRgb(0.3f, 0.0f, 0.25f), {0.25f, 0.25f, 0.25f}));
        // a system: hues from cyan to magenta (magic_color_sparkle's range), full saturation and value
        Item item;
        const auto def = define(R"("maxcount":200,"emitter":[{"name":"sphererandom","instantaneous":200,"distancemax":10}],)"
                                R"("initializer":[{"name":"lifetimerandom","min":5,"max":5},{"name":"hsvcolorrandom","huemin":0.49,"huemax":0.93,"saturationmin":1,"valuemin":1}])",
                                item);
        QVERIFY(def.unsupported.empty());
        ParticleSystem sys(def, {}, 3);
        sys.advance(0.05);
        QCOMPARE(sys.count(), size_t{200});
        bool someGreen = false, someRed = false;
        for (size_t i = 0; i < sys.count(); ++i) {
            const float r = sys.column(Col::R)[i], g = sys.column(Col::G)[i], b = sys.column(Col::B)[i];
            QVERIFY(std::fabs(std::max({r, g, b}) - 1.0f) < 1e-4f); // value 1
            QVERIFY(std::min({r, g, b}) < 1e-4f);                  // saturation 1
            QVERIFY(!(r > 0.01f && g > 0.01f));                    // nothing from the yellows: red and green never mix
            someGreen = someGreen || g > 0.5f;
            someRed = someRed || r > 0.5f;
        }
        QVERIFY(someGreen && someRed);
    }

    void boidsGatherAlignAndKeepApart()
    {
        Item item;
        std::vector<std::unique_ptr<ParticleDefinition>> defs;
        const auto spread = [](const ParticleSystem &s, Col a, Col b) {
            double ma = 0, mb = 0;
            for (size_t i = 0; i < s.count(); ++i) {
                ma += s.column(a)[i];
                mb += s.column(b)[i];
            }
            ma /= static_cast<double>(s.count());
            mb /= static_cast<double>(s.count());
            double d = 0;
            for (size_t i = 0; i < s.count(); ++i)
                d += std::hypot(s.column(a)[i] - ma, s.column(b)[i] - mb);
            return d / static_cast<double>(s.count());
        };
        const auto run = [&](const QByteArray &boids, float distance, float speed) {
            const QByteArray json = "\"maxcount\":60,\"emitter\":[{\"name\":\"sphererandom\",\"instantaneous\":60,\"distancemax\":" + QByteArray::number(distance)
                + "}],\"initializer\":[{\"name\":\"lifetimerandom\",\"min\":50,\"max\":50},{\"name\":\"velocityrandom\",\"min\":\"-" + QByteArray::number(speed)
                + " -" + QByteArray::number(speed) + " 0\",\"max\":\"" + QByteArray::number(speed) + " " + QByteArray::number(speed) + " 0\"}],"
                  "\"operator\":[{\"name\":\"movement\"}" + (boids.isEmpty() ? QByteArray() : "," + boids) + "]";
            const ParticleDefinition &def = *defs.emplace_back(std::make_unique<ParticleDefinition>(define(json, item))); // the system keeps a reference
            if (!def.unsupported.empty())
                qFatal("%s", def.unsupported[0].c_str());
            auto sys = std::make_unique<ParticleSystem>(def, ParticleOverrides{}, 9);
            sys->advance(0.02);
            for (int i = 0; i < 60; ++i)
                sys->advance(1.0 / 60.0);
            return sys;
        };
        // cohesion: a loose cloud stays tighter than without it
        const auto free = run({}, 60, 0);
        const auto gathered = run(R"({"name":"boids","neighborthreshold":150,"cohesionfactor":5})", 60, 0);
        QVERIFY2(spread(*gathered, Col::X, Col::Y) < 0.7 * spread(*free, Col::X, Col::Y),
                 qPrintable(QString("%1 %2").arg(spread(*gathered, Col::X, Col::Y)).arg(spread(*free, Col::X, Col::Y))));
        // alignment: the velocities come together
        const auto wild = run({}, 30, 50);
        const auto aligned = run(R"({"name":"boids","neighborthreshold":150,"alignmentfactor":5})", 30, 50);
        QVERIFY(spread(*aligned, Col::VX, Col::VY) < 0.3 * spread(*wild, Col::VX, Col::VY));
        // separation: a tight knot opens up
        const auto knot = run({}, 3, 0);
        const auto apart = run(R"({"name":"boids","neighborthreshold":40,"separationfactor":5})", 3, 0);
        QVERIFY(spread(*apart, Col::X, Col::Y) > 2.0 * spread(*knot, Col::X, Col::Y));
        // beyond the neighbour distance nothing steers
        const auto far = run(R"({"name":"boids","neighborthreshold":0.001,"cohesionfactor":5,"alignmentfactor":5})", 60, 50);
        QCOMPARE(spread(*far, Col::VX, Col::VY), spread(*run({}, 60, 50), Col::VX, Col::VY));
    }

    void aVortexPushesParticlesAroundItsAxis()
    {
        Item item;
        const auto spin = [&](const char *origin, const char *axis, float *vx, float *vy, float *vz) {
            const QByteArray json = QByteArray("\"maxcount\":4,\"emitter\":[{\"name\":\"sphererandom\",\"instantaneous\":1,\"distancemax\":0,\"origin\":\"") + origin
                + "\"}],\"initializer\":[{\"name\":\"lifetimerandom\",\"min\":5,\"max\":5}],"
                  "\"operator\":[{\"name\":\"vortex\",\"axis\":\"" + axis + "\",\"distanceinner\":0,\"distanceouter\":50,\"speedinner\":100,\"speedouter\":0}]";
            const auto def = define(json, item);
            ParticleSystem s(def, {}, 1);
            s.advance(0.1);
            QVERIFY(s.count() == 1);
            *vx = s.column(Col::VX)[0];
            *vy = s.column(Col::VY)[0];
            *vz = s.column(Col::VZ)[0];
        };
        float vx = 0, vy = 0, vz = 0;
        spin("0.5 0 0", "0 0 1", &vx, &vy, &vz); // hardly off the axis: about the full inner speed, 100 u/s for 0.1 s, along +y (axis x radial)
        QVERIFY2(std::fabs(vx) < 0.1f && std::fabs(vy - 10.0f) < 0.5f && std::fabs(vz) < 0.1f, qPrintable(QString("%1 %2 %3").arg(vx).arg(vy).arg(vz)));
        spin("25 0 0", "0 0 1", &vx, &vy, &vz); // half way out: half the speed
        QVERIFY2(std::fabs(vy - 5.0f) < 0.5f, qPrintable(QString::number(vy)));
        spin("60 0 0", "0 0 1", &vx, &vy, &vz); // beyond the outer distance: nothing
        QVERIFY(std::fabs(vx) < 1e-4f && std::fabs(vy) < 1e-4f);
        spin("0 25 0", "0 0 1", &vx, &vy, &vz); // above the axis the circle runs to -x
        QVERIFY2(vx < -4.0f && std::fabs(vy) < 0.5f, qPrintable(QString("%1 %2").arg(vx).arg(vy)));
        spin("25 0 7", "0 0 1", &vx, &vy, &vz); // height along the axis does not matter
        QVERIFY2(std::fabs(vy - 5.0f) < 0.5f && std::fabs(vz) < 0.1f, qPrintable(QString("%1 %2").arg(vy).arg(vz)));
        spin("25 0 0", "0 0 0", &vx, &vy, &vz); // a zero axis does nothing
        QVERIFY(std::fabs(vy) < 1e-4f);
    }

    void remapValueSetsVelocityOrSpeedFromNoiseOfThePosition()
    {
        Item item;
        const auto run = [&](const QByteArray &remap, const char *origin, size_t n = 1) {
            const QByteArray json = QByteArray("\"maxcount\":") + QByteArray::number(static_cast<int>(n))
                + ",\"emitter\":[{\"name\":\"boxrandom\",\"instantaneous\":" + QByteArray::number(static_cast<int>(n)) + ",\"distancemax\":\"400 400 0\",\"origin\":\"" + origin
                + "\"}],\"initializer\":[{\"name\":\"lifetimerandom\",\"min\":5,\"max\":5},{\"name\":\"velocityrandom\",\"min\":\"0 0 0\",\"max\":\"0 30 0\"}],"
                  "\"operator\":[" + remap + "]";
            const auto def = define(json, item);
            std::unique_ptr<ParticleSystem> s(new ParticleSystem(def, {}, 1));
            s->advance(0.05);
            return s;
        };
        // velocity: every component inside [min, max], and not the same for particles in different places
        const QByteArray velocity = "{\"name\":\"remapvalue\",\"operation\":\"remap\",\"output\":\"velocity\",\"outputrangemin\":\"-200 -100 0\","
                                    "\"outputrangemax\":\"200 -1000 0\",\"transformfunction\":\"simplexnoise\",\"transforminputscale\":10}";
        auto many = run(velocity, "0 0 0", 64);
        QCOMPARE(many->count(), size_t{64});
        float lowest = 1e9f, highest = -1e9f;
        for (size_t i = 0; i < many->count(); ++i) {
            const float vx = many->column(Col::VX)[i], vy = many->column(Col::VY)[i];
            QVERIFY2(vx >= -200.01f && vx <= 200.01f && vy <= -99.99f && vy >= -1000.01f, qPrintable(QString("%1 %2").arg(vx).arg(vy)));
            lowest = std::min(lowest, vx);
            highest = std::max(highest, vx);
        }
        QVERIFY2(highest - lowest > 60.0f, qPrintable(QString("%1 %2").arg(lowest).arg(highest))); // they differ
        // speed: the direction of the velocity stays, its length is the remapped value
        const QByteArray speed = "{\"name\":\"remapvalue\",\"output\":\"speed\",\"outputrangemin\":50,\"outputrangemax\":50,"
                                 "\"transformfunction\":\"fbmnoise\",\"transforminputscale\":8}";
        auto fixed = run(speed, "0 0 0", 8);
        for (size_t i = 0; i < fixed->count(); ++i) {
            const float vx = fixed->column(Col::VX)[i], vy = fixed->column(Col::VY)[i], vz = fixed->column(Col::VZ)[i];
            QVERIFY2(std::fabs(std::sqrt(vx * vx + vy * vy + vz * vz) - 50.0f) < 0.01f, qPrintable(QString::number(vx)));
            QVERIFY(vy >= 0.0f); // the random velocity went up, and so does the remapped one
        }
        // an output it does not know changes nothing
        auto none = run("{\"name\":\"remapvalue\",\"output\":\"glitter\",\"outputrangemin\":1,\"outputrangemax\":2}", "0 0 0", 4);
        QCOMPARE(none->count(), size_t{4});
    }

    void curlNoiseIsSmoothAndBounded()
    {
        float maxLen = 0.0f;
        for (int i = 0; i < 200; ++i) {
            const Vec3 p{i * 0.37f, i * 0.11f - 5.0f, i * 0.23f};
            const Vec3 c = curlNoise(p);
            QVERIFY(std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z));
            maxLen = std::max(maxLen, std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z));
            const Vec3 q = curlNoise({p.x + 0.001f, p.y, p.z});
            QVERIFY(std::fabs(q.x - c.x) < 0.05f && std::fabs(q.y - c.y) < 0.05f);
        }
        QVERIFY(maxLen > 0.1f && maxLen < 100.0f);
    }

    void curlNoiseIsDivergenceFree()
    {
        // the field is a curl, so its divergence is zero wherever the gradients are right (a wrong derivative shows here)
        float worst = 0.0f, mean = 0.0f;
        int n = 0;
        for (int i = 0; i < 400; ++i) {
            const Vec3 p{0.37f * static_cast<float>(i) + 0.11f, 1.9f - 0.23f * static_cast<float>(i), 0.07f * static_cast<float>(i * i % 17) + 0.5f};
            constexpr float e = 0.01f;
            const float div = (curlNoise({p.x + e, p.y, p.z}).x - curlNoise({p.x - e, p.y, p.z}).x
                               + curlNoise({p.x, p.y + e, p.z}).y - curlNoise({p.x, p.y - e, p.z}).y
                               + curlNoise({p.x, p.y, p.z + e}).z - curlNoise({p.x, p.y, p.z - e}).z) / (2.0f * e);
            worst = std::max(worst, std::fabs(div));
            { const Vec3 c = curlNoise(p); mean += std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z); }
            ++n;
        }
        QVERIFY2(worst < 0.5f, qPrintable(QString::number(worst)));
        QVERIFY(mean / static_cast<float>(n) > 0.2f); // and it is not the zero field
    }

    void mapSequenceBetweenControlPointsPlacesParticlesAlongTheLine()
    {
        Item item;
        const auto body = [](const char *behaviour) {
            return QByteArray(R"("maxcount":50,"controlpoint":[{"id":0,"flags":0,"offset":"0 0 0"},{"id":1,"flags":2,"offset":"100 40 0"}],)"
                              R"("emitter":[{"name":"sphererandom","instantaneous":9,"distancemax":0}],"initializer":[{"name":"lifetimerandom","min":50,"max":50},)"
                              R"({"name":"mapsequencebetweencontrolpoints","count":5,"limitbehavior":")") + behaviour + "\"}]";
        };
        const auto xs = [&](const char *behaviour, std::optional<Vec3> override_ = {}) {
            const auto def = define(body(behaviour), item);
            ParticleOverrides ov;
            ov.controlPoints[1] = override_;
            ParticleSystem s(def, ov, 1);
            s.updateControlPoints({0, 0}, false, Mat4::identity());
            s.advance(1.0 / 60.0);
            std::vector<float> out(s.column(Col::X));
            return out;
        };
        // places 0..4 along x = 0..100 (and y = 0..40), nine particles in turn
        const auto mirror = xs("mirror");
        QCOMPARE(mirror.size(), size_t{9});
        const std::vector<float> expectMirror{0, 25, 50, 75, 100, 75, 50, 25, 0};
        for (size_t i = 0; i < mirror.size(); ++i)
            QVERIFY2(std::fabs(mirror[i] - expectMirror[i]) < 0.01f, qPrintable(QString("mirror %1: %2").arg(i).arg(mirror[i])));
        const auto wrap = xs("wrap");
        const std::vector<float> expectWrap{0, 25, 50, 75, 100, 0, 25, 50, 75};
        for (size_t i = 0; i < wrap.size(); ++i)
            QVERIFY2(std::fabs(wrap[i] - expectWrap[i]) < 0.01f, qPrintable(QString("wrap %1: %2").arg(i).arg(wrap[i])));
        const auto keep = xs("clamp");
        QVERIFY(std::fabs(keep.back() - 100.0f) < 0.01f);
        // the object's own `controlpoint1` moves the far end
        const auto moved = xs("wrap", Vec3{200, 0, 0});
        QVERIFY(std::fabs(moved[4] - 200.0f) < 0.01f);
    }

    void mapSequenceAroundSpreadsTheDirections()
    {
        Item item;
        const auto def = define(R"("maxcount":50,"emitter":[{"name":"sphererandom","instantaneous":4,"distancemax":0}],"initializer":[{"name":"lifetimerandom","min":50,"max":50},)"
                                R"({"name":"mapsequencearoundcontrolpoint","count":4,"speedmin":"100 0 0","speedmax":"100 0 0"}])",
                                item);
        ParticleSystem s(def, {}, 1);
        s.advance(1.0 / 60.0);
        QCOMPARE(s.count(), size_t{4});
        const auto &vx = s.column(Col::VX);
        const auto &vy = s.column(Col::VY);
        QVERIFY(std::fabs(vx[0] - 100.0f) < 0.1f && std::fabs(vy[0]) < 0.1f); // 0 degrees
        QVERIFY(std::fabs(vx[1]) < 0.1f && std::fabs(vy[1] - 100.0f) < 0.1f); // 90
        QVERIFY(std::fabs(vx[2] + 100.0f) < 0.1f);                              // 180
        QVERIFY(std::fabs(vy[3] + 100.0f) < 0.1f);                              // 270
    }

    void sheetFramesFollowTheModes()
    {
        Item item;
        const auto def = define(R"("maxcount":1,"animationmode":"sequence","emitter":[{"name":"sphererandom","instantaneous":1,"distancemax":0}],"initializer":[{"name":"lifetimerandom","min":4,"max":4}])", item);
        ParticleSystem s(def, {}, 1);
        s.setSpriteSheet(8, 0.0f); // frames spread over the lifetime
        for (int i = 0; i < 120; ++i)
            s.advance(1.0 / 60.0); // half the life
        QVERIFY(std::fabs(s.column(Col::Frame)[0] - 4.0f) < 0.3f);
        // WE knows only "randomframe": any other mode, "once" included, loops the sequence
        const auto once = define(R"("maxcount":1,"animationmode":"once","sequencemultiplier":3,"emitter":[{"name":"sphererandom","instantaneous":1,"distancemax":0}],"initializer":[{"name":"lifetimerandom","min":4,"max":4}])", item);
        ParticleSystem o(once, {}, 1);
        o.setSpriteSheet(8, 0.0f);
        for (int i = 0; i < 120; ++i)
            o.advance(1.0 / 60.0);
        QVERIFY(std::fabs(o.column(Col::Frame)[0] - 4.0f) < 0.3f); // 8 frames x 3 at half the life: 12, wrapped to 4 (not clamped to 7)
    }

    // ---- collisions ----

    void collisionOperatorsAreAcceptedAndOddBehavioursDegrade()
    {
        Item item;
        const auto d = define(R"("operator":[{"name":"collisionplane"},{"name":"collisionsphere"},{"name":"collisionbox"},{"name":"collisionquad"},)"
                              R"({"name":"collisionbounds"},{"name":"collisionplane","collisionbehavior":"explode"},{"name":"collisionmodel"}])",
                              item);
        QCOMPARE(d.unsupported.size(), size_t{1});
        QCOMPARE(d.unsupported[0], std::string("particles: operator collisionmodel"));
        QCOMPARE(d.degraded.size(), size_t{1});
        QCOMPARE(d.degraded[0], std::string("particle collision behavior explode"));
    }

    void collisionPlaneBouncesSlidesStopsAndDeletes()
    {
        Item item;
        // falling onto the plane y = -100 from y = 0 at 300 u/s: there in a third of a second
        auto bounce = single(item, "0 0 0", "30 -300 0", R"({"name":"collisionplane","distance":-100,"bouncefactor":0.5})");
        run(*bounce, 0.4);
        QVERIFY2(bounce->column(Col::Y)[0] >= -100.0f, qPrintable(QString::number(bounce->column(Col::Y)[0])));
        QVERIFY2(std::fabs(bounce->column(Col::VY)[0] - 150.0f) < 1e-2f, qPrintable(QString::number(bounce->column(Col::VY)[0])));
        QCOMPARE(bounce->column(Col::VX)[0], 30.0f);
        auto slide = single(item, "0 0 0", "30 -300 0", R"({"name":"collisionplane","distance":-100,"collisionbehavior":"slide"})");
        run(*slide, 0.6);
        QVERIFY(std::fabs(slide->column(Col::Y)[0] + 100.0f) < 0.01f);
        QCOMPARE(slide->column(Col::VY)[0], 0.0f);
        QCOMPARE(slide->column(Col::VX)[0], 30.0f);
        auto stop = single(item, "0 0 0", "30 -300 0", R"({"name":"collisionplane","distance":-100,"collisionbehavior":"stop"})");
        run(*stop, 0.6);
        QCOMPARE(stop->column(Col::VX)[0], 0.0f);
        QVERIFY(std::fabs(stop->column(Col::Y)[0] + 100.0f) < 0.01f);
        auto gone = single(item, "0 0 0", "30 -300 0", R"({"name":"collisionplane","distance":-100,"collisionbehavior":"delete"})");
        run(*gone, 0.6);
        QCOMPARE(gone->count(), size_t{0});
        // a tilted plane (normal along +x through x = 50) and a particle that starts behind a plane is left alone
        auto tilted = single(item, "0 0 0", "300 0 0", R"({"name":"collisionplane","plane":"-1 0 0","distance":-50})");
        run(*tilted, 0.4);
        QVERIFY(tilted->column(Col::X)[0] <= 50.0f && tilted->column(Col::VX)[0] < 0.0f);
        auto behind = single(item, "0 -200 0", "0 -100 0", R"({"name":"collisionplane","distance":-100})");
        run(*behind, 0.5);
        QVERIFY(behind->column(Col::Y)[0] < -240.0f);
    }

    void collisionSphereKeepsParticlesOutside()
    {
        Item item;
        // dropped with gravity on a sphere of radius 50 at (0, -100), sliding off it
        auto withGravity = define(R"("maxcount":1,"emitter":[{"name":"sphererandom","instantaneous":1,"distancemax":0,"origin":"10 0 0"}],)"
                                  R"("initializer":[{"name":"lifetimerandom","min":100,"max":100}],)"
                                  R"("operator":[{"name":"movement","gravity":"0 -500 0"},{"name":"collisionsphere","origin":"0 -100 0","radius":50,"collisionbehavior":"slide"}])",
                                  item);
        ParticleSystem g(withGravity, {}, 1);
        float closest = 1e9f;
        for (int i = 0; i < 90; ++i) {
            g.advance(1.0 / 60.0);
            closest = std::min(closest, std::hypot(g.column(Col::X)[0], g.column(Col::Y)[0] + 100.0f));
        }
        QVERIFY2(closest >= 49.99f, qPrintable(QString::number(closest)));
        QVERIFY(g.column(Col::X)[0] > 30.0f); // it slid off to the side it was on
        // straight on with a bounce: back up
        auto head = single(item, "0 0 0", "0 -200 0", R"({"name":"collisionsphere","origin":"0 -100 0","radius":50})");
        run(*head, 0.5);
        QVERIFY(head->column(Col::VY)[0] > 199.0f && head->column(Col::Y)[0] > -50.0f);
    }

    void collisionBoxIsHitFromOutside()
    {
        Item item;
        // a box 50 wide around x = 100: a particle from the left bounces off its left face
        auto s = single(item, "0 0 0", "200 0 0", R"({"name":"collisionbox","origin":"100 0 0","size":"50 50 50"})");
        run(*s, 0.5);
        QVERIFY2(s->column(Col::VX)[0] < -199.0f && s->column(Col::X)[0] < 75.0f, qPrintable(QString::number(s->column(Col::X)[0])));
        // one that starts inside flies out
        auto inside = single(item, "100 0 0", "200 0 0", R"({"name":"collisionbox","origin":"100 0 0","size":"50 50 50"})");
        run(*inside, 0.5);
        QVERIFY(inside->column(Col::X)[0] > 190.0f);
    }

    void collisionQuadIsHitInsideItsRectangleAndFromTheFront()
    {
        Item item;
        const QByteArray quad = R"({"name":"collisionquad","origin":"0 -100 0","size":"100 100"})";
        auto onIt = single(item, "20 0 0", "0 -300 0", quad);
        run(*onIt, 0.5);
        QVERIFY(onIt->column(Col::VY)[0] > 299.0f && onIt->column(Col::Y)[0] >= -100.0f);
        auto beside = single(item, "80 0 0", "0 -300 0", quad);
        run(*beside, 0.5);
        QVERIFY(beside->column(Col::Y)[0] < -140.0f);
        auto below = single(item, "0 -200 0", "0 300 0", quad);
        run(*below, 0.5);
        QVERIFY(below->column(Col::Y)[0] > -60.0f); // from behind it passes
        // the library's wide floor: 4000 x 200 at y = -300
        auto floor = single(item, "1500 0 0", "0 -600 0", R"({"name":"collisionquad","controlpoint":0.1,"origin":"0 -300 0","size":"4000 200","collisionbehavior":"stop"})");
        run(*floor, 1.0);
        QVERIFY(std::fabs(floor->column(Col::Y)[0] + 300.0f) < 0.01f);
    }

    void collisionBoundsKeepParticlesInTheScene()
    {
        Item item;
        auto s = single(item, "0 0 0", "1000 -400 0", R"({"name":"collisionbounds"})");
        // a 200 x 100 scene with the system at its middle
        const Mat4 toScene = translation({100.0f, 50.0f, 0.0f});
        s->setSceneSpace({200.0f, 100.0f}, toScene, inverse(toScene));
        float lowX = 1e9f, highX = -1e9f, lowY = 1e9f, highY = -1e9f;
        for (int i = 0; i < 120; ++i) {
            s->advance(1.0 / 60.0);
            lowX = std::min(lowX, s->column(Col::X)[0]);
            highX = std::max(highX, s->column(Col::X)[0]);
            lowY = std::min(lowY, s->column(Col::Y)[0]);
            highY = std::max(highY, s->column(Col::Y)[0]);
        }
        QVERIFY2(lowX >= -100.0f && highX <= 100.0f && lowY >= -50.0f && highY <= 50.0f,
                 qPrintable(QString("%1 %2 %3 %4").arg(lowX).arg(highX).arg(lowY).arg(highY)));
        QVERIFY(highX > 99.0f && lowX < -99.0f); // it went from wall to wall
        // without a scene there are no bounds
        auto free = single(item, "0 0 0", "1000 0 0", R"({"name":"collisionbounds"})");
        run(*free, 0.5);
        QVERIFY(free->column(Col::X)[0] > 400.0f);
        // a system scaled by 2 in the scene: the walls are where the scene's are
        auto scaled = single(item, "0 0 0", "1000 0 0", R"({"name":"collisionbounds","collisionbehavior":"stop"})");
        const Mat4 big = translation({100.0f, 50.0f, 0.0f}) * scaling({2.0f, 2.0f, 2.0f});
        scaled->setSceneSpace({200.0f, 100.0f}, big, inverse(big));
        run(*scaled, 0.5);
        QVERIFY(std::fabs(scaled->column(Col::X)[0] - 50.0f) < 0.01f);
    }
    // ---- the remaining operators ----

    void capVelocityLimitsTheSpeedInsideItsBlend()
    {
        Item item;
        auto s = single(item, "0 0 0", "300 400 0", R"({"name":"capvelocity","maxspeed":100})");
        run(*s, 0.1);
        const float speed = std::hypot(s->column(Col::VX)[0], s->column(Col::VY)[0]);
        QVERIFY2(std::fabs(speed - 100.0f) < 1e-3f, qPrintable(QString::number(speed)));
        QVERIFY(std::fabs(s->column(Col::VX)[0] - 60.0f) < 1e-3f); // the direction is kept
        // blended in from half to 0.6 of the life (100 s): untouched before
        auto later = single(item, "0 0 0", "300 400 0", R"({"name":"capvelocity","maxspeed":100,"blendinstart":0.5,"blendinend":0.6})");
        run(*later, 0.5);
        QVERIFY(std::fabs(std::hypot(later->column(Col::VX)[0], later->column(Col::VY)[0]) - 500.0f) < 1e-2f);
    }

    void vortexV2SwirlsAtEveryDistanceAndPullsOntoTheRing()
    {
        Item item;
        // beyond distanceouter the outer speed still applies (the presets' 32 unit vortex swirls particles 256 units out)
        auto far = single(item, "100 0 0", "0 0 0", R"({"name":"vortex_v2","distanceinner":0,"distanceouter":32,"speedinner":0,"speedouter":1000})");
        run(*far, 0.1);
        QVERIFY2(std::fabs(far->column(Col::VY)[0] - 100.0f) < 5.0f, qPrintable(QString::number(far->column(Col::VY)[0])));
        // the centre force pulls towards the axis
        auto pulled = single(item, "100 0 0", "0 0 0", R"({"name":"vortex_v2","centerforce":200})");
        run(*pulled, 0.1);
        QVERIFY(std::fabs(pulled->column(Col::VX)[0] + 20.0f) < 0.5f);
        // flag 2 keeps the distance: a particle flying out at 100 u/s stays on its circle while it turns
        auto kept = single(item, "50 0 0", "100 0 0", R"({"name":"vortex_v2","flags":3,"speedouter":200})");
        run(*kept, 1.0);
        const float r = std::hypot(kept->column(Col::X)[0], kept->column(Col::Y)[0]);
        QVERIFY2(std::fabs(r - 51.7f) < 2.0f, qPrintable(QString::number(r))); // (the first step moves before the operator sees it)
        QVERIFY(kept->column(Col::Y)[0] > 20.0f || kept->column(Col::X)[0] < 40.0f); // it went round
        // without flag 1 the distance is the one to the centre: far along the axis the outer speed applies even on the axis' line
        auto spherical = single(item, "0.5 0 300", "0 0 0", R"({"name":"vortex_v2","distanceouter":100,"speedinner":0,"speedouter":100})");
        run(*spherical, 0.1);
        QVERIFY(spherical->column(Col::VY)[0] > 9.0f);
        auto line = single(item, "0.5 0 300", "0 0 0", R"({"name":"vortex_v2","flags":1,"distanceouter":100,"speedinner":0,"speedouter":100})");
        run(*line, 0.1);
        QVERIFY(line->column(Col::VY)[0] < 0.2f);
        // flag 4: onto the ring 50 out, from inside and from outside
        for (const char *origin : {"10 0 0", "120 0 0"}) {
            auto ring = single(item, origin, "0 0 0", R"({"name":"vortex_v2","flags":4,"ringradius":50,"ringwidth":2,"ringpulldistance":200,"speedouter":20,"distanceouter":1})");
            run(*ring, 4.0);
            const float radius = std::hypot(ring->column(Col::X)[0], ring->column(Col::Y)[0]);
            QVERIFY2(std::fabs(radius - 50.0f) < 6.0f, qPrintable(QString::number(radius)));
        }
        // beyond the pull distance the ring does not reach
        auto out = single(item, "500 0 0", "0 0 0", R"({"name":"vortex_v2","flags":4,"ringradius":50,"ringpulldistance":100})");
        run(*out, 1.0);
        QCOMPARE(out->column(Col::X)[0], 500.0f);
    }

    void maintainDistanceToControlPointKeepsTheRadius()
    {
        Item item;
        auto kept = single(item, "100 0 0", "200 0 0", R"({"name":"maintaindistancetocontrolpoint","variablestrength":20})");
        run(*kept, 1.0);
        const float r = std::hypot(kept->column(Col::X)[0], kept->column(Col::Y)[0]);
        QVERIFY2(std::fabs(r - 100.0f) < 15.0f, qPrintable(QString::number(r))); // free it would be 300 out
        auto given = single(item, "100 0 0", "0 0 0", R"({"name":"maintaindistancetocontrolpoint","distance":40})");
        run(*given, 2.0);
        QVERIFY(std::fabs(given->column(Col::X)[0] - 40.0f) < 0.1f);
    }

    void maintainDistanceBetweenControlPointsKeepsThePlaceOnTheLine()
    {
        Item item;
        auto s = single(item, "25 0 0", "100 50 0", R"({"name":"maintaindistancebetweencontrolpoints"})", {},
                        R"("controlpoint":[{"id":0,"offset":"0 0 0"},{"id":1,"offset":"100 0 0"}],)");
        s->updateControlPoints({}, false, Mat4::identity());
        run(*s, 1.0 / 60.0); // the place is taken when the operator first sees the particle
        const float x0 = s->column(Col::X)[0];
        QVERIFY(x0 > 25.0f && x0 < 27.0f);
        run(*s, 0.5);
        QVERIFY2(std::fabs(s->column(Col::X)[0] - x0) < 0.01f, qPrintable(QString::number(s->column(Col::X)[0])));
        QCOMPARE(s->column(Col::VX)[0], 0.0f); // the push along the line is undone
        QVERIFY(std::fabs(s->column(Col::Y)[0] - 50.0f * 31.0f / 60.0f) < 0.5f); // across the line it moves freely
        // the end moves: the particle keeps its share of the way
        s->setControlPoint(1, {200.0f, 0.0f, 0.0f});
        run(*s, 1.0 / 60.0);
        QVERIFY2(std::fabs(s->column(Col::X)[0] - 2.0f * x0) < 0.01f, qPrintable(QString::number(s->column(Col::X)[0])));
    }

    void reduceMovementNearAControlPoint()
    {
        Item item;
        const QByteArray reduce = R"({"name":"reducemovementnearcontrolpoint","distanceinner":50,"distanceouter":150,"reductioninner":10,"reductionouter":0})";
        auto near = single(item, "0 0 0", "0 100 0", reduce);
        run(*near, 0.1);
        QVERIFY2(std::fabs(near->column(Col::VY)[0] - 100.0f * std::exp(-1.0f)) < 1.0f, qPrintable(QString::number(near->column(Col::VY)[0])));
        auto far = single(item, "1000 0 0", "0 100 0", reduce);
        run(*far, 0.1);
        QCOMPARE(far->column(Col::VY)[0], 100.0f);
        auto between = single(item, "100 0 0", "0 1 0", reduce); // half way: 5 per second
        run(*between, 0.1);
        QVERIFY(std::fabs(between->column(Col::VY)[0] - std::exp(-0.5f)) < 0.02f);
    }

    void remapValueFollowsAClockThroughATransform()
    {
        Item item;
        auto s = single(item, "0 0 0", "0 0 0", R"({"name":"remapvalue","input":"particlesystemtime","output":"opacity","transformfunction":"sine"})");
        run(*s, 0.25);
        QVERIFY2(s->column(Col::Alpha)[0] > 0.99f, qPrintable(QString::number(s->column(Col::Alpha)[0])));
        run(*s, 0.5);
        QVERIFY(s->column(Col::Alpha)[0] < 0.01f);
        // multiply on a value no earlier operator rebuilds works on the initial value: it does not pile up
        auto twice = single(item, "0 0 0", "0 0 0", R"({"name":"remapvalue","input":"lifetimefraction","output":"size","operation":"multiply","outputrangemin":2,"outputrangemax":2})",
                            R"(,{"name":"sizerandom","min":10,"max":10})");
        run(*twice, 1.0);
        QCOMPARE(twice->column(Col::Size)[0], 10.0f);
        // a velocity input, per component, onto a velocity: the saw of x
        auto saw = single(item, "0 0 0", "0.25 0.5 0", R"({"name":"remapvalue","input":"velocity","output":"velocity","transformfunction":"triangle","outputrangemin":"0 0 0","outputrangemax":"8 8 0"})");
        run(*saw, 1.0 / 60.0);
        QVERIFY2(std::fabs(saw->column(Col::VX)[0] - 4.0f) < 1e-3f && std::fabs(saw->column(Col::VY)[0] - 8.0f) < 1e-3f,
                 qPrintable(QString("%1 %2").arg(saw->column(Col::VX)[0]).arg(saw->column(Col::VY)[0])));
    }

    void eventValuesAreInheritedEveryStep()
    {
        Item item;
        ParticleEventValues parent;
        parent.color = {1.0f, 0.25f, 0.0f};
        parent.velocity = {60.0f, 0.0f, 0.0f};
        // every step: the colour follows the parent's, and "addvelocity" carries the particle with the parent
        auto follow = single(item, "0 0 0", "0 0 0", R"({"name":"inheritvaluefromevent"},{"name":"inheritvaluefromevent","mode":"addvelocity"})");
        follow->setEventSource(parent);
        run(*follow, 0.5);
        QCOMPARE(follow->column(Col::R)[0], 1.0f);
        QCOMPARE(follow->column(Col::G)[0], 0.25f);
        QVERIFY(std::fabs(follow->column(Col::X)[0] - 30.0f) < 0.1f);
        QCOMPARE(follow->column(Col::VX)[0], 0.0f);
        parent.color = {0.0f, 1.0f, 0.0f};
        follow->setEventSource(parent);
        run(*follow, 1.0 / 60.0);
        QCOMPARE(follow->column(Col::R)[0], 0.0f);
        // the parent side: what a particle hands on
        const ParticleEventValues e = follow->eventValues(0);
        QCOMPARE(e.color.y, 1.0f);
        QVERIFY(std::fabs(e.position.x - 31.0f) < 0.1f);
    }

    void eventChildProbabilityPicksAFixedShareOfParticles()
    {
        Item item;
        item.file(u"particles/kid.json"_s, R"({"material":"materials/presets/ember.json"})");
        const auto d = define(R"("children":[{"name":"particles/kid.json","type":"eventfollow","probability":0.5},{"name":"particles/kid.json","type":"eventdeath","probability":0},)"
                              R"({"name":"particles/kid.json","type":"eventspawn"}])",
                              item);
        QCOMPARE(d.children[0].probability, 0.5f);
        QCOMPARE(d.children[1].probability, 0.0f);
        QCOMPARE(d.children[2].probability, 1.0f);
        int chosen = 0;
        for (uint32_t serial = 0; serial < 1000; ++serial) {
            chosen += particleEventChosen(serial, 7, 0.5f);
            QCOMPARE(particleEventChosen(serial, 7, 0.5f), particleEventChosen(serial, 7, 0.5f)); // the same every frame
            QVERIFY(!particleEventChosen(serial, 7, 0.0f));
            QVERIFY(particleEventChosen(serial, 7, 1.0f));
        }
        QVERIFY2(chosen > 430 && chosen < 570, qPrintable(QString::number(chosen)));
    }

    void oscillatorsFadeInWithTheirBlendWindow()
    {
        Item item;
        // the library's oscillatealpha with blendinstart / blendinend: before the window the alpha is the base one
        auto s = single(item, "0 0 0", "0 0 0", R"({"name":"oscillatealpha","frequencymin":10,"frequencymax":10,"scalemin":0,"scalemax":0,"blendinstart":0.5,"blendinend":0.6})");
        run(*s, 1.0);
        QCOMPARE(s->column(Col::Alpha)[0], 1.0f);
        auto now = single(item, "0 0 0", "0 0 0", R"({"name":"oscillatealpha","frequencymin":10,"frequencymax":10,"scalemin":0,"scalemax":0})");
        run(*now, 1.0);
        QCOMPARE(now->column(Col::Alpha)[0], 0.0f);
    }

    void blendWindowsLimitTheForceOperators()
    {
        Item item;
        // a control point pull only from half of the life (100 s) on: nothing in the first second
        auto s = single(item, "100 0 0", "0 0 0", R"({"name":"controlpointattract","scale":100,"threshold":1000,"blendinstart":0.5,"blendinend":0.6})");
        run(*s, 1.0);
        QCOMPARE(s->column(Col::VX)[0], 0.0f);
        auto now = single(item, "100 0 0", "0 0 0", R"({"name":"controlpointattract","scale":100,"threshold":1000,"blendoutstart":0.5,"blendoutend":0.6})");
        run(*now, 1.0);
        QVERIFY(now->column(Col::VX)[0] < -99.0f);
    }
    // ---- the remaining initializers ----

    void remainingNamesAreAcceptedAndOddWordsDegrade()
    {
        Item item;
        const auto d = define(R"("initializer":[{"name":"colorlist","colors":["1 0 0"]},{"name":"inheritcontrolpointvelocity"},{"name":"positionoffsetrandom"},)"
                              R"({"name":"remapinitialvalue"},{"name":"inheritinitialvaluefromevent"}],)"
                              R"("operator":[{"name":"vortex_v2"},{"name":"capvelocity"},{"name":"maintaindistancetocontrolpoint"},{"name":"maintaindistancebetweencontrolpoints"},)"
                              R"({"name":"reducemovementnearcontrolpoint"},{"name":"collisionplane"},{"name":"collisionsphere"},{"name":"collisionbox"},)"
                              R"({"name":"collisionquad"},{"name":"collisionbounds"},{"name":"inheritvaluefromevent"}])",
                              item);
        QVERIFY2(d.unsupported.empty(), d.unsupported.empty() ? "" : d.unsupported[0].c_str());
        QVERIFY(d.degraded.empty());
        QCOMPARE(d.initializers[0].lists.at("colors").size(), size_t{1});

        const auto odd = define(R"("emitter":[{"name":"layerimage"}],"operator":[{"name":"collisionmodel"},{"name":"collisionplane","collisionbehavior":"explode"},)"
                                R"({"name":"vortex_v2","flags":11},{"name":"remapvalue","input":"moonphase","output":"glitter","operation":"divide","transformfunction":"wobble"}])",
                                item);
        const auto refused = [&](const char *r) { return std::find(odd.unsupported.begin(), odd.unsupported.end(), r) != odd.unsupported.end(); };
        const auto degraded = [&](const char *r) { return std::find(odd.degraded.begin(), odd.degraded.end(), r) != odd.degraded.end(); };
        QVERIFY(refused("particles: emitter layerimage"));
        QVERIFY(refused("particles: operator collisionmodel"));
        QVERIFY(degraded("particle collision behavior explode"));
        QVERIFY(degraded("particle vortex_v2 flags 8"));
        QVERIFY(degraded("particle remap input moonphase"));
        QVERIFY(degraded("particle remap output glitter"));
        QVERIFY(degraded("particle remap operation divide"));
        QVERIFY(degraded("particle remap transform wobble"));
        // an output it reads but cannot write (a clock) is degraded as well
        const auto clock = define(R"("initializer":[{"name":"remapinitialvalue","output":"runtime"}])", item);
        QCOMPARE(clock.degraded.size(), size_t{1});
    }

    void colorListPicksOneOfTheColours()
    {
        Item item;
        const auto def = define(R"("maxcount":300,"emitter":[{"name":"sphererandom","instantaneous":300,"distancemax":0}],)"
                                R"("initializer":[{"name":"lifetimerandom","min":5,"max":5},{"name":"colorrandom","min":"255 255 255"},)"
                                R"({"name":"colorlist","colors":["0 0.5 1","1 1 0","255 0 255"]}])",
                                item);
        ParticleOverrides o;
        o.color = {0.5f, 1.0f, 1.0f};
        ParticleSystem s(def, o, 1);
        s.advance(1.0 / 60.0);
        QCOMPARE(s.count(), size_t{300});
        std::array<int, 3> seen{};
        const std::array<Vec3, 3> colours{Vec3{0.0f, 0.5f, 1.0f}, Vec3{1.0f, 1.0f, 0.0f}, Vec3{1.0f, 0.0f, 1.0f}};
        for (size_t i = 0; i < s.count(); ++i) {
            const Vec3 c{s.column(Col::R)[i], s.column(Col::G)[i], s.column(Col::B)[i]};
            int which = -1;
            for (int k = 0; k < 3; ++k) {
                if (std::fabs(c.x - colours[k].x * 0.5f) < 1e-5f && std::fabs(c.y - colours[k].y) < 1e-5f && std::fabs(c.z - colours[k].z) < 1e-5f)
                    which = k;
            }
            QVERIFY2(which >= 0, qPrintable(QString("%1 %2 %3").arg(c.x).arg(c.y).arg(c.z)));
            ++seen[static_cast<size_t>(which)];
            QCOMPARE(s.column(Col::InitialR)[i], c.x);
        }
        for (int n : seen)
            QVERIFY2(n > 60, qPrintable(QString::number(n))); // each about a third
    }

    void hsvHueStepsGiveOnlyThatManyHues()
    {
        Item item;
        const auto def = define(R"("maxcount":100,"emitter":[{"name":"sphererandom","instantaneous":100,"distancemax":0}],)"
                                R"("initializer":[{"name":"lifetimerandom","min":5,"max":5},{"name":"hsvcolorrandom","huesteps":2}])",
                                item);
        ParticleSystem s(def, {}, 1);
        s.advance(1.0 / 60.0);
        int red = 0, cyan = 0;
        for (size_t i = 0; i < s.count(); ++i) {
            const float r = s.column(Col::R)[i], g = s.column(Col::G)[i];
            red += r > 0.99f && g < 0.01f;
            cyan += r < 0.01f && g > 0.99f;
        }
        QCOMPARE(red + cyan, 100);
        QVERIFY(red > 20 && cyan > 20);
    }

    void inheritControlPointVelocityAddsTheControlPointsMotion()
    {
        Item item;
        const auto def = define(R"("maxcount":50,"emitter":[{"name":"sphererandom","rate":60,"distancemax":0}],)"
                                R"("initializer":[{"name":"lifetimerandom","min":100,"max":100},{"name":"inheritcontrolpointvelocity","controlpoint":1,"min":0.5,"max":0.5}])",
                                item);
        ParticleSystem s(def, {}, 1);
        s.setControlPoint(1, {0.0f, 0.0f, 0.0f});
        s.advance(0.1);
        s.setControlPoint(1, {10.0f, -5.0f, 0.0f}); // 100, -50 units/s
        QVERIFY(std::fabs(s.controlPointVelocities()[1].x - 100.0f) < 0.01f);
        const size_t before = s.count();
        s.advance(0.1);
        QVERIFY(s.count() > before);
        QVERIFY(std::fabs(s.column(Col::VX)[s.count() - 1] - 50.0f) < 0.01f);
        QVERIFY(std::fabs(s.column(Col::VY)[s.count() - 1] + 25.0f) < 0.01f);
        QCOMPARE(s.column(Col::VX)[0], 0.0f); // born before it moved
    }

    void positionOffsetRandomScattersAlongASmoothLine()
    {
        Item item;
        const auto def = define(R"("maxcount":64,"emitter":[{"name":"sphererandom","instantaneous":64,"distancemax":0}],)"
                                R"("initializer":[{"name":"lifetimerandom","min":5,"max":5},{"name":"positionoffsetrandom","distance":100,"scale":0,"timescale":5}])",
                                item);
        ParticleSystem s(def, {}, 1);
        s.advance(1.0 / 60.0);
        float spread = 0.0f, jump = 0.0f;
        for (size_t i = 0; i < s.count(); ++i) {
            const float x = s.column(Col::X)[i], y = s.column(Col::Y)[i];
            QVERIFY(std::fabs(x) <= 100.0f && std::fabs(y) <= 100.0f);
            QCOMPARE(s.column(Col::Z)[i], 0.0f); // a flat system stays flat
            spread = std::max(spread, std::hypot(x, y));
            if (i > 0)
                jump = std::max(jump, std::hypot(x - s.column(Col::X)[i - 1], y - s.column(Col::Y)[i - 1]));
        }
        QVERIFY2(spread > 20.0f, qPrintable(QString::number(spread)));
        QVERIFY2(jump < spread, qPrintable(QString("%1 %2").arg(jump).arg(spread))); // neighbours are near each other
    }

    void remapInitialValueMapsADistanceOntoColourAndSize()
    {
        Item item;
        const auto make = [&](const char *origin, const QByteArray &remap) {
            Sim sim;
            sim.def = std::make_unique<ParticleDefinition>(
                define("\"maxcount\":1,\"controlpoint\":[{\"id\":1,\"offset\":\"300 0 0\"}],\"emitter\":[{\"name\":\"sphererandom\",\"instantaneous\":1,"
                       "\"distancemax\":0,\"origin\":\"" + QByteArray(origin) + "\"}],\"initializer\":[{\"name\":\"lifetimerandom\",\"min\":5,\"max\":5},"
                       "{\"name\":\"sizerandom\",\"min\":40,\"max\":40}," + remap + "]",
                       item));
            sim.s = std::make_unique<ParticleSystem>(*sim.def, ParticleOverrides{}, 1);
            sim->updateControlPoints({}, false, Mat4::identity());
            sim->advance(1.0 / 60.0);
            return sim;
        };
        const QByteArray colour = R"({"name":"remapinitialvalue","input":"distancetocontrolpoint","inputcontrolpoint0":1,"inputrangemax":300,)"
                                  R"("output":"color","outputrangemin":"0 0 1","outputrangemax":"1 0 0"})";
        auto atPoint = make("300 0 0", colour);
        QVERIFY(atPoint->column(Col::B)[0] > 0.999f && atPoint->column(Col::R)[0] < 0.001f);
        auto far = make("0 0 0", colour);
        QVERIFY(far->column(Col::R)[0] > 0.999f && far->column(Col::B)[0] < 0.001f);
        QCOMPARE(far->column(Col::InitialR)[0], far->column(Col::R)[0]);
        // the lightning's taper: no output is size, multiplied by the distance over 50
        const QByteArray taper = R"({"name":"remapinitialvalue","input":"distancetocontrolpoint","inputcontrolpoint0":1,"inputrangemax":50,"operation":"multiply"})";
        auto half = make("275 0 0", taper);
        QVERIFY2(std::fabs(half->column(Col::Size)[0] - 10.0f) < 0.01f, qPrintable(QString::number(half->column(Col::Size)[0])));
        auto whole = make("0 0 0", taper);
        QCOMPARE(whole->column(Col::Size)[0], 20.0f);
    }

    void eventValuesAreInheritedAtBirth()
    {
        Item item;
        ParticleEventValues parent;
        parent.color = {1.0f, 0.25f, 0.0f};
        parent.alpha = 0.5f;
        parent.size = 7.0f;
        parent.velocity = {60.0f, 0.0f, 0.0f};
        // birth: default sets the colour; the option is read from the element's text member
        const auto birth = define(R"("maxcount":2,"emitter":[{"name":"sphererandom","instantaneous":2,"distancemax":0}],)"
                                  R"("initializer":[{"name":"lifetimerandom","min":5,"max":5},{"name":"sizerandom","min":10,"max":10},)"
                                  R"({"name":"inheritinitialvaluefromevent"},{"name":"inheritinitialvaluefromevent","value":"multiplysize"}])",
                                  item);
        ParticleSystem b(birth, {}, 1);
        b.setEventSource(parent);
        b.advance(1.0 / 60.0);
        QCOMPARE(b.column(Col::G)[0], 0.25f);
        QCOMPARE(b.column(Col::InitialG)[0], 0.25f);
        QCOMPARE(b.column(Col::Size)[0], 35.0f);
        // without a parent nothing happens
        ParticleSystem orphan(birth, {}, 1);
        orphan.advance(1.0 / 60.0);
        QCOMPARE(orphan.column(Col::G)[0], 1.0f);
    }
    // ---- review fixes ----

    void unreadMembersAreReported()
    {
        Item item;
        const auto d = define(R"("initializer":[{"name":"colorlist"},{"name":"colorlist","colors":[]},{"name":"colorlist","colors":"1 0 0"}],)"
                              R"("operator":[{"name":"remapvalue","flags":3},{"name":"movement","blendinstart":0.5},{"name":"collisionplane","blendoutend":0.9},)"
                              R"({"name":"turbulence","blendinstart":0.5},{"name":"capvelocity","blendinend":0.5}])",
                              item);
        const auto degraded = [&](const std::string &r) { return std::find(d.degraded.begin(), d.degraded.end(), r) != d.degraded.end(); };
        QVERIFY(degraded("particle remapvalue flags"));
        QVERIFY(degraded("particle operator blend window movement"));
        QVERIFY(degraded("particle operator blend window collisionplane"));
        QVERIFY(!degraded("particle operator blend window turbulence"));
        QVERIFY(!degraded("particle operator blend window capvelocity"));
        QVERIFY(degraded("particle colorlist without colors"));
        QCOMPARE(d.degraded.size(), size_t{4});
        // a list longer than the limit is cut, and said so
        QByteArray many = "\"initializer\":[{\"name\":\"colorlist\",\"colors\":[";
        for (int i = 0; i < 300; ++i)
            many += QByteArray(i ? "," : "") + "\"1 0 0\"";
        const auto big = define(many + "]}]", item);
        QCOMPARE(big.initializers[0].lists.at("colors").size(), size_t{256});
        QVERIFY(std::find(big.degraded.begin(), big.degraded.end(), "particle list colors cut to 256") != big.degraded.end());
    }

    void rainStyleRemapHonoursAnExplicitOperation()
    {
        Item item;
        // no `input` and a noise function: the position noise of the rain presets, but `operation` is now read
        auto twice = single(item, "0 0 0", "0 30 0", R"({"name":"remapvalue","output":"speed","transformfunction":"simplexnoise","outputrangemin":2,"outputrangemax":2,"operation":"multiply"})");
        run(*twice, 1.0);
        QVERIFY2(std::fabs(twice->column(Col::VY)[0] - 60.0f) < 0.05f, qPrintable(QString::number(twice->column(Col::VY)[0]))); // x2 per second
        auto more = single(item, "0 0 0", "0 30 0", R"({"name":"remapvalue","output":"speed","transformfunction":"fbmnoise","outputrangemin":2,"outputrangemax":2,"operation":"add"})");
        run(*more, 1.0);
        QVERIFY(std::fabs(more->column(Col::VY)[0] - 32.0f) < 0.01f); // +2 per second
        auto set = single(item, "0 0 0", "0 30 0", R"({"name":"remapvalue","output":"speed","transformfunction":"simplexnoise","outputrangemin":2,"outputrangemax":2,"operation":"remap"})");
        run(*set, 1.0 / 60.0);
        QCOMPARE(set->column(Col::VY)[0], 2.0f);
    }

    void aControlPointHasNoVelocityUntilItIsKnownTwice()
    {
        Item item;
        const auto def = define(R"("controlpoint":[{"id":1,"flags":1}])", item);
        ParticleSystem s(def, {}, 1);
        s.updateControlPoints({0.0f, 0.0f}, false, Mat4::identity()); // the pointer is not known yet
        s.advance(0.1);
        s.updateControlPoints({5000.0f, 0.0f}, true, Mat4::identity()); // first known place: no jump from the offset
        QCOMPARE(s.controlPointVelocities()[1].x, 0.0f);
        s.advance(0.1);
        s.updateControlPoints({5010.0f, 0.0f}, true, Mat4::identity());
        QVERIFY(std::fabs(s.controlPointVelocities()[1].x - 100.0f) < 0.01f);
    }

    void timeOfDayIsTheLocalDayFraction()
    {
        Item item;
        auto s = single(item, "0 0 0", "0 0 0", R"({"name":"remapvalue","input":"timeofday","output":"opacity"})");
        run(*s, 1.0 / 60.0);
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        const float day = static_cast<float>(local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec) / 86400.0f;
        QVERIFY(std::fabs(s->column(Col::Alpha)[0] - day) < 0.001f);
    }
};

QTEST_GUILESS_MAIN(TstSceneVkParticles)
#include "tst_scenevk_particles.moc"
