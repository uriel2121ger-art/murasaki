// murasaki-scene-vk materials and effects: the JSON recipes of a draw and of an effect chain, and a scene's overrides.

#include "scene/effect.h"
#include "scene/material.h"
#include "scene/scene.h"
#include "scenevkfixtures.h"

#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki::scene;
using namespace scenevkfix;

namespace {

Vfs vfsOf(Item &item)
{
    return Vfs::forItem(item.root().toStdString(), {});
}

} // namespace

class TstSceneVkMaterials : public QObject
{
    Q_OBJECT

private slots:
    void materialPassesAreRead()
    {
        Item item;
        item.file(u"materials/m.json"_s,
                  "{\"passes\":[{\"shader\":\"effects/tint\",\"blending\":\"additive\",\"depthtest\":\"disabled\","
                  "\"textures\":[null,\"effects/mask\",null,\"third\"],\"combos\":{\"version\":2,\"Fancy\":1,\"off\":false},"
                  "\"constantshadervalues\":{\"alpha\":0.5,\"color\":\"1 0.5 0\",\"count\":3,\"on\":true,"
                  "\"bound\":{\"user\":\"prop\",\"value\":\"0.2 0.4\"},\"scripted\":{\"script\":\"x\",\"value\":1}}}]}");
        std::string error;
        const auto m = loadMaterial(vfsOf(item), "materials/m.json", &error);
        QVERIFY2(m.has_value(), error.c_str());
        QCOMPARE(m->passes.size(), size_t{1});
        const MaterialPass &p = m->passes[0];
        QCOMPARE(p.shader, std::string("effects/tint"));
        QCOMPARE(p.blending, Blending::Additive);
        QVERIFY(p.blendingKnown);
        QCOMPARE(p.textures, (std::vector<std::string>{"", "effects/mask", "", "third"}));
        QCOMPARE(p.combos.at("VERSION"), 2); // upper-cased
        QCOMPARE(p.combos.at("FANCY"), 1);
        QCOMPARE(p.combos.at("OFF"), 0);
        QCOMPARE(p.constants.at("alpha").count, 1);
        QCOMPARE(p.constants.at("alpha").value[0], 0.5f);
        QCOMPARE(p.constants.at("color").count, 3);
        QCOMPARE(p.constants.at("color").value[1], 0.5f);
        QCOMPARE(p.constants.at("count").value[0], 3.0f);
        QCOMPARE(p.constants.at("on").value[0], 1.0f);
        QCOMPARE(p.constants.at("bound").user, std::string("prop"));
        QCOMPARE(p.constants.at("bound").count, 2);
        QCOMPARE(p.constants.at("bound").value[1], 0.4f);
        QVERIFY(p.constants.at("scripted").scripted);
        QVERIFY(!p.constants.at("alpha").scripted);
    }

    void blendingNames()
    {
        for (const auto &[name, blending, known] : {std::tuple{"translucent", Blending::Translucent, true}, std::tuple{"normal", Blending::Normal, true},
                                                      std::tuple{"disabled", Blending::Normal, true}, std::tuple{"additive", Blending::Additive, true},
                                                      std::tuple{"screen", Blending::Translucent, false}}) {
            const auto json = parseJson(std::string("{\"shader\":\"s\",\"blending\":\"") + name + "\"}");
            QVERIFY(json.has_value());
            const MaterialPass p = parseMaterialPass(*json);
            QCOMPARE(p.blending, blending);
            QCOMPARE(p.blendingKnown, known);
        }
        const MaterialPass none = parseMaterialPass(*parseJson("{\"shader\":\"s\"}"));
        QCOMPARE(none.blending, Blending::Translucent); // the default
        // alpha to coverage is the shader's own (ALPHATOCOVERAGE: sharpened alpha, discard under one half), drawn opaque
        const MaterialPass coverage = parseMaterialPass(*parseJson("{\"shader\":\"genericparticle\",\"blending\":\"alphatocoverage\"}"));
        QVERIFY(coverage.blendingKnown);
        QCOMPARE(coverage.blending, Blending::Normal);
        QCOMPARE(coverage.combos.at("ALPHATOCOVERAGE"), 1);
        const MaterialPass chosen = parseMaterialPass(*parseJson("{\"shader\":\"s\",\"blending\":\"alphatocoverage\",\"combos\":{\"ALPHATOCOVERAGE\":0}}"));
        QCOMPARE(chosen.combos.at("ALPHATOCOVERAGE"), 0); // a material that sets the combo keeps its value
    }

