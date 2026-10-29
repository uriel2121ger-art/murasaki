// `murasaki-scene-vk --check DIR`: the feature-detection verdict Murasaki uses to pick an engine. Runs the real
// binary on synthetic items and reads its JSON.

#include "core/context.h"
#include "audiofixtures.h"
#include "scenevkfixtures.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace scenevkfix;

namespace {

struct Verdict {
    bool parsed = false;
    bool ok = false;
    QStringList reasons;
    QJsonObject raw;
    int exitCode = -1;
};

Verdict check(const QString &dir, const QStringList &extra = {})
{
    QProcess p;
    p.start(QString::fromUtf8(MURASAKI_SCENE_VK_PATH), QStringList{u"--check"_s, dir} + extra);
    p.waitForFinished(30000);
    Verdict v;
    v.exitCode = p.exitCode();
    // exactly one line of JSON on stdout
    const QByteArray out = p.readAllStandardOutput();
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(out, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject() || out.count('\n') != 1) {
        return v;
    }
    v.parsed = true;
    v.raw = doc.object();
    v.ok = v.raw.value("ok").toBool();
    for (const auto &r : v.raw.value("reasons").toArray()) {
        v.reasons << r.toString();
    }
    return v;
}

bool mentions(const Verdict &v, const QString &part)
{
    for (const QString &r : v.reasons) {
        if (r.contains(part)) {
            return true;
        }
    }
    return false;
}

} // namespace

