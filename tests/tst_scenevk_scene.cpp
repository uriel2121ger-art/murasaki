// murasaki-scene-vk scene parsing: image layers, groups, properties and every reason an item is turned away.
// Items are synthetic directories (and one package) the test writes itself.

#include "assets/vfs.h"
#include "scene/properties.h"
#include "scene/scene.h"
#include "puppetfixtures.h"
#include "support/meshwriter.h"
#include "scenevkfixtures.h"
#include "testutil.h"

#include <QDir>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

#include <algorithm>

using namespace Qt::StringLiterals;
using namespace murasaki::scene;
using namespace scenevkfix;

namespace {

bool hasReason(const Scene &scene, const char *part)
{
    return std::any_of(scene.unsupported.begin(), scene.unsupported.end(), [&](const std::string &r) { return r.find(part) != std::string::npos; });
}

Scene load(Item &item, std::string *error = nullptr)
{
    Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
    auto scene = loadScene(vfs, "scene.json", error);
    if (!scene)
        qFatal("loadScene failed: %s", error ? error->c_str() : "?");
    return *scene;
}

} // namespace

class TstSceneVkScene : public QObject
{
    Q_OBJECT

private slots:
    void theSceneFileIsTheOneProjectJsonNames()
    {
        // some items ship "gifscene.json" (in gifscene.pkg or loose) and say so in project.json; every reader (the engine, --check and the
        // development tools) takes the name from there
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")));
        {
            const Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
            QCOMPARE(sceneFileName(vfs), std::string("scene.json")); // no project.json
        }
        item.file(u"project.json"_s, R"({"type":"scene","file":"gifscene.json","title":"x"})");
        item.file(u"gifscene.json"_s, sceneJson(imageObject(1, "bg")));
        QFile::remove(item.root() + u"/scene.json"_s);
        const Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        QCOMPARE(sceneFileName(vfs), std::string("gifscene.json"));
        std::string error;
        QVERIFY2(loadScene(vfs, sceneFileName(vfs), &error).has_value(), error.c_str());
        QVERIFY(!loadScene(vfs, "scene.json", &error).has_value()); // what the snapshot tool used to read
        // an empty or missing name falls back
        item.file(u"project.json"_s, R"({"type":"scene","file":""})");
        QCOMPARE(sceneFileName(Vfs::forItem(item.root().toStdString(), {})), std::string("scene.json"));
    }