    void depthAndCullKeysAreRead()
    {
        const MaterialPass mesh = parseMaterialPass(*parseJson(R"({"shader":"generic4","depthtest":"disabled","depthwrite":"disabled","cullmode":"nocull"})"));
        QVERIFY(!mesh.depthTest);
        QVERIFY(!mesh.depthWrite);
        QCOMPARE(mesh.cull, CullMode::None);
        const MaterialPass defaults = parseMaterialPass(*parseJson(R"({"shader":"generic4"})"));
        QVERIFY(defaults.depthTest && defaults.depthWrite);
        QCOMPARE(defaults.cull, CullMode::Back);
        const MaterialPass normal = parseMaterialPass(*parseJson(R"({"shader":"generic4","depthtest":"enabled","cullmode":"normal"})"));
        QVERIFY(normal.depthTest);
        QCOMPARE(normal.cull, CullMode::Back);
    }

    void windowsNanConstantsBecomeZero()
    {
        const Constant c = parseConstant(*parseJson("\"-1.9 -nan(ind) -nan(ind)\""));
        QCOMPARE(c.count, 3);
        QCOMPARE(c.value[0], -1.9f);
        QCOMPARE(c.value[1], 0.0f);
        QCOMPARE(c.value[2], 0.0f);
    }

    void brokenMaterialsAreErrors()
    {
        Item item;
        std::string error;
        QVERIFY(!loadMaterial(vfsOf(item), "materials/none.json", &error).has_value());
        QVERIFY(error.find("not found") != std::string::npos);
        item.file(u"materials/a.json"_s, "{ nope");
        QVERIFY(!loadMaterial(vfsOf(item), "materials/a.json", &error).has_value());
        item.file(u"materials/b.json"_s, "{\"passes\":[]}");
        QVERIFY(!loadMaterial(vfsOf(item), "materials/b.json", &error).has_value());
        QVERIFY(error.find("no passes") != std::string::npos);
        item.file(u"materials/c.json"_s, "{\"passes\":[5]}");
        QVERIFY(!loadMaterial(vfsOf(item), "materials/c.json", &error).has_value());
    }

    void effectChainWithTargetsAndBinds()
    {
        Item item;
        for (const char *n : {"a", "b", "c"})
            item.file(QString("materials/effects/%1.json").arg(n), QByteArray("{\"passes\":[{\"shader\":\"effects/") + n + "\"}]}");
        item.file(u"effects/blur/effect.json"_s,
                  "{\"version\":1,\"passes\":["
                  "{\"material\":\"materials/effects/a.json\",\"target\":\"_rt_Q1\",\"bind\":[{\"name\":\"previous\",\"index\":0}]},"
                  "{\"material\":\"materials/effects/b.json\",\"target\":\"_rt_Q2\",\"bind\":[{\"name\":\"_rt_Q1\",\"index\":0}]},"
                  "{\"command\":\"copy\",\"target\":\"_rt_Q1\",\"source\":\"_rt_Q2\"},"
                  "{\"material\":\"materials/effects/c.json\",\"compose\":true,\"bind\":[{\"name\":\"_rt_Q1\",\"index\":0},{\"name\":\"previous\",\"index\":2}]},],"
                  "\"fbos\":[{\"name\":\"_rt_Q1\",\"scale\":4,\"format\":\"rgba_backbuffer\"},{\"name\":\"_rt_Q2\",\"scale\":2,\"format\":\"r8\",\"unique\":true}]}");
        std::string error;
        const auto e = loadEffect(vfsOf(item), "effects/blur/effect.json", &error);
        QVERIFY2(e.has_value(), error.c_str());
        QCOMPARE(e->passes.size(), size_t{4});
        QCOMPARE(e->passes[0].target, std::string("_rt_Q1"));
        QCOMPARE(e->passes[0].bind[0].name, std::string("previous"));
        QCOMPARE(e->passes[1].bind[0].index, 0);
        QCOMPARE(e->passes[1].material.shader, std::string("effects/b"));
        QCOMPARE(e->passes[2].command, std::string("copy"));
        QCOMPARE(e->passes[2].source, std::string("_rt_Q2"));
        QVERIFY(e->passes[3].compose);
        QCOMPARE(e->passes[3].target, std::string()); // the layer's own buffer
        QCOMPARE(e->passes[3].bind[1].index, 2);
        QCOMPARE(e->fbos.size(), size_t{2});
        QCOMPARE(e->fbos[0].scale, 4);
        QCOMPARE(e->fbos[0].format, std::string("rgba_backbuffer"));
        QCOMPARE(e->fbos[1].scale, 2);
        QVERIFY(e->fbos[1].unique);
    }