class TstSceneVkCheck : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { qputenv("MURASAKI_SCENE_VK_CHECK_DEVICE", "0"); }

    void scriptsAreCompiledAndTheirInitRunAtCheckTime()
    {
        const auto verdictFor = [&](const QByteArray &script) {
            Item item;
            item.image(u"bg"_s);
            item.tex(u"bg"_s, simpleRgba(4, 4));
            item.file(u"project.json"_s, "{\"type\":\"Scene\",\"file\":\"scene.json\"}");
            item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", ",\"alpha\":{\"script\":" + QJsonDocument(QJsonArray{QString::fromUtf8(script)}).toJson(QJsonDocument::Compact).mid(1).chopped(1)
                                                                      + ",\"value\":1}")));
            return check(item.root());
        };
        // a script that works
        const Verdict good = verdictFor("export function update(v) { return v; }");
        QVERIFY2(good.ok, qPrintable(good.reasons.join(" | ")));
        // one that does not compile never runs: the property keeps its value and the scene is drawn without the script, named
        const Verdict broken = verdictFor("// asset line\nexport let __workshopId = '1';\n\"properties\": {\n\t\"slider\": {\"value\": 100}\n}\n");
        QVERIFY2(broken.ok, qPrintable(broken.reasons.join(" | ")));
        QVERIFY(broken.raw.value(u"degraded"_s).toArray().contains(u"script error"_s));
        QVERIFY(verdictFor("export function update(v { }").ok);
        const auto degradedAsScriptError = [&](const Verdict &v) { return v.ok && v.raw.value(u"degraded"_s).toArray().contains(u"script error"_s); };
        // one whose init throws a ReferenceError or a TypeError dies the same way in WE, which goes on without it: the scene is accepted,
        // drawn without the script and named `script error`
        QVERIFY(degradedAsScriptError(verdictFor("export function init() { nothingHere(); }")));
        // a script that looks up a layer that is not there dies on null, in WE too: the scene is not refused for that
        QVERIFY(verdictFor("export function init() { thisScene.getLayer('nowhere').visible = false; }").ok);
        // an API WE does not have either (3448877775's getParticleSystem, 3212731906's getSprite, 3326873240's bare `scene`) is the same
        QVERIFY(degradedAsScriptError(verdictFor("export function init() { thisLayer.getParticleSystem(); }")));
        QVERIFY(degradedAsScriptError(verdictFor("export function init() { thisScene.getSprite('x'); }")));
        QVERIFY(degradedAsScriptError(verdictFor("const l = scene.getLayer('x'); export function update(v) { return v; }")));
        // what is the engine's own limit, not the script's, still refuses: one that never ends is named and refused
        QVERIFY(mentions(verdictFor("export function init() { for (;;) {} }"), "SceneScript"));
        // custom model geometry is stored and drawn by nothing: accepted, and named
        const Verdict geometry = verdictFor("export function init() { thisScene.createLayer({model: thisScene.createModelData({})}); }");
        QVERIFY2(geometry.ok, qPrintable(geometry.reasons.join(" | ")));
        QVERIFY(geometry.raw.value(u"degraded"_s).toArray().contains(u"custom model geometry (not drawn)"_s));
        // the timers, the cursor events and the user property hook are implemented: the scene is accepted
        QVERIFY(verdictFor("export function init() { engine.setTimeout(() => {}, 10); }").ok);
        QVERIFY(verdictFor("export function cursorClick(e) {} export function applyUserProperties(p) {}").ok);
        // browser APIs a SceneScript cannot have are named
        const Verdict fetchCall = verdictFor("export function init() { fetch('http://x'); }");
        QVERIFY(!fetchCall.ok);
        QVERIFY(mentions(fetchCall, "fetch"));
        const Verdict frames = verdictFor("export function init() { requestAnimationFrame(() => {}); }");
        QVERIFY(!frames.ok);
        QVERIFY(mentions(frames, "requestAnimationFrame"));
        // a video texture can be driven (IVideoTexture): asking for one is no reason to refuse the scene
        QVERIFY(verdictFor("export function init() { const v = thisLayer.getVideoTexture(); if (v) v.play(); }").ok);
    }

    void scriptsAreJudgedByWhatTheyCost()
    {
        const auto verdictFor = [&](int count, const char *script) {
            Item item;
            item.image(u"bg"_s);
            item.tex(u"bg"_s, simpleRgba(4, 4));
            item.file(u"project.json"_s, "{\"type\":\"Scene\",\"file\":\"scene.json\"}");
            QByteArray objects;
            for (int i = 1; i <= count; ++i) {
                if (i > 1)
                    objects += ",";
                objects += imageObject(i, "bg", ",\"alpha\":{\"script\":\"" + QByteArray(script) + "\",\"value\":1}");
            }
            item.file(u"scene.json"_s, sceneJson(objects));
            return check(item.root());
        };
        // a thousand scripts share one realm: no longer too many
        const Verdict many = verdictFor(1000, "export function update(v) { return v; }");
        QVERIFY2(many.ok, qPrintable(many.reasons.join(" | ")));
        // updates that take longer than a frame at the engine's lowest rate are named; far longer ones refuse the scene
        const Verdict slow = verdictFor(3, "export function update(v) { const end = Date.now() + 30; while (Date.now() < end) {} return v; }");
        QVERIFY2(slow.ok, qPrintable(slow.reasons.join(" | ")));
        QVERIFY(slow.raw.value(u"degraded"_s).toArray().contains(u"slow scripts"_s));
        // an init that takes longer than a quarter of a second (3798688689 builds its particle meshes in ~0.23 s) is named, not refused: the
        // renderer gives it the same second the check does
        const Verdict slowStart = verdictFor(1, "export function init(v) { const end = Date.now() + 400; while (Date.now() < end) {} return v; }");
        QVERIFY2(slowStart.ok, qPrintable(slowStart.reasons.join(" | ")));
        QVERIFY(slowStart.raw.value(u"degraded"_s).toArray().contains(u"slow script start"_s));
        const Verdict crawl = verdictFor(6, "export function update(v) { const end = Date.now() + 45; while (Date.now() < end) {} return v; }");
        QVERIFY(!crawl.ok);
        QVERIFY2(mentions(crawl, "scripts takes"), qPrintable(crawl.reasons.join(" | ")));
        // a script that throws in its updates is stopped and named; the scene is drawn
        const Verdict throwing = verdictFor(2, "let n = 0; export function update(v) { if (++n > 1) throw new Error('later'); return v; }");
        QVERIFY2(throwing.ok, qPrintable(throwing.reasons.join(" | ")));
        QVERIFY(throwing.raw.value(u"degraded"_s).toArray().contains(u"script error"_s));
        // nor scripts that cost more memory than the engine gives them: 700 that each need a realm of their own
        const Verdict big = verdictFor(700, "globalThis.mine = 1; export function update(v) { return v; }");
        QVERIFY(!big.ok);
        QVERIFY2(mentions(big, "MiB, over the 128"), qPrintable(big.reasons.join(" | ")));
    }

    void soundsAreProbedForDecodableMp3()
    {
        const auto verdictFor = [&](const std::vector<uint8_t> &mp3) {
            Item item;
            item.image(u"bg"_s);
            item.tex(u"bg"_s, simpleRgba(4, 4));
            item.file(u"project.json"_s, "{\"type\":\"Scene\",\"file\":\"scene.json\"}");
            item.file(u"sounds/a.mp3"_s, QByteArray(reinterpret_cast<const char *>(mp3.data()), static_cast<qsizetype>(mp3.size())));
            item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + ",{\"id\":2,\"name\":\"s\",\"sound\":[\"sounds/a.mp3\"],\"volume\":1,\"playbackmode\":\"loop\"}"));
            return check(item.root());
        };
        const Verdict good = verdictFor(audiofix::sineMp3());
        QVERIFY2(good.ok, qPrintable(good.reasons.join(" | ")));
        const Verdict junk = verdictFor(std::vector<uint8_t>(5000, 0x41));
        QVERIFY(!junk.ok);
        QVERIFY(mentions(junk, "audio"));
        const Verdict empty = verdictFor({});
        QVERIFY(!empty.ok);
    }

    void supportedItem()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"project.json"_s, "{\"type\":\"Scene\",\"file\":\"scene.json\"}");
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")));
        const Verdict v = check(item.root());
        QVERIFY(v.parsed);
        QCOMPARE(v.exitCode, 0);
        QVERIFY(v.ok);
        QVERIFY(v.reasons.isEmpty());
        QCOMPARE(v.raw.value("engine").toString(), u"murasaki-scene-vk"_s);
        QVERIFY(!v.raw.value("version").toString().isEmpty());
    }

    void itemWithoutProjectJsonIsJudgedByItsScene()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")));
        QVERIFY(check(item.root()).ok);
    }

    void unsupportedFeaturesAreListedWithTheirMilestone()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"materials/effects/x.json"_s, "{\"passes\":[{\"shader\":\"effects/x\"}]}");
        item.file(u"effects/x/effect.json"_s, "{\"passes\":[{\"material\":\"materials/effects/x.json\"}]}");
        item.file(u"scene.json"_s,
                  sceneJson(imageObject(1, "bg", ",\"effects\":[{\"file\":\"effects/x/effect.json\"}]") + ",{\"id\":2,\"particle\":\"p.json\",\"image\":null},{\"id\":3,\"light\":{\"x\":1}}"));
        const Verdict v = check(item.root());
        QVERIFY(v.parsed);
        QCOMPARE(v.exitCode, 0); // a verdict is not a failure
        QVERIFY(!v.ok);
        QVERIFY(mentions(v, u"particles: particle file 'p.json' not found"_s));
        QVERIFY(mentions(v, u"light type '?'"_s));
    }

    void effectsAreAcceptedWhenTheirShadersCompile()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"materials/effects/x.json"_s, "{\"passes\":[{\"shader\":\"effects/x\"}]}");
        item.file(u"effects/x/effect.json"_s, "{\"passes\":[{\"material\":\"materials/effects/x.json\"}]}");
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", ",\"effects\":[{\"file\":\"effects/x/effect.json\"}]")));
        // the effect's shader is missing: the verdict names it
        Verdict v = check(item.root());
        QVERIFY(v.parsed && !v.ok);
        QVERIFY(mentions(v, u"shader compile: effects/x"_s));
        // and with the shader there, the item is accepted
        item.file(u"shaders/effects/x.vert"_s,
                  "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
                  "void main() { gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix); v_TexCoord = a_TexCoord; }\n");
        item.file(u"shaders/effects/x.frag"_s,
                  "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\nvoid main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord); }\n");
        v = check(item.root());
        QVERIFY2(v.ok, qPrintable(v.reasons.join("; ")));
    }

    void notScenesAndPresets()
    {
        Item video;
        video.file(u"project.json"_s, "{\"type\":\"video\",\"file\":\"clip.mp4\"}");
        video.file(u"clip.mp4"_s, "x");
        const Verdict v = check(video.root());
        QVERIFY(v.parsed && !v.ok);
        QVERIFY(mentions(v, u"not a scene"_s));

        Item preset;
        preset.file(u"project.json"_s, "{\"type\":\"scene\",\"dependency\":\"1081733658\",\"preset\":{}}");
        const Verdict p = check(preset.root());
        QVERIFY(p.parsed && !p.ok);
        QVERIFY(mentions(p, u"1081733658"_s));
    }

    void brokenItemsAreReasonsToo()
    {
        Verdict missing = check(u"/nonexistent/item"_s);
        QVERIFY(missing.parsed && !missing.ok);
        QVERIFY(mentions(missing, u"does not exist"_s));

        Item empty;
        const Verdict none = check(empty.root());
        QVERIFY(none.parsed && !none.ok);
        QVERIFY(mentions(none, u"scene.json"_s));

        Item badTexture;
        badTexture.image(u"bg"_s);
        badTexture.file(u"materials/bg.tex"_s, "garbage");
        badTexture.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")));
        const Verdict t = check(badTexture.root());
        QVERIFY(t.parsed && !t.ok);
        QVERIFY(mentions(t, u"materials/bg.tex"_s));
    }

    void completeScenesListNoDegradedFeatures()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")));
        const Verdict plain = check(item.root());
        QVERIFY(plain.parsed && plain.raw.value("degraded").isArray());
        QVERIFY(plain.raw.value("degraded").toArray().isEmpty());

        // (no feature is dropped silently now: a text with effects is drawn with them)
        const QByteArray text = R"({"id":2,"name":"t","text":"hi","pointsize":20,"font":"systemfont_sans","origin":"10 10 0"})";
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + "," + text));
        const Verdict v = check(item.root());
        QVERIFY(v.parsed);
        QCOMPARE(v.exitCode, 0);
        QVERIFY(v.raw.value("degraded").toArray().isEmpty());
    }

    void reasonsWithQuotesStayValidJson()
    {
        Item item;
        item.image(u"bg"_s);
        item.file(u"materials/bg.json"_s, "{\"passes\":[{\"shader\":\"genericimage2\",\"textures\":[\"we\\\"ird\\\\name\\t\"]}]}");
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")));
        const Verdict v = check(item.root());
        QVERIFY(v.parsed);
        QVERIFY(mentions(v, u"we\"ird\\name\t"_s));
    }

    void deviceProbeCanFailTheVerdict()
    {
        std::string error;
        if (!murasaki::scene::VkContext::create({}, error))
            QSKIP("no Vulkan 1.3 device");
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")));
        qputenv("MURASAKI_SCENE_VK_CHECK_DEVICE", "1");
        const Verdict ok = check(item.root());
        QVERIFY(ok.ok);
        qputenv("MURASAKI_VK_DEVICE", "no-such-gpu");
        const Verdict bad = check(item.root());
        qunsetenv("MURASAKI_VK_DEVICE");
        qputenv("MURASAKI_SCENE_VK_CHECK_DEVICE", "0");
        QVERIFY(bad.parsed && !bad.ok);
        QVERIFY(mentions(bad, u"Vulkan"_s));
    }
};

QTEST_GUILESS_MAIN(TstSceneVkCheck)
#include "tst_scenevk_check.moc"
