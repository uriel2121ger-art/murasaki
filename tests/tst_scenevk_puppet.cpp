// murasaki-scene-vk puppets: the MDL reader (on models written here, laid out as docs/scene-vk/puppets.md describes), its
// limits, and the bone matrices of layered animations.

#include "puppet/mdl.h"
#include "puppetfixtures.h"
#include "support/meshwriter.h"

#include <QFile>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using namespace murasaki::scene;

namespace {

using namespace puppetfix;

// the position a skinned vertex ends at: the bone matrix (g_Bones layout) applied to (x, y, 0, 1)
std::array<float, 3> skin(const std::vector<float> &bones, size_t bone, float x, float y)
{
    const float *m = bones.data() + bone * 16;
    return {m[0] * x + m[4] * y + m[12], m[1] * x + m[5] * y + m[13], m[2] * x + m[6] * y + m[14]};
}

} // namespace

class TstSceneVkPuppet : public QObject
{
    Q_OBJECT

private slots:
    void aModelIsReadWithItsMeshSkeletonAndAnimations()
    {
        const auto mdl = makeMdl({{"root", -1, 5, 6}, {"arm", 0, 10, 0}},
                                 {{7, "Animation 1", "loop", 30.0f, 2, {{{frame(5, 6), frame(5, 6), frame(5, 6)}}, {{frame(10, 0), frame(10, 0), frame(10, 0)}}}}});
        std::string error;
        const auto puppet = parsePuppet(mdl, &error);
        QVERIFY2(puppet.has_value(), error.c_str());
        QCOMPARE(puppet->material, std::string("materials/test.json"));
        QCOMPARE(puppet->vertexCount(), size_t{4});
        QCOMPARE(puppet->indices.size(), size_t{6});
        QCOMPARE(puppet->bones.size(), size_t{2});
        QCOMPARE(puppet->bones[1].parent, 0);
        QCOMPARE(puppet->bones[1].bindLocal.m[12], 10.0f); // translation, transposed into the column layout
        QCOMPARE(puppet->usedBones, size_t{1});
        QCOMPARE(puppet->animations.size(), size_t{1});
        const PuppetAnimation &a = puppet->animations[0];
        QCOMPARE(a.id, 7u);
        QCOMPARE(a.name, std::string("Animation 1"));
        QCOMPARE(a.fps, 30.0f);
        QCOMPARE(a.tracks.size(), size_t{2});
        QCOMPARE(a.tracks[0].size(), size_t{3});
        QCOMPARE(a.tracks[0][1].position.y, 6.0f);
    }

    void oneBoneAtRestSkinsToTheIdentity()
    {
        const auto puppet = parsePuppet(makeMdl({{"root", -1, 5, 6}, {"arm", 0, 10, 0}}, {}));
        QVERIFY(puppet.has_value());
        // no layers: the bind pose, so the matrices leave every vertex where it is
        const auto m = puppetBoneMatrices(*puppet, {}, 3.0);
        QCOMPARE(m.size(), size_t{32});
        for (size_t bone = 0; bone < 2; ++bone) {
            const auto p = skin(m, bone, 7.0f, -3.0f);
            QVERIFY(std::fabs(p[0] - 7.0f) < 1e-4f && std::fabs(p[1] + 3.0f) < 1e-4f);
        }
    }

    void animationsMoveBonesAndTheirChildrenAndLoopMirrorOrHold()
    {
        // the root slides 4 units a frame along x for 4 frames (length 4), the child stays put relative to it
        std::vector<std::array<float, 9>> slide, still;
        for (int i = 0; i <= 4; ++i) {
            slide.push_back(frame(5.0f + 4.0f * static_cast<float>(i), 6.0f));
            still.push_back(frame(10.0f, 0.0f));
        }
        const auto bones = std::vector<BoneSpec>{{"root", -1, 5, 6}, {"arm", 0, 10, 0}};
        const auto build = [&](const char *mode) {
            return parsePuppet(makeMdl(bones, {{3, "slide", mode, 4.0f, 4, {{slide}, {still}}}}));
        };
        auto puppet = build("loop");
        QVERIFY(puppet.has_value());
        const std::vector<PuppetLayerState> layer{{3, 1.0f, 1.0f}};
        // fps 4: one frame a quarter second. at 0.5 s the root has moved 8 units; a vertex of bone 0 at (0,0) is at 8
        auto m = puppetBoneMatrices(*puppet, layer, 0.5);
        QVERIFY(std::fabs(skin(m, 0, 0, 0)[0] - 8.0f) < 1e-3f);
        // the child follows its parent: its animated world is the root's plus its own offset, the bind one likewise
        QVERIFY(std::fabs(skin(m, 1, 0, 0)[0] - 8.0f) < 1e-3f);
        // between frames it interpolates
        m = puppetBoneMatrices(*puppet, layer, 0.375);
        QVERIFY(std::fabs(skin(m, 0, 0, 0)[0] - 6.0f) < 1e-3f);
        // loop: one period (1 s) later the same; mirror runs back; single holds the last frame
        const float atLoop = skin(puppetBoneMatrices(*puppet, layer, 1.5), 0, 0, 0)[0];
        QVERIFY2(std::fabs(atLoop - 8.0f) < 1e-3f, qPrintable(QString::number(atLoop)));
        puppet = build("mirror");
        QVERIFY(std::fabs(skin(puppetBoneMatrices(*puppet, layer, 1.25), 0, 0, 0)[0] - 12.0f) < 1e-3f); // 5 frames in: 3 back from 4 -> 12? (head = 2*4-5 = 3)
        puppet = build("single");
        QVERIFY(std::fabs(skin(puppetBoneMatrices(*puppet, layer, 9.0), 0, 0, 0)[0] - 16.0f) < 1e-3f); // held at frame 4
    }