    void fboFitAndUvsAreRead()
    {
        Item item;
        item.file(u"materials/effects/a.json"_s, R"({"passes":[{"shader":"effects/a"}]})");
        item.file(u"effects/ripple/effect.json"_s,
                  R"({"passes":[{"material":"materials/effects/a.json","target":"_rt_E1"}],"fbos":[)"
                  R"({"name":"_rt_E1","fit":256,"format":"rgba8888"},{"name":"_rt_E2","uvs":"repeat","width":256,"height":256,"format":"r8"},)"
                  R"({"name":"_rt_E3","uvs":"clamp","scale":2},{"name":"_rt_E4","uvs":"mirror"},{"name":"_rt_E5","fit":true},{"name":"_rt_E6","fit":16384}]})");
        std::string error;
        const auto e = loadEffect(vfsOf(item), "effects/ripple/effect.json", &error);
        QVERIFY2(e.has_value(), error.c_str());
        QCOMPARE(e->fbos[0].fit, 256);
        QVERIFY(!e->fbos[0].fitBare && !e->fbos[0].repeat);
        QVERIFY(e->fbos[1].repeat && !e->fbos[1].uvsOther);
        QVERIFY(!e->fbos[2].repeat && !e->fbos[2].uvsOther); // clamp is the default
        QVERIFY(e->fbos[3].uvsOther && !e->fbos[3].repeat);
        QVERIFY(e->fbos[4].fitBare && e->fbos[4].fit == 0);
        QCOMPARE(e->fbos[5].fit, 8192); // read no larger than a target can be
    }

    void fboSizeFollowsFitWidthAndScale()
    {
        EffectFbo fbo;
        // no hint: the layer's size, divided by the scale and by the chain's reduction
        fbo.scale = 2;
        QCOMPARE(fboSize(fbo, 1920, 1080).width, 960u);
        QCOMPARE(fboSize(fbo, 1920, 1080, 2).height, 270u);
        // fit: the long side in pixels, the layer's aspect kept, whatever the scale and the reduction
        fbo = {};
        fbo.fit = 256;
        QCOMPARE(fboSize(fbo, 1920, 1080).width, 256u);
        QCOMPARE(fboSize(fbo, 1920, 1080).height, 144u);
        QCOMPARE(fboSize(fbo, 1080, 1920).width, 144u);
        QCOMPARE(fboSize(fbo, 1080, 1920).height, 256u);
        QCOMPARE(fboSize(fbo, 512, 512, 2).width, 256u);
        QCOMPARE(fboSize(fbo, 512, 512, 2).height, 256u);
        QCOMPARE(fboSize(fbo, 4000, 10).height, 1u); // never an empty side
        fbo.scale = 4;
        QCOMPARE(fboSize(fbo, 1920, 1080).width, 256u);
        // no side goes over the renderer's limit, the short one included
        fbo.scale = 1;
        fbo.fit = 16384;
        QCOMPARE(fboSize(fbo, 100, 50).width, kMaxFboDimension);
        QCOMPARE(fboSize(fbo, 100, 50).height, kMaxFboDimension / 2);
        QCOMPARE(fboSize(fbo, 50, 100).width, kMaxFboDimension / 2);
        fbo.fit = 0;
        fbo.width = 20000;
        fbo.height = 10;
        QCOMPARE(fboSize(fbo, 100, 50).width, kMaxFboDimension);
        fbo.width = 0;
        fbo.height = 0;
        fbo.fit = 256;
        fbo.scale = 4;
        // an explicit size beats fit
        fbo.width = 64;
        fbo.height = 32;
        QCOMPARE(fboSize(fbo, 1920, 1080).width, 64u);
        QCOMPARE(fboSize(fbo, 1920, 1080).height, 32u);
    }