    void theEditorsDefaultClearColourReadsAsBlackAndAChosenOneStays()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        const auto clearOf = [&](const char *clear) {
            item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg"), QByteArray(",\"clearcolor\":\"") + clear + "\"").replace("\"clearcolor\":\"0.1 0.2 0.3\",", ""));
            return load(item).clearColor;
        };
        const Vec3 untouched = clearOf("0.7 0.7 0.7");
        QCOMPARE(untouched.x + untouched.y + untouched.z, 0.0f);
        const Vec3 withDigits = clearOf("0.70000 0.70000 0.70000");
        QCOMPARE(withDigits.x, 0.0f);
        const Vec3 chosen = clearOf("0.529 0.09 0.09");
        QCOMPARE(chosen.x, 0.529f);
        const Vec3 nearlyGrey = clearOf("0.7 0.7 0.8");
        QCOMPARE(nearlyGrey.z, 0.8f);
        // bound to a user property it is somebody's explicit setting: kept
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")).replace("\"clearcolor\":\"0.1 0.2 0.3\"", "\"clearcolor\":{\"user\":\"bg\",\"value\":\"0.7 0.7 0.7\"}"));
        QCOMPARE(load(item).clearColor.x, 0.7f);
    }

    void soundObjectsReadTheirPlaybackModeFilesAndDelays()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"sounds/a.mp3"_s, "x");
        item.file(u"sounds/b.mp3"_s, "x");
        const auto load1 = [&](const QByteArray &sound) {
            item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + ",{\"id\":2,\"name\":\"s\"," + sound + "}"));
            return load(item);
        };
        // random: every file that exists, the delays kept
        Scene random = load1(R"("sound":["sounds/a.mp3","sounds/missing.mp3","sounds/b.mp3"],"playbackmode":"random","mintime":1.5,"maxtime":4,"volume":0.5)");
        QVERIFY2(random.supported(), "random playback is supported");
        QCOMPARE(random.sounds.size(), size_t{1});
        QVERIFY(random.sounds[0].random);
        QVERIFY(!random.sounds[0].loop);
        QCOMPARE(random.sounds[0].files.size(), size_t{2});
        QCOMPARE(random.sounds[0].minTime, 1.5f);
        QCOMPARE(random.sounds[0].maxTime, 4.0f);
        // a maximum under the minimum is raised to it; the numbers are clamped
        random = load1(R"("sound":["sounds/a.mp3"],"playbackmode":"random","mintime":3,"maxtime":1)");
        QCOMPARE(random.sounds[0].maxTime, 3.0f);
        random = load1(R"("sound":["sounds/a.mp3"],"playbackmode":"random","mintime":-5,"maxtime":1e9)");
        QCOMPARE(random.sounds[0].minTime, 0.0f);
        QVERIFY(random.sounds[0].maxTime <= 3600.0f);
        // loop (or no mode at all) plays the list in order
        const Scene loop = load1(R"("sound":["sounds/a.mp3","sounds/b.mp3"])");
        QVERIFY(loop.sounds[0].loop);
        QVERIFY(!loop.sounds[0].random);
        QCOMPARE(loop.sounds[0].files.size(), size_t{2});
        // single plays the first file once
        const Scene single = load1(R"("sound":["sounds/a.mp3"],"playbackmode":"single")");
        QVERIFY(single.supported());
        QVERIFY(!single.sounds[0].loop);
        QVERIFY(!single.sounds[0].random);
        // formats: mp3, ogg, flac and wav (the extension decides here, the bytes later), others are named
        item.file(u"sounds/c.OGG"_s, "x");
        item.file(u"sounds/d.flac"_s, "x");
        item.file(u"sounds/e.wav"_s, "x");
        item.file(u"sounds/f.m4a"_s, "x");
        QVERIFY(load1(R"("sound":["sounds/c.OGG","sounds/d.flac","sounds/e.wav"])").supported());
        QVERIFY(hasReason(load1(R"("sound":["sounds/f.m4a"])"), "only mp3, ogg, flac and wav"));
        // anything else is a named reason, and so is a list over the limit
        QVERIFY(hasReason(load1(R"("sound":["sounds/a.mp3"],"playbackmode":"shuffle")"), "playback mode 'shuffle'"));
        QByteArray many = R"("sound":[)";
        for (size_t i = 0; i < kMaxSoundFiles + 1; ++i)
            many += QByteArray(i ? "," : "") + "\"sounds/a.mp3\"";
        many += "]";
        QVERIFY(hasReason(load1(many), "files in one sound object"));
        // a sound with no file that exists, or none named, plays nothing and is no reason
        const auto none = load1(R"("sound":["sounds/none.mp3"])");
        QVERIFY(none.supported());
        QVERIFY(load1(R"("sound":[""])").supported());
    }

    void theProjectsDefaultsFillWhatNobodySet()
    {
        Item item;
        item.file(u"project.json"_s,
                  R"({"general":{"properties":{"flag":{"type":"bool","value":true},"amount":{"type":"slider","value":0.3},)"
                  R"("choice":{"type":"combo","value":"2"},"tint":{"type":"color","value":"1 0.5 0"},"group":{"type":"group","value":""},"kept":{"type":"bool","value":false}}}})");
        const Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        PropertyStore props;
        props.set("kept", true); // set before: the default does not take it back
        applyProjectDefaults(vfs, props);
        QCOMPARE(props.getBool("flag"), std::optional<bool>(true));
        QVERIFY(props.getNumber("amount") && std::abs(*props.getNumber("amount") - 0.3) < 1e-9);
        QCOMPARE(props.getText("choice"), std::optional<std::string>("2"));
        QVERIFY(props.getColor("tint").has_value());
        QCOMPARE(props.getBool("kept"), std::optional<bool>(true));
        // an item without a project.json has no defaults to give
        Item bare;
        PropertyStore none;
        applyProjectDefaults(Vfs::forItem(bare.root().toStdString(), {}), none);
        QVERIFY(none.values().empty());
    }

    void aStringOfTwoScriptsKeepsTheLast()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        // the template's empty update, then the script itself: both start with 'use strict' at the beginning of a line
        const QByteArray two = "'use strict';\nexport function update(v) { return v; }\n'use strict';\nexport function update(v) { return 7; }";
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", ",\"alpha\":{\"value\":1,\"script\":" + QJsonDocument(QJsonArray{QString::fromUtf8(two)}).toJson(QJsonDocument::Compact).mid(1).chopped(1) + "}")));
        const Scene scene = load(item);
        QCOMPARE(scene.scripts.size(), size_t{1});
        QVERIFY2(scene.scripts[0].source.find("return 7") != std::string::npos && scene.scripts[0].source.find("return v") == std::string::npos, scene.scripts[0].source.c_str());
        // a lone script, and a 'use strict' that is not at the start of a line, are left alone
        const QByteArray one = "'use strict';\nexport function update(v) { 'use strict'; return v; }";
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", ",\"alpha\":{\"value\":1,\"script\":" + QJsonDocument(QJsonArray{QString::fromUtf8(one)}).toJson(QJsonDocument::Compact).mid(1).chopped(1) + "}")));
        QCOMPARE(load(item).scripts[0].source, one.toStdString());
    }

    void scriptsOnTheScenesOwnPropertiesAreBoundToTheScene()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg"), ",\"bloom\":true,\"bloomstrength\":{\"value\":2,\"script\":\"export function update(v) { return v; }\"},"
                                                                  "\"camerashakeamplitude\":{\"value\":0.5,\"script\":\"export function update(v) { return v; }\"}"));
        const Scene scene = load(item);
        QVERIFY2(scene.supported(), "the scene's own properties take scripts");
        QCOMPARE(scene.scripts.size(), size_t{2});
        QVERIFY(scene.scripts[0].scene);
        QCOMPARE(scene.scripts[0].property, std::string("scene.camerashakeamplitude")); // in the order the loader reads them
        QCOMPARE(scene.scripts[1].property, std::string("scene.bloomstrength"));
    }

    void keyframedPropertiesAreReadAndSampled()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        const QString anim = QStringLiteral("\"animation\":{\"c0\":[{\"frame\":10,\"value\":1},{\"frame\":0,\"value\":0},{\"frame\":20,\"value\":5}],"
                                "\"options\":{\"fps\":10,\"length\":20,\"mode\":\"%1\"}}");
        const auto sceneWith = [&](const char *mode, const QByteArray &extra = {}) {
            item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", ",\"alpha\":{\"value\":1," + anim.arg(QString::fromLatin1(mode)).toUtf8() + extra + "}"
                                                                      ",\"scale\":{\"value\":\"1 1 1\"," + anim.arg(QString::fromLatin1(mode)).toUtf8() + "}")));
            return load(item);
        };
        const Scene scene = sceneWith("single");
        QCOMPARE(scene.animations.size(), size_t{2});
        const PropertyAnimation &alpha = scene.animations[1].target == PropertyAnimation::Target::Alpha ? scene.animations[1] : scene.animations[0];
        QCOMPARE(alpha.curves[0].size(), size_t{3});
        QCOMPARE(alpha.curves[0].front().frame, 0.0f); // sorted by frame
        // single: linear between keys, then it holds; 10 fps, so frame 5 is half a second
        QCOMPARE(sampleAnimation(alpha, 0, 0.0, -1.0f), 0.0f);
        QVERIFY(std::fabs(sampleAnimation(alpha, 0, 0.5, -1.0f) - 0.5f) < 1e-5f);
        QVERIFY(std::fabs(sampleAnimation(alpha, 0, 1.5, -1.0f) - 3.0f) < 1e-5f);
        QCOMPARE(sampleAnimation(alpha, 0, 99.0, -1.0f), 5.0f);
        QCOMPARE(sampleAnimation(alpha, 1, 0.5, -1.0f), -1.0f); // a component without keys keeps the fallback
        // loop wraps at the length (20 frames = 2 s); mirror runs back
        const Scene looping = sceneWith("loop");
        QVERIFY(std::fabs(sampleAnimation(looping.animations[0], 0, 2.5, -1.0f) - 0.5f) < 1e-5f);
        const Scene mirrored = sceneWith("mirror");
        QVERIFY(std::fabs(sampleAnimation(mirrored.animations[0], 0, 2.5, -1.0f) - 3.0f) < 1e-5f); // frame 25 runs back to 15
        // the editor's handles: the default pair (front 1,0 / back -1,0) is an ease in and out, a disabled one the straight line
        {
            PropertyAnimation eased;
            eased.fps = 10.0f;
            eased.length = 10.0f;
            AnimationKey a, b;
            a.frame = 0.0f;
            a.value = 0.0f;
            a.frontOn = true; // (1, 0)
            b.frame = 10.0f;
            b.value = 1.0f;
            b.backOn = true; // (-1, 0)
            eased.curves[0] = {a, b};
            QVERIFY(std::fabs(sampleAnimation(eased, 0, 0.5, -1.0f) - 0.5f) < 1e-4f);     // symmetric: half way at half time
            QVERIFY(sampleAnimation(eased, 0, 0.2, -1.0f) < 0.1f);                        // slow at the start (a straight line says 0.2)
            QVERIFY(sampleAnimation(eased, 0, 0.8, -1.0f) > 0.9f);                        // and at the end
            QVERIFY(std::fabs(sampleAnimation(eased, 0, 0.3, -1.0f) - 0.048f) < 0.01f);   // the bezier's time is warped too: x(t) = 0.3 at t = 0.132, 3t^2 - 2t^3 there
            // only the start eased: the end handle is the straight line's third
            eased.curves[0][1].backOn = false;
            QVERIFY(sampleAnimation(eased, 0, 0.2, -1.0f) < 0.2f);
            // handles that overshoot are followed, and the value at a key is the key's
            eased.curves[0][1].backOn = true;
            eased.curves[0][1].backY = 0.2f;
            QVERIFY(sampleAnimation(eased, 0, 0.9, -1.0f) > 1.0f);
            QCOMPARE(sampleAnimation(eased, 0, 1.0, -1.0f), 1.0f);
        }
        // a property that has a script keeps its keyframes (the script runs after them and may play, pause and stop them)
        const Scene scripted = sceneWith("single", ",\"script\":\"export function update(v) { return v; }\"");
        QCOMPARE(scripted.animations.size(), size_t{2});
        // `startpaused` holds an animation at its first frame until a script plays it; `name` is what getAnimation finds
        const Scene paused = sceneWith("single\",\"startpaused\":true,\"name\":\"intro", "");
        QVERIFY(!paused.animations[0].playback.playing);
        QCOMPARE(paused.animations[0].name, std::string("intro"));
        QCOMPARE(paused.animations[0].endSeconds(), 0.0);
    }

    void maxWidthWrapsOnlyWhileLimitWidthIsOn()
    {
        Item item;
        const auto widthOf = [&](const char *extra) {
            item.file(u"scene.json"_s, sceneJson(QByteArray("{\"id\":1,\"name\":\"t\",\"text\":\"Some words here\",\"font\":\"fonts/f.ttf\",\"pointsize\":20,\"maxwidth\":500,") + extra
                                                 + "\"origin\":\"0 0 0\"}"));
            const Scene scene = load(item);
            return scene.layers.empty() || !scene.layers[0].text ? -1.0f : scene.layers[0].text->maxWidth;
        };
        QCOMPARE(widthOf("\"limitwidth\":true,"), 500.0f);
        QCOMPARE(widthOf(""), 500.0f);                       // no `limitwidth` key: the width counts
        QCOMPARE(widthOf("\"limitwidth\":false,"), 0.0f);    // off: a stale maxwidth is not a wrap width (3796026374's 2095 wide title)
    }

    void keyframesBelongToTheLayerThatLoaded()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        const QByteArray anim = "\"animation\":{\"c0\":[{\"frame\":0,\"value\":0},{\"frame\":10,\"value\":1}],\"options\":{\"fps\":10,\"length\":10,\"mode\":\"single\"}}";
        // the first object's model does not exist: no layer, and its keyframes must not move on to the second one
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "missing", ",\"alpha\":{\"value\":1," + anim + "}") + "," + imageObject(2, "bg")));
        Scene scene = load(item);
        QCOMPARE(scene.layers.size(), size_t{1});
        QCOMPARE(scene.animations.size(), size_t{0});
        // a user bound alpha is the property's, not the keyframes'
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", ",\"alpha\":{\"user\":\"fade\",\"value\":1," + anim + "}")));
        QCOMPARE(load(item).animations.size(), size_t{0});
        // a free one is animated, and the keyframes name the layer that exists
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + "," + imageObject(2, "bg", ",\"alpha\":{\"value\":1," + anim + "}")));
        scene = load(item);
        QCOMPARE(scene.animations.size(), size_t{1});
        QCOMPARE(scene.animations[0].index, size_t{1});
    }

    void aConditionOnTheCameraParallaxIsKept()
    {
        // 3462491575, 3660962877: the parallax on while a combo reads "0"; any value read as a boolean takes a condition
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg"), ",\"cameraparallax\":{\"user\":{\"name\":\"parallax\",\"condition\":\"0\"},\"value\":true}"));
        const Scene scene = load(item);
        QVERIFY2(scene.supported(), scene.unsupported.empty() ? "" : scene.unsupported[0].c_str());
        QCOMPARE(scene.parallax.enabled.user, std::string("parallax"));
        QCOMPARE(scene.parallax.enabled.condition, std::string("0"));
        PropertyStore props;
        props.set("parallax", std::string("0"));
        QVERIFY(resolve(scene.parallax.enabled, props));
        props.set("parallax", std::string("1"));
        QVERIFY(!resolve(scene.parallax.enabled, props));
    }

    void theAmbientColoursAreReadAsWritten()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg"), ",\"ambientcolor\":\"0.3 0.4 0.5\""));
        const Scene scene = load(item);
        QCOMPARE(scene.ambientColor.y, 0.4f);
        QCOMPARE(scene.skylightColor.x, 0.3f); // the default
        // a script on the ambient colour (3769716842) does not refuse the scene: the value it was written with is the one used
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg"), ",\"ambientcolor\":{\"script\":\"export function update(v) { return v; }\",\"value\":\"0.1 0.2 0.3\"}"));
        const Scene scripted = load(item);
        QVERIFY2(scripted.supported(), scripted.unsupported.empty() ? "" : scripted.unsupported[0].c_str());
        QCOMPARE(scripted.ambientColor.z, 0.3f);
    }

    void aPuppetModelAndItsAnimationLayersAreLoaded()
    {
        using namespace puppetfix;
        Item item;
        item.image(u"bg"_s, {}, ",\"puppet\":\"models/q.mdl\"");
        item.tex(u"bg"_s, simpleRgba(4, 4));
        const auto mdl = makeMdl({{"root", -1, 0, 0}}, {{7, "Animation 1", "loop", 30.0f, 1, {{{frame(0, 0), frame(0, 0)}}}}});
        item.file(u"models/q.mdl"_s, QByteArray(reinterpret_cast<const char *>(mdl.data()), static_cast<qsizetype>(mdl.size())));
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", ",\"animationlayers\":[{\"animation\":7,\"blend\":0.5,\"rate\":2,\"visible\":{\"user\":\"walk\",\"value\":true}},"
                                                                  "{\"animation\":9}]")));
        const Scene scene = load(item);
        QVERIFY2(scene.supported(), "puppets are supported");
        QCOMPARE(scene.layers.size(), size_t{1});
        QVERIFY(scene.layers[0].puppet);
        QCOMPARE(scene.layers[0].puppet->vertexCount(), size_t{4});
        QCOMPARE(scene.layers[0].animationLayers.size(), size_t{2});
        QCOMPARE(scene.layers[0].animationLayers[0].animation, 7u);
        QCOMPARE(scene.layers[0].animationLayers[0].blend.value, 0.5f);
        QCOMPARE(scene.layers[0].animationLayers[0].rate.value, 2.0f);
        QCOMPARE(scene.layers[0].animationLayers[0].visible.user, std::string("walk"));
        QCOMPARE(scene.layers[0].animationLayers[1].blend.value, 1.0f); // defaults
        // an animation layer's visibility may be a property compared with a value, like an object's
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", ",\"animationlayers\":[{\"animation\":7,\"visible\":{\"user\":{\"name\":\"mode\",\"condition\":\"2\"},\"value\":true}}]")));
        const Scene conditional = load(item);
        QVERIFY2(conditional.supported(), "a condition on an animation layer's visibility");
        QCOMPARE(conditional.layers[0].animationLayers[0].visible.user, std::string("mode"));
        QCOMPARE(conditional.layers[0].animationLayers[0].visible.condition, std::string("2"));
        // a model that is not a puppet, missing, or hostile is a reason
        item.file(u"models/q.mdl"_s, "MDLV0023\0not a model at all, long enough to look like one......................");
        QVERIFY(hasReason(load(item), "puppet:"));
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", ",\"animationlayers\":[{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}]")));
        QVERIFY(hasReason(load(item), "animation layers"));
    }

    void lightObjectsAreReadWithTheirKeysAndWhatIsNotDrawnIsNamed()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        const auto loadWith = [&](const QByteArray &lights) {
            item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + "," + lights));
            return load(item);
        };
        using Names = std::vector<std::string>;
        // every type, with the keys each one has
        const Scene all = loadWith(
            R"({"id":2,"name":"p","light":"lpoint","origin":"1 2 3","color":"1 0.5 0","intensity":4,"radius":250,"exponent":1.5,"density":0.5,"volumetricsexponent":2,)"
            R"("cascadedistance0":10,"cascadedistance1":20,"cascadedistance2":40,"castshadow":false},)"
            R"({"id":3,"name":"s","light":"lspot","angles":"0 1.5 0","innercone":12,"outercone":35,"lightsourcesize":2,"parent":2,"visible":false},)"
            R"({"id":4,"name":"t","light":"ltube","controlpoint":"100 0 0"},)"
            R"({"id":5,"name":"d","light":"ldirectional","intensity":{"user":"sun","value":3}},)"
            R"({"id":6,"name":"o","light":"point","radius":800})");
        QVERIFY(all.supported());
        QCOMPARE(all.degraded, Names{});
        QCOMPARE(all.lights.size(), size_t(5));
        const SceneLight &p = all.lights[0];
        QCOMPARE(p.type, LightType::Point);
        QCOMPARE(p.color.value.y, 0.5f);
        QCOMPARE(p.intensity.value, 4.0f);
        QCOMPARE(p.radius.value, 250.0f);
        QCOMPARE(p.exponent.value, 1.5f);
        QCOMPARE(p.density.value, 0.5f);
        QCOMPARE(p.volumetricsExponent.value, 2.0f);
        QCOMPARE(p.cascadeDistances[2].value, 40.0f);
        QCOMPARE(all.nodes[p.node].origin.z, 3.0f);
        const SceneLight &spot = all.lights[1];
        QCOMPARE(spot.type, LightType::Spot);
        QCOMPARE(spot.innerCone.value, 12.0f);
        QCOMPARE(spot.outerCone.value, 35.0f);
        QCOMPARE(spot.lightSourceSize.value, 2.0f);
        QCOMPARE(all.nodes[spot.node].parentId, 2);
        QVERIFY(!all.nodes[spot.node].visible.value);
        QCOMPARE(all.lights[2].type, LightType::Tube);
        QVERIFY(all.lights[2].controlPoint.has_value());
        QCOMPARE(all.lights[2].controlPoint->x, 100.0f);
        QCOMPARE(all.lights[3].type, LightType::Directional);
        QCOMPARE(all.lights[3].intensity.user, std::string("sun"));
        QCOMPARE(all.lights[4].type, LightType::LegacyPoint);
        QCOMPARE(all.lights[4].radius.value, 800.0f);
        // the defaults: white, intensity 1, radius 1000, exponent 2, cones 20 and 30 degrees
        const SceneLight bare = loadWith(R"({"id":2,"light":"lspot"})").lights.at(0);
        QCOMPARE(bare.intensity.value, 1.0f);
        QCOMPARE(bare.radius.value, 1000.0f);
        QCOMPARE(bare.exponent.value, 2.0f);
        QCOMPARE(bare.innerCone.value, 20.0f);
        QCOMPARE(bare.outerCone.value, 30.0f);

        // keyframes on the intensity play; a script on a light's value, keyframes on another one are named
        const Scene animated = loadWith(R"({"id":2,"light":"lpoint","intensity":{"animation":{"c0":[{"frame":0,"value":0},{"frame":30,"value":2}],)"
                                        R"("options":{"fps":30,"length":30,"mode":"loop"}},"value":1}})");
        QCOMPARE(animated.degraded, Names{});
        QCOMPARE(animated.animations.size(), size_t(1));
        QCOMPARE(animated.animations[0].target, PropertyAnimation::Target::LightIntensity);
        QCOMPARE(animated.animations[0].index, size_t(0));
        // a script on one of a light's values runs, as the handle's "light.<key>" property; on a switch it is named
        const Scene scripted = loadWith(R"({"id":2,"light":"lpoint","intensity":{"script":"export function update(v) { return v; }","value":2}})");
        QCOMPARE(scripted.degraded, Names{});
        QVERIFY(scripted.supported());
        QCOMPARE(scripted.scripts.size(), size_t(1));
        QCOMPARE(scripted.scripts[0].property, std::string("light.intensity"));
        QCOMPARE(scripted.lights[0].intensity.value, 2.0f);
        QCOMPARE(loadWith(R"({"id":2,"light":"lpoint","castshadow":{"script":"export function update(v) { return v; }","value":false}})").degraded,
                 Names{"light script"});
        // keyframes on the other values play too
        const Scene radius = loadWith(R"({"id":2,"light":"lpoint","radius":{"animation":{"c0":[{"frame":0,"value":1}]},"value":5}})");
        QCOMPARE(radius.degraded, Names{});
        QCOMPARE(radius.animations.size(), size_t(1));
        QCOMPARE(radius.animations[0].target, PropertyAnimation::Target::LightRadius);
        // scripts on a light's transform are the node's, like any other object's
        const Scene moved = loadWith(R"({"id":2,"name":"sun","light":"lpoint","origin":{"script":"export function update(v) { return v; }","value":"1 2 3"}})");
        QVERIFY(moved.supported());
        QCOMPARE(moved.scripts.size(), size_t(1));
        QCOMPARE(moved.scripts[0].property, std::string("origin"));

        // what is not drawn yet
        // shadows are drawn for point, spot and directional lights (formats.md, "Shadows"), as many per type as the atlas holds
        QCOMPARE(loadWith(R"({"id":2,"light":"lpoint","castshadow":true})").degraded, Names{});
        QCOMPARE(loadWith(R"({"id":2,"light":"ltube","controlpoint":"1 0 0","castshadow":true})").degraded, Names{"tube light shadows"});
        QCOMPARE(loadWith(R"({"id":2,"light":"point","castshadow":true})").degraded, Names{"older point light shadows"});
        QCOMPARE(loadWith(R"({"id":2,"light":"ldirectional","castshadow":true},{"id":3,"light":"ldirectional","castshadow":true},)"
                          R"({"id":4,"light":"ldirectional","castshadow":true})").degraded,
                 Names{"too many shadowed lights"});
        QCOMPARE(loadWith(imageObject(4, "bg", R"(,"castshadow":true)")).degraded, Names{"shadows from 2D layers"}); // only meshes cast
        QCOMPARE(loadWith(imageObject(4, "bg", R"(,"castshadow":false)")).degraded, Names{});
        // the beam of a point or spot light is drawn; the other kinds' is not
        QCOMPARE(loadWith(R"({"id":2,"light":"lspot","castvolumetrics":true})").degraded, Names{});
        QCOMPARE(loadWith(R"({"id":2,"light":"lpoint","castvolumetrics":true})").degraded, Names{});
        QCOMPARE(loadWith(R"({"id":2,"light":"ltube","controlpoint":"1 0 0","castvolumetrics":true})").degraded, Names{"tube light volumetrics"});
        QCOMPARE(loadWith(R"({"id":2,"light":"ldirectional","castvolumetrics":true})").degraded, Names{"directional light volumetrics"});
        QCOMPARE(loadWith(R"({"id":2,"light":"lspot","usecookie":true,"cookie":"cookie/flashlight1"})").degraded, Names{"light cookie"});
        QCOMPARE(loadWith(R"({"id":2,"light":"lpoint","controlpoint":"1 2 3"})").degraded, Names{"light control point"});
        QCOMPARE(loadWith(R"({"id":2,"light":"ltube","controlpoint":"1 2 3"})").degraded, Names{});
        QByteArray many;
        for (size_t i = 0; i <= kMaxLightsPerType; ++i) {
            many += (i ? "," : "") + QByteArray(R"({"id":)") + QByteArray::number(int(10 + i)) + R"(,"light":"lspot"})";
        }
        const Scene crowded = loadWith(many);
        QCOMPARE(crowded.degraded, Names{"too many lights"});
        QCOMPARE(crowded.lights.size(), kMaxLightsPerType + 1);
        QCOMPARE(loadWith(R"({"id":2,"light":"point"},{"id":3,"light":"point"},{"id":4,"light":"point"},{"id":5,"light":"point"},{"id":6,"light":"point"})").degraded,
                 Names{"too many lights"});
        // a type nobody knows turns the scene away
        QVERIFY(hasReason(loadWith(R"({"id":2,"light":"larea"})"), "light type 'larea'"));

        // every value may be bound to a user property; the tube's end may not (named, the file's end stays)
        const Scene bound = loadWith(R"({"id":2,"light":"lspot","exponent":{"user":"e","value":3},"innercone":{"user":"in","value":10},)"
                                     R"("outercone":{"user":"out","value":40},"density":{"user":"d","value":1}})");
        QCOMPARE(bound.degraded, Names{});
        QCOMPARE(bound.lights[0].exponent.user, std::string("e"));
        QCOMPARE(bound.lights[0].exponent.value, 3.0f);
        QCOMPARE(bound.lights[0].innerCone.user, std::string("in"));
        QCOMPARE(bound.lights[0].outerCone.user, std::string("out"));
        QCOMPARE(bound.lights[0].density.user, std::string("d"));
        QCOMPARE(loadWith(R"({"id":2,"light":"ltube","controlpoint":{"user":"end","value":"1 2 3"}})").degraded, Names{"light binding"});
        // a tube without an end is a point at its origin: named
        QCOMPARE(loadWith(R"({"id":2,"light":"ltube"})").degraded, Names{"tube without end"});
        // the node's scale is not applied to the radius: named whatever the type
        QCOMPARE(loadWith(R"({"id":2,"light":"lpoint","scale":"2.7 1.5 1.5"})").degraded, Names{"light scale"});
        QCOMPARE(loadWith(R"({"id":2,"light":"ltube","controlpoint":"1 0 0","scale":{"user":"s","value":"1 1 1"}})").degraded, Names{"light scale"});
        QCOMPARE(loadWith(R"({"id":2,"light":"lpoint","scale":"1 1 1"})").degraded, Names{});
        // keyframes and a user binding on the intensity: the keyframes own it, the binding is dropped
        const Scene both = loadWith(R"({"id":2,"light":"lpoint","intensity":{"user":"power","animation":{"c0":[{"frame":0,"value":0},{"frame":30,"value":2}],)"
                                    R"("options":{"fps":30,"length":30,"mode":"loop"}},"value":1}})");
        QCOMPARE(both.animations.size(), size_t(1));
        QVERIFY(both.lights[0].intensity.user.empty());
        QCOMPARE(both.degraded, Names{});
        // a binding alone is kept
        QCOMPARE(loadWith(R"({"id":2,"light":"lpoint","intensity":{"user":"power","value":1}})").lights[0].intensity.user, std::string("power"));
    }

    void aMeshObjectIsLoadedWithTheMaterialsItsModelNames()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.tex(u"models/box/skin"_s, simpleRgba(4, 4));
        meshgen::Model m;
        m.parts.push_back(meshgen::box({-1, -1, -1}, {1, 1, 1}, "materials/models/box/outer.json"));
        m.parts.push_back(meshgen::box({-2, -2, -2}, {-1, -1, -1}, "materials/models/box/inner.json"));
        const auto mdl = meshgen::write(m);
        item.file(u"models/box/box.mdl"_s, QByteArray(reinterpret_cast<const char *>(mdl.data()), static_cast<qsizetype>(mdl.size())));
        item.file(u"materials/models/box/outer.json"_s,
                  R"({"passes":[{"shader":"generic4","blending":"translucent","cullmode":"nocull","depthtest":"enabled","depthwrite":"disabled",)"
                  R"("combos":{"LIGHTING":1,"REFLECTION":0},"constantshadervalues":{"alpha":0.5},"textures":["models/box/skin"]}]})");
        item.file(u"materials/models/box/inner.json"_s, R"({"passes":[{"shader":"generic4","blending":"normal","textures":["models/box/skin"]}]})");
        const QByteArray mesh = R"({"id":2,"name":"box","model":"models/box/box.mdl","origin":"100 200 30","angles":"0.5 0 0","scale":"10 10 10",)"
                                R"("parent":1,"solid":true,"castshadow":false,"disablepropagation":false})";
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + "," + mesh + "," + imageObject(3, "bg")));
        const Scene scene = load(item);
        QVERIFY2(scene.supported(), scene.unsupported.empty() ? "" : scene.unsupported[0].c_str());
        QCOMPARE(scene.meshes.size(), size_t{1});
        const MeshLayer &box = scene.meshes[0];
        QCOMPARE(box.path, std::string("models/box/box.mdl"));
        QCOMPARE(box.model->parts.size(), size_t{2});
        QCOMPARE(box.materials.size(), size_t{2});
        QCOMPARE(box.materials[0].shader, std::string("generic4"));
        QCOMPARE(box.materials[0].combos.at("LIGHTING"), 1);
        QCOMPARE(box.materials[0].cull, CullMode::None);
        QVERIFY(box.materials[0].depthTest);
        QVERIFY(!box.materials[0].depthWrite);
        QCOMPARE(box.materials[0].blending, Blending::Translucent);
        QCOMPARE(box.materials[1].cull, CullMode::Back); // the defaults: culled, depth tested and written
        QVERIFY(box.materials[1].depthTest && box.materials[1].depthWrite);
        QCOMPARE(box.materials[1].blending, Blending::Normal);
        QVERIFY(box.solid);
        QVERIFY(!box.castShadow); // the file says so; without the key a mesh casts (formats.md, "Shadows")
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + "," + QByteArray(mesh).replace(",\"castshadow\":false", "") + "," + imageObject(3, "bg")));
        QVERIFY(load(item).meshes.at(0).castShadow);
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + "," + mesh + "," + imageObject(3, "bg")));
        // the node: origin, angles, scale and parent like any object's
        const SceneNode &node = scene.nodes[box.node];
        QCOMPARE(node.origin.z, 30.0f);
        QCOMPARE(node.angles.x, 0.5f);
        QCOMPARE(node.scale.y, 10.0f);
        QCOMPARE(node.parentId, 1);
        // drawn in file order, between the two image layers
        QCOMPARE(scene.order.size(), size_t{3});
        QVERIFY(scene.order[0].kind == DrawEntry::Kind::Image && scene.order[1].kind == DrawEntry::Kind::Mesh
                && scene.order[2].kind == DrawEntry::Kind::Image);
        QCOMPARE(scene.order[1].index, size_t{0});
        QVERIFY(scene.degraded.empty());

        // morph targets are drawn at rest, and named
        meshgen::Model morph = m;
        const char tag[] = "MDMP0001";
        morph.trailer.assign(tag, tag + 9);
        morph.trailer.resize(40, 0);
        const auto morphBytes = meshgen::write(morph);
        item.file(u"models/box/box.mdl"_s, QByteArray(reinterpret_cast<const char *>(morphBytes.data()), static_cast<qsizetype>(morphBytes.size())));
        Scene degraded = load(item);
        QVERIFY(degraded.supported());
        QVERIFY(std::find(degraded.degraded.begin(), degraded.degraded.end(), std::string("morph targets")) != degraded.degraded.end());

        // a skinned mesh plays its animation layers: its materials are drawn with SKINNING and the skeleton's BONECOUNT
        meshgen::Model skinned = m;
        skinned.skinned = true;
        for (meshgen::Vertex &v : skinned.parts[1].vertices) {
            v.bones = {1, 0, 0, 0};
        }
        skinned.bones = {{"root", -1, {}}, {"arm", 0, {}}};
        mdlgen::Animation wave;
        wave.id = 4;
        wave.length = 1;
        wave.tracks = {{mdlgen::Pose{}, mdlgen::Pose{}}, {mdlgen::Pose{}, mdlgen::Pose{}}};
        skinned.animations = {wave};
        const auto skinnedBytes = meshgen::write(skinned);
        item.file(u"models/box/box.mdl"_s, QByteArray(reinterpret_cast<const char *>(skinnedBytes.data()), static_cast<qsizetype>(skinnedBytes.size())));
        const QByteArray playing = QByteArray(mesh).replace("\"solid\":true", "\"solid\":true,\"animationlayers\":[{\"animation\":4,\"rate\":{\"user\":\"speed\",\"value\":0.5}}]");
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + "," + playing));
        Scene rigged = load(item);
        QVERIFY2(rigged.supported(), rigged.unsupported.empty() ? "" : rigged.unsupported[0].c_str());
        QVERIFY(rigged.meshes[0].model->skinned);
        QVERIFY(rigged.meshes[0].skinning);
        QCOMPARE(rigged.meshes[0].animationLayers.size(), size_t{1});
        QCOMPARE(rigged.meshes[0].animationLayers[0].animation, 4u);
        QCOMPARE(rigged.meshes[0].animationLayers[0].rate.user, std::string("speed"));
        QCOMPARE(rigged.meshes[0].materials[1].combos.at("SKINNING"), 1);
        QCOMPARE(rigged.meshes[0].materials[1].combos.at("BONECOUNT"), 2);
        // without animation layers it stands as authored: no skinning
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + "," + mesh));
        rigged = load(item);
        QVERIFY(rigged.supported() && !rigged.meshes[0].skinning);
        QVERIFY(!rigged.meshes[0].materials[0].combos.contains("SKINNING"));

        // a part textured by another object's composite is read (the renderer draws that layer for it); another render target is not
        item.file(u"models/box/box.mdl"_s, QByteArray(reinterpret_cast<const char *>(mdl.data()), static_cast<qsizetype>(mdl.size())));
        item.file(u"materials/models/box/inner.json"_s, R"({"passes":[{"shader":"generic4","textures":["_rt_imageLayerComposite_3_a"]}]})");
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + "," + mesh + "," + imageObject(3, "bg")));
        QVERIFY(load(item).supported());
        item.file(u"materials/models/box/inner.json"_s, R"({"passes":[{"shader":"generic4","textures":["_rt_HalfCompoBuffer1"]}]})");
        QVERIFY(hasReason(load(item), "render target '_rt_HalfCompoBuffer1' on a 3D mesh"));
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + "," + mesh));

        // a material the model names that is missing, a model that is missing or not a mesh
        item.file(u"materials/models/box/inner.json"_s, "not json");
        QVERIFY(hasReason(load(item), "material of 3D model"));
        item.file(u"scene.json"_s, sceneJson(R"({"id":2,"model":"models/none.mdl"})"));
        QVERIFY(hasReason(load(item), "3D model 'models/none.mdl' not found"));
        item.file(u"models/bad.mdl"_s, "MDLV0023\0 not a mesh");
        item.file(u"scene.json"_s, sceneJson(R"({"id":2,"model":"models/bad.mdl"})"));
        QVERIFY(hasReason(load(item), "3D model:"));
        // `transparentsorting` is read; a `customsortorder` is named
        item.file(u"models/box/box.mdl"_s, QByteArray(reinterpret_cast<const char *>(mdl.data()), static_cast<qsizetype>(mdl.size())));
        item.file(u"materials/models/box/inner.json"_s, R"({"passes":[{"shader":"generic4","textures":["models/box/skin"]}]})");
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + "," + mesh, ",\"transparentsorting\":true"));
        Scene sorting = load(item);
        QVERIFY(sorting.transparentSorting);
        QVERIFY(sorting.degraded.empty());
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + "," + mesh, ",\"customsortorder\":[2,1]"));
        sorting = load(item);
        QVERIFY(!sorting.transparentSorting);
        QVERIFY(std::find(sorting.degraded.begin(), sorting.degraded.end(), std::string("custom sort order")) != sorting.degraded.end());
        // a light beside it is loaded too (phase 2: lights)
        item.file(u"materials/models/box/inner.json"_s, R"({"passes":[{"shader":"generic4","textures":["models/box/skin"]}]})");
        item.file(u"scene.json"_s, sceneJson(mesh + R"(,{"id":5,"light":"lpoint"})"));
        const Scene lit = load(item);
        QVERIFY(!hasReason(lit, "light"));
        QCOMPARE(lit.lights.size(), size_t(1));
    }

    void anAutoProjectionTakesTheSizeOfThePictureThatStartsAtTheOrigin()
    {
        Item item;
        item.image(u"back"_s);
        item.image(u"main"_s);
        item.tex(u"back"_s, simpleRgba(4, 4));
        item.tex(u"main"_s, simpleRgba(4, 4));
        const auto sceneWith = [&](const QByteArray &ortho) {
            return "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},\"general\":{\"orthogonalprojection\":" + ortho
                + "},\"objects\":[{\"id\":1,\"name\":\"back\",\"image\":\"models/back.json\",\"origin\":\"750 500 0\",\"scale\":\"1.05 1.05 1\",\"size\":\"1500 1000\"},"
                  "{\"id\":2,\"name\":\"main\",\"image\":\"models/main.json\",\"origin\":\"750 500 0\",\"size\":\"1500 1000\"}]}";
        };
        // the picture whose corner is on (0, 0) is the scene (the 1.05 scaled one sticks out past it)
        item.file(u"scene.json"_s, sceneWith(R"({"auto":true,"height":null,"width":null})"));
        Scene scene = load(item);
        QVERIFY(scene.supported());
        QCOMPARE(scene.orthoSize.x, 1500.0f);
        QCOMPARE(scene.orthoSize.y, 1000.0f);
        // a size that is given wins, auto or not
        item.file(u"scene.json"_s, sceneWith(R"({"auto":true,"height":720,"width":1280})"));
        QCOMPARE(load(item).orthoSize.x, 1280.0f);
        // nothing at the origin: the largest picture
        item.file(u"scene.json"_s, QByteArray(sceneWith(R"({"auto":true})")).replace("750 500 0", "900 600 0"));
        QCOMPARE(load(item).orthoSize.x, 1575.0f);
    }

    void anAutoProjectionSkipsHiddenLayersFollowsParentsAndPrefersTheLargest()
    {
        Item item;
        item.image(u"a"_s);
        item.image(u"b"_s);
        item.tex(u"a"_s, simpleRgba(4, 4));
        item.tex(u"b"_s, simpleRgba(4, 4));
        const auto sceneOf = [&](const QByteArray &objects) {
            item.file(u"scene.json"_s, "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},\"general\":{\"orthogonalprojection\":{\"auto\":true}},\"objects\":[" + objects + "]}");
            return load(item).orthoSize;
        };
        // a hidden layer that starts at the origin does not count; the visible one that does, not centred, is the scene
        Vec2 size = sceneOf(R"({"id":1,"name":"a","image":"models/a.json","origin":"400 300 0","size":"800 600","visible":false},)"
                            R"({"id":2,"name":"b","image":"models/b.json","origin":"320 180 0","size":"640 360"})");
        QCOMPARE(size.x, 640.0f);
        QCOMPARE(size.y, 360.0f);
        // two layers start at the origin: the larger one
        size = sceneOf(R"({"id":1,"name":"a","image":"models/a.json","origin":"320 180 0","size":"640 360"},)"
                       R"({"id":2,"name":"b","image":"models/b.json","origin":"400 300 0","size":"800 600"})");
        QCOMPARE(size.x, 800.0f);
        // a parent scales and moves its child: 400 x 300 at the child's (0.5, 0.5) of a parent at (400, 300) scaled 2 is 800 x 600
        // starting at (400 - 400, 300 - 300) = the origin
        size = sceneOf(R"({"id":10,"name":"group","origin":"400 300 0","scale":"2 2 1"},)"
                       R"({"id":2,"name":"b","image":"models/b.json","parent":10,"origin":"0 0 0","size":"400 300"})");
        QCOMPARE(size.x, 800.0f);
        QCOMPARE(size.y, 600.0f);
        // a group that is hidden hides its children for this too
        size = sceneOf(R"({"id":10,"name":"group","origin":"400 300 0","scale":"2 2 1","visible":false},)"
                       R"({"id":2,"name":"b","image":"models/b.json","parent":10,"origin":"0 0 0","size":"400 300"},)"
                       R"({"id":3,"name":"a","image":"models/a.json","origin":"100 50 0","size":"200 100"})");
        QCOMPARE(size.x, 200.0f);
    }

    void aMaterialMayBindSeveralTexturesBesideTheFirst()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.tex(u"mask"_s, simpleRgba(4, 4));
        item.file(u"materials/bg.json"_s, R"({"passes":[{"shader":"genericimage2","textures":["bg","mask"]}]})");
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")));
        const Scene scene = load(item);
        QVERIFY2(scene.supported(), "several textures are bound now");
        QCOMPARE(scene.layers[0].base.textures.size(), size_t{2});
        QCOMPARE(scene.layers[0].base.textures[1], std::string("mask"));
        // a missing second texture is a reason, named
        item.file(u"materials/bg.json"_s, R"({"passes":[{"shader":"genericimage2","textures":["bg","nowhere"]}]})");
        QVERIFY(hasReason(load(item), "nowhere"));
    }

    void plainImageLayerIsSupported()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(8, 8));
        item.file(u"scene.json"_s, sceneJson(imageObject(7, "bg", ",\"angles\":\"0 0 1.5\",\"scale\":\"2 3 1\",\"visible\":true,\"alpha\":0.5,\"color\":\"1 0.5 0\"")));
        const Scene scene = load(item);
        QVERIFY2(scene.supported(), scene.unsupported.empty() ? "" : scene.unsupported[0].c_str());
        QCOMPARE(scene.orthoSize.x, 1920.0f);
        QCOMPARE(scene.orthoSize.y, 1080.0f);
        QCOMPARE(scene.clearColor.y, 0.2f);
        QCOMPARE(scene.layers.size(), size_t{1});
        const ImageLayer &l = scene.layers[0];
        QCOMPARE(l.size.x, 400.0f);
        QCOMPARE(l.alpha.value, 0.5f);
        QCOMPARE(l.color.value.y, 0.5f);
        QCOMPARE(l.material.shader, std::string("genericimage2"));
        QCOMPARE(l.material.texturePath, std::string("materials/bg.tex"));
        QCOMPARE(l.material.blending, Blending::Translucent);
        const SceneNode &n = scene.nodes[l.node];
        QCOMPARE(n.id, 7);
        QCOMPARE(n.origin.x, 960.0f);
        QCOMPARE(n.scale.y, 3.0f);
        QCOMPARE(n.angles.z, 1.5f);
    }

    void textLayersCarryTheirEffectsAndBlendMode()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"materials/effects/x.json"_s, "{\"passes\":[{\"shader\":\"effects/x\"}]}");
        item.file(u"effects/x/effect.json"_s, "{\"passes\":[{\"material\":\"materials/effects/x.json\"}]}");
        const QByteArray fx = R"({"file":"effects/x/effect.json","name":"fx","id":9,"passes":[{}]})";
        const QByteArray text = R"({"id":2,"name":"t","text":"hi","pointsize":20,"font":"fonts/x.ttf","origin":"10 10 0","colorBlendMode":9,)"
                                R"("effects":[)" + fx + "," + fx + "]}";
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + "," + text));
        const Scene scene = load(item);
        QVERIFY(scene.supported());
        QVERIFY(scene.degraded.empty());
        QCOMPARE(scene.layers.size(), size_t(2));
        QCOMPARE(scene.layers[1].effects.size(), size_t(2));
        QCOMPARE(scene.layers[1].colorBlendMode, 9);

        // an effect that cannot be loaded refuses the scene, as it does on an image
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + "," + QByteArray(text).replace("effects/x/", "effects/none/")));
        QVERIFY(!load(item).supported());

        // an empty array is not a feature
        const QByteArray bare = R"({"id":2,"name":"t","text":"hi","pointsize":20,"font":"fonts/x.ttf","origin":"10 10 0","effects":[]})";
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + "," + bare));
        QVERIFY(load(item).degraded.empty());
        QVERIFY(load(item).layers[1].effects.empty());
    }

    // The scripts and keyframes on an effect are registered for text and shape layers as they are for images.
    void effectScriptsAndKeyframesWorkOnTextAndShapeLayers()
    {
        Item item;
        item.tex(u"util/white"_s, simpleRgba(4, 4));
        item.file(u"materials/effects/x.json"_s, "{\"passes\":[{\"shader\":\"effects/x\"}]}");
        item.file(u"effects/x/effect.json"_s, "{\"passes\":[{\"material\":\"materials/effects/x.json\"}]}");
        const QByteArray script = R"("script":"export function update(v) { return v; }")";
        const QByteArray fx = R"({"file":"effects/x/effect.json","id":9,"visible":{)" + script + R"(,"value":true},"passes":[{"constantshadervalues":{)"
            R"("Amount":{)" + script + R"(,"value":1},)"
            R"("Fade":{"value":0,"animation":{"c0":[{"frame":0,"value":0},{"frame":30,"value":1}],"options":{"fps":30,"length":30,"mode":"single"}}}}}]})";
        const QByteArray text = R"({"id":2,"name":"t","text":"hi","pointsize":20,"font":"fonts/x.ttf","origin":"10 10 0","effects":[)" + fx + "]}";
        const QByteArray quad = R"({"id":2,"name":"t","shape":"quad","effects":[)" + fx + "]}";
        for (const QByteArray &object : {text, quad}) {
            item.file(u"scene.json"_s, sceneJson(object));
            const Scene scene = load(item);
            QVERIFY2(scene.supported(), qPrintable(QString::fromStdString(scene.unsupported.empty() ? "" : scene.unsupported[0])));
            QCOMPARE(scene.scripts.size(), size_t(2));
            QCOMPARE(scene.scripts[0].property, std::string("fxv:0"));
            QCOMPARE(scene.scripts[1].property, std::string("fx:0:0:amount"));
            QCOMPARE(scene.animations.size(), size_t(1));
            const PropertyAnimation &a = scene.animations[0];
            QVERIFY(a.target == PropertyAnimation::Target::EffectConstant);
            QCOMPARE(a.index, size_t(0));
            QCOMPARE(a.effect, size_t(0));
            QCOMPARE(a.pass, size_t(0));
            QCOMPARE(a.key, std::string("fade"));
        }
        // an effect that cannot be loaded refuses a shape, as it does an image
        item.file(u"scene.json"_s, sceneJson(QByteArray(quad).replace("effects/x/", "effects/none/")));
        QVERIFY(!load(item).supported());
    }

    void shapeLayersAreQuadsCarryingTheirEffects()
    {
        Item item;
        item.tex(u"util/white"_s, simpleRgba(4, 4));
        item.file(u"materials/effects/x.json"_s, "{\"passes\":[{\"shader\":\"effects/x\"}]}");
        item.file(u"effects/x/effect.json"_s, "{\"passes\":[{\"material\":\"materials/effects/x.json\"}]}");
        const QByteArray fx = R"({"file":"effects/x/effect.json","id":9,"passes":[{}]})";
        const QByteArray quad = R"({"id":2,"name":"rays","shape":"quad","origin":"10 20 0","scale":"2 3 1","castshadow":false,"effects":[)" + fx + "," + fx + "]}";
        item.file(u"scene.json"_s, sceneJson(quad));
        const Scene scene = load(item);
        QVERIFY2(scene.supported(), qPrintable(QString::fromStdString(scene.unsupported.empty() ? "" : scene.unsupported[0])));
        QVERIFY(scene.degraded.empty());
        QCOMPARE(scene.layers.size(), size_t(1));
        QCOMPARE(scene.layers[0].effects.size(), size_t(2));
        QCOMPARE(scene.layers[0].size.x, kShapeSize);
        QCOMPARE(scene.nodes[0].scale.x, 2.0f);

        // an own size wins
        item.file(u"scene.json"_s, sceneJson(QByteArray(quad).replace(R"("shape":"quad",)", R"("shape":"quad","size":"300 100",)")));
        QCOMPARE(load(item).layers[0].size.y, 100.0f);

        // any other shape is refused by name
        item.file(u"scene.json"_s, sceneJson(QByteArray(quad).replace(R"("quad")", R"("circle")")));
        const Scene other = load(item);
        QVERIFY(!other.supported());
        QCOMPARE(other.unsupported[0], std::string("shape 'circle'"));
    }

    // --- phase 1 task 7: every remaining feature a scene is drawn without is named, and only when it is actually used ---

    void generalKeysTheEngineDoesNotFollowAreNamedWhenSet()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        const auto degradedWith = [&](const QByteArray &general) {
            item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg"), general));
            const Scene scene = load(item);
            if (!scene.supported()) qFatal("scene not supported");
            return scene.degraded;
        };
        using Names = std::vector<std::string>;
        QCOMPARE(degradedWith({}), Names{});
        // zoom: 1 is no zoom
        QCOMPARE(degradedWith(R"(,"zoom":1.0)"), Names{});
        // (the scene's own zoom is applied: the camera's, SceneCamera::zoom)
        QCOMPARE(degradedWith(R"(,"zoom":1.1)"), Names{});
        QCOMPARE(degradedWith(R"(,"zoom":{"user":"z","value":1.04})"), Names{});
        // the field of view only reads in a perspective camera
        QCOMPARE(degradedWith(R"(,"perspectiveoverridefov":90.0)"), Names{});
        // fog: the distance and height bands are drawn in a perspective scene; under the orthographic camera (this scene) there is no
        // eye distance or height to fog by, and a fog switch the reader does not know is named too
        QCOMPARE(degradedWith(R"(,"fogenabled":false)"), Names{});
        QCOMPARE(degradedWith(R"(,"fogenabled":true)"), Names{"general fog"});
        QCOMPARE(degradedWith(R"(,"fogdistance":false,"fogheight":false,"fogdistancestart":3)"), Names{});
        QCOMPARE(degradedWith(R"(,"fogdistance":true,"fogdistanceend":10)"), Names{"general fog"});
        QCOMPARE(degradedWith(R"(,"fogheight":{"user":"mist","value":false})"), Names{"general fog"});
        // the light configuration is what the light objects say: the shaders are compiled for those
        QCOMPARE(degradedWith(R"(,"lightconfig":{"point":0,"spot":0})"), Names{});
        QCOMPARE(degradedWith(R"(,"lightconfig":{"point":2,"spot":0})"), Names{});
        // HDR: float buffers and the HDR bloom chain are drawn; an HDR display's output is not (the output is 8 bit)
        const QByteArray hdrBloom = R"(,"hdr":true,"bloom":true,"bloomhdriterations":8,"bloomhdrscatter":1.619,"bloomhdrfeather":0.1)";
        QCOMPARE(degradedWith(hdrBloom), Names{});
        QCOMPARE(degradedWith(R"(,"hdr":true,"bloom":{"user":"glow","value":false})"), Names{});
        QCOMPARE(degradedWith(R"(,"hdr":true,"bloom":true,"bloomhdriterations":{"user":"it","value":8})"), Names{});
        QCOMPARE(degradedWith(R"(,"hdr":true)"), Names{});
        QCOMPARE(degradedWith(R"(,"hdr":true,"displayhdr":true)"), Names{"display hdr"});
        QCOMPARE(degradedWith(R"(,"hdr":true,"displayhdr":false)"), Names{});
        QCOMPARE(degradedWith(R"(,"hdr":false,"bloom":true,"bloomhdriterations":3)"), Names{});
    }

    void fogBandsAndHdrBloomSettingsAreRead()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        // a perspective scene (no orthogonal projection) with both fog bands, one colour bound to a property, and the HDR bloom keys
        item.file(u"scene.json"_s,
                  R"({"camera":{"center":"0 0 0","eye":"0 0 10","up":"0 1 0"},"general":{"fov":50,"orthogonalprojection":null,)"
                  R"("fogdistance":true,"fogdistancecolor":{"user":"tint","value":"0.1 0.2 0.3"},"fogdistancestart":1,"fogdistanceend":600,)"
                  R"("fogdistancestartdensity":0,"fogdistanceenddensity":0.92,"fogheight":{"user":"low","value":true},"fogheightcolor":"0 0 0",)"
                  R"("fogheightstart":-14.08,"fogheightend":-49.3,"fogheightstartdensity":0,"fogheightenddensity":1,)"
                  R"("hdr":true,"bloom":true,"bloomhdrstrength":{"user":"glow","value":4},"bloomhdrthreshold":0.46,"bloomhdriterations":6,)"
                  R"("bloomhdrscatter":2,"bloomhdrfeather":0.88},"objects":[)" + imageObject(1, "bg") + "]}");
        const Scene scene = load(item);
        QVERIFY(scene.supported());
        QCOMPARE(scene.degraded, std::vector<std::string>{});
        const FogBand &d = scene.fog.distance;
        QVERIFY(d.enabled.value);
        QCOMPARE(d.color.user, std::string("tint"));
        QCOMPARE(d.color.value.z, 0.3f);
        QCOMPARE(d.start.value, 1.0f);
        QCOMPARE(d.end.value, 600.0f);
        QCOMPARE(d.endDensity.value, 0.92f);
        const FogBand &h = scene.fog.height;
        QCOMPARE(h.enabled.user, std::string("low"));
        QCOMPARE(h.start.value, -14.08f);
        QCOMPARE(h.end.value, -49.3f);
        QVERIFY(scene.hdr);
        QCOMPARE(scene.bloom.strength.user, std::string("glow"));
        QCOMPARE(scene.bloom.strength.value, 4.0f);
        QCOMPARE(scene.bloom.threshold.value, 0.46f);
        QCOMPARE(scene.bloom.iterations.value, 6.0f);
        QCOMPARE(scene.bloom.scatter.value, 2.0f);
        QCOMPARE(scene.bloom.feather.value, 0.88f);
    }

    void cameraFadeIsNamedOnlyWhereACameraPathCouldUseIt()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        const QByteArray path = R"(,{"id":9,"name":"","camera":"default","path":"scripts/camera_paths_9.json","queuemode":"random"})";
        const auto degradedWith = [&](const QByteArray &general, const QByteArray &objects) {
            item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + objects, general));
            return load(item).degraded;
        };
        using Names = std::vector<std::string>;
        QCOMPARE(degradedWith(R"(,"camerafade":true)", {}), Names{}); // the editor writes it true everywhere
        QCOMPARE(degradedWith(R"(,"camerafade":false)", path), Names{"camera paths"}); // the camera object names its own path
        QCOMPARE(degradedWith(R"(,"camerafade":true)", path), (Names{"camera fade", "camera paths"}));
        QCOMPARE(degradedWith(R"(,"camerafade":true)", R"(,{"id":9,"name":"","camera":"default"})"), Names{});
        // an empty path file ({"paths": []}, every orthographic scene of the library that has a camera path): nothing to play,
        // nothing to fade, so neither `camera fade` nor `camera paths` is named (a change of this phase: such scenes were degraded)
        item.file(u"scripts/camera_paths_9.json"_s, R"({"paths": []})"_ba);
        QCOMPARE(degradedWith(R"(,"camerafade":true)", path), Names{});
    }

    void imageObjectKeysTheEngineDoesNotFollowAreNamed()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        const auto degradedWith = [&](const QByteArray &extra) {
            item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", extra)));
            const Scene scene = load(item);
            if (!scene.supported()) qFatal("scene not supported");
            return scene.degraded;
        };
        using Names = std::vector<std::string>;
        QCOMPARE(degradedWith({}), Names{});
        QCOMPARE(degradedWith(R"(,"instanceoverride":{})"), Names{});
        QCOMPARE(degradedWith(R"(,"instanceoverride":{"alpha":0.5})"), Names{"image instanceoverride"});
        QCOMPARE(degradedWith(R"(,"attachment":"")"), Names{});
        QCOMPARE(degradedWith(R"(,"attachment":"hand")"), Names{"attachment 'hand': the object has no parent"});
        QCOMPARE(degradedWith(R"(,"attachments":[])"), Names{});
        QCOMPARE(degradedWith(R"(,"attachments":["a","b"])"), Names{"attachments"});
    }

    void anInstanceOfAnInstancedModelIsNamedOnlyForWhatIsNotFollowed()
    {
        Item item;
        item.file(u"models/solid.json"_s, R"({"instanced":true,"material":"materials/solid.json","solidlayer":true})");
        item.file(u"materials/solid.json"_s, R"({"passes":[{"shader":"flat","blending":"translucent"}]})");
        const auto degradedWith = [&](const QByteArray &instance) {
            item.file(u"scene.json"_s, sceneJson(R"({"id":1,"name":"s","image":"models/solid.json","origin":"960 540 0","size":"400 300","instance":)" + instance + "}"));
            const Scene scene = load(item);
            if (!scene.supported()) qFatal("scene not supported");
            return scene.degraded;
        };
        using Names = std::vector<std::string>;
        // what the editor writes for a solid placeholder
        QCOMPARE(degradedWith(R"({"combos":{"version":2},"id":2,"textures":["util/white"]})"), Names{});
        // a user texture slot (a property, a media thumbnail, a dock shortcut) shows the slot's default while nothing fills it
        QCOMPARE(degradedWith(R"({"combos":{"version":2},"id":2,"textures":["util/white"],"usertextures":["custombackground"]})"), Names{});
        QCOMPARE(degradedWith(R"({"id":2,"textures":["util/white"],"usertextures":[{"name":"$mediaThumbnail","type":"system"}]})"), Names{});
        QCOMPARE(degradedWith(R"({"id":2,"textures":["util/white"],"usertextures":[{"name":"$mediaPreviousThumbnail","type":"system"}]})"), Names{});
        QCOMPARE(degradedWith(R"({"id":2,"textures":["util/white"],"usertextures":[{"name":"s01c","type":"usershortcut"}]})"), Names{});
        QCOMPARE(degradedWith(R"({"id":2,"textures":["util/white"],"usertextures":["texturecustom"]})"), Names{}); // a bare property: its default is what WE shows
        QCOMPARE(degradedWith(R"({"id":2,"textures":["util/white"],"usertextures":[{"name":"x","type":"future"}]})"), Names{"usertextures future"});
        QCOMPARE(degradedWith(R"({"id":2,"usertextures":[null]})"), Names{});
        QCOMPARE(degradedWith(R"({"id":2,"usershadervalues":{"schemecolor":"tint"}})"), Names{}); // a property name: bound
        QCOMPARE(degradedWith(R"({"id":2,"usershadervalues":{"schemecolor":7}})"), Names{"usershadervalues"}); // no property to bind
        // constants and the combos the material loader accepts are applied; a combo it would refuse stays named
        QCOMPARE(degradedWith(R"({"id":2,"constantshadervalues":{"alpha":0.5}})"), Names{});
        QCOMPARE(degradedWith(R"({"id":2,"textures":["bg"]})"), Names{}); // a flat colour has no picture for it to replace
        QCOMPARE(degradedWith(R"({"id":2,"combos":{"blur":1}})"), Names{"instance values"});
        QCOMPARE(degradedWith(R"({"id":2,"combos":{"lighting":1}})"), Names{}); // the scene's lights reach it
        // constants apply on a flat layer too (it has no picture, so only its textures are left alone)
        QCOMPARE(degradedWith(R"({"id":2,"constantshadervalues":{"alpha":0.5,"brightness":2},"textures":["bg"]})"), Names{});
        // a user-bound combo is not applied: named
        QCOMPARE(degradedWith(R"({"id":2,"combos":{"version":{"user":"v","value":2}}})"), Names{"instance values"});
        QCOMPARE(degradedWith(R"({"id":2,"combos":{"spritesheet":{"user":"v","value":1}}})"), Names{"instance values"});
        // `spritesheet` is accepted like `version` (in any case), a zero combo is not a value
        QCOMPARE(degradedWith(R"({"id":2,"combos":{"SPRITESHEET":1,"version":2}})"), Names{});
        QCOMPARE(degradedWith(R"({"id":2,"combos":{"blur":0}})"), Names{});
    }

    void anInstanceReplacesTheTexturesCombosAndConstantsOfTheLayersMaterial()
    {
        Item item;
        item.tex(u"util/white"_s, simpleRgba(4, 4));
        item.tex(u"icon"_s, simpleRgba(4, 4));
        item.tex(u"mask"_s, simpleRgba(4, 4));
        item.file(u"models/solid.json"_s, R"({"instanced":true,"material":"materials/solid.json","solidlayer":true})");
        item.file(u"materials/solid.json"_s, R"({"passes":[{"shader":"genericimage4","blending":"translucent","textures":["util/white"],"combos":{"version":2}}]})");
        const auto loadWith = [&](const QByteArray &instance, const QByteArray &model = "models/solid.json") {
            item.file(u"scene.json"_s, sceneJson(R"({"id":1,"name":"s","image":")" + model + R"(","origin":"960 540 0","size":"400 300","instance":)" + instance + "}"));
            const Scene scene = load(item);
            if (!scene.supported()) qFatal("scene not supported");
            return scene;
        };
        using Names = std::vector<std::string>;
        // textures: a name replaces the slot, an empty one keeps the material's
        Scene scene = loadWith(R"({"combos":{"version":2},"id":2,"textures":["icon","mask"]})");
        QCOMPARE(scene.degraded, Names{});
        QCOMPARE(scene.layers.size(), size_t{1});
        QVERIFY(!scene.layers[0].flatColor);
        QCOMPARE(scene.layers[0].base.textures, (std::vector<std::string>{"icon", "mask"}));
        QCOMPARE(scene.layers[0].material.texturePath, std::string("materials/icon.tex"));
        scene = loadWith(R"({"id":2,"textures":[null,"mask"]})");
        QCOMPARE(scene.layers[0].base.textures, (std::vector<std::string>{"util/white", "mask"}));
        QCOMPARE(scene.layers[0].material.texturePath, std::string("materials/util/white.tex"));
        // a texture the item lacks refuses the scene like a material's would
        item.file(u"scene.json"_s, sceneJson(R"({"id":1,"name":"s","image":"models/solid.json","size":"4 4","instance":{"id":2,"textures":["nothere"]}})"));
        QVERIFY(!load(item).supported());
        // constants override the material's; combos the material loader accepts are set
        scene = loadWith(R"({"id":2,"constantshadervalues":{"alpha":0.5,"color":"1 0 0"},"combos":{"version":1,"blur":0}})");
        QCOMPARE(scene.degraded, Names{});
        QCOMPARE(scene.layers[0].material.alpha.value, 0.5f);
        QCOMPARE(scene.layers[0].material.color.value.x, 1.0f);
        QCOMPARE(scene.layers[0].material.color.value.y, 0.0f);
        QCOMPARE(scene.layers[0].base.constants.at("alpha").value[0], 0.5f);
        QCOMPARE(scene.layers[0].base.combos.at("VERSION"), 1);
        scene = loadWith(R"({"id":2,"constantshadervalues":{"brightness":2.0}})");
        QCOMPARE(scene.layers[0].material.brightness.value, 2.0f);
        QCOMPARE(scene.layers[0].material.alpha.value, 1.0f); // keys the block does not name keep the material's
        // user textures: the property is listed (the renderer says when it is set), a shortcut and media slot are named
        scene = loadWith(R"({"id":2,"textures":["icon"],"usertextures":["mine",{"name":"$mediaPreviousThumbnail","type":"system"}]})");
        QCOMPARE(scene.degraded, Names{});
        QCOMPARE(scene.layers[0].base.media.size(), size_t{1}); // slot 1 shows the cover before the last
        QCOMPARE(scene.layers[0].base.media[0].slot, 1u);
        QVERIFY(scene.layers[0].base.media[0].previous);
        QCOMPARE(scene.userTextureProperties, Names{"mine"});
        QCOMPARE(scene.layers[0].base.textures, (std::vector<std::string>{"icon"}));
        // a texture beyond the slots the engine binds is named, not dropped silently
        {
            QByteArray many = R"({"id":2,"textures":["icon")";
            for (int i = 1; i < 33; ++i) many += i == 32 ? R"(,"mask")" : R"(,null)";
            scene = loadWith(many + "]}");
            QCOMPARE(scene.degraded, Names{"instance texture slot"});
            QCOMPARE(scene.layers[0].base.textures.size(), size_t{1});
        }
        // a combo the engine has no data for is not applied and is named
        scene = loadWith(R"({"id":2,"combos":{"blur":1}})");
        QCOMPARE(scene.degraded, Names{"instance values"});
        QCOMPARE(scene.layers[0].base.combos.count("BLUR"), size_t{0});
        // a model that is not `instanced` does not take an instance block
        item.file(u"models/plain.json"_s, R"({"material":"materials/solid.json"})");
        scene = loadWith(R"({"id":2,"textures":["icon"]})", "models/plain.json");
        QCOMPARE(scene.degraded, Names{"instance values"});
        QCOMPARE(scene.layers[0].base.textures, (std::vector<std::string>{"util/white"}));
    }

    void aFlatMaterialWithAnEmptyFirstTextureIsAFlatColour()
    {
        Item item;
        item.file(u"models/solid.json"_s, R"({"instanced":true,"material":"materials/solid.json"})");
        item.file(u"scene.json"_s, sceneJson(R"({"id":1,"name":"s","image":"models/solid.json","size":"4 4"})"));
        for (const char *textures : {R"(,"textures":[""])", R"(,"textures":[null])", ""}) {
            item.file(u"materials/solid.json"_s, QByteArray(R"({"passes":[{"shader":"flat","blending":"translucent")") + textures + "}]}");
            const Scene scene = load(item);
            QVERIFY(scene.supported());
            QVERIFY(scene.layers[0].flatColor);
        }
        // its instance's constants still apply, its textures do not
        item.file(u"scene.json"_s, sceneJson(R"({"id":1,"name":"s","image":"models/solid.json","size":"4 4","instance":{"id":2,"textures":["x"],"constantshadervalues":{"alpha":0.25}}})"));
        const Scene scene = load(item);
        QVERIFY(scene.layers[0].flatColor);
        QCOMPARE(scene.layers[0].material.alpha.value, 0.25f);
    }

    void materialPassUserBindingsAreNamed()
    {
        Item item;
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"models/bg.json"_s, R"({"autosize":true,"material":"materials/bg.json"})");
        const auto degradedWith = [&](const QByteArray &passExtra) {
            item.file(u"materials/bg.json"_s, R"({"passes":[{"blending":"translucent","shader":"genericimage2","textures":["bg"])" + passExtra + "}]}");
            item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")));
            const Scene scene = load(item);
            if (!scene.supported()) qFatal("scene not supported");
            return scene.degraded;
        };
        using Names = std::vector<std::string>;
        QCOMPARE(degradedWith({}), Names{});
        QCOMPARE(degradedWith(R"(,"usertextures":[null])"), Names{});
        QCOMPARE(degradedWith(R"(,"usertextures":[{"name":"$mediaThumbnail","type":"system"}])"), Names{});
        QCOMPARE(degradedWith(R"(,"usertextures":[{"name":"$mediaPreviousThumbnail","type":"system"}])"), Names{});
        QCOMPARE(degradedWith(R"(,"usertextures":["customimage"])"), Names{});
        QCOMPARE(degradedWith(R"(,"usertextures":[{"name":"r1","type":"usershortcut"}])"), Names{});
        QCOMPARE(degradedWith(R"(,"usertextures":[{"name":"$somethingElse","type":"system"}])"), Names{"usertextures system $somethingElse"});
        QCOMPARE(degradedWith(R"(,"usertextures":[{"name":"x","type":"camera"},{"name":"y","type":"camera"}])"), Names{"usertextures camera"});
        QCOMPARE(degradedWith(R"(,"usertextures":[7])"), Names{"usertextures value"});
        QCOMPARE(degradedWith(R"(,"usershadervalues":{})"), Names{});
        QCOMPARE(degradedWith(R"(,"usershadervalues":{"schemecolor":"tint"})"), Names{});
        QCOMPARE(degradedWith(R"(,"usershadervalues":{"schemecolor":""})"), Names{"usershadervalues"});
    }

    void baseShaderOptionsTheEngineHasNoDataForAreNamed()
    {
        Item item;
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"models/bg.json"_s, R"({"autosize":true,"material":"materials/bg.json"})");
        const auto degradedWith = [&](const QByteArray &combos) {
            item.file(u"materials/bg.json"_s, R"({"passes":[{"blending":"translucent","shader":"genericimage2","textures":["bg"],"combos":)" + combos + "}]}");
            item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")));
            const Scene scene = load(item);
            if (!scene.supported()) qFatal("scene not supported");
            return scene.degraded;
        };
        using Names = std::vector<std::string>;
        QCOMPARE(degradedWith(R"({"lighting":0,"reflection":0,"fog":0})"), Names{});
        QCOMPARE(degradedWith(R"({"lighting":false})"), Names{});
        QCOMPARE(degradedWith(R"({"lighting":1})"), Names{}); // the scene's lights and ambient colour reach it
        QCOMPARE(degradedWith(R"({"REFLECTION":1})"), Names{}); // the shader reads the scene drawn so far, with its mip chain
        QCOMPARE(degradedWith(R"({"fog":1,"reflection":1})"), Names{});
        QCOMPARE(degradedWith(R"({"reflection":{"user":"mirror","value":1}})"), Names{"combo reflection"}); // a user binding is not followed
        QCOMPARE(degradedWith(R"({"fog":true})"), Names{});
        QCOMPARE(degradedWith(R"({"lighting":{"value":1}})"), Names{});
    }

    void alphaToCoverageBlendingIsAcceptedOnABasePass()
    {
        Item item;
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"models/bg.json"_s, R"({"autosize":true,"material":"materials/bg.json"})");
        item.file(u"materials/bg.json"_s, R"({"passes":[{"blending":"alphatocoverage","shader":"genericimage4","textures":["bg"]}]})");
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")));
        const Scene scene = load(item);
        QVERIFY(scene.supported());
        QVERIFY(scene.degraded.empty());
        QCOMPARE(scene.layers[0].material.blending, Blending::Normal);
        // the combo the shipped shaders read to sharpen the edge
        QCOMPARE(scene.layers[0].base.combos.at("ALPHATOCOVERAGE"), 1);
        // a name nobody knows is still refused
        item.file(u"materials/bg.json"_s, R"({"passes":[{"blending":"nonsense","shader":"genericimage4","textures":["bg"]}]})");
        QVERIFY(!load(item).supported());
    }

    void effectFboFitAndUvsAndTheInstancesUserBindingsAreNamed()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"materials/effects/e.json"_s, R"({"passes":[{"shader":"genericimage2","blending":"normal","textures":["bg"]}]})");
        const auto degradedWith = [&](const QByteArray &fbo, const QByteArray &instancePass = "{}") {
            item.file(u"effects/e/effect.json"_s, R"({"passes":[{"material":"materials/effects/e.json","target":"_rt_t"}],"fbos":[)" + fbo + "]}");
            item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", R"(,"effects":[{"file":"effects/e/effect.json","id":5,"passes":[)" + instancePass + "]}]")));
            const Scene scene = load(item);
            if (!scene.supported()) qFatal("scene not supported");
            return scene.degraded;
        };
        using Names = std::vector<std::string>;
        QCOMPARE(degradedWith(R"({"name":"_rt_t","scale":2,"format":"rgba8888"})"), Names{});
        QCOMPARE(degradedWith(R"({"name":"_rt_t","fit":256,"format":"rgba8888"})"), Names{}); // followed: a long side of 256 pixels
        QCOMPARE(degradedWith(R"({"name":"_rt_t","fit":true,"format":"rgba8888"})"), Names{"fbo fit"}); // no size to follow
        QCOMPARE(degradedWith(R"({"name":"_rt_t","fit":false})"), Names{});
        QCOMPARE(degradedWith(R"({"name":"_rt_t","width":8,"height":8,"uvs":"clamp"})"), Names{});
        QCOMPARE(degradedWith(R"({"name":"_rt_t","width":8,"height":8,"uvs":"repeat"})"), Names{}); // followed: wrapping coordinates
        QCOMPARE(degradedWith(R"({"name":"_rt_t","width":8,"height":8,"uvs":"mirror"})"), Names{"fbo uvs"});
        QCOMPARE(degradedWith(R"({"name":"_rt_t","scale":2})", R"({"usertextures":["mine"]})"), Names{});
        QCOMPARE(degradedWith(R"({"name":"_rt_t","scale":2})", R"({"usertextures":[null,{"name":"q","type":"odd"}]})"), Names{"usertextures odd"});
        QCOMPARE(degradedWith(R"({"name":"_rt_t","scale":2})", R"({"usershadervalues":{"a":"b"}})"), Names{});
    }

    void userBindingsInAnEffectsOwnMaterialAndOnTextAndShapeLayersAreNamed()
    {
        Item item;
        item.tex(u"util/white"_s, simpleRgba(4, 4));
        item.file(u"effects/x/effect.json"_s, R"({"passes":[{"material":"materials/effects/x.json"}]})");
        const auto degradedWith = [&](const QByteArray &material, const QByteArray &object) {
            item.file(u"materials/effects/x.json"_s, R"({"passes":[{"shader":"effects/x")" + material + "}]}");
            item.file(u"scene.json"_s, sceneJson(object));
            const Scene scene = load(item);
            if (!scene.supported()) qFatal("scene not supported");
            return scene.degraded;
        };
        using Names = std::vector<std::string>;
        const QByteArray plain = R"({"file":"effects/x/effect.json","id":9,"passes":[{}]})";
        const QByteArray bound = R"({"file":"effects/x/effect.json","id":9,"passes":[{"usertextures":["mine"]}]})";
        const auto text = [](const QByteArray &fx) { return R"({"id":2,"name":"t","text":"hi","pointsize":20,"font":"fonts/x.ttf","effects":[)" + fx + "]}"; };
        const auto quad = [](const QByteArray &fx) { return R"({"id":2,"name":"q","shape":"quad","effects":[)" + fx + "]}"; };
        // inside the effect's own material
        QCOMPARE(degradedWith(R"(,"usershadervalues":{"c":"tint"})", text(plain)), Names{});
        QCOMPARE(degradedWith(R"(,"usertextures":[{"name":"$mediaThumbnail","type":"system"}])", quad(plain)), Names{});
        QCOMPARE(degradedWith(R"(,"usertextures":[{"name":"$x","type":"system"}])", quad(plain)), Names{"usertextures system $x"});
        QCOMPARE(degradedWith({}, text(plain)), Names{});
        QCOMPARE(degradedWith({}, quad(plain)), Names{});
        // in the instance's own pass, on a text and on a shape layer
        QCOMPARE(degradedWith({}, text(bound)), Names{});
        QCOMPARE(degradedWith({}, quad(bound)), Names{});
    }

    void aProjectLayerTakesTheSceneSizeAfterAnAutoProjectionSettled()
    {
        Item item;
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"models/bg.json"_s, R"({"material":"materials/bg.json","autosize":true})");
        item.file(u"materials/bg.json"_s, R"({"passes":[{"blending":"translucent","shader":"genericimage2","textures":["bg"]}]})");
        item.file(u"models/p.json"_s, R"({"material":"materials/bg.json","autosize":true,"projectlayer":true})");
        item.file(u"scene.json"_s,
                  R"({"general":{"clearcolor":"0 0 0","orthogonalprojection":{"auto":true}},"objects":[)"
                  R"({"id":1,"name":"bg","image":"models/bg.json","origin":"750 500 0","size":"1500 1000"},)"
                  R"({"id":2,"name":"p","image":"models/p.json","origin":"750 500 0"}]})");
        const Scene scene = load(item);
        QCOMPARE(scene.orthoSize.x, 1500.0f);
        QCOMPARE(scene.layers.at(1).size.x, 1500.0f);
        QCOMPARE(scene.layers.at(1).size.y, 1000.0f);
    }

    void unknownObjectKindsAreNamedButGroupsShapesAndCamerasAreNot()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        const auto degradedWith = [&](const QByteArray &object) {
            item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + "," + object));
            const Scene scene = load(item);
            if (!scene.supported()) qFatal("scene not supported");
            return scene.degraded;
        };
        using Names = std::vector<std::string>;
        QCOMPARE(degradedWith(R"({"id":2,"name":"g","origin":"0 0 0"})"), Names{});
        QCOMPARE(degradedWith(R"({"id":2,"name":"r","reflected":"models/x.json","image":null})"), Names{"object 'reflected'"});
        QCOMPARE(degradedWith(R"({"id":2,"name":"s","sprite":"models/x.json"})"), Names{"object 'sprite'"});
        QCOMPARE(degradedWith(R"({"id":2,"name":"s","sprite":null})"), Names{});
    }

    void aModelFileDeclaresTheSizeOfALayerThatHasNone()
    {
        Item item;
        item.tex(u"bg"_s, simpleRgba(4, 4));
        const auto sizeWith = [&](const QByteArray &model, const QByteArray &objectSize) {
            item.file(u"models/bg.json"_s, model);
            item.file(u"materials/bg.json"_s, R"({"passes":[{"blending":"translucent","shader":"genericimage2","textures":["bg"]}]})");
            item.file(u"scene.json"_s, sceneJson(R"({"id":1,"name":"bg","image":"models/bg.json","origin":"960 540 0")" + objectSize + "}"));
            const Scene scene = load(item);
            if (!scene.supported()) qFatal("scene not supported");
            if (!scene.degraded.empty()) qFatal("unexpectedly degraded");
            return scene.layers.at(0).size;
        };
        const QByteArray model = R"({"material":"materials/bg.json","width":200,"height":120,"nopadding":true})";
        // the object's own size wins (every library item that has both agrees)
        QCOMPARE(sizeWith(model, R"(,"size":"400 300")").x, 400.0f);
        // without one, the model file's
        const Vec2 declared = sizeWith(model, {});
        QCOMPARE(declared.x, 200.0f);
        QCOMPARE(declared.y, 120.0f);
        // a model that only says autosize leaves the size to the picture (the renderer takes the texture's)
        QCOMPARE(sizeWith(R"({"material":"materials/bg.json","autosize":true})", {}).x, 0.0f);
        // a full composition layer covers the wallpaper
        const Vec2 project = sizeWith(R"({"material":"materials/bg.json","autosize":true,"projectlayer":true,"passthrough":true})", {});
        QCOMPARE(project.x, 1920.0f);
        QCOMPARE(project.y, 1080.0f);
        // a user texture reference is a binding the engine does not follow
        item.file(u"models/bg.json"_s, R"({"material":"materials/bg.json","usertexturereference":"mine"})");
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")));
        QCOMPARE(load(item).degraded, std::vector<std::string>{"usertexturereference"});
        item.file(u"models/bg.json"_s, R"({"material":"materials/bg.json","usertexturereference":false})");
        QVERIFY(load(item).degraded.empty());
    }

    void clearEnabledIsReadAndDefaultsOn()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        const auto sceneWith = [&](const QByteArray &general) {
            item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg"), general));
            return load(item);
        };
        QVERIFY(sceneWith({}).clearEnabled.value);
        QVERIFY(sceneWith(R"(,"clearenabled":true)").clearEnabled.value);
        QVERIFY(!sceneWith(R"(,"clearenabled":false)").clearEnabled.value);
        const Scene bound = sceneWith(R"(,"clearenabled":{"user":"trails","value":false})");
        QCOMPARE(bound.clearEnabled.user, std::string("trails"));
        QVERIFY(!bound.clearEnabled.value);
    }

    void particleDefinitionReasonsReachTheScene()
    {
        Item item;
        item.file(u"materials/presets/ember.json"_s, R"({"passes":[{"blending":"additive","shader":"genericparticle","textures":["particle/halo_2"]}]})");
        item.file(u"particles/x.json"_s, R"({"material":"materials/presets/ember.json","renderer":[{"name":"sprite","flags":1}]})");
        item.file(u"scene.json"_s, sceneJson(R"({"id":2,"name":"p","particle":"particles/x.json","origin":"10 10 0"})"));
        const Scene scene = load(item);
        QVERIFY(scene.supported());
        QCOMPARE(scene.degraded, std::vector<std::string>{"particle renderer flags"});
    }

    void drawOrderIsFileOrderAndBlendingIsRead()
    {
        Item item;
        for (const char *name : {"a", "b"}) {
            item.image(QString::fromLatin1(name));
            item.tex(QString::fromLatin1(name), simpleRgba(4, 4));
        }
        item.file(u"materials/b.json"_s,
                  "{\"passes\":[{\"blending\":\"additive\",\"shader\":\"genericimage2\",\"textures\":[\"b\"],"
                  "\"constantshadervalues\":{\"alpha\":0.25,\"brightness\":2,\"color\":\"0.5 0.5 1\"}}]}");
        item.file(u"scene.json"_s, sceneJson(imageObject(2, "b") + "," + imageObject(1, "a")));
        const Scene scene = load(item);
        QVERIFY(scene.supported());
        QCOMPARE(scene.layers.size(), size_t{2});
        QCOMPARE(scene.nodes[scene.layers[0].node].id, 2);
        QCOMPARE(scene.layers[0].material.blending, Blending::Additive);
        QCOMPARE(scene.layers[0].material.alpha.value, 0.25f);
        QCOMPARE(scene.layers[0].material.brightness.value, 2.0f);
        QCOMPARE(scene.layers[0].material.color.value.z, 1.0f);
    }

    void userPropertiesBindAndFallBack()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"scene.json"_s,
                  sceneJson(imageObject(1, "bg", ",\"visible\":{\"user\":\"show\",\"value\":false},\"alpha\":{\"user\":\"opacity\",\"value\":0.5},"
                                                 "\"color\":{\"user\":\"tint\",\"value\":\"1 1 1\"}")));
        const Scene scene = load(item);
        QVERIFY(scene.supported());
        PropertyStore props;
        const ImageLayer &l = scene.layers[0];
        QVERIFY(!nodeVisible(scene, l.node, props)); // the object's own value
        QCOMPARE(resolve(l.alpha, props), 0.5f);
        props.setFromText("show", "true");
        props.set("opacity", 0.8);
        QVERIFY(props.setFromJson("{\"tint\":[1.0,0.0,0.5],\"other\":null}"));
        QVERIFY(nodeVisible(scene, l.node, props));
        QCOMPARE(resolve(l.alpha, props), 0.8f);
        QCOMPARE(resolve(l.color, props).z, 0.5f);
        QVERIFY(props.revision() > 0);
    }

    void userPropertiesDriveOriginScaleAnglesSizeAndParallaxDepth()
    {
        Item item;
        item.image(u"leaf"_s);
        item.tex(u"leaf"_s, simpleRgba(4, 4));
        item.file(u"scene.json"_s,
                  sceneJson(R"({"id":1,"name":"leaf","image":"models/leaf.json","origin":{"user":"o","value":"10 20 0"},)"
                            R"("scale":{"user":"s","value":"1 1 1"},"angles":{"user":"a","value":"0 0 0.5"},)"
                            R"("size":{"user":"z","value":"4 4"},"parallaxDepth":{"user":"p","value":"0.5 0.25"}})"));
        Scene scene = load(item);
        QVERIFY2(scene.supported(), scene.unsupported.empty() ? "" : scene.unsupported[0].c_str());
        QCOMPARE(scene.nodes[0].scaleUser, std::string("s"));
        QCOMPARE(scene.nodes[0].originUser, std::string("o"));
        QCOMPARE(scene.nodes[0].anglesUser, std::string("a"));
        QCOMPARE(scene.layers[0].sizeUser, std::string("z"));
        QCOMPARE(scene.layers[0].parallaxDepthUser, std::string("p"));
        const size_t node = scene.layers[0].node;
        PropertyStore props;
        // never set: the file's values
        applyUserVectors(scene, props);
        QCOMPARE(scene.nodes[node].scale.x, 1.0f);
        QCOMPARE(scene.nodes[node].angles.z, 0.5f);
        QCOMPARE(scene.layers[0].size.x, 4.0f);
        QCOMPARE(scene.layers[0].parallaxDepth.y, 0.25f);
        // a string sets the components
        props.set("s", std::string("2 2 1"));
        props.set("o", std::string("100 200 3"));
        props.set("a", std::string("0.1 0.2 0.3"));
        props.set("z", std::string("40 30"));
        props.set("p", std::string("1 2"));
        QVERIFY(applyUserVectors(scene, props));
        QCOMPARE(nodeWorldMatrix(scene, node).at(0, 3), 100.0f); // the world matrix follows
        QCOMPARE(scene.nodes[node].scale.x, 2.0f);
        QCOMPARE(scene.nodes[node].scale.z, 1.0f);
        QCOMPARE(scene.nodes[node].origin.x, 100.0f);
        QCOMPARE(scene.nodes[node].origin.z, 3.0f);
        QCOMPARE(scene.nodes[node].angles.y, 0.2f);
        QCOMPARE(scene.layers[0].size.y, 30.0f);
        QCOMPARE(scene.layers[0].parallaxDepth.x, 1.0f);
        QVERIFY(!applyUserVectors(scene, props)); // nothing new
        // a number is the value of every component
        props.set("s", 0.5);
        props.set("z", 8.0);
        QVERIFY(applyUserVectors(scene, props));
        QCOMPARE(scene.nodes[node].scale.x, 0.5f);
        QCOMPARE(scene.nodes[node].scale.y, 0.5f);
        QCOMPARE(scene.nodes[node].scale.z, 0.5f);
        QCOMPARE(scene.layers[0].size.x, 8.0f);
        QCOMPARE(scene.layers[0].size.y, 8.0f);
        // an array works as well, and a short one leaves the other components at the file's value
        QVERIFY(props.setFromJson("{\"s\":[3,4,5],\"z\":[9]}"));
        applyUserVectors(scene, props);
        QCOMPARE(scene.nodes[node].scale.z, 5.0f);
        QCOMPARE(scene.layers[0].size.y, 9.0f);
        props.set("p", std::string("7"));
        applyUserVectors(scene, props);
        QCOMPARE(scene.layers[0].parallaxDepth.x, 7.0f);
        QCOMPARE(scene.layers[0].parallaxDepth.y, 7.0f);
        // text that is not numbers, or a boolean, keeps the file's value
        props.set("s", std::string("big"));
        props.set("a", true);
        applyUserVectors(scene, props);
        QCOMPARE(scene.nodes[node].scale.x, 1.0f);
        QCOMPARE(scene.nodes[node].angles.z, 0.5f);
    }

    void vectorPropertiesReadNumbersAndIgnoreTheRest()
    {
        PropertyStore props;
        props.set("a", std::string("1 2 3 4"));
        props.set("b", std::string("nan 1"));
        props.set("c", std::string(""));
        props.set("d", std::vector<double>{5});
        props.set("e", true);
        props.set("f", 2.5);
        props.set("g", std::string("1 x"));
        QCOMPARE(props.getVector("a")->size(), size_t{4}); // the extra component is the caller's to ignore
        QVERIFY(!props.getVector("b"));
        QVERIFY(!props.getVector("c"));
        QCOMPARE(props.getVector("d")->size(), size_t{1}); // one number: every component
        QVERIFY(!props.getVector("e"));
        QCOMPARE((*props.getVector("f"))[0], 2.5);
        QVERIFY(!props.getVector("g"));
        QVERIFY(!props.getVector("missing"));
    }

    void relativeKeyframesAreAddedToTheBaseValueAndLinkedTracksShareTheParentsClock()
    {
        Item item;
        item.image(u"leaf"_s);
        item.tex(u"leaf"_s, simpleRgba(4, 4));
        // shaped like a library camera intro: the zoom 3 -> 1, and a relative origin that ends at base + last key = (0, 0)
        item.file(u"scene.json"_s,
                  sceneJson(R"({"id":1,"name":"leaf","image":"models/leaf.json","size":"4 4","alpha":{"value":0.5,"animation":{"relative":true,)"
                            R"("c0":[{"frame":0,"value":0},{"frame":10,"value":0.25}],"options":{"fps":10,"length":10,"mode":"single"}}}},)"
                            R"({"id":2,"camera":"default","zoom":{"value":3,"animation":{"c0":[{"frame":0,"value":3},{"frame":36,"value":1}],)"
                            R"("options":{"fps":12,"length":60,"mode":"single","children":[{"key":"origin"}]}}},)"
                            R"("origin":{"value":"-244.5 1468.5 500","animation":{"relative":true,"c0":[{"frame":0,"value":159},{"frame":36,"value":244.5}],)"
                            R"("c1":[{"frame":0,"value":1.5},{"frame":36,"value":-1468.5}],"c2":[{"frame":0,"value":0},{"frame":36,"value":0}],)"
                            R"("options":{"fps":6,"length":30,"mode":"loop","parent":{"key":"zoom"}}}}})"));
        const Scene scene = load(item);
        QVERIFY2(scene.supported(), scene.unsupported.empty() ? "" : scene.unsupported[0].c_str());
        const PropertyAnimation *alpha = nullptr, *origin = nullptr, *zoom = nullptr;
        for (const PropertyAnimation &a : scene.animations) {
            alpha = a.target == PropertyAnimation::Target::Alpha ? &a : alpha;
            origin = a.target == PropertyAnimation::Target::Origin ? &a : origin;
            zoom = a.target == PropertyAnimation::Target::CameraZoom ? &a : zoom;
        }
        QVERIFY(alpha && origin && zoom);
        QVERIFY(alpha->relative);
        QCOMPARE(sampleAnimation(*alpha, 0, 0.0, 9.0f), 0.5f);  // base + 0
        QCOMPARE(sampleAnimation(*alpha, 0, 5.0, 9.0f), 0.75f); // base + last key, held
        // the child plays on the parent's clock: fps 12, length 60, single (its own 6 / 30 / loop are replaced)
        QCOMPARE(origin->fps, 12.0f);
        QCOMPARE(origin->length, 60.0f);
        QVERIFY(origin->mode == PropertyAnimation::Mode::Single);
        QCOMPARE(sampleAnimation(*origin, 0, 0.0, 9.0f), -244.5f + 159.0f);
        QCOMPARE(sampleAnimation(*origin, 0, 4.0, 9.0f), 0.0f); // at 4 s (frame 48) the clip holds its last key: centred
        QCOMPARE(sampleAnimation(*origin, 1, 4.0, 9.0f), 0.0f);
        QCOMPARE(sampleAnimation(*zoom, 0, 4.0, 9.0f), 1.0f);
        QCOMPARE(sampleAnimation(*zoom, 0, 0.0, 9.0f), 3.0f);
    }

    void keyframesOnABoundVectorOwnTheProperty()
    {
        Item item;
        item.image(u"leaf"_s);
        item.tex(u"leaf"_s, simpleRgba(4, 4));
        item.file(u"scene.json"_s,
                  sceneJson(R"({"id":1,"name":"leaf","image":"models/leaf.json","size":"4 4","scale":{"user":"s","value":"1 1 1",)"
                            R"("animation":{"c0":[{"frame":0,"value":1},{"frame":30,"value":2}],"options":{"fps":30,"length":30,"mode":"single"}}}})"));
        const Scene scene = load(item);
        QVERIFY(scene.nodes[0].scaleUser.empty());
        QVERIFY(scene.userVectors.empty());
        QCOMPARE(scene.animations.size(), size_t{1});
    }

    void cameraObjectsAreReadWithTheirZoomVisibilityAndPaths()
    {
        Item item;
        item.image(u"leaf"_s);
        item.tex(u"leaf"_s, simpleRgba(4, 4));
        item.file(u"scripts/empty_paths.json"_s, R"({"paths": []})");
        item.file(u"scripts/some_paths.json"_s,
                  R"({"paths": [{"center": {"c0": [{"frame": 0, "value": 1}], "c1": [], "c2": []}}]})");
        const auto cameras = [&](const QByteArray &objects) {
            item.file(u"scene.json"_s, sceneJson(imageObject(1, "leaf") + "," + objects));
            return load(item);
        };
        Scene scene = cameras(R"({"id":2,"name":"","camera":"default","fov":50,"zoom":{"user":"lens","value":1.5},"origin":"10 20 500",)"
                              R"("path":"scripts/empty_paths.json","queuemode":"random"},)"
                              R"({"id":3,"camera":"default","zoom":2,"visible":{"user":"second","value":false}})");
        QVERIFY2(scene.supported(), scene.unsupported.empty() ? "" : scene.unsupported[0].c_str());
        QVERIFY(scene.degraded.empty());
        QCOMPARE(scene.cameraObjects.size(), size_t{2});
        QCOMPARE(scene.cameraObjects[0].zoom.user, std::string("lens"));
        QCOMPARE(scene.cameraObjects[0].zoom.value, 1.5f);
        QCOMPARE(scene.cameraObjects[0].queueMode, std::string("random"));
        QCOMPARE(scene.cameraObjects[0].pathCount, size_t{0});
        QCOMPARE(scene.nodes[scene.cameraObjects[0].node].origin.y, 20.0f);
        QCOMPARE(scene.cameraObjects[1].zoom.value, 2.0f);
        PropertyStore props;
        QVERIFY(!nodeVisible(scene, scene.cameraObjects[1].node, props));
        props.set("second", true);
        QVERIFY(nodeVisible(scene, scene.cameraObjects[1].node, props));
        // keyframes on the zoom play (a user property would own it)
        scene = cameras(R"({"id":2,"camera":"default","zoom":{"value":1,"animation":{"c0":[{"frame":0,"value":1},{"frame":30,"value":2}],)"
                        R"("options":{"fps":30,"length":30,"mode":"single"}}}})");
        QCOMPARE(scene.animations.size(), size_t{1});
        QVERIFY(scene.animations[0].target == PropertyAnimation::Target::CameraZoom);
        // a path in the file is named: nothing draws it
        scene = cameras(R"({"id":2,"camera":"default","path":"scripts/some_paths.json"})");
        QCOMPARE(scene.cameraObjects[0].pathCount, size_t{1});
        QVERIFY(std::find(scene.degraded.begin(), scene.degraded.end(), "camera paths") != scene.degraded.end());
        // script properties bound to user properties keep the user's name and the file's value
        scene = cameras(R"({"id":2,"camera":"default","origin":{"value":"0 0 500","script":"export function update(v) { return v; }",)"
                        R"("scriptproperties":{"x":{"user":"lensx","value":0.5},"plain":3}}})");
        QCOMPARE(scene.scripts.size(), size_t{1});
        QCOMPARE(scene.scripts[0].propertyUsers.at("x"), std::string("lensx"));
        QCOMPARE(scene.scripts[0].propertyUsers.size(), size_t{1});
        QCOMPARE(scene.scripts[0].properties.at("x").number, 0.5);
        QCOMPARE(scene.scripts[0].properties.at("plain").number, 3.0);
        // a path file that is not there may hold paths: named as well
        scene = cameras(R"({"id":2,"camera":"default","path":"scripts/missing.json"})");
        QVERIFY(std::find(scene.degraded.begin(), scene.degraded.end(), "camera paths") != scene.degraded.end());
        // a zoom script is named, not refused
        scene = cameras(R"({"id":2,"camera":"default","zoom":{"value":2,"script":"export function update(v) { return v; }"}})");
        QVERIFY(scene.supported());
        QCOMPARE(scene.cameraObjects[0].zoom.value, 2.0f);
        QVERIFY(std::find(scene.degraded.begin(), scene.degraded.end(), "camera zoom script") != scene.degraded.end());
    }

    void groupsComposeTransformsAndVisibility()
    {
        Item item;
        item.image(u"leaf"_s);
        item.tex(u"leaf"_s, simpleRgba(4, 4));
        item.file(u"scene.json"_s,
                  sceneJson(QByteArray("{\"id\":10,\"name\":\"group\",\"origin\":\"100 200 0\",\"scale\":\"2 2 1\",\"visible\":{\"user\":\"g\",\"value\":true}},")
                            + "{\"id\":11,\"name\":\"leaf\",\"image\":\"models/leaf.json\",\"parent\":10,\"origin\":\"10 0 0\",\"size\":\"4 4\"}"));
        const Scene scene = load(item);
        QVERIFY2(scene.supported(), scene.unsupported.empty() ? "" : scene.unsupported[0].c_str());
        QCOMPARE(scene.layers.size(), size_t{1});
        const Mat4 world = nodeWorldMatrix(scene, scene.layers[0].node);
        // leaf origin (10,0) scaled by the group (x2) plus the group origin
        QCOMPARE(world.at(0, 3), 120.0f);
        QCOMPARE(world.at(1, 3), 200.0f);
        QCOMPARE(world.at(0, 0), 2.0f); // the leaf's own scale (1) times the group's
        PropertyStore props;
        QVERIFY(nodeVisible(scene, scene.layers[0].node, props));
        props.set("g", false);
        QVERIFY(!nodeVisible(scene, scene.layers[0].node, props)); // hidden through its parent
    }

    void unsupportedFeaturesAreNamedWithTheirMilestone()
    {
        struct Case {
            const char *object;
            const char *reason;
        };
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        const std::vector<Case> cases = {
            {"{\"id\":2,\"particle\":\"particles/x.json\",\"image\":null}", "particles"},
            {"{\"id\":2,\"model\":\"models/x.json\",\"image\":null}", "3D model"},
        };
        for (const Case &c : cases) {
            item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg") + "," + c.object));
            const Scene scene = load(item);
            QVERIFY2(hasReason(scene, c.reason), c.reason);
            QVERIFY(!scene.supported());
        }
    }

    void bloomSettingsAreRead()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg"), ",\"bloom\":true,\"bloomstrength\":1.5,\"bloomthreshold\":{\"user\":\"th\",\"value\":0.8}"));
        const Scene scene = load(item);
        QVERIFY(scene.supported());
        QVERIFY(scene.bloom.enabled.value);
        QCOMPARE(scene.bloom.strength.value, 1.5f);
        QCOMPARE(scene.bloom.threshold.user, std::string("th"));
        QCOMPARE(scene.bloom.threshold.value, 0.8f);
    }

    void visibilityConditionsAreKeptAndFollowTheProperty()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"materials/effects/x.json"_s, "{\"passes\":[{\"shader\":\"effects/x\"}]}");
        item.file(u"effects/x/effect.json"_s, "{\"passes\":[{\"material\":\"materials/effects/x.json\"}]}");
        item.file(u"scene.json"_s,
                  sceneJson(imageObject(1, "bg", ",\"visible\":{\"user\":{\"name\":\"mode\",\"condition\":\"2\"},\"value\":false}"
                                                 ",\"effects\":[{\"file\":\"effects/x/effect.json\",\"visible\":{\"user\":{\"name\":\"mode\",\"condition\":\"1\"},\"value\":true}}]")));
        const Scene scene = load(item);
        QVERIFY2(scene.supported(), qPrintable(QString::fromStdString(scene.unsupported.empty() ? "" : scene.unsupported[0])));
        const SceneNode &node = scene.nodes[0];
        QCOMPARE(node.visible.user, std::string("mode"));
        QCOMPARE(node.visible.condition, std::string("2"));
        PropertyStore props;
        QVERIFY(!resolve(node.visible, props)); // unset: the file's own value
        props.set("mode", 2.0);
        QVERIFY(resolve(node.visible, props));
        props.set("mode", 1.0);
        QVERIFY(!resolve(node.visible, props));
        const EffectInstance &fx = scene.layers[0].effects[0];
        QVERIFY(resolve(fx.visible, props)); // mode is 1
        props.set("mode", std::string("2"));
        QVERIFY(!resolve(fx.visible, props));
        QVERIFY(resolve(node.visible, props)); // a string choice compares as text too
        props.set("mode", true);
        PropertyStore other;
        other.set("flag", true);
        Bound<bool> b{false, "flag"};
        b.condition = "true";
        QVERIFY(resolve(b, other)); // a checkbox reads as true / false
    }

    void scriptsOnObjectPropertiesAreKept()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"scene.json"_s,
                  sceneJson(imageObject(1, "bg", ",\"scale\":{\"script\":\"export function update(v) { return v; }\",\"scriptproperties\":{\"k\":2,\"flag\":true,\"t\":\"x\"},\"value\":\"1 2 3\"}")
                            + ",{\"id\":2,\"name\":\"clock\",\"text\":{\"script\":\"export function update(v) { return 'x'; }\",\"value\":\"12:00\"},\"font\":\"fonts/f.ttf\","
                              "\"pointsize\":33,\"horizontalalign\":\"left\",\"alignment\":\"topleft\",\"origin\":\"100 200 0\",\"padding\":8}"));
        const Scene scene = load(item);
        QVERIFY2(scene.supported(), qPrintable(QString::fromStdString(scene.unsupported.empty() ? "" : scene.unsupported[0])));
        QCOMPARE(scene.scripts.size(), size_t{2});
        QCOMPARE(scene.scripts[0].property, std::string("scale"));
        QCOMPARE(scene.scripts[0].properties.at("k").number, 2.0);
        QVERIFY(scene.scripts[0].properties.at("flag").boolean);
        QCOMPARE(scene.scripts[0].properties.at("t").text, std::string("x"));
        QCOMPARE(scene.scripts[1].property, std::string("text"));
        QCOMPARE(scene.scripts[1].name, std::string("clock.text"));
        // the text object is an image layer with a text description
        QCOMPARE(scene.layers.size(), size_t{2});
        QVERIFY(scene.layers[1].text.has_value());
        const TextSpec &t = *scene.layers[1].text;
        QCOMPARE(t.value, std::string("12:00"));
        QCOMPARE(t.pointSize, 33.0f);
        QCOMPARE(t.horizontal, std::string("left"));
        QCOMPARE(t.vertical, std::string("top")); // from the legacy `alignment`
        QCOMPARE(scene.layers[1].alignment, std::string("topleft"));
        QCOMPARE(t.padding, 8.0f);
        QCOMPARE(scene.nodes[scene.layers[1].node].origin.y, 200.0f);
    }

    void layersCanBeAddedToALoadedScene()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.image(u"bar"_s);
        item.tex(u"bar"_s, simpleRgba(2, 8));
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")));
        Scene scene = load(item);
        const Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        std::string error;
        const int bar = addImageLayerFromModel(scene, vfs, "models/bar.json", &error);
        QVERIFY2(bar >= 0, error.c_str());
        QCOMPARE(scene.layers.size(), size_t{2});
        QCOMPARE(scene.order.size(), size_t{2});
        QVERIFY(scene.nodes[scene.layers[static_cast<size_t>(bar)].node].id > 1);
        const int text = addTextLayer(scene, vfs, R"({"text":"shadow","pointsize":10,"color":"0 0 0","alpha":0.5})", &error);
        QVERIFY2(text >= 0, error.c_str());
        QCOMPARE(scene.layers[static_cast<size_t>(text)].text->value, std::string("shadow"));
        QCOMPARE(scene.layers[static_cast<size_t>(text)].alpha.value, 0.5f);
        QVERIFY(addImageLayerFromModel(scene, vfs, "models/missing.json", &error) < 0);
        QVERIFY(!error.empty());
        QCOMPARE(scene.nodes.size(), size_t{3} + 1 - 1 + 0); // the failed one left no node behind
        QVERIFY(addTextLayer(scene, vfs, "not json", &error) < 0);
    }

    void effectsScriptsAndSceneFlagsAreRejected()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"materials/effects/x.json"_s, "{\"passes\":[{\"shader\":\"effects/x\"}]}");
        item.file(u"effects/x/effect.json"_s, "{\"passes\":[{\"material\":\"materials/effects/x.json\"}]}");
        item.file(u"e.json"_s, "{\"passes\":[{\"material\":\"materials/effects/x.json\"}]}");
        const auto expect = [&](const QByteArray &extra, const QByteArray &general, const char *reason) {
            item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", extra), general));
            const Scene scene = load(item);
            QVERIFY2(hasReason(scene, reason), reason);
        };
        expect({}, ",\"bloomtint\":{\"script\":\"x\",\"value\":\"1 1 1\"}", "SceneScript"); // a scene property scripts cannot reach
        expect(",\"colorBlendMode\":40", {}, "colour blend mode");
        expect(",\"alpha\":{\"user\":{\"name\":\"p\",\"condition\":\"1\"},\"value\":1}", {}, "property condition"); // only a visibility takes one
        // an HDR scene is accepted and takes its bloom from the bloomhdr* settings (strength and threshold)
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg"), ",\"hdr\":true,\"bloom\":true,\"bloomstrength\":2,\"bloomthreshold\":0.5,\"bloomhdrstrength\":{\"user\":\"hs\",\"value\":3.5},\"bloomhdrthreshold\":0.9"));
        const Scene hdr = load(item);
        QVERIFY(hdr.supported());
        QCOMPARE(hdr.bloom.strength.value, 3.5f);
        QCOMPARE(hdr.bloom.strength.user, std::string("hs"));
        QCOMPARE(hdr.bloom.threshold.value, 0.9f);
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg"), ",\"hdr\":true,\"bloomstrength\":2"));
        QCOMPARE(load(item).bloom.strength.value, 2.0f); // no HDR settings: the plain ones stay

        // effects are loaded now, whatever their visibility: whether their shaders compile is the analysis' business
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", ",\"effects\":[{\"file\":\"effects/x/effect.json\",\"visible\":true}]")));
        QVERIFY(load(item).supported());
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", ",\"effects\":[{\"file\":\"e.json\",\"visible\":false}]")));
        QVERIFY(load(item).supported());
    }

    void cameraParallaxIsParsedWithItsDefaults()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", ",\"parallaxDepth\":\"0.5 2\"")));
        Scene off = load(item);
        QVERIFY(!off.parallax.enabled.value);
        QCOMPARE(off.parallax.amount.value, 1.0f);          // defaults when the scene says nothing
        QCOMPARE(off.parallax.mouseInfluence.value, 1.0f);
        QCOMPARE(off.layers[0].parallaxDepth.x, 0.5f);
        QCOMPARE(off.layers[0].parallaxDepth.y, 2.0f);
        item.file(u"scene.json"_s,
                  sceneJson(imageObject(1, "bg"),
                            ",\"cameraparallax\":true,\"cameraparallaxamount\":0.6,\"cameraparallaxdelay\":0.1,\"cameraparallaxmouseinfluence\":{\"user\":\"infl\",\"value\":0.5}"));
        const Scene on = load(item);
        QVERIFY(on.supported());
        QVERIFY(on.parallax.enabled.value);
        QCOMPARE(on.parallax.amount.value, 0.6f);
        QCOMPARE(on.parallax.delay.value, 0.1f);
        QCOMPARE(on.parallax.mouseInfluence.user, std::string("infl"));
        QCOMPARE(on.layers[0].parallaxDepth.x, 1.0f);       // the default depth
    }

    void anyCameraOrientationAndAPerspectiveCameraLoad()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")));
        QVERIFY(load(item).supported());
        QVERIFY(!load(item).perspective);
        QByteArray shifted = sceneJson(imageObject(1, "bg"));
        shifted.replace("\"eye\":\"0 0 0\"", "\"eye\":\"-5.5 -17.5 1\"").replace("\"center\":\"0 0 -1\"", "\"center\":\"-5.5 -17.5 0\"");
        item.file(u"scene.json"_s, shifted);
        Scene scene = load(item);
        QVERIFY(scene.supported()); // a shifted eye is allowed: the orthogonal camera does not use it
        QCOMPARE(scene.camera.eye.x, -5.5f);
        // any orientation: the orthogonal camera turns the scene about its centre
        QByteArray rotated = sceneJson(imageObject(1, "bg"));
        rotated.replace("\"up\":\"0 1 0\"", "\"up\":\"1 0 0\"");
        item.file(u"scene.json"_s, rotated);
        scene = load(item);
        QVERIFY(scene.supported());
        QCOMPARE(scene.camera.up.x, 1.0f);
        // no orthogonal projection: a perspective camera with general's fov, nearz and farz (the fov may be a user property)
        QByteArray perspective = sceneJson(imageObject(1, "bg"));
        perspective.replace("\"orthogonalprojection\":{\"width\":1920,\"height\":1080}",
                            "\"fov\":{\"user\":\"fov\",\"value\":42.5},\"nearz\":0.1,\"farz\":30,\"orthogonalprojection\":null");
        item.file(u"scene.json"_s, perspective);
        scene = load(item);
        QVERIFY2(scene.supported(), scene.unsupported.empty() ? "" : scene.unsupported[0].c_str());
        QVERIFY(scene.perspective);
        QCOMPARE(scene.camera.fov, 42.5f);
        QCOMPARE(scene.camera.fovUser, std::string("fov"));
        QCOMPARE(scene.camera.nearZ, 0.1f);
        QCOMPARE(scene.camera.farZ, 30.0f);
        QCOMPARE(scene.orthoSize.x, kPerspectiveCanvas.x);
        QCOMPARE(scene.orthoSize.y, kPerspectiveCanvas.y);
    }

    void cameraPathsAreReadAndPlayInAPerspectiveScene()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        // two paths: a moving one (eye and center over 60 frames, up, a fov list), and a hidden one with nothing in it
        item.file(u"scripts/camera_paths_9.json"_s, R"({"paths":[
            {"center":{"c0":[{"frame":0,"value":0},{"frame":60,"value":1}],"c1":[{"frame":0,"value":0}],"c2":[{"frame":0,"value":-1},{"frame":60,"value":-2}]},
             "eye":{"c0":[{"frame":0,"value":0},{"frame":60,"value":1}],"c1":[{"frame":0,"value":0}],"c2":[{"frame":0,"value":5},{"frame":60,"value":3}]},
             "up":{"c0":[{"frame":0,"value":0}],"c1":[{"frame":0,"value":1}],"c2":[{"frame":0,"value":0}]},
             "fov":[{"frame":0,"value":50},{"frame":60,"value":30}],"zoom":null,"visible":true,
             "options":{"cameramode":"fly","fps":30.0,"length":60,"mode":"single","wraploop":false}},
            {"center":null,"eye":null,"fov":null,"up":null,"zoom":null,"visible":false,"options":{"fps":30.0,"length":60}}]})"_ba);
        QByteArray json = sceneJson(imageObject(1, "bg") + R"(,{"id":9,"name":"","camera":"default","path":"scripts/camera_paths_9.json","queuemode":"sequential","fov":40})");
        json.replace("\"orthogonalprojection\":{\"width\":1920,\"height\":1080}", "\"fov\":50,\"camerafade\":false");
        item.file(u"scene.json"_s, json);
        const Scene scene = load(item);
        QVERIFY(scene.supported());
        QVERIFY(scene.perspective);
        QVERIFY(std::find(scene.degraded.begin(), scene.degraded.end(), "camera paths") == scene.degraded.end()); // played, not named
        QCOMPARE(scene.cameraObjects.size(), size_t{1});
        const SceneCameraObject &camera = scene.cameraObjects[0];
        QCOMPARE(camera.fov.value, 40.0f);
        QCOMPARE(camera.paths.size(), size_t{2});
        QCOMPARE(camera.paths[0].seconds(), 2.0);
        QVERIFY(!camera.paths[1].visible);
        // a two-key path ends at its second key, and holds it
        for (double t : {2.0, 5.0}) {
            const CameraPathPose end = sampleCameraPath(camera.paths[0], t);
            QVERIFY(end.eye && end.center && end.up && end.fov);
            QCOMPARE(end.eye->x, 1.0f);
            QCOMPARE(end.eye->z, 3.0f);
            QCOMPARE(end.center->z, -2.0f);
            QCOMPARE(end.fov.value(), 30.0f);
            QVERIFY(!end.zoom);
        }
        const CameraPathPose start = sampleCameraPath(camera.paths[0], 0.0);
        QCOMPARE(start.eye->z, 5.0f);
        QCOMPARE(start.up->y, 1.0f);
        // halfway the eased curve is halfway too (the default handles are symmetric)
        QVERIFY(std::fabs(sampleCameraPath(camera.paths[0], 1.0).eye->z - 4.0f) < 1e-3f);
        // the empty path drives nothing
        const CameraPathPose none = sampleCameraPath(camera.paths[1], 1.0);
        QVERIFY(!none.eye && !none.center && !none.up && !none.fov && !none.zoom);
        // a path needs both an eye and a centre curve to move the camera: one without is skipped by the queue
        QVERIFY(camera.paths[0].drivesCamera());
        QVERIFY(!camera.paths[1].drivesCamera());
    }

    void aScriptOnAMaterialsConstantsIsTheLayersProperty()
    {
        Item item;
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.image(u"bg"_s);
        item.file(u"materials/bg.json"_s, R"({"passes":[{"shader":"genericimage2","textures":["bg"],"constantshadervalues":{)"
                                          R"("color":{"script":"export function update(v) { return v; }","value":"1 0 0"},)"
                                          R"("alpha":{"script":"export function update(v) { return v; }","value":0.5}}}]})");
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")));
        const Scene scene = load(item);
        QVERIFY2(scene.supported(), scene.unsupported.empty() ? "" : scene.unsupported[0].c_str());
        QCOMPARE(scene.scripts.size(), size_t{2});
        std::set<std::string> properties{scene.scripts[0].property, scene.scripts[1].property};
        QVERIFY(properties.count("material.color") && properties.count("material.alpha"));
        QCOMPARE(scene.layers[0].material.alpha.value, 0.5f);
    }

    void materialProblemsAreReasons()
    {
        Item item;
        item.tex(u"bg"_s, simpleRgba(4, 4));
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")));

        // missing model
        QVERIFY(hasReason(load(item), "model 'models/bg.json' not found"));
        // any shader name is accepted here: the renderability analysis translates it
        item.image(u"bg"_s);
        item.file(u"materials/bg.json"_s, "{\"passes\":[{\"shader\":\"waterripple\",\"blending\":\"translucent\",\"textures\":[\"bg\"]}]}");
        QVERIFY(load(item).supported());
        // unknown blending, extra pass, extra texture, option combos
        item.file(u"materials/bg.json"_s, "{\"passes\":[{\"shader\":\"genericimage2\",\"blending\":\"screen\",\"textures\":[\"bg\"]}]}");
        QVERIFY(hasReason(load(item), "blending 'screen'"));
        item.file(u"materials/bg.json"_s,
                  "{\"passes\":[{\"shader\":\"genericimage2\",\"textures\":[\"bg\"]},{\"shader\":\"genericimage2\",\"textures\":[\"bg\"]}]}");
        QVERIFY(hasReason(load(item), "multi-pass"));
        item.file(u"materials/bg.json"_s, "{\"passes\":[{\"shader\":\"genericimage2\",\"textures\":[\"bg\",\"normal\"]}]}");
        QVERIFY(hasReason(load(item), "texture 'materials/normal.tex' not found")); // a second texture is bound, so it has to exist
        item.file(u"materials/bg.json"_s, "{\"passes\":[{\"shader\":\"genericimage2\",\"textures\":[\"_rt_FullFrameBuffer\",\"bg\"]}]}");
        QVERIFY(load(item).supported()); // a target plus a file: the file is bound beside it (2244339517's clouds)
        item.file(u"materials/bg.json"_s, "{\"passes\":[{\"shader\":\"genericimage2\",\"textures\":[\"_rt_FullFrameBuffer\",\"_rt_Reflection\"]}]}");
        QVERIFY(hasReason(load(item), "render target '_rt_Reflection' as a base texture")); // two targets stay out
        item.file(u"materials/bg.json"_s,
                  "{\"passes\":[{\"shader\":\"genericimage2\",\"textures\":[\"bg\"],\"combos\":{\"VERSION\":2,\"SPRITESHEET\":1,\"LIGHTING\":1,\"REFLECTION\":1,\"FOG\":1,\"SOMETHINGELSE\":1,\"BLENDMODE\":2,\"SHADING\":1}}]}");
        const Scene combos = load(item);
        QVERIFY(hasReason(combos, "shader option SOMETHINGELSE")); // the ones the engine does not know are named
        QVERIFY(!hasReason(combos, "VERSION"));
        QVERIFY(!hasReason(combos, "SPRITESHEET"));
        QVERIFY(!hasReason(combos, "LIGHTING")); // translated with the combo and the scene's lights
        QVERIFY(!hasReason(combos, "REFLECTION"));
        QVERIFY(!hasReason(combos, "FOG"));
        QVERIFY(!hasReason(combos, "BLENDMODE")); // a shader's own options (2244339517's fullcloudsbg): the translator takes them
        QVERIFY(!hasReason(combos, "SHADING"));
        // puppets and missing textures: a model that is not in the item leaves the plain picture (named); one that is there and is not a
        // model is a reason
        item.image(u"bg"_s, {}, ",\"puppet\":\"models/x.mdl\"");
        const Scene missing = load(item);
        QVERIFY(!hasReason(missing, "puppet"));
        QVERIFY(std::find(missing.degraded.begin(), missing.degraded.end(), "puppet model missing") != missing.degraded.end());
        item.file(u"models/x.mdl"_s, "not a model");
        QVERIFY(hasReason(load(item), "puppet"));
        QFile::remove(item.dir.filePath(u"materials/bg.tex"_s));
        QVERIFY(hasReason(load(item), "texture 'materials/bg.tex' not found"));
    }

    void textureProblemsAreReasons()
    {
        Item item;
        item.image(u"bg"_s);
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg")));

        TexSpec animated = simpleRgba(8, 4);
        animated.flags = TexFlags::IsGif;
        animated.sheetVersion = 2;
        animated.frames = {{0, 0, 4, 0, 0, 4}, {4, 0, 4, 0, 0, 4}};
        item.tex(u"bg"_s, animated);
        QVERIFY(load(item).supported()); // a sprite sheet loads; whether its shader can place frames is the analysis' business

        animated.frames = {{0, 0, 8, 0, 0, 4}}; // one frame is just a picture
        item.tex(u"bg"_s, animated);
        QVERIFY(load(item).supported());

        TexSpec video = simpleRgba(4, 4);
        video.version = 4;
        video.isVideo = 1;
        video.freeImage = 35;
        video.flags = TexFlags::Video;
        item.tex(u"bg"_s, video);
        QVERIFY(hasReason(load(item), "video texture"));

        TexSpec odd = simpleRgba(4, 4);
        odd.format = 99;
        item.tex(u"bg"_s, odd);
        QVERIFY(hasReason(load(item), "texture format unknown"));

        item.file(u"materials/bg.tex"_s, "not a texture");
        QVERIFY(hasReason(load(item), "texture 'materials/bg.tex'"));
    }

    void absurdValuesAreRefused()
    {
        Item item;
        item.image(u"bg"_s);
        item.tex(u"bg"_s, simpleRgba(4, 4));
        const auto ortho = [](const char *w, const char *h) {
            return QByteArray("{\"general\":{\"orthogonalprojection\":{\"width\":") + w + ",\"height\":" + h + "}},\"objects\":[]}";
        };
        for (const auto &wh : {std::pair{"0", "100"}, std::pair{"100", "-5"}, std::pair{"1e9", "100"}}) {
            item.file(u"scene.json"_s, ortho(wh.first, wh.second));
            QVERIFY(hasReason(load(item), "projection size out of range"));
        }
        // "inf" and "nan" read by strtof must not reach the matrices: a broken component is the default (WE's own
        // editor writes "-nan(ind)" for a broken value)
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", ",\"scale\":\"inf -nan(ind) 2\"")));
        Scene broken = load(item);
        QVERIFY(broken.supported());
        QCOMPARE(broken.nodes[broken.layers[0].node].scale.x, 1.0f);
        QCOMPARE(broken.nodes[broken.layers[0].node].scale.y, 1.0f);
        QCOMPARE(broken.nodes[broken.layers[0].node].scale.z, 2.0f);
        item.file(u"scene.json"_s, sceneJson(imageObject(1, "bg", ",\"alpha\":\"nan\"")));
        QCOMPARE(load(item).layers[0].alpha.value, 1.0f);
        // object count
        QByteArray many;
        for (int i = 0; i < 5001; ++i)
            many += (i ? "," : "") + QByteArray("{\"id\":") + QByteArray::number(i) + "}";
        item.file(u"scene.json"_s, sceneJson(many));
        QVERIFY(hasReason(load(item), "too many objects"));
    }

    void parentLookupUsesIdsAndSurvivesCyclesAndDuplicates()
    {
        Item item;
        item.image(u"leaf"_s);
        item.tex(u"leaf"_s, simpleRgba(4, 4));
        // a and b are each other's parent; two objects share id 5; a child points at a missing parent
        item.file(u"scene.json"_s,
                  sceneJson(QByteArray("{\"id\":1,\"parent\":2,\"origin\":\"1 0 0\"},{\"id\":2,\"parent\":1,\"origin\":\"2 0 0\"},"
                                       "{\"id\":5,\"origin\":\"10 0 0\"},{\"id\":5,\"origin\":\"20 0 0\"},")
                            + "{\"id\":6,\"name\":\"leaf\",\"image\":\"models/leaf.json\",\"parent\":5,\"size\":\"4 4\"},"
                            + "{\"id\":7,\"parent\":99}"));
        const Scene scene = load(item);
        QCOMPARE(scene.nodeIndexById.at(5), size_t{2}); // the first object with an id keeps it
        QCOMPARE(nodeWorldMatrix(scene, scene.layers[0].node).at(0, 3), 10.0f);
        PropertyStore props;
        QVERIFY(nodeVisible(scene, 0, props)); // the cycle ends instead of looping
        (void)nodeWorldMatrix(scene, 1);
        QCOMPARE(nodeWorldMatrix(scene, 5).at(0, 3), 0.0f); // missing parent: just its own transform
    }

    void projectInfoIsReadOnce()
    {
        Item item;
        item.file(u"project.json"_s, "{\"type\":\"Scene\",\"file\":\"x.json\",\"dependency\":\"42\"}");
        Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        const auto project = readProject(vfs);
        QVERIFY(project.has_value());
        QCOMPARE(project->type, std::string("Scene"));
        QCOMPARE(project->file, std::string("x.json"));
        QCOMPARE(project->dependency, std::string("42"));
        item.file(u"project.json"_s, "[]");
        Vfs bad = Vfs::forItem(item.root().toStdString(), {});
        QVERIFY(!readProject(bad).has_value());
    }

    void broadFailuresAreErrors()
    {
        Item item;
        Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        std::string error;
        QVERIFY(!loadScene(vfs, "scene.json", &error).has_value());
        QVERIFY(!error.empty());
        item.file(u"scene.json"_s, "{ not json");
        Vfs vfs2 = Vfs::forItem(item.root().toStdString(), {});
        QVERIFY(!loadScene(vfs2, "scene.json", &error).has_value());
        item.file(u"scene.json"_s, "[1,2]");
        Vfs vfs3 = Vfs::forItem(item.root().toStdString(), {});
        QVERIFY(!loadScene(vfs3, "scene.json", &error).has_value());
    }

    void sceneComesFromAPackageAndProjectNamesTheFile()
    {
        Item item;
        item.file(u"project.json"_s, "{\"type\":\"scene\",\"file\":\"main.json\",\"title\":\"x\"}");
        item.file(u"scene.pkg"_s,
                  toQ(makePkg({{"main.json", std::string(sceneJson(imageObject(1, "bg")).constData())},
                               {"models/bg.json", "{\"material\":\"materials/bg.json\"}"},
                               {"materials/bg.json", "{\"passes\":[{\"shader\":\"genericimage2\",\"textures\":[\"bg\"]}]}"},
                               {"materials/bg.tex", std::string(toQ(makeTex(simpleRgba(4, 4))).constData(), makeTex(simpleRgba(4, 4)).size())}})));
        Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        QCOMPARE(sceneFileName(vfs), std::string("main.json"));
        std::string error;
        const auto scene = loadScene(vfs, sceneFileName(vfs), &error);
        QVERIFY2(scene.has_value(), error.c_str());
        QVERIFY2(scene->supported(), scene->unsupported.empty() ? "" : scene->unsupported[0].c_str());
        QCOMPARE(scene->layers.size(), size_t{1});

        Item bare;
        Vfs none = Vfs::forItem(bare.root().toStdString(), {});
        QCOMPARE(sceneFileName(none), std::string("scene.json"));
    }

    void propertyStoreConversions()
    {
        PropertyStore p;
        p.setFromText("b", "true");
        p.setFromText("n", "2.5");
        p.setFromText("v", "0.5 1 0");
        p.setFromText("s", "hello world");
        QCOMPARE(p.getBool("b").value(), true);
        QCOMPARE(p.getNumber("n").value(), 2.5);
        QCOMPARE(p.getColor("v").value().x, 0.5f);
        QVERIFY(!p.getBool("s").value());
        QVERIFY(!p.getNumber("s").has_value());
        QVERIFY(!p.getBool("missing").has_value());
        // a 0..255 colour is scaled
        p.set("c255", std::vector<double>{255, 128, 0});
        QCOMPARE(p.getColor("c255").value().x, 1.0f);
        QVERIFY(qAbs(p.getColor("c255").value().y - 128.0f / 255.0f) < 1e-6f);
        // unchanged values do not bump the revision
        const auto rev = p.revision();
        p.setFromText("n", "2.5");
        QCOMPARE(p.revision(), rev);
        QVERIFY(!p.setFromJson("[1]"));
        QVERIFY(!p.setFromJson("garbage"));
    }
};

QTEST_GUILESS_MAIN(TstSceneVkScene)
#include "tst_scenevk_scene.moc"
