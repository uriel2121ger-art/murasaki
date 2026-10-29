// murasaki-scene-vk renderability analysis: what makes an otherwise well-formed scene undrawable, found without a GPU.

#include "audio/soundstream.h"
#include "render/analysis.h"
#include "render/passes.h"
#include "render/sprite.h"
#include "scene/scene.h"
#include "scenevkfixtures.h"
#include "support/meshwriter.h"

#include <QFile>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki::scene;
using namespace scenevkfix;

namespace {

const char *kVert = "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
                    "void main() { gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix); v_TexCoord = a_TexCoord; }\n";
const char *kFrag = "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\nvoid main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord); }\n";

void addEffect(Item &item, const QString &name, const QByteArray &vert, const QByteArray &frag, const QByteArray &effectPasses = {})
{
    item.file("shaders/effects/" + name + ".vert", vert);
    item.file("shaders/effects/" + name + ".frag", frag);
    item.file("materials/effects/" + name + ".json", "{\"passes\":[{\"shader\":\"effects/" + name.toUtf8() + "\"}]}");
    item.file("effects/" + name + "/effect.json",
              "{\"passes\":" + (effectPasses.isEmpty() ? "[{\"material\":\"materials/effects/" + name.toUtf8() + ".json\"}]" : effectPasses) + "}");
}

QByteArray sceneWithEffect(const QString &name)
{
    return sceneJson(imageObject(1, "bg", ",\"effects\":[{\"file\":\"effects/" + name.toUtf8() + "/effect.json\"}]"));
}

std::vector<std::string> analyse(Item &item)
{
    Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
    std::string error;
    const auto scene = loadScene(vfs, "scene.json", &error);
    if (!scene) {
        qFatal("loadScene: %s", error.c_str());
    }
    return renderabilityReasons(*scene, vfs);
}

bool mentions(const std::vector<std::string> &reasons, const char *part)
{
    for (const std::string &r : reasons) {
        if (r.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

class TstSceneVkAnalysis : public QObject
{
    Q_OBJECT

private slots:
    void aPlainSceneAndAWorkingEffectHaveNoReasons()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        addEffect(item, u"ok"_s, kVert, kFrag);
        item.file(u"scene.json"_s, sceneWithEffect(u"ok"_s));
        const auto reasons = analyse(item);
        QVERIFY2(reasons.empty(), reasons.empty() ? "" : reasons[0].c_str());
    }

    // 1432621679 holds a 3-hour mp3 over the limit: the scene is drawn here without that sound, a broken sound still refuses
    void aSoundOverTheSizeLimitDegradesAndABrokenOneRefuses()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"sounds/big.mp3"_s, {});
        QFile big(item.dir.filePath(u"sounds/big.mp3"_s));
        QVERIFY(big.resize(static_cast<qint64>(murasaki::scene::kMaxSoundFileBytes) + 1)); // sparse: no memory until read
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + R"(,{"id":2,"name":"music","sound":["sounds/big.mp3"]})"));
        Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        std::string error;
        const auto scene = loadScene(vfs, "scene.json", &error);
        QVERIFY2(scene, error.c_str());
        std::vector<std::string> degraded;
        const auto reasons = renderabilityReasons(*scene, vfs, &degraded);
        QVERIFY2(reasons.empty(), reasons.empty() ? "" : reasons[0].c_str());
        QVERIFY(mentions(degraded, "(not played)"));