    void userShaderValuesBindAConstantToAProperty()
    {
        const auto pass = parseJson(
            "{\"shader\":\"flag\",\"constantshadervalues\":{\"FlagColor1\":\"0.1 0.2 0.3\",\"other\":2},"
            "\"usershadervalues\":{\"flagcolor1\":\"color2\",\"schemecolor\":\"color1\",\"bad\":3}}");
        QVERIFY(pass.has_value());
        const MaterialPass p = parseMaterialPass(*pass);
        QCOMPARE(p.constants.at("flagcolor1").user, std::string("color2"));
        QCOMPARE(p.constants.at("flagcolor1").count, 3); // the literal stays, for the unset property
        QCOMPARE(p.constants.at("flagcolor1").value[2], 0.3f);
        QCOMPARE(p.constants.at("schemecolor").user, std::string("color1"));
        QCOMPARE(p.constants.at("schemecolor").count, 0);
        QVERIFY(p.constants.at("other").user.empty());
        QVERIFY(!p.constants.count("bad"));
        QCOMPARE(p.degraded, std::vector<std::string>{"usershadervalues"}); // the entry that names no property

        // an effect instance's pass binds too, and keeps the effect material's literal under it
        Item item;
        item.file(u"materials/m.json"_s, R"({"passes":[{"shader":"effects/w","constantshadervalues":{"strength":0.1}}]})");
        item.file(u"effects/w/effect.json"_s, R"({"passes":[{"material":"materials/m.json"}]})");
        const auto entry = parseJson(R"({"file":"effects/w/effect.json","id":9,"passes":[{"usershadervalues":{"strength":"amount"}}]})");
        QVERIFY(entry.has_value());
        std::string error;
        std::vector<std::string> reasons;
        const auto inst = loadEffectInstance(vfsOf(item), *entry, &error, &reasons);
        QVERIFY2(inst.has_value(), error.c_str());
        QVERIFY(inst->degraded.empty());
        const MaterialPass merged = effectivePass(inst->def.passes[0], &inst->overrides[0]);
        QCOMPARE(merged.constants.at("strength").user, std::string("amount"));
        QCOMPARE(merged.constants.at("strength").value[0], 0.1f);
    }

    void anEffectsOwnBufferNamesGetThePrefixTheEngineKnowsThemBy()
    {
        // a Workshop bloom names its buffers "blur_start_2": they are the effect's own, and read like the _rt_ ones
        Item item;
        for (const char *n : {"a", "b"})
            item.file(QString("materials/effects/%1.json").arg(n), QByteArray("{\"passes\":[{\"shader\":\"effects/") + n + "\"}]}");
        item.file(u"effects/bloom/effect.json"_s,
                  "{\"passes\":["
                  "{\"material\":\"materials/effects/a.json\",\"target\":\"blur_start_2\",\"bind\":[{\"name\":\"previous\",\"index\":0}]},"
                  "{\"material\":\"materials/effects/b.json\",\"bind\":[{\"name\":\"blur_start_2\",\"index\":0},{\"name\":\"somewhere_else\",\"index\":1}]}],"
                  "\"fbos\":[{\"name\":\"blur_start_2\",\"scale\":2,\"format\":\"rgba_backbuffer\",\"unique\":true}]}");
        std::string error;
        const auto e = loadEffect(vfsOf(item), "effects/bloom/effect.json", &error);
        QVERIFY2(e.has_value(), error.c_str());
        QCOMPARE(e->fbos[0].name, std::string("_rt_blur_start_2"));
        QCOMPARE(e->passes[0].target, std::string("_rt_blur_start_2"));
        QCOMPARE(e->passes[0].bind[0].name, std::string("previous"));
        QCOMPARE(e->passes[1].bind[0].name, std::string("_rt_blur_start_2"));
        QCOMPARE(e->passes[1].bind[1].name, std::string("somewhere_else")); // not one of its buffers: left alone
    }

