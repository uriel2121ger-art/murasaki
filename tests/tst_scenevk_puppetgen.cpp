// murasaki-scene-vk puppets against a CPU reference: models written by tests/support/mdlwriter.h (deep chains, many frames, every
// loop mode, layers that blend and layers that leave the bind pose) and the skinning matrices the engine computes for them,
// compared with an independent implementation of the rules of docs/scene-vk/puppets.md.

#include "puppet/mdl.h"
#include "support/mdlwriter.h"

#include <QTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

using namespace murasaki::scene;

namespace {

// ---- a small reference: 4x4 matrices of doubles, column vectors, m[col][row] ----
using M4 = std::array<std::array<double, 4>, 4>;

M4 identity()
{
    M4 m{};
    for (size_t i = 0; i < 4; ++i)
        m[i][i] = 1.0;
    return m;
}

M4 mul(const M4 &a, const M4 &b)
{
    M4 r{};
    for (size_t c = 0; c < 4; ++c)
        for (size_t row = 0; row < 4; ++row)
            for (size_t k = 0; k < 4; ++k)
                r[c][row] += a[k][row] * b[c][k];
    return r;
}

// T * Rz * Ry * Rx * S, with the angles in radians
M4 compose(const std::array<double, 3> &p, const std::array<double, 3> &a, const std::array<double, 3> &s)
{
    const double cx = std::cos(a[0]), sx = std::sin(a[0]), cy = std::cos(a[1]), sy = std::sin(a[1]), cz = std::cos(a[2]), sz = std::sin(a[2]);
    const double r[3][3] = {{cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx}, {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx}, {-sy, cy * sx, cy * cx}};
    M4 m = identity();
    for (size_t row = 0; row < 3; ++row)
        for (size_t col = 0; col < 3; ++col)
            m[col][row] = r[row][col] * s[col];
    m[3][0] = p[0];
    m[3][1] = p[1];
    m[3][2] = p[2];
    return m;
}

// the inverse of an affine matrix (a rotation-scale part and a translation), by the adjugate of the 3x3
M4 inverseAffine(const M4 &m)
{
    double a[3][3];
    for (size_t row = 0; row < 3; ++row)
        for (size_t col = 0; col < 3; ++col)
            a[row][col] = m[col][row];
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    double inv[3][3];
    inv[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
    inv[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
    inv[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
    inv[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det;
    inv[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
    inv[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
    inv[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
    inv[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
    inv[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
    M4 r = identity();
    for (size_t row = 0; row < 3; ++row)
        for (size_t col = 0; col < 3; ++col)
            r[col][row] = inv[row][col];
    for (size_t row = 0; row < 3; ++row)
        r[3][row] = -(inv[row][0] * m[3][0] + inv[row][1] * m[3][1] + inv[row][2] * m[3][2]);
    return r;
}

struct Layer {
    uint32_t animation;
    double blend, rate;
};

// The playhead in frames of an animation at `time` seconds: loop wraps, mirror runs there and back, single holds the last frame.
double playhead(const mdlgen::Animation &a, double time, double rate)
{
    const double span = std::max(1.0, static_cast<double>(a.length));
    const double head = time * rate * a.fps;
    if (a.mode == "loop")
        return std::fmod(std::max(0.0, head), span);
    if (a.mode == "mirror") {
        const double m = std::fmod(std::max(0.0, head), 2.0 * span);
        return m <= span ? m : 2.0 * span - m;
    }
    return std::clamp(head, 0.0, span);
}

// What docs/scene-vk/puppets.md says: every layer pulls a bone it has a track for towards its (interpolated) pose by its blend,
// blends summing over one are normalised, and what is left over is the bind pose (its position, its angles about all three axes
// taken the whole turns nearest the pulling tracks' first frames, and its scale).
// The skinning matrix of a bone is its animated world matrix times the inverse of its bind world matrix, in the layout of
// g_Bones: 16 floats a bone, the upper three rows of the four columns.
constexpr double kPi = 3.14159265358979323846;

std::vector<float> reference(const mdlgen::Model &model, const std::vector<Layer> &layers, double time)
{
    const size_t n = model.bones.size();
    std::vector<M4> bindWorld(n), animWorld(n);
    const auto bindLocal = [&](size_t i) {
        const mdlgen::Pose &p = model.bones[i].bind;
        return compose({p.position[0], p.position[1], p.position[2]}, {p.angles[0], p.angles[1], p.angles[2]}, {p.scale[0], p.scale[1], p.scale[2]});
    };
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i)
        order[i] = i;
    // parents before children whatever the file's order (the file here always has them first; the reference does not care)
    std::vector<bool> done(n, false);
    std::vector<size_t> sorted;
    while (sorted.size() < n) {
        for (size_t i = 0; i < n; ++i) {
            const int p = model.bones[i].parent;
            if (!done[i] && (p < 0 || done[static_cast<size_t>(p)])) {
                done[i] = true;
                sorted.push_back(i);
            }
        }
    }
    for (size_t i : sorted) {
        const int p = model.bones[i].parent;
        bindWorld[i] = p >= 0 ? mul(bindWorld[static_cast<size_t>(p)], bindLocal(i)) : bindLocal(i);
    }
    struct Pull {
        std::array<double, 3> position{0, 0, 0}, angles{0, 0, 0}, scale{0, 0, 0}, first{0, 0, 0};
        double weight = 0.0;
    };
    std::vector<Pull> pull(n);
    double total = 0.0;
    for (const Layer &l : layers)
        total += std::max(0.0, l.blend);
    const double norm = total > 1.0 ? 1.0 / total : 1.0;
    for (const Layer &l : layers) {
        const mdlgen::Animation *anim = nullptr;
        for (const mdlgen::Animation &a : model.animations)
            if (a.id == l.animation)
                anim = &a;
        const double w = std::max(0.0, l.blend) * norm;
        if (!anim || w <= 0.0)
            continue;
        for (size_t b = 0; b < n && b < anim->tracks.size(); ++b) {
            const auto &track = anim->tracks[b];
            if (track.empty())
                continue;
            const double head = playhead(*anim, time, l.rate);
            const size_t k0 = std::min(static_cast<size_t>(head), track.size() - 1);
            const size_t k1 = std::min(k0 + 1, track.size() - 1);
            const double f = head - std::floor(head);
            for (size_t c = 0; c < 3; ++c) {
                pull[b].position[c] += (track[k0].position[c] + (track[k1].position[c] - track[k0].position[c]) * f) * w;
                pull[b].angles[c] += (track[k0].angles[c] + (track[k1].angles[c] - track[k0].angles[c]) * f) * w;
                pull[b].scale[c] += (track[k0].scale[c] + (track[k1].scale[c] - track[k0].scale[c]) * f) * w;
                pull[b].first[c] += track[0].angles[c] * w;
            }
            pull[b].weight += w;
        }
    }
    for (size_t i : sorted) {
        M4 local = bindLocal(i);
        if (pull[i].weight > 0.0) {
            const double rest = std::max(0.0, 1.0 - pull[i].weight);
            const mdlgen::Pose &bp = model.bones[i].bind;
            std::array<double, 3> pos, ang, sc;
            for (size_t c = 0; c < 3; ++c) {
                pos[c] = pull[i].position[c] + bp.position[c] * rest;
                const double near = pull[i].first[c] / pull[i].weight;
                const double bind = bp.angles[c] + 2.0 * kPi * std::round((near - bp.angles[c]) / (2.0 * kPi));
                ang[c] = pull[i].angles[c] + bind * rest;
                sc[c] = pull[i].scale[c] + bp.scale[c] * rest;
            }
            local = compose(pos, ang, sc);
        }
        const int p = model.bones[i].parent;
        animWorld[i] = p >= 0 ? mul(animWorld[static_cast<size_t>(p)], local) : local;
    }
    std::vector<float> out(n * 16, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const M4 skin = mul(animWorld[i], inverseAffine(bindWorld[i]));
        for (size_t col = 0; col < 4; ++col)
            for (size_t row = 0; row < 3; ++row)
                out[i * 16 + col * 4 + row] = static_cast<float>(skin[col][row]);
    }
    return out;
}

// A chain of `bones` bones, each hanging on the one before, with bind poses that differ (rotations about x, y and z, a scale, a
// step along x and up) and animations of `frames` frames: 1 "wave" (loop, every bone moves), 2 "sweep" (mirror, only the even bones have a
// track), 3 "once" (single, the first half of the bones).
mdlgen::Model chain(size_t bones, uint32_t frames)
{
    mdlgen::Model m;
    for (size_t b = 0; b < bones; ++b) {
        mdlgen::Bone bone;
        bone.name = "bone" + std::to_string(b);
        bone.parent = b == 0 ? -1 : static_cast<int>(b) - 1;
        bone.bind.position = {b == 0 ? 0.0f : 10.0f, b % 3 == 0 ? 2.0f : -1.0f, 0.0f};
        bone.bind.angles = {0.1f * static_cast<float>(b % 3), -0.07f * static_cast<float>(b % 4), 0.05f * static_cast<float>(b % 5)};
        bone.bind.scale = {1.0f + 0.02f * static_cast<float>(b % 4), 1.0f, 1.0f};
        m.bones.push_back(bone);
    }
    // a few vertices skinned to the first bones, a quad's worth of triangles
    for (size_t v = 0; v < 8; ++v) {
        mdlgen::Vertex vertex;
        vertex.position = {static_cast<float>(v) * 3.0f - 10.0f, static_cast<float>(v % 2) * 4.0f, 0.0f};
        vertex.bones = {static_cast<uint32_t>(v % bones), static_cast<uint32_t>((v + 1) % bones), 0, 0};
        vertex.weights = {0.7f, 0.3f, 0, 0};
        m.vertices.push_back(vertex);
    }
    m.indices = {0, 1, 2, 2, 3, 4, 4, 5, 6, 6, 7, 0};
    const auto animation = [&](uint32_t id, const char *name, const char *mode, float fps, uint32_t length, auto includes) {
        mdlgen::Animation a;
        a.id = id;
        a.name = name;
        a.mode = mode;
        a.fps = fps;
        a.length = length;
        for (size_t b = 0; b < bones; ++b) {
            std::vector<mdlgen::Pose> track;
            if (includes(b)) {
                for (uint32_t f = 0; f <= length; ++f) {
                    const float t = static_cast<float>(f) * 0.31f + static_cast<float>(b);
                    mdlgen::Pose p;
                    p.position = {m.bones[b].bind.position[0] + 2.0f * std::sin(t), m.bones[b].bind.position[1] + 1.5f * std::cos(t * 0.7f), 0.0f};
                    p.angles = {m.bones[b].bind.angles[0] + 0.2f * std::sin(t * 0.4f), m.bones[b].bind.angles[1] + 0.15f * std::cos(t * 0.3f),
                                0.4f * std::sin(t * 0.5f)};
                    p.scale = {1.0f + 0.2f * std::sin(t * 0.3f), 1.0f + 0.1f * std::cos(t), 1.0f};
                    track.push_back(p);
                }
            }
            a.tracks.push_back(std::move(track));
        }
        m.animations.push_back(std::move(a));
    };
    animation(1, "wave", "loop", 20.0f, frames, [](size_t) { return true; });
    animation(2, "sweep", "mirror", 15.0f, frames / 2 + 1, [](size_t b) { return b % 2 == 0; });
    animation(3, "once", "single", 10.0f, frames / 4 + 1, [bones](size_t b) { return b < bones / 2; });
    return m;
}

void compare(const std::vector<float> &engine, const std::vector<float> &expected, const QString &what)
{
    QCOMPARE(engine.size(), expected.size());
    for (size_t i = 0; i < engine.size(); ++i) {
        // 16 floats a bone: the fourth row of each column is padding the engine leaves at zero
        if (i % 4 == 3)
            continue;
        const float tolerance = 2e-4f * std::max(1.0f, std::fabs(expected[i]));
        if (std::fabs(engine[i] - expected[i]) > tolerance) {
            QFAIL(qPrintable(QStringLiteral("%1: bone %2 element %3: engine %4, reference %5").arg(what).arg(i / 16).arg(i % 16).arg(engine[i]).arg(expected[i])));
        }
    }
}

std::optional<Puppet> parse(const mdlgen::Model &model, std::string *error = nullptr)
{
    return parsePuppet(mdlgen::write(model), error);
}

} // namespace

class TstSceneVkPuppetGen : public QObject
{
    Q_OBJECT

private slots:
    void aDeepChainSkinsLikeTheReferenceAtRestAndUnderEveryLoopMode_data()
    {
        QTest::addColumn<int>("layerSet");
        QTest::addColumn<double>("time");
        const double times[] = {0.0, 0.37, 1.9, 5.123, 41.0, 300.5};
        for (int set = 0; set < 6; ++set)
            for (const double t : times)
                QTest::addRow("set%d at %g", set, t) << set << t;
    }
    void aDeepChainSkinsLikeTheReferenceAtRestAndUnderEveryLoopMode()
    {
        QFETCH(int, layerSet);
        QFETCH(double, time);
        static const mdlgen::Model model = chain(12, 200);
        static const std::optional<Puppet> puppet = parse(model);
        QVERIFY(puppet.has_value());
        const std::vector<std::vector<Layer>> sets = {
            {},                                                  // no layer: the bind pose
            {{1, 1.0, 1.0}},                                     // one looping animation
            {{2, 1.0, 1.0}},                                     // mirror, half the bones have a track
            {{3, 1.0, 2.0}},                                     // single, run twice as fast
            {{1, 0.6, 1.0}, {2, 0.3, 0.5}},                      // blends summing to less than one: the bind pose keeps the rest
            {{1, 0.8, 2.0}, {2, 0.8, 1.0}, {3, 0.5, 1.0}},       // over one: normalised
        };
        std::vector<PuppetLayerState> states;
        for (const Layer &l : sets[static_cast<size_t>(layerSet)])
            states.push_back({l.animation, static_cast<float>(l.blend), static_cast<float>(l.rate)});
        compare(puppetBoneMatrices(*puppet, states, time), reference(model, sets[static_cast<size_t>(layerSet)], time), QStringLiteral("set %1 at %2").arg(layerSet).arg(time));
    }

    void restingPuppetsSkinToTheIdentity()
    {
        const mdlgen::Model model = chain(12, 20);
        const auto puppet = parse(model);
        QVERIFY(puppet.has_value());
        const auto m = puppetBoneMatrices(*puppet, {}, 0.0);
        for (size_t b = 0; b < 12; ++b)
            for (size_t col = 0; col < 4; ++col)
                for (size_t row = 0; row < 3; ++row)
                    QVERIFY2(std::fabs(m[b * 16 + col * 4 + row] - (col == row ? 1.0f : 0.0f)) < 1e-4f, qPrintable(QStringLiteral("bone %1").arg(b)));
    }

    void theBoneAndFrameLimitsAreExact()
    {
        std::string error;
        // 128 bones are the most (g_Bones[] must fit the uniform window), 129 are refused
        QVERIFY2(parse(chain(128, 4), &error).has_value(), error.c_str());
        QVERIFY(!parse(chain(129, 4), &error).has_value());
        QVERIFY(!error.empty()); // (today's reason is the vague "bad skeleton header")
        // a track may hold 65,536 frames (a 60 fps animation of ten thousand frames is real); a longer one is refused
        QVERIFY2(parse(chain(2, 65535), &error).has_value(), error.c_str()); // 65,536 poses
        QVERIFY(!parse(chain(2, 65536), &error).has_value());               // 65,537 poses
        QVERIFY2(!error.empty(), error.c_str());
    }

    void aCycleOfParentsIsRefusedWithAReason()
    {
        mdlgen::Model model = chain(3, 4);
        model.bones[0].parent = 2; // 0 -> 2 -> 1 -> 0
        std::string error;
        QVERIFY(!parse(model, &error).has_value());
        QVERIFY2(error.find("parent") != std::string::npos, error.c_str());
    }

    void aParentThatComesAfterItsChildIsAcceptedWhenThereIsNoCycle()
    {
        // The file lists the child first: bone 0 hangs on bone 1, which is the root. The format does not say parents come first
        // (the library's models happen to be written that way); the reader refuses this today. When it learns to put the bones
        // in dependency order the skinning must equal the reference's, which orders them itself.
        mdlgen::Model model = chain(2, 8);
        std::swap(model.bones[0], model.bones[1]); // now [child(parent 0 -> itself!), root]; fix the parents by hand
        model.bones[0].parent = 1;
        model.bones[1].parent = -1;
        for (auto &a : model.animations)
            std::swap(a.tracks[0], a.tracks[1]);
        for (auto &v : model.vertices)
            for (auto &b : v.bones)
                b = 1 - (b % 2);
        std::string error;
        const auto puppet = parse(model, &error);
        QVERIFY2(puppet.has_value(), error.c_str());
        if (puppet) {
            const std::vector<Layer> layers{{1, 1.0, 1.0}};
            compare(puppetBoneMatrices(*puppet, {{1, 1.0f, 1.0f}}, 0.4), reference(model, layers, 0.4), QStringLiteral("forward parent"));
        }
    }

    void manyFramesInterpolateAcrossTheWholeTrack()
    {
        // a track of 4096 frames: the playhead at frame 3000.5 is half way between poses 3000 and 3001
        mdlgen::Model model = chain(2, 4095);
        const auto puppet = parse(model);
        QVERIFY(puppet.has_value());
        const double time = 3000.5 / 20.0; // wave runs at 20 fps
        compare(puppetBoneMatrices(*puppet, {{1, 1.0f, 1.0f}}, time), reference(model, {{1, 1.0, 1.0}}, time), QStringLiteral("frame 3000.5"));
    }

    void unknownAnimationsAndZeroBlendsLeaveTheBindPose()
    {
        const mdlgen::Model model = chain(5, 16);
        const auto puppet = parse(model);
        QVERIFY(puppet.has_value());
        compare(puppetBoneMatrices(*puppet, {{99, 1.0f, 1.0f}, {1, 0.0f, 1.0f}}, 0.3), reference(model, {}, 0.3), QStringLiteral("unknown and zero blend"));
    }
};

QTEST_GUILESS_MAIN(TstSceneVkPuppetGen)
#include "tst_scenevk_puppetgen.moc"