    void layersBlendByTheirWeightsAndAnUnknownAnimationDoesNothing()
    {
        const auto bones = std::vector<BoneSpec>{{"root", -1, 0, 0}};
        const auto puppet = parsePuppet(makeMdl(bones, {{1, "left", "loop", 10.0f, 1, {{{frame(-10, 0), frame(-10, 0)}}}}, {2, "right", "loop", 10.0f, 1, {{{frame(30, 0), frame(30, 0)}}}}}));
        QVERIFY(puppet.has_value());
        // equal weights: halfway between -10 and 30
        auto m = puppetBoneMatrices(*puppet, {{1, 1.0f, 1.0f}, {2, 1.0f, 1.0f}}, 0.0);
        QVERIFY(std::fabs(skin(m, 0, 0, 0)[0] - 10.0f) < 1e-3f);
        // half a layer: the rest stays the bind pose (the root's bind is at the origin): -5
        m = puppetBoneMatrices(*puppet, {{1, 0.5f, 1.0f}}, 0.0);
        QVERIFY(std::fabs(skin(m, 0, 0, 0)[0] + 5.0f) < 1e-3f);
        // a layer for an animation that is not there, and a zero blend
        m = puppetBoneMatrices(*puppet, {{99, 1.0f, 1.0f}, {1, 0.0f, 1.0f}}, 0.0);
        QVERIFY(std::fabs(skin(m, 0, 3, 4)[0] - 3.0f) < 1e-4f);
    }

    void additiveLayersAddTheirDifferenceFromTheBindPose()
    {
        const auto puppet = parsePuppet(makeMdl({{"root", -1, 2, 0}}, {{1, "a", "loop", 30.0f, 1, {{{frame(10, 0), frame(10, 0)}}}}, {2, "b", "loop", 30.0f, 1, {{{frame(6, 0), frame(6, 0)}}}}}));
        QVERIFY(puppet.has_value());
        // the bind is at x = 2; the skin matrix moves a point by (animated - bind)
        const auto moved = [&](const std::vector<PuppetLayerState> &layers) { return skin(puppetBoneMatrices(*puppet, layers, 0.0), 0, 0, 0)[0]; };
        QVERIFY(std::fabs(moved({{1, 1.0f, 1.0f, false}}) - 8.0f) < 1e-3f);
        QVERIFY(std::fabs(moved({{1, 1.0f, 1.0f, true}}) - 8.0f) < 1e-3f);            // alone, additive is the same as a plain layer
        QVERIFY(std::fabs(moved({{1, 1.0f, 1.0f, true}, {2, 1.0f, 1.0f, true}}) - 12.0f) < 1e-3f); // 8 + 4: they add, they do not average
        QVERIFY(std::fabs(moved({{1, 1.0f, 1.0f, false}, {2, 0.5f, 1.0f, true}}) - 10.0f) < 1e-3f);  // 8 + 0.5 * 4
        QVERIFY(std::fabs(moved({{2, 0.0f, 1.0f, true}}) - 0.0f) < 1e-3f);
    }

    void aHalfLayerOnAConstantAnimationMovesHalfWay()
    {
        const auto puppet = parsePuppet(makeMdl({{"root", -1, 0, 0}}, {{7, "slide", "loop", 30.0f, 1, {{{frame(20, 0), frame(20, 0)}}}}}));
        QVERIFY(puppet.has_value());
        const auto m = puppetBoneMatrices(*puppet, {{7, 0.5f, 1.0f}}, 0.0);
        for (float v : m)
            QVERIFY(std::isfinite(v));
        const auto p = skin(m, 0, 0, 0);
        QVERIFY2(std::fabs(p[0] - 10.0f) < 1e-3f, qPrintable(QString("%1 %2 %3 | m0 %4 m5 %5").arg(p[0]).arg(p[1]).arg(p[2]).arg(m[0]).arg(m[5])));
    }

    void rotationAndScaleComposeInTheBoneAndTheVerticesFollow()
    {
        const auto bones = std::vector<BoneSpec>{{"root", -1, 0, 0}};
        const float quarter = 1.5707963f;
        const auto puppet = parsePuppet(makeMdl(bones, {{1, "turn", "loop", 10.0f, 1, {{{frame(0, 0, quarter, 2.0f), frame(0, 0, quarter, 2.0f)}}}}}));
        QVERIFY(puppet.has_value());
        const auto m = puppetBoneMatrices(*puppet, {{1, 1.0f, 1.0f}}, 0.0);
        const auto p = skin(m, 0, 1.0f, 0.0f); // scaled by 2, turned a quarter: (1,0) -> (0,2)
        QVERIFY2(std::fabs(p[0]) < 1e-3f && std::fabs(p[1] - 2.0f) < 1e-3f, qPrintable(QString("%1, %2").arg(p[0]).arg(p[1])));
    }

    void theOlderWritersModelsAreWidenedToTheCurrentVertex()
    {
        // MDLV0013..0016 have a 52 byte vertex (no normal, no tangent), MDLS0001..0003 and MDLA0001..0003 lay the rest out as the
        // newer ones do; MDLV0017 has the long vertex
        const auto anims = std::vector<AnimSpec>{{3, "a", "loop", 20.0f, 2, {{{frame(0, 0), frame(4, 0), frame(8, 0)}}}}};
        const auto bones = std::vector<BoneSpec>{{"root", -1, 0, 0}, {"b", 0, 1, 1}};
        const auto current = parsePuppet(makeMdl(bones, anims, true));
        QVERIFY(current.has_value());
        for (const char *version : {"MDLV0013", "MDLV0014", "MDLV0016"}) {
            std::string error;
            const auto old = parsePuppet(makeMdl(bones, anims, true, version, "MDLS0001", "MDLA0001", true), &error);
            QVERIFY2(old.has_value(), error.c_str());
            QCOMPARE(old->vertices.size(), current->vertices.size());
            QCOMPARE(old->vertices, current->vertices); // the normal and tangent it lacked are the ones the fixture writes
            QCOMPARE(old->indices, current->indices);
            QCOMPARE(old->animations.size(), size_t(1));
        }
        for (const char *skeleton : {"MDLS0001", "MDLS0002", "MDLS0003"})
            QVERIFY(parsePuppet(makeMdl(bones, anims, false, "MDLV0017", skeleton, "MDLA0004")).has_value());
        QVERIFY(parsePuppet(makeMdl(bones, anims, false, "MDLV0021", "MDLS0003", "MDLA0006")).has_value());
        // a short vertex under a long version is not a model: the block is not found
        QVERIFY(!parsePuppet(makeMdl(bones, anims, false, "MDLV0023", "MDLS0004", "MDLA0006", true)).has_value());
        // long tracks (3796494818-like: 60 fps, ten thousand frames) are within the caps
        std::vector<std::array<float, 9>> frames;
        for (int i = 0; i < 10200; ++i)
            frames.push_back(frame(static_cast<float>(i), 0));
        QVERIFY(parsePuppet(makeMdl(bones, {{1, "long", "loop", 60.0f, 10200, {{frames}}}})).has_value());
    }