    void brokenEffectsAreErrors()
    {
        Item item;
        std::string error;
        QVERIFY(!loadEffect(vfsOf(item), "effects/x/effect.json", &error).has_value());
        item.file(u"effects/x/effect.json"_s, "{\"passes\":[]}");
        QVERIFY(!loadEffect(vfsOf(item), "effects/x/effect.json", &error).has_value());
        item.file(u"effects/y/effect.json"_s, "{\"passes\":[{\"material\":\"materials/none.json\"}]}");
        QVERIFY(!loadEffect(vfsOf(item), "effects/y/effect.json", &error).has_value());
        QVERIFY(error.find("none.json") != std::string::npos);
        item.file(u"materials/two.json"_s, "{\"passes\":[{\"shader\":\"a\"},{\"shader\":\"b\"}]}");
        item.file(u"effects/z/effect.json"_s, "{\"passes\":[{\"material\":\"materials/two.json\"}]}");
        QVERIFY(!loadEffect(vfsOf(item), "effects/z/effect.json", &error).has_value());
    }

    void instancesOverridePasses()
    {
        Item item;
        item.file(u"materials/m.json"_s,
                  "{\"passes\":[{\"shader\":\"effects/w\",\"textures\":[null,null,\"effects/normal\"],\"combos\":{\"A\":1},"
                  "\"constantshadervalues\":{\"strength\":0.1,\"keep\":7}}]}");
        item.file(u"effects/w/effect.json"_s, "{\"passes\":[{\"material\":\"materials/m.json\"}]}");
        const auto entry = parseJson(
            "{\"file\":\"effects/w/effect.json\",\"id\":9,\"name\":\"Ripple\",\"visible\":{\"user\":\"fx\",\"value\":false},"
            "\"passes\":[{\"constantshadervalues\":{\"strength\":0.8,\"extra\":{\"user\":\"k\",\"value\":2}},"
            "\"combos\":{\"b\":2},\"textures\":[null,\"masks/m\",null]}]}");
        QVERIFY(entry.has_value());
        std::string error;
        std::vector<std::string> reasons;
        const auto inst = loadEffectInstance(vfsOf(item), *entry, &error, &reasons);
        QVERIFY2(inst.has_value(), error.c_str());
        QVERIFY(reasons.empty());
        QCOMPARE(inst->name, std::string("Ripple"));
        QCOMPARE(inst->visible.user, std::string("fx"));
        QVERIFY(!inst->visible.value);
        const MaterialPass p = effectivePass(inst->def.passes[0], &inst->overrides[0]);
        QCOMPARE(p.constants.at("strength").value[0], 0.8f);
        QCOMPARE(p.constants.at("keep").value[0], 7.0f);
        QCOMPARE(p.constants.at("extra").user, std::string("k"));
        QCOMPARE(p.combos.at("A"), 1);
        QCOMPARE(p.combos.at("B"), 2);
        QCOMPARE(p.textures[1], std::string("masks/m"));
        QCOMPARE(p.textures[2], std::string("effects/normal")); // an empty entry leaves the material's texture
        // no override at all is the material itself
        QCOMPARE(effectivePass(inst->def.passes[0], nullptr).constants.at("strength").value[0], 0.1f);

        // a condition on the effect's visibility is reported, not silently dropped (a script on it or on a constant is bound by the
        // scene loader, see sceneLayersCarryTheirEffects)
        const auto scripted = parseJson(
            "{\"file\":\"effects/w/effect.json\",\"visible\":{\"script\":\"s\",\"user\":{\"name\":\"p\"}},"
            "\"passes\":[{\"constantshadervalues\":{\"strength\":{\"script\":\"x\",\"value\":1}}}]}");
        reasons.clear();
        QVERIFY(loadEffectInstance(vfsOf(item), *scripted, &error, &reasons).has_value());
        QCOMPARE(reasons.size(), size_t{1});

        // an instance with fewer pass entries than the effect has passes still gets one override per pass
        const auto bare = parseJson("{\"file\":\"effects/w/effect.json\"}");
        const auto b = loadEffectInstance(vfsOf(item), *bare, &error);
        QVERIFY(b.has_value());
        QCOMPARE(b->overrides.size(), b->def.passes.size());
        QVERIFY(b->visible.value);
    }