        item.file(u"sounds/big.mp3"_s, "not a sound at all");
        QVERIFY(mentions(analyse(item), "audio: 'sounds/big.mp3'"));
    }

    void shaderProblemsNameTheShader()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"materials/effects/ghost.json"_s, "{\"passes\":[{\"shader\":\"effects/ghost\"}]}");
        item.file(u"effects/ghost/effect.json"_s, "{\"passes\":[{\"material\":\"materials/effects/ghost.json\"}]}");
        item.file(u"scene.json"_s, sceneWithEffect(u"ghost"_s));
        QVERIFY(mentions(analyse(item), "shader compile: effects/ghost"));

        addEffect(item, u"broken"_s, kVert, "varying vec2 v_TexCoord;\nvoid main() { gl_FragColor = nowhere; }\n");
        item.file(u"scene.json"_s, sceneWithEffect(u"broken"_s));
        const auto reasons = analyse(item);
        QVERIFY(mentions(reasons, "shader compile: effects/broken"));
        QVERIFY(mentions(reasons, "nowhere"));

        // an invisible effect is checked too: a property can switch it on
        item.file(u"scene.json"_s,
                  sceneJson(imageObject(1, "bg", ",\"effects\":[{\"file\":\"effects/broken/effect.json\",\"visible\":false}]")));
        QVERIFY(mentions(analyse(item), "shader compile: effects/broken"));
    }

    void whatAQuadCannotFeed()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        addEffect(item, u"normals"_s,
                  "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec3 a_Normal;\nvarying vec3 v_N;\n"
                  "void main() { gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix); v_N = a_Normal; }\n",
                  "varying vec3 v_N;\nvoid main() { gl_FragColor = vec4(v_N, 1.0); }\n");
        item.file(u"scene.json"_s, sceneWithEffect(u"normals"_s));
        QVERIFY(mentions(analyse(item), "needs the vertex attribute a_Normal"));

        addEffect(item, u"slots"_s, kVert,
                  "uniform sampler2D g_Texture0;\nuniform sampler2D g_Texture9;\nvarying vec2 v_TexCoord;\n"
                  "void main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord) + texSample2D(g_Texture9, v_TexCoord); }\n");
        item.file(u"scene.json"_s, sceneWithEffect(u"slots"_s));
        QVERIFY(mentions(analyse(item), "texture slot 9"));

        addEffect(item, u"big"_s, kVert,
                  "uniform sampler2D g_Texture0;\nuniform vec4 g_Huge[1000];\nuniform vec4 g_More[100];\nvarying vec2 v_TexCoord;\n"
                  "void main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord) * g_Huge[999] * g_More[99]; }\n");
        item.file(u"scene.json"_s, sceneWithEffect(u"big"_s));
        const auto big = analyse(item);
        QVERIFY2(mentions(big, "over the 16384 byte limit"), big.empty() ? "no reasons" : big[0].c_str());
    }

    void unknownBlendingSelfReadsAndBudgetsAreRejected()
    {
        // an unknown blending name
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        addEffect(item, u"odd"_s, kVert, kFrag);
        item.file("materials/effects/odd.json", "{\"passes\":[{\"shader\":\"effects/odd\",\"blending\":\"multiply2\"}]}");
        item.file(u"scene.json"_s, sceneWithEffect(u"odd"_s));
        QVERIFY(mentions(analyse(item), "blending 'multiply2'"));

        // a pass that names a target and reads it
        Item self;
        self.image(u"bg"_s);
        self.tex(u"bg"_s, simpleRgba(4, 4));
        addEffect(self, u"self"_s, kVert, kFrag,
                  "[{\"material\":\"materials/effects/self.json\",\"target\":\"_rt_imageLayerComposite_1_b\",\"bind\":[{\"name\":\"_rt_imageLayerComposite_1_b\",\"index\":0}]}]");
        self.file(u"scene.json"_s, sceneWithEffect(u"self"_s));
        QVERIFY(mentions(analyse(self), "reads the target it writes"));
        // the layer's own buffer named for the one the pass is about to write (no target of its own: the first pass writes b) means
        // the picture so far, as the CRT and sine wave effects of the library have it saved
        Item saved;
        saved.image(u"bg"_s);
        saved.tex(u"bg"_s, simpleRgba(4, 4));
        addEffect(saved, u"saved"_s, kVert, kFrag,
                  "[{\"material\":\"materials/effects/saved.json\",\"bind\":[{\"name\":\"_rt_imageLayerComposite_1_b\",\"index\":0}]}]");
        saved.file(u"scene.json"_s, sceneWithEffect(u"saved"_s));
        QVERIFY(analyse(saved).empty());
        // reading the other buffer is fine
        Item fine;
        fine.image(u"bg"_s);
        fine.tex(u"bg"_s, simpleRgba(4, 4));
        addEffect(fine, u"fine"_s, kVert, kFrag,
                  "[{\"material\":\"materials/effects/fine.json\",\"bind\":[{\"name\":\"_rt_imageLayerComposite_1_a\",\"index\":0}]}]");
        fine.file(u"scene.json"_s, sceneWithEffect(u"fine"_s));
        QVERIFY(analyse(fine).empty());

        // more draws in a frame than one descriptor pool holds are fine (the pools chain), up to the engine's ceiling
        Item many;
        many.image(u"bg"_s);
        many.tex(u"bg"_s, simpleRgba(4, 4));
        addEffect(many, u"ok"_s, kVert, kFrag);
        const auto manyPasses = [&](int count) {
            QByteArray passes = "[";
            for (int i = 0; i < count; ++i)
                passes += QByteArray(i ? "," : "") + "{\"material\":\"materials/effects/ok.json\"}";
            passes += "]";
            many.file("effects/ok/effect.json", "{\"passes\":" + passes + "}");
        };
        many.file(u"scene.json"_s, sceneWithEffect(u"ok"_s));
        manyPasses(2100);
        QVERIFY(!mentions(analyse(many), "draws per frame"));
        manyPasses(int(kPassMaxDraws) + 100);
        QVERIFY(mentions(analyse(many), "draws per frame"));
    }

    void aBasePassOptionTheShaderReadsIsNamed()
    {
        // BLENDMODE and SHADING pass the loader (2244339517's shader declares both and reads neither); one a shader does read with a value
        // other than 0 is named, since what it draws was not checked
        const auto degradedOf = [&](const char *frag, const char *combos) {
            Item item;
            item.image(u"bg"_s);
            item.tex(u"bg"_s, simpleRgba(4, 4));
            item.file(u"shaders/own.vert"_s, kVert);
            item.file(u"shaders/own.frag"_s, frag);
            item.file(u"materials/bg.json"_s, QByteArray(R"({"passes":[{"shader":"own","blending":"translucent","textures":["bg"],"combos":)") + combos + "}]}");
            item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")));
            Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
            std::string error;
            const auto scene = loadScene(vfs, "scene.json", &error);
            if (!scene) {
                qFatal("loadScene: %s", error.c_str());
            }
            std::vector<std::string> degraded;
            const auto reasons = renderabilityReasons(*scene, vfs, &degraded);
            if (!reasons.empty()) {
                qFatal("refused: %s", reasons.front().c_str());
            }
            return degraded;
        };
        const char *declares = "// [COMBO] {\"material\":\"b\",\"combo\":\"BLENDMODE\",\"default\":0}\n"
                               "// [COMBO] {\"material\":\"s\",\"combo\":\"SHADING\",\"default\":1}\n"
                               "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\n";
        const std::string unread = std::string(declares) + "void main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord); }\n";
        const std::string reads = std::string(declares) + "void main() {\n  vec4 c = texSample2D(g_Texture0, v_TexCoord);\n#if BLENDMODE == 2\n  c.rgb = 1.0 - c.rgb;\n#endif\n  gl_FragColor = c;\n}\n";
        const auto has = [](const std::vector<std::string> &list, const char *name) { return std::find(list.begin(), list.end(), name) != list.end(); };
        QVERIFY(!has(degradedOf(unread.c_str(), R"({"BLENDMODE":2,"SHADING":1})"), "combo blendmode"));
        QVERIFY(!has(degradedOf(unread.c_str(), R"({"BLENDMODE":2,"SHADING":1})"), "combo shading"));
        QVERIFY(has(degradedOf(reads.c_str(), R"({"BLENDMODE":2})"), "combo blendmode"));
        QVERIFY(!has(degradedOf(reads.c_str(), R"({"BLENDMODE":0})"), "combo blendmode")); // at its default it changes nothing
        QVERIFY(!has(degradedOf(reads.c_str(), R"({"BLENDMODE":2,"SHADING":1})"), "combo shading"));
    }

    void aGlobalInTheWrongCaseOrOneWeLacksIsAScriptError()
    {
        // a global spelt in the wrong case is undefined in WE too: the script dies there and the scene goes on, a script error; so does
        // one that uses a global nobody has
        const auto verdict = [&](const char *script, std::vector<std::string> *degraded) {
            Item item;
            item.image(u"bg"_s);
            item.tex(u"bg"_s, simpleRgba(4, 4));
            item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", QByteArray(",\"visible\":{\"script\":\"") + script + "\",\"value\":true}")));
            Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
            std::string error;
            const auto scene = loadScene(vfs, "scene.json", &error);
            if (!scene) {
                qFatal("loadScene: %s", error.c_str());
            }
            return renderabilityReasons(*scene, vfs, degraded);
        };
        std::vector<std::string> degraded;
        auto reasons = verdict("export function init(v) { localstorage.set('k', 1); return v; }", &degraded);
        QVERIFY(reasons.empty());
        QVERIFY(std::find(degraded.begin(), degraded.end(), "script error") != degraded.end());
        degraded.clear();
        reasons = verdict("export function init(v) { localStorage.set('k', 1); return v; }", &degraded); // the right case: it exists
        QVERIFY(reasons.empty());
        QVERIFY(std::find(degraded.begin(), degraded.end(), "script error") == degraded.end());
        degraded.clear();
        reasons = verdict("export function init(v) { somethingNobodyHas.run(); return v; }", &degraded);
        QVERIFY(reasons.empty());
        QVERIFY(std::find(degraded.begin(), degraded.end(), "script error") != degraded.end());
        degraded.clear();
        reasons = verdict("export function init(v) { for (;;) {} }", &degraded); // the engine's own limit: refused
        QVERIFY(mentions(reasons, "SceneScript"));
    }

    void bindsAndCommands()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        addEffect(item, u"b"_s, kVert, kFrag,
                  "[{\"material\":\"materials/effects/b.json\",\"bind\":[{\"name\":\"nonsense\",\"index\":0}]},"
                  "{\"material\":\"materials/effects/b.json\",\"bind\":[{\"name\":\"_rt_imageLayerComposite_99_a\",\"index\":0}]},"
                  "{\"command\":\"explode\"},"
                  "{\"command\":\"copy\",\"source\":\"_rt_A\",\"target\":\"_rt_B\"},"
                  "{\"command\":\"swap\",\"source\":\"_rt_A\",\"target\":\"_rt_B\"},"
                  "{\"material\":\"materials/effects/b.json\",\"bind\":[{\"name\":\"_rt_imageLayerComposite_1_b\",\"index\":0},{\"name\":\"previous\",\"index\":1}]}]");
        item.file(u"scene.json"_s, sceneWithEffect(u"b"_s));
        const auto reasons = analyse(item);
        QVERIFY(mentions(reasons, "'nonsense', which is neither previous"));
        QVERIFY(mentions(reasons, "another layer's composite (_rt_imageLayerComposite_99_a)"));
        QVERIFY(mentions(reasons, "effect command 'explode'"));
        QVERIFY(!mentions(reasons, "_rt_imageLayerComposite_1_b")); // the layer's own is fine
        QVERIFY(!mentions(reasons, "'copy'"));
        QVERIFY(!mentions(reasons, "'swap'"));
    }

    void texturesOfEffectsAreChecked()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        addEffect(item, u"t"_s, kVert, "uniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\nvarying vec2 v_TexCoord;\n"
                                       "void main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord) * texSample2D(g_Texture1, v_TexCoord); }\n");
        const QByteArray scene = sceneJson(imageObject(
            1, "bg", ",\"effects\":[{\"file\":\"effects/t/effect.json\",\"passes\":[{\"textures\":[null,\"masks/m\"]}]}]"));
        item.file(u"scene.json"_s, scene);
        QVERIFY(mentions(analyse(item), "texture 'materials/masks/m.tex' not found"));
        // present as a .tex: fine
        item.tex(u"masks/m"_s, simpleRgba(2, 2));
        QVERIFY(analyse(item).empty());
        // present only as a plain image file: also fine
        Item second;
        second.image(u"bg"_s);
        second.tex(u"bg"_s, simpleRgba(4, 4));
        addEffect(second, u"t"_s, kVert, "uniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\nvarying vec2 v_TexCoord;\n"
                                          "void main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord) * texSample2D(g_Texture1, v_TexCoord); }\n");
        second.file(u"scene.json"_s, scene);
        second.file(u"materials/masks/m.png"_s, toQ(makePng(2, 2, rgbaPattern(2, 2))));
        QVERIFY(analyse(second).empty());
        // a texture format without a GPU equivalent
        TexSpec odd = simpleRgba(2, 2);
        odd.format = 99;
        second.tex(u"masks/m"_s, odd);
        QVERIFY(mentions(analyse(second), "texture format unknown"));
    }

    // 3D meshes: a skinned one feeds the bone attributes, a static one does not; a part may sample the composite of an image layer,
    // not of an object that is none (or of no object)
    void whatAMeshFeedsAndSamples()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"shaders/skin.vert"_s,
                  "uniform mat4 g_ModelMatrix;\nuniform mat4 g_ViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute uvec4 a_BlendIndices;\n"
                  "attribute vec4 a_BlendWeights;\nvarying vec2 v_TexCoord;\n"
                  "void main() { gl_Position = mul(mul(vec4(a_Position * a_BlendWeights.x + float(a_BlendIndices.x), 1.0), g_ModelMatrix), g_ViewProjectionMatrix);"
                  " v_TexCoord = vec2(0.5, 0.5); }\n");
        item.file(u"shaders/skin.frag"_s, kFrag);
        item.file(u"materials/models/m.json"_s, R"({"passes":[{"shader":"skin","textures":["_rt_imageLayerComposite_1_a"]}]})");
        meshgen::Model m;
        m.parts.push_back(meshgen::box({-1, -1, -1}, {1, 1, 1}, "materials/models/m.json"));
        const auto write = [&](const meshgen::Model &model) {
            const auto bytes = meshgen::write(model);
            item.file(u"models/m.mdl"_s, QByteArray(reinterpret_cast<const char *>(bytes.data()), static_cast<qsizetype>(bytes.size())));
        };
        const auto scene = [&](int sampled) {
            item.file(u"materials/models/m.json"_s, R"({"passes":[{"shader":"skin","textures":["_rt_imageLayerComposite_)" + QByteArray::number(sampled)
                                                       + R"(_a"]}]})");
            item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + R"(,{"id":2,"name":"m","model":"models/m.mdl"},{"id":3,"name":"g"})"));
        };
        write(m);
        scene(1);
        QVERIFY(mentions(analyse(item), "needs the vertex attribute a_BlendIndices"));
        meshgen::Model skinned = m;
        skinned.skinned = true;
        skinned.bones = {{"root", -1, {}}};
        write(skinned);
        auto reasons = analyse(item);
        QVERIFY2(reasons.empty(), reasons.empty() ? "" : reasons[0].c_str());
        scene(3); // a group, not an image layer
        QVERIFY(mentions(analyse(item), "composite of object 3, which is not an image layer"));
        scene(99);
        QVERIFY(mentions(analyse(item), "composite of object 99"));
        // the name's parts
        QCOMPARE(layerCompositeId("_rt_imageLayerComposite_42_a"), std::optional<int>(42));
        QCOMPARE(layerCompositeId("_rt_imageLayerComposite_42_b"), std::optional<int>(42));
        QVERIFY(!layerCompositeId("_rt_imageLayerComposite__a"));
        QVERIFY(!layerCompositeId("_rt_imageLayerComposite_4x_a"));
        QVERIFY(!layerCompositeId("_rt_imageLayerComposite_42_c"));
        QVERIFY(!layerCompositeId("_rt_imageLayerComposite_99999999999_a"));
        QVERIFY(!layerCompositeId("_rt_FullFrameBuffer"));
    }

    void spriteSheetsNeedTheirShaderSupport()
    {
        TexSpec sheet = simpleRgba(8, 4);
        sheet.flags = TexFlags::IsGif;
        sheet.sheetVersion = 2;
        sheet.frames = {{0, 0, 4, 0, 0, 4}, {4, 0, 4, 0, 0, 4}};
        Item plain;
        plain.image(u"bg"_s);
        plain.tex(u"bg"_s, sheet);
        plain.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")));
        QVERIFY(mentions(analyse(plain), "no SPRITESHEET support"));

        Item sprite;
        sprite.image(u"bg"_s, ",\"combos\":{\"spritesheet\":1}");
        sprite.tex(u"bg"_s, sheet);
        sprite.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")));
        const auto reasons = analyse(sprite);
        QVERIFY2(reasons.empty(), reasons.empty() ? "" : reasons[0].c_str());

        // one frame is a picture
        sheet.frames = {{0, 0, 8, 0, 0, 4}};
        plain.tex(u"bg"_s, sheet);
        QVERIFY(analyse(plain).empty());
    }

    void spriteFramesOnLaterImagesUseThatImagesSize()
    {
        TexFrame a;
        a.duration = 0.5f;
        a.axisXx = 32;
        a.axisYy = 16;
        TexFrame b = a;
        b.index = 1; // lies on the second image, which is smaller
        b.x = 8;
        b.y = 4;
        b.axisXx = 16;
        b.axisYy = 8;
        TexFrame c = b;
        c.index = 5; // past the last image: the last
        const std::vector<TexFrame> frames = {a, b, c};
        const std::vector<std::array<uint32_t, 2>> sizes = {{64, 32}, {32, 16}};
        const auto f0 = spriteFrame(frames, sizes, 0.0);
        QCOMPARE(f0.page, size_t{0});
        QCOMPARE(f0.rotation[0], 0.5f);
        const auto f1 = spriteFrame(frames, sizes, 0.6);
        QCOMPARE(f1.frame, size_t{1});
        QCOMPARE(f1.page, size_t{1});
        QCOMPARE(f1.translation[0], 0.25f); // 8 of 32
        QCOMPARE(f1.translation[1], 0.25f); // 4 of 16
        QCOMPARE(f1.rotation[0], 0.5f);     // 16 of 32
        QCOMPARE(f1.rotation[3], 0.5f);     // 8 of 16
        QCOMPARE(spriteFrame(frames, sizes, 1.1).page, size_t{1});
        // a quarter turn keeps its off-diagonal axes: x components over the width, y components over the height
        TexFrame r = a;
        r.axisXx = 0;
        r.axisXy = -64;
        r.axisYx = 32;
        r.axisYy = 0;
        const auto fr = spriteFrame({r}, sizes, 0.0);
        QCOMPARE(fr.rotation[1], -2.0f);
        QCOMPARE(fr.rotation[2], 0.5f);
        QCOMPARE(spriteFrame({}, sizes, 0.0).page, size_t{0});
        QCOMPARE(spriteFrame(frames, {}, 0.0).rotation[0], 1.0f);
    }

    void spriteFrameMath()
    {
        TexFrame a;
        a.duration = 0.25f;
        a.x = 0;
        a.y = 0;
        a.axisXx = 32;
        a.axisYy = 16;
        TexFrame b = a;
        b.duration = 0.5f;
        b.x = 32;
        b.y = 16;
        const std::vector<TexFrame> frames = {a, b};
        // a sheet of 64 x 32 texels
        auto f0 = spriteFrame(frames, 64, 32, 0.0);
        QCOMPARE(f0.frame, size_t{0});
        QCOMPARE(f0.rotation[0], 0.5f);
        QCOMPARE(f0.rotation[3], 0.5f);
        QCOMPARE(f0.translation[0], 0.0f);
        auto f1 = spriteFrame(frames, 64, 32, 0.3);
        QCOMPARE(f1.frame, size_t{1});
        QCOMPARE(f1.translation[0], 0.5f);
        QCOMPARE(f1.translation[1], 0.5f);
        QCOMPARE(spriteFrame(frames, 64, 32, 0.74).frame, size_t{1});
        QCOMPARE(spriteFrame(frames, 64, 32, 0.75).frame, size_t{0}); // the loop is 0.75 s long
        QCOMPARE(spriteFrame(frames, 64, 32, 3.0).frame, size_t{0});
        QCOMPARE(spriteFrame(frames, 64, 32, 3.3).frame, size_t{1});
        // degenerate input: no frames, no sizes, no durations
        QCOMPARE(spriteFrame({}, 64, 32, 1.0).rotation[0], 1.0f);
        QCOMPARE(spriteFrame(frames, 0, 0, 1.0).rotation[0], 1.0f);
        std::vector<TexFrame> still = frames;
        still[0].duration = still[1].duration = 0.0f;
        QCOMPARE(spriteFrame(still, 64, 32, 5.0).frame, size_t{0});
        QCOMPARE(spriteFrame(frames, 64, 32, -1.0).frame, size_t{0});
        // a frame a script chose holds whatever the clock says, and past the last is the last
        QCOMPARE(spriteFrame(frames, 64, 32, 0.0, 1).frame, size_t{1});
        QCOMPARE(spriteFrame(frames, 64, 32, 0.3, 0).frame, size_t{0});
        QCOMPARE(spriteFrame(frames, 64, 32, 0.3, 9).frame, size_t{1});
        QCOMPARE(spriteFrame(frames, 64, 32, 0.3, -1).frame, size_t{1}); // none chosen: the clock
    }
};

QTEST_GUILESS_MAIN(TstSceneVkAnalysis)
#include "tst_scenevk_analysis.moc"