    void aMeshWithAFloatAfterThePositionIsRead()
    {
        // mesh block flags 0x181000e (3810943704, 3577990983): an 84 byte vertex, the long one with a float after the position
        const auto anims = std::vector<AnimSpec>{{3, "a", "loop", 20.0f, 2, {{{frame(0, 0), frame(4, 0), frame(8, 0)}}}}};
        const auto bones = std::vector<BoneSpec>{{"root", -1, 0, 0}, {"b", 0, 1, 1}};
        const std::vector<uint8_t> plain = makeMdl(bones, anims, true);
        const auto reference = parsePuppet(plain);
        QVERIFY(reference.has_value());
        const size_t count = reference->vertexCount();
        // find the block the reader found, and rewrite it with the wider vertex (the section offsets after it move by as much)
        size_t at = 0;
        for (size_t i = 9; i + 8 < plain.size(); ++i) {
            uint32_t bytes;
            std::memcpy(&bytes, plain.data() + i + 4, 4);
            if (bytes == count * kPuppetVertexStride && std::memcmp(plain.data() + i + 8, reference->vertices.data(), 12) == 0) {
                at = i;
                break;
            }
        }
        QVERIFY(at != 0);
        std::vector<uint8_t> wide(plain.begin(), plain.begin() + static_cast<std::ptrdiff_t>(at));
        const auto put = [&](const void *p, size_t n) { wide.insert(wide.end(), static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n); };
        const uint32_t flags = 0x181000e, bytes = static_cast<uint32_t>(count * kExtendedVertexStride);
        put(&flags, 4);
        put(&bytes, 4);
        for (size_t v = 0; v < count; ++v) {
            const uint8_t *in = plain.data() + at + 8 + v * kPuppetVertexStride;
            const float extra = 0.0f;
            put(in, 12);
            put(&extra, 4);
            put(in + 12, kPuppetVertexStride - 12);
        }
        const size_t delta = count * (kExtendedVertexStride - kPuppetVertexStride);
        wide.insert(wide.end(), plain.begin() + static_cast<std::ptrdiff_t>(at + 8 + count * kPuppetVertexStride), plain.end());
        for (const char *tag : {"MDLS", "MDLA"}) {
            for (size_t i = 0; i + 13 < wide.size(); ++i) {
                if (std::memcmp(wide.data() + i, tag, 4) == 0) {
                    uint32_t end;
                    std::memcpy(&end, wide.data() + i + 9, 4);
                    end += static_cast<uint32_t>(delta);
                    std::memcpy(wide.data() + i + 9, &end, 4);
                    break;
                }
            }
        }
        std::string error;
        const auto puppet = parsePuppet(wide, &error);
        QVERIFY2(puppet.has_value(), error.c_str());
        QCOMPARE(puppet->vertices, reference->vertices);
        QCOMPARE(puppet->indices, reference->indices);
        QCOMPARE(puppet->bones.size(), reference->bones.size());
        QCOMPARE(puppet->animations.size(), reference->animations.size());
    }

    void aPuppetWithoutASkeletonIsAStaticMesh()
    {
        // an editor puppet nobody rigged (3822491687, 3818947747): no MDLS section, a 48 byte vertex (position, normal, tangent, uv) and
        // the mesh block running to the end of the file
        std::vector<uint8_t> bytes;
        const auto put = [&](const void *p, size_t n) { bytes.insert(bytes.end(), static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n); };
        const auto u32 = [&](uint32_t v) { put(&v, 4); };
        put("MDLV0023", 9);
        u32(9);
        u32(1);
        u32(1);
        put("materials/head.json", 20);
        bytes.resize(bytes.size() + 24, 0);
        u32(15); // the mesh block's flags
        const float quad[4][12] = {{-10, -10, 0, 0, 0, 1, 1, 0, 0, 1, 0, 1}, {10, -10, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1},
                                   {10, 10, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0}, {-10, 10, 0, 0, 0, 1, 1, 0, 0, 1, 0, 0}};
        u32(sizeof(quad));
        put(quad, sizeof(quad));
        const uint16_t triangles[6] = {0, 1, 2, 0, 2, 3};
        u32(sizeof(triangles));
        put(triangles, sizeof(triangles));
        bytes.resize(bytes.size() + 7, 0); // the tail the files have
        std::string error;
        const auto puppet = parsePuppet(bytes, &error);
        QVERIFY2(puppet.has_value(), error.c_str());
        QCOMPARE(puppet->vertexCount(), size_t{4});
        QCOMPARE(puppet->indices, (std::vector<uint32_t>{0, 1, 2, 0, 2, 3}));
        QVERIFY(puppet->bones.empty());
        QVERIFY(puppet->animations.empty());
        QCOMPARE(puppet->usedBones, size_t{0});
        QCOMPARE(puppet->material, std::string("materials/head.json"));
        // widened to the long vertex: position, normal and tangent where they were, the uv at 72, bone 0 with all the weight
        for (size_t v = 0; v < 4; ++v) {
            float f[20];
            uint32_t bone[4];
            std::memcpy(f, puppet->vertices.data() + v * kPuppetVertexStride, sizeof(f));
            std::memcpy(bone, puppet->vertices.data() + v * kPuppetVertexStride + 40, sizeof(bone));
            QCOMPARE(f[0], quad[v][0]);
            QCOMPARE(f[1], quad[v][1]);
            QCOMPARE(f[5], 1.0f);
            QCOMPARE(f[6], 1.0f);
            QCOMPARE(bone[0], 0u);
            QCOMPARE(f[14], 1.0f);
            QCOMPARE(f[15], 0.0f);
            QCOMPARE(f[18], quad[v][10]);
            QCOMPARE(f[19], quad[v][11]);
        }
        // nothing that looks like a mesh is still refused
        std::vector<uint8_t> empty(200, 0xff);
        std::copy_n(bytes.begin(), 64, empty.begin());
        QVERIFY(!parsePuppet(empty, &error).has_value());
        QVERIFY(error.find("mesh") != std::string::npos);
    }