    void sceneLayersCarryTheirEffects()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"materials/effects/t.json"_s, "{\"passes\":[{\"shader\":\"effects/tint\"}]}");
        item.file(u"effects/tint/effect.json"_s, "{\"passes\":[{\"material\":\"materials/effects/t.json\"}]}");
        const QByteArray fx = "{\"file\":\"effects/tint/effect.json\",\"visible\":%1,\"passes\":[{\"constantshadervalues\":{\"alpha\":0.5}}]}";
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", ",\"effects\":[" + QByteArray(fx).replace("%1", "true") + "," + QByteArray(fx).replace("%1", "false") + "]")));
        Vfs vfs = vfsOf(item);
        std::string error;
        const auto scene = loadScene(vfs, "scene.json", &error);
        QVERIFY2(scene.has_value(), error.c_str());
        QCOMPARE(scene->layers.size(), size_t{1});
        QCOMPARE(scene->layers[0].effects.size(), size_t{2});
        QCOMPARE(scene->layers[0].base.shader, std::string("genericimage2"));
        QCOMPARE(scene->layers[0].effects[0].overrides[0].constants.at("alpha").value[0], 0.5f);
        QVERIFY(!scene->layers[0].effects[1].visible.value);

        // a script on an effect constant is bound as "fx:<effect>:<pass>:<constant>" and is no reason
        const QByteArray scripted = "{\"file\":\"effects/tint/effect.json\",\"passes\":[{\"constantshadervalues\":{\"Alpha\":{\"value\":0.5,\"script\":\"export function update(v) { return v; }\"}}}]}";
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", ",\"effects\":[" + QByteArray(fx).replace("%1", "true") + "," + scripted + "]")));
        Vfs withScript = vfsOf(item);
        const auto bound = loadScene(withScript, "scene.json", &error);
        QVERIFY2(bound.has_value() && bound->supported(), error.c_str());
        QCOMPARE(bound->scripts.size(), size_t{1});
        QCOMPARE(bound->scripts[0].property, std::string("fx:1:0:alpha")); // the second effect, its first pass, lower-cased

        // a script on an effect's visibility is bound as "fxv:<effect>" too
        const QByteArray visibleScript = "{\"file\":\"effects/tint/effect.json\",\"visible\":{\"value\":true,\"script\":\"export function update(v) { return v; }\"}}";
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", ",\"effects\":[" + QByteArray(fx).replace("%1", "true") + "," + visibleScript + "]")));
        Vfs withVisible = vfsOf(item);
        const auto boundVisible = loadScene(withVisible, "scene.json", &error);
        QVERIFY2(boundVisible.has_value() && boundVisible->supported(), error.c_str());
        QCOMPARE(boundVisible->scripts.size(), size_t{1});
        QCOMPARE(boundVisible->scripts[0].property, std::string("fxv:1"));

        // a missing effect is a reason of its own
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", ",\"effects\":[{\"file\":\"effects/nope/effect.json\"}]")));
        Vfs again = vfsOf(item);
        const auto missing = loadScene(again, "scene.json", &error);
        QVERIFY(missing.has_value());
        bool found = false;
        for (const std::string &r : missing->unsupported)
            found = found || r.find("effects/nope/effect.json") != std::string::npos;
        QVERIFY(found);
    }
};

QTEST_GUILESS_MAIN(TstSceneVkMaterials)
#include "tst_scenevk_materials.moc"