    void theOlderSectionVersionsReadTheSame()
    {
        // MDLV0019 with MDLS0002 and MDLA0005 (3261565977) lay the sections out as the newer ones do
        const auto anims = std::vector<AnimSpec>{{3, "a", "loop", 20.0f, 2, {{{frame(0, 0), frame(4, 0), frame(8, 0)}}}}};
        const auto bones = std::vector<BoneSpec>{{"root", -1, 0, 0}};
        const auto old = parsePuppet(makeMdl(bones, anims, false, "MDLV0019", "MDLS0002", "MDLA0005"));
        const auto current = parsePuppet(makeMdl(bones, anims));
        QVERIFY(old.has_value() && current.has_value());
        QCOMPARE(old->vertices, current->vertices);
        QCOMPARE(old->indices, current->indices);
        QCOMPARE(old->bones.size(), current->bones.size());
        QCOMPARE(old->animations.size(), size_t(1));
        QCOMPARE(old->animations[0].tracks[0].size(), size_t(3));
        // each version is accepted on its own, and a version in between is not
        for (const char *skeleton : {"MDLS0002", "MDLS0004"})
            QVERIFY(parsePuppet(makeMdl(bones, anims, false, "MDLV0023", skeleton)).has_value());
        for (const char *animation : {"MDLA0005", "MDLA0006"})
            QVERIFY(parsePuppet(makeMdl(bones, anims, false, "MDLV0023", "MDLS0004", animation)).has_value());
        std::string error;
        QVERIFY(!parsePuppet(makeMdl(bones, anims, false, "MDLV0019", "MDLS0009"), &error));
        QVERIFY(error.find("MDLS0009") != std::string::npos);
        QVERIFY(!parsePuppet(makeMdl(bones, anims, false, "MDLV0019", "MDLS0002", "MDLA0009"), &error));
        QVERIFY(error.find("MDLA0009") != std::string::npos);
    }

    void realModelsShowTheirOddities()
    {
        // 3799253558: a long note string after each bone matrix, bones whose parent comes later in the file, more track records
        // than declared; 3796494818: a gap of 35 bytes (not a multiple of four) between two animations
        const std::string note(700, 'n');
        const auto forward = parsePuppet(makeMdl({{"leaf", 1, 5, 0, note}, {"root", -1, 0, 0, note}}, {}));
        QVERIFY(forward.has_value());
        QCOMPARE(forward->order, (std::vector<uint32_t>{1, 0})); // the parent first, whatever the file's order
        // a leaf that comes first still ends up where its parent puts it: bind world (5,0) then skin identity at rest
        const auto rest = puppetBoneMatrices(*forward, {}, 0.0);
        for (size_t b = 0; b < 2; ++b)
            QVERIFY(std::fabs(rest[b * 16] - 1.0f) < 1e-4f && std::fabs(rest[b * 16 + 12]) < 1e-4f);
        std::string error;
        QVERIFY(!parsePuppet(makeMdl({{"a", 1, 0, 0}, {"b", 0, 0, 0}}, {}), &error)); // a loop of parents
        QVERIFY(error.find("loop") != std::string::npos);
        QVERIFY(!parsePuppet(makeMdl({{"a", 0, 0, 0}}, {}), &error)); // its own parent
        QVERIFY(!parsePuppet(makeMdl({{"a", 7, 0, 0}}, {}), &error)); // a parent that is not there
        QVERIFY(!parsePuppet(makeMdl({{"a", -1, 0, 0, std::string(70000, 'n')}}, {}), &error)); // a note beyond the cap

        const auto track = TrackSpec{{frame(0, 0), frame(1, 0)}};
        for (const int gap : {0, 4, 35, 36, 100}) {
            const auto p = parsePuppet(makeMdl({{"root", -1, 0, 0}}, {{1, "first", "loop", 30.0f, 1, {track}, gap, 2}, {2, "second", "mirror", 30.0f, 1, {track}, gap, 0}}), &error);
            QVERIFY2(p.has_value(), error.c_str());
            QCOMPARE(p->animations.size(), size_t(2));
            QCOMPARE(p->animations[0].tracks.size(), size_t(1)); // the extra records are walked over, not kept
            QCOMPARE(p->animations[1].id, 2u);
            QCOMPARE(QString::fromStdString(p->animations[1].name), QString("second"));
        }
        // an animation whose id is 0 after a gap is an animation, not an empty track
        for (const int gap : {0, 8, 35, 36}) {
            const auto zeroId = parsePuppet(makeMdl({{"root", -1, 0, 0}}, {{5, "first", "loop", 30.0f, 1, {track}, gap, 1}, {0, "zero", "single", 30.0f, 1, {track}, gap, 0}, {9, "last", "mirror", 30.0f, 1, {track}, gap, 0}}), &error);
            QVERIFY2(zeroId.has_value(), error.c_str());
            QCOMPARE(zeroId->animations.size(), size_t(3));
            QCOMPARE(zeroId->animations[1].id, 0u);
            QCOMPARE(QString::fromStdString(zeroId->animations[2].name), QString("last"));
        }
        // what follows the animations and does not read as one ends the list without refusing the model
        auto junk = makeMdl({{"root", -1, 0, 0}}, {{1, "only", "loop", 30.0f, 1, {track}, 36, 0}});
        for (uint8_t b = 0; b < 200; ++b)
            junk.push_back(static_cast<uint8_t>(0x80 + b % 7));
        const auto kept = parsePuppet(junk, &error);
        QVERIFY2(kept.has_value(), error.c_str());
        QCOMPARE(kept->animations.size(), size_t(1));
    }

    // Opt-in: a real model from the library (MURASAKI_TEST_MDL=path), for checking the reader against what WE writes.
    void aRealModelParses()
    {
        const QByteArray path = qgetenv("MURASAKI_TEST_MDL");
        if (path.isEmpty())
            QSKIP("MURASAKI_TEST_MDL not set");
        QFile f(QString::fromUtf8(path));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray bytes = f.readAll();
        std::string error;
        const auto puppet = parsePuppet(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(bytes.constData()), static_cast<size_t>(bytes.size())), &error);
        QVERIFY2(puppet.has_value(), error.c_str());
        qInfo("real puppet: %zu vertices, %zu indices, %zu bones, %zu animations, uses %zu bones", puppet->vertexCount(), puppet->indices.size(), puppet->bones.size(),
              puppet->animations.size(), puppet->usedBones);
        for (const PuppetAnimation &a : puppet->animations)
            qInfo("  animation %u '%s' fps %g length %u tracks %zu", a.id, a.name.c_str(), static_cast<double>(a.fps), a.length, a.tracks.size());
        // the bind pose skins to the identity
        const auto m = puppetBoneMatrices(*puppet, {}, 1.0);
        for (size_t b = 0; b < puppet->bones.size(); ++b)
            QVERIFY(std::fabs(m[b * 16] - 1.0f) < 1e-3f && std::fabs(m[b * 16 + 12]) < 1e-3f);
    }

    void hostileModelsAreRefusedWithAReason()
    {
        std::string error;
        QVERIFY(!parsePuppet({}, &error));
        QVERIFY(!parsePuppet(std::vector<uint8_t>(200, 0x41), &error));
        QVERIFY(!parsePuppet(makeMdl({{"root", -1, 0, 0}}, {}, false, "MDLV0099"), &error));
        QVERIFY(error.find("MDLV0099") != std::string::npos);
        auto good = makeMdl({{"root", -1, 0, 0}, {"b", 0, 1, 1}}, {{1, "a", "loop", 30.0f, 1, {{{frame(0, 0), frame(0, 0)}}, {{frame(0, 0), frame(0, 0)}}}}});
        QVERIFY(parsePuppet(good, &error).has_value());
        // every truncation either parses (the animations are optional) or says why, and never reads out of bounds
        for (size_t cut = 0; cut < good.size(); cut += 3) {
            std::vector<uint8_t> part(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
            parsePuppet(part, &error);
        }
        // a bone that names a later parent
        auto badParent = makeMdl({{"root", 1, 0, 0}, {"b", 0, 1, 1}}, {});
        QVERIFY(!parsePuppet(badParent, &error));
        // a vertex on a bone the skeleton does not have
        auto noBone = makeMdl({{"root", -1, 0, 0}}, {}, true); // half the vertices use bone 1
        QVERIFY(!parsePuppet(noBone, &error));
        QVERIFY(error.find("bone") != std::string::npos);
        // a huge bone count in the header
        auto manyBones = good;
        for (size_t i = 0; i + 8 < manyBones.size(); ++i) {
            if (std::memcmp(manyBones.data() + i, "MDLS0004", 8) == 0) {
                const uint32_t big = 0x7fffffff;
                std::memcpy(manyBones.data() + i + 13, &big, 4);
                break;
            }
        }
        QVERIFY(!parsePuppet(manyBones, &error));
        // an animation track whose size is not a whole number of frames, and one claiming far more than the file holds
        auto badTrack = good;
        for (size_t i = 0; i + 8 < badTrack.size(); ++i) {
            if (std::memcmp(badTrack.data() + i, "MDLA0006", 8) == 0) {
                // find the first track header: after the strings; patch the first byteSize found by scanning for 72 (2 frames)
                for (size_t j = i; j + 4 < badTrack.size(); ++j) {
                    uint32_t v;
                    std::memcpy(&v, badTrack.data() + j, 4);
                    if (v == 72) {
                        const uint32_t odd = 73;
                        std::memcpy(badTrack.data() + j, &odd, 4);
                        break;
                    }
                }
                break;
            }
        }
        QVERIFY(!parsePuppet(badTrack, &error));
        auto hugeTrack = good;
        for (size_t i = 0; i + 8 < hugeTrack.size(); ++i) {
            if (std::memcmp(hugeTrack.data() + i, "MDLA0006", 8) == 0) {
                for (size_t j = i; j + 4 < hugeTrack.size(); ++j) {
                    uint32_t v;
                    std::memcpy(&v, hugeTrack.data() + j, 4);
                    if (v == 72) {
                        const uint32_t huge = 36u * 1000000u;
                        std::memcpy(hugeTrack.data() + j, &huge, 4);
                        break;
                    }
                }
                break;
            }
        }
        QVERIFY(!parsePuppet(hugeTrack, &error));
        // a vertex whose position is not a number
        auto nan = good;
        for (size_t i = 0; i + 8 < nan.size(); ++i) {
            if (std::memcmp(nan.data() + i, "\x00\x00\x20\xc1\x00\x00\x20\xc1", 8) == 0) { // the first vertex: (-10, -10)
                const uint32_t bad = 0x7fc00000;
                std::memcpy(nan.data() + i, &bad, 4);
                break;
            }
        }
        QVERIFY(!parsePuppet(nan, &error));

        // a bone index out of range is refused whatever its weight says (the shader reads all four)
        const std::string vertexZero("\x00\x00\x20\xc1\x00\x00\x20\xc1", 8);
        for (const float weight : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN()}) {
            auto stray = good;
            for (size_t i = 0; i + 80 < stray.size(); ++i) {
                if (std::memcmp(stray.data() + i, vertexZero.data(), 8) == 0) {
                    const uint32_t outside = 99;
                    std::memcpy(stray.data() + i + 44, &outside, 4);
                    std::memcpy(stray.data() + i + 60, &weight, 4);
                    break;
                }
            }
            QVERIFY(!parsePuppet(stray, &error));
            QVERIFY(error.find("bone") != std::string::npos);
        }

        // a section tag at the very end of the file: the version string is eight characters, the reader needs all of them
        for (size_t extra = 1; extra <= 9; ++extra) {
            std::vector<uint8_t> head(good.begin(), good.end());
            for (size_t i = 0; i + 8 < head.size(); ++i) {
                if (std::memcmp(head.data() + i, "MDLS0004", 8) == 0) {
                    head.resize(i + extra); // exact size: an out of bounds read is an ASan report
                    head.shrink_to_fit();
                    break;
                }
            }
            QVERIFY(!parsePuppet(head, &error));
        }

        // numbers that are not numbers are read as zero (the reader never hands one on), a bind matrix as much as a keyframe
        const auto nanBind = parsePuppet(makeMdl({{"root", -1, std::numeric_limits<float>::quiet_NaN(), 0}}, {}), &error);
        QVERIFY(nanBind.has_value());
        QCOMPARE(nanBind->bones[0].bindLocal.m[12], 0.0f);
    }

    void playbackWithAbsurdRatesStaysFinite()
    {
        const auto puppet = parsePuppet(makeMdl({{"root", -1, 0, 0}}, {{1, "a", "single", 30.0f, 2, {{{frame(5, 0), frame(9, 0), frame(9, 0)}}}}, {2, "b", "loop", 30.0f, 2, {{{frame(5, 0), frame(9, 0), frame(9, 0)}}}}}));
        QVERIFY(puppet.has_value());
        const float inf = std::numeric_limits<float>::infinity();
        for (const double time : {1e300, 1e30, 0.0}) {
            for (const float rate : {inf, -inf, std::numeric_limits<float>::quiet_NaN(), 1e30f}) {
                for (const float v : puppetBoneMatrices(*puppet, {{1, 1.0f, rate}, {2, 1.0f, rate}}, time)) {
                    QVERIFY(std::isfinite(v));
                }
            }
        }
    }

    // 3D mesh objects (`"model": "x.mdl"`): the static mesh layout, parts with their own materials, wide indices.
    void aMeshModelIsReadPartByPart()
    {
        meshgen::Model m;
        m.parts.push_back(meshgen::box({-1, -1, -1}, {1, 1, 1}, "materials/models/a/one.json"));
        meshgen::Part second = meshgen::box({2, 0, 0}, {3, 1, 1}, "materials/models/a/two.json");
        second.wideIndices = true;
        m.parts.push_back(second);
        std::string error;
        const auto mesh = parseMeshModel(meshgen::write(m), &error);
        QVERIFY2(mesh.has_value(), error.c_str());
        QCOMPARE(mesh->parts.size(), size_t{2});
        QCOMPARE(mesh->parts[0].material, std::string("materials/models/a/one.json"));
        QCOMPARE(mesh->parts[1].material, std::string("materials/models/a/two.json"));
        QCOMPARE(mesh->parts[0].vertexCount, size_t{24});
        QCOMPARE(mesh->parts[0].indexCount, size_t{36});
        QVERIFY(!mesh->parts[0].wideIndices);
        QCOMPARE(mesh->parts[0].indices.size(), size_t{72}); // u16 kept as they are
        QCOMPARE(mesh->parts[1].indexCount, size_t{36});
        QVERIFY(mesh->parts[1].wideIndices);
        QCOMPARE(mesh->parts[1].index(35), 23u);
        QCOMPARE(mesh->parts[1].boundsMin.x, 2.0f);
        QCOMPARE(mesh->parts[1].boundsMax.z, 1.0f);
        // the vertices are the file's 48 bytes: position, normal, tangent, uv
        float first[12];
        std::memcpy(first, mesh->parts[0].vertices.data(), sizeof(first));
        QCOMPARE(first[0], 1.0f);  // the +x face first
        QCOMPARE(first[3], 1.0f);  // its normal
        QCOMPARE(first[10], 0.25f);
        QVERIFY(!mesh->skinned);
        QVERIFY(!mesh->morphTargets);
        // the older writer (MDLV0021) has fewer zero bytes between and after the parts
        m.version = "MDLV0021";
        QVERIFY(parseMeshModel(meshgen::write(m), &error).has_value());
        QCOMPARE(parseMeshModel(meshgen::write(m))->parts.size(), size_t{2});
    }

    void aMeshModelNamesWhatItCannotBeDrawnWith()
    {
        meshgen::Model m;
        m.parts.push_back(meshgen::box({-1, -1, -1}, {1, 1, 1}));
        // morph targets after the parts
        meshgen::Model morph = m;
        const char tag[] = "MDMP0001";
        morph.trailer.assign(tag, tag + 9);
        morph.trailer.resize(64, 0);
        auto mesh = parseMeshModel(meshgen::write(morph));
        QVERIFY(mesh && mesh->morphTargets && !mesh->skinned);
        // a version not seen in the library's mesh objects
        std::string error;
        meshgen::Model other = m;
        other.version = "MDLV0017";
        QVERIFY(!parseMeshModel(meshgen::write(other), &error));
        QVERIFY2(error.find("version") != std::string::npos, error.c_str());
        // a vertex format other than the three known ones
        other = m;
        other.flags = 0x1f;
        QVERIFY(!parseMeshModel(meshgen::write(other), &error));
        QVERIFY2(error.find("vertex format") != std::string::npos, error.c_str());
        // a 2D puppet (its own layout before the mesh) is not a mesh model
        QVERIFY(!parseMeshModel(makeMdl({{"root", -1, 0, 0}}, {}, false, "MDLV0019")));
    }

    // A skinned mesh: the parts have the puppet's 80 byte vertex, the skeleton and animations follow (as a puppet's do).
    static meshgen::Model skinnedModel(const char *version = "MDLV0023")
    {
        meshgen::Model m;
        m.version = version;
        m.skinned = true;
        meshgen::Part a = meshgen::box({-1, 0, -1}, {1, 1, 1}, "materials/models/s/a.json");
        meshgen::Part b = meshgen::box({-1, 1, -1}, {1, 2, 1}, "materials/models/s/b.json");
        for (meshgen::Vertex &v : b.vertices) {
            v.bones = {1, 0, 0, 0};
        }
        m.parts = {a, b};
        mdlgen::Pose joint;
        joint.position = {0, 1, 0};
        joint.angles = {0.3f, -0.2f, 0.1f};
        m.bones = {{"root", -1, {}}, {"joint", 0, joint}};
        mdlgen::Animation anim;
        anim.id = 9;
        anim.length = 2;
        mdlgen::Pose turned = joint;
        turned.angles = {0.3f, -0.2f, 1.1f};
        anim.tracks = {{mdlgen::Pose{}, mdlgen::Pose{}, mdlgen::Pose{}}, {joint, turned, turned}};
        m.animations = {anim};
        return m;
    }

    // A bone mirrored with a half turn about x: its bind matrix reads back as x = -pi (atan2 of -0), its frames may store +pi or
    // -pi. A layer at half blend whose frames equal the bind pose must leave the bone where it is, whichever sign they use: the bind's
    // angle is taken the whole turn nearest the frames, not blended from -pi to +pi through 0.
    void aMirroredBindStaysMirroredAtHalfBlend()
    {
        for (const float sign : {1.0f, -1.0f}) {
            mdlgen::Model m;
            m.vertices = {{{0, 0, 0}}, {{1, 0, 0}}, {{0, 1, 0}}};
            m.indices = {0, 1, 2};
            mdlgen::Pose mirrored;
            mirrored.position = {3, 1, 0};
            mirrored.angles = {3.14159265f, 0, 0.4f};
            m.bones = {{"root", -1, mirrored}};
            mdlgen::Pose frame = mirrored;
            frame.angles[0] = sign * 3.14159265f;
            mdlgen::Animation hold;
            hold.id = 3;
            hold.length = 1;
            hold.tracks = {{frame, frame}};
            m.animations = {hold};
            const auto puppet = parsePuppet(mdlgen::write(m));
            QVERIFY(puppet);
            QVERIFY(std::fabs(std::fabs(puppet->bindFrames[0].angles.x) - 3.14159265f) < 1e-4f);
            for (const PuppetLayerState &layer : {PuppetLayerState{3, 0.5f, 1.0f}, PuppetLayerState{3, 0.5f, 1.0f, true}}) {
                const auto bones = puppetBoneMatrices(*puppet, {layer}, 0.25);
                for (size_t i = 0; i < 12; ++i) {
                    const float identity = (i == 0 || i == 5 || i == 10) ? 1.0f : 0.0f;
                    QVERIFY2(std::fabs(bones[i] - identity) < 1e-4f,
                             qPrintable(QString("sign %1 additive %2 element %3: %4").arg(sign).arg(layer.additive).arg(i).arg(bones[i])));
                }
            }
        }
    }

    // BONECOUNT is the bones the vertices name (all four of a vertex, weighted or not), not the whole skeleton
    void theBoneSlotsAreTheBonesTheVerticesName()
    {
        meshgen::Model m = skinnedModel();
        m.bones.push_back({"unused", 1, {}});
        auto mesh = parseMeshModel(meshgen::write(m));
        QVERIFY(mesh);
        QCOMPARE(mesh->rig.bones.size(), size_t{3});
        QCOMPARE(mesh->rig.boneSlots, size_t{2});
        QCOMPARE(mesh->rig.usedBones, size_t{2});
        // a zero-weight index still counts: the shader reads it
        m.parts[0].vertices[0].bones = {0, 2, 0, 0};
        m.parts[0].vertices[0].weights = {1, 0, 0, 0};
        mesh = parseMeshModel(meshgen::write(m));
        QVERIFY(mesh);
        QCOMPARE(mesh->rig.boneSlots, size_t{3});
        QCOMPARE(mesh->rig.usedBones, size_t{2});
    }

    void aSkinnedMeshModelIsReadWithItsSkeleton()
    {
        for (const char *version : {"MDLV0023", "MDLV0021", "MDLV0019"}) {
            std::string error;
            const auto mesh = parseMeshModel(meshgen::write(skinnedModel(version)), &error);
            QVERIFY2(mesh.has_value(), (std::string(version) + ": " + error).c_str());
            QVERIFY(mesh->skinned);
            QCOMPARE(mesh->parts.size(), size_t{2});
            QCOMPARE(mesh->parts[1].material, std::string("materials/models/s/b.json"));
            QCOMPARE(mesh->parts[0].stride, kPuppetVertexStride);
            QCOMPARE(mesh->parts[0].vertices.size(), size_t{24 * 80});
            QCOMPARE(mesh->rig.bones.size(), size_t{2});
            QCOMPARE(mesh->rig.bones[1].parent, 0);
            QCOMPARE(mesh->rig.animations.size(), size_t{1});
            QCOMPARE(mesh->rig.animations[0].id, 9u);
            QCOMPARE(mesh->rig.usedBones, size_t{2});
            // the second part's vertices are on bone 1: the bone index sits after position, normal and tangent
            uint32_t bone = 0;
            std::memcpy(&bone, mesh->parts[1].vertices.data() + 40, 4);
            QCOMPARE(bone, 1u);
        }
        // the bind pose with rotations about x, y and z: a layer at its first frame (the bind) leaves every vertex where it is, and a
        // half blend of a frame equal to the bind pose too (the bind's angles are recovered in full, not only the one about z)
        const auto mesh = parseMeshModel(meshgen::write(skinnedModel()));
        QVERIFY(mesh);
        for (const float blend : {1.0f, 0.5f}) {
            const auto bones = puppetBoneMatrices(mesh->rig, {{9, blend, 1.0f}}, 0.0);
            for (size_t i = 0; i < bones.size(); ++i) {
                const float identity = (i % 16 == 0 || i % 16 == 5 || i % 16 == 10) ? 1.0f : 0.0f;
                QVERIFY2(std::fabs(bones[i] - identity) < 1e-4f, qPrintable(QString("blend %1 element %2: %3").arg(blend).arg(i).arg(bones[i])));
            }
        }
        // one frame on, bone 1 has turned a radian further about its z: its vertices move, bone 0's do not
        const auto moved = puppetBoneMatrices(mesh->rig, {{9, 1.0f, 1.0f}}, 1.0 / 30.0);
        QVERIFY(std::fabs(moved[16] - 1.0f) > 0.1f);
        QVERIFY(std::fabs(moved[0] - 1.0f) < 1e-5f);
    }

    void olderStaticMeshVersionsAreRead()
    {
        meshgen::Model m;
        m.parts.push_back(meshgen::box({-1, -2, -3}, {1, 2, 3}, "materials/models/old/a.json"));
        for (const char *version : {"MDLV0007", "MDLV0016", "MDLV0019"}) {
            m.version = version;
            m.trailer.clear();
            if (std::string(version) == "MDLV0007") {
                // version 7 ends in a section the draw does not read
                const char tag[] = "MDLE0001";
                m.trailer.assign(tag, tag + 9);
                m.trailer.insert(m.trailer.end(), {1, 0, 0, 0, 0, 0});
            }
            std::string error;
            const auto mesh = parseMeshModel(meshgen::write(m), &error);
            QVERIFY2(mesh.has_value(), (std::string(version) + ": " + error).c_str());
            QCOMPARE(mesh->parts.size(), size_t{1});
            QCOMPARE(mesh->parts[0].material, std::string("materials/models/old/a.json"));
            QCOMPARE(mesh->parts[0].vertexCount, size_t{24});
            QCOMPARE(mesh->parts[0].indexCount, size_t{36});
            QCOMPARE(mesh->parts[0].stride, kMeshVertexStride);
            // the bounds, stored or (before version 19) worked out from the vertices
            QCOMPARE(mesh->parts[0].boundsMin.y, -2.0f);
            QCOMPARE(mesh->parts[0].boundsMax.z, 3.0f);
            QVERIFY(!mesh->skinned);
        }
        // version 7 with the vertex format 0x0b: no tangents, a 32 byte vertex widened to 48 (a tangent along +x)
        m.version = "MDLV0007";
        m.flags = 0x0b;
        std::string error;
        const auto mesh = parseMeshModel(meshgen::write(m), &error);
        QVERIFY2(mesh.has_value(), error.c_str());
        QCOMPARE(mesh->parts[0].vertices.size(), size_t{24 * 48});
        float first[12];
        std::memcpy(first, mesh->parts[0].vertices.data(), sizeof(first));
        QCOMPARE(first[0], 1.0f);   // the +x face's position
        QCOMPARE(first[3], 1.0f);   // its normal
        QCOMPARE(first[6], 1.0f);   // the tangent the file does not have
        QCOMPARE(first[9], 1.0f);
        QCOMPARE(first[10], 0.25f); // the uv, after it
    }

    void aHostileSkinnedOrOldMeshModelIsRefusedWithoutReadingPastItsEnd()
    {
        meshgen::Model old;
        old.version = "MDLV0007";
        old.parts.push_back(meshgen::box({-1, -1, -1}, {1, 1, 1}));
        old.parts.push_back(meshgen::box({1, 1, 1}, {2, 2, 2}));
        // every truncation fails cleanly: of version 7 (nothing follows its indices but the optional section), of a skinned mesh
        // before its animations (they may stop early: the whole skeleton is needed)
        for (const auto &good : {meshgen::write(skinnedModel()), meshgen::write(skinnedModel("MDLV0019")), meshgen::write(old)}) {
            const bool skinned = good[12] == 0x01; // the header flags 0x0180000f
            const size_t end = skinned ? std::string_view(reinterpret_cast<const char *>(good.data()), good.size()).find("MDLA") : good.size();
            for (size_t n = 0; n < end; ++n) {
                std::vector<uint8_t> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
                QVERIFY2(!parseMeshModel(cut), qPrintable(QString::number(n)));
            }
            QVERIFY(parseMeshModel(good).has_value());
        }
        // a vertex on a bone the skeleton does not have
        meshgen::Model bad = skinnedModel();
        bad.parts[1].vertices[3].bones = {0, 2, 0, 0};
        std::string error;
        QVERIFY(!parseMeshModel(meshgen::write(bad), &error));
        QVERIFY2(error.find("bone") != std::string::npos, error.c_str());
        // a skinned header without the skeleton after the parts
        auto bytes = meshgen::write(skinnedModel());
        const size_t at = std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()).find("MDLS");
        bytes[at] = 'X';
        QVERIFY(!parseMeshModel(bytes, &error));
        // a skinned header over static parts, and a skeleton after static parts
        bytes = meshgen::write(skinnedModel());
        const uint32_t staticFlags = 0x0f;
        std::memcpy(bytes.data() + 9, &staticFlags, 4);
        QVERIFY(!parseMeshModel(bytes));
        meshgen::Model statics;
        statics.parts.push_back(meshgen::box({-1, -1, -1}, {1, 1, 1}));
        auto withSkeleton = meshgen::write(statics);
        mdlgen::appendRig(withSkeleton, {{"root", -1, {}}}, {});
        QVERIFY(!parseMeshModel(withSkeleton, &error));
        // a skeleton too big for g_Bones, and a loop of parents
        bad = skinnedModel();
        bad.bones.assign(kMaxPuppetBones + 1, mdlgen::Bone{"b", -1, {}});
        QVERIFY(!parseMeshModel(meshgen::write(bad)));
        bad = skinnedModel();
        bad.bones[0].parent = 1;
        QVERIFY(!parseMeshModel(meshgen::write(bad)));
        // a vertex count that is not whole in the old version's 32 byte vertex
        meshgen::Model narrow = old;
        narrow.flags = 0x0b;
        bytes = meshgen::write(narrow);
        const size_t countAt = 21 + narrow.parts[0].material.size() + 1 + 4;
        uint32_t count = 0;
        std::memcpy(&count, bytes.data() + countAt, 4);
        QCOMPARE(count, uint32_t{24 * 32});
        count += 16;
        std::memcpy(bytes.data() + countAt, &count, 4);
        QVERIFY(!parseMeshModel(bytes));
    }

    void aHostileMeshModelIsRefusedWithoutReadingPastItsEnd()
    {
        meshgen::Model m;
        m.parts.push_back(meshgen::box({-1, -1, -1}, {1, 1, 1}));
        m.parts.push_back(meshgen::box({1, 1, 1}, {2, 2, 2}));
        const auto good = meshgen::write(m);
        // every truncation fails cleanly (the trailing zeros are optional)
        for (size_t n = 0; n + 7 < good.size(); ++n) {
            std::vector<uint8_t> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
            QVERIFY2(!parseMeshModel(cut), qPrintable(QString::number(n)));
        }
        // an index past the vertices, a count of parts nobody could hold, an index width the format does not have
        meshgen::Model bad = m;
        bad.parts[0].indices[5] = 24;
        QVERIFY(!parseMeshModel(meshgen::write(bad)));
        auto bytes = good;
        const uint32_t many = 1u << 30;
        std::memcpy(bytes.data() + 17, &many, 4);
        QVERIFY(!parseMeshModel(bytes));
        // the header's NUL after the version and its u32 1
        bytes = good;
        bytes[8] = 'x';
        QVERIFY(!parseMeshModel(bytes));
        bytes = good;
        bytes[13] = 2;
        QVERIFY(!parseMeshModel(bytes));
        bytes = good;
        const size_t widthAt = 21 + m.parts[0].material.size() + 1;
        bytes[widthAt] = 7;
        QVERIFY(!parseMeshModel(bytes));
        // a position that is not a number
        bad = m;
        bad.parts[1].vertices[3].position[1] = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(!parseMeshModel(meshgen::write(bad)));
    }
};

QTEST_APPLESS_MAIN(TstSceneVkPuppet)
#include "tst_scenevk_puppet.moc"
