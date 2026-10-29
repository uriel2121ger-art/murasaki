// murasaki-scene-vk SceneScript: the QuickJS host against a fake scene. Scripts here are written for the tests.

#include "render/scenescripts.h"
#include "script/scripthost.h"

#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QRegularExpression>
#include <QTest>

#include <cmath>
#include <map>

using namespace murasaki::scene;

namespace {

// A scene of a few layers with named properties.
class FakeScene : public ScriptSceneBridge
{
public:
    std::map<std::pair<int, std::string>, ScriptValue> values;
    std::vector<std::pair<int, std::string>> sets;
    std::vector<std::string> created;
    std::vector<std::pair<int, int>> sorted;

    ScriptValue &at(int layer, const char *property) { return values[{layer, property}]; }

    FakeScene()
    {
        values[{0, "origin"}] = ScriptValue::ofVector({10, 20, 0});
        values[{0, "text"}] = ScriptValue::ofText("start");
        values[{0, "visible"}] = ScriptValue::ofBool(true);
        values[{0, "alpha"}] = ScriptValue::ofNumber(0.5);
        values[{0, "pointsize"}] = ScriptValue::ofNumber(32);
    }
    ScriptValue get(int layer, const std::string &p) override
    {
        const auto it = values.find({layer, p});
        return it == values.end() ? ScriptValue{} : it->second;
    }
    void set(int layer, const std::string &p, const ScriptValue &v) override
    {
        values[{layer, p}] = v;
        sets.emplace_back(layer, p);
    }
    int createLayer(const std::string &spec, bool json) override
    {
        created.push_back((json ? "json:" : "model:") + spec);
        return 100 + static_cast<int>(created.size());
    }
    int indexOf(int layer) override { return layer == 0 ? 7 : layer; }
    void sort(int layer, int index) override { sorted.emplace_back(layer, index); }
    int find(const std::string &name) override { return name == "zero" ? 0 : -1; }
    int parentOf(int layer) override { return layer == 0 ? 1 : -1; }
    std::array<double, 16> worldMatrix(int layer) override { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 100.0 + layer, 200.0, 0, 1}; }
    // layer 0 is the box (0,0)-(10,10), layer 1 the box (5,0)-(15,10)
    bool hitTest(int layer, double x, double y, double *localX, double *localY) override
    {
        const double left = layer == 1 ? 5 : 0;
        if (localX) {
            *localX = x - left - 5;
        }
        if (localY) {
            *localY = y - 5;
        }
        return x >= left && x <= left + 10 && y >= 0 && y <= 10;
    }
    std::string resolvePath(const std::string &path, const std::string &id) override { return path == "models/bar.json" ? "models/workshop/" + id + "/bar.json" : path; }
};

ScriptTarget target(const char *source, const char *property = "text", int layer = 0)
{
    ScriptTarget t;
    t.layer = layer;
    t.property = property;
    t.source = source;
    t.name = std::string("test-") + property;
    return t;
}

ScriptFrame frame()
{
    ScriptFrame f;
    f.time = 1.0;
    f.frametime = 0.05;
    return f;
}

} // namespace

class TstSceneVkScript : public QObject
{
    Q_OBJECT

private slots:
    void aPropertyScriptReceivesTheValueAndItsResultReplacesIt()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target(R"(
            let initial;
            export function init(value) { initial = value; }
            export function update(value) { return initial + ':' + value + '!'; }
        )"),
                          ScriptValue::ofText("first"), &error),
                 error.c_str());
        QVERIFY(host.hasUpdates());
        host.update(frame());
        QCOMPARE(scene.at(0, "text").text, std::string("first:start!"));
        host.update(frame());
        QCOMPARE(scene.at(0, "text").text, std::string("first:first:start!!"));
    }

    void vectorsBooleansAndNumbersCrossTheBoundary()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target(R"(
            export function update(value) {
                const o = thisLayer.origin;                 // a Vec3
                thisLayer.origin = new Vec3(o.x + 1, o.y * 2, 5);
                thisLayer.alpha = thisLayer.alpha * 2;
                thisLayer.visible = !thisLayer.visible;
                return value;                               // unchanged text
            }
        )"),
                          ScriptValue{}, &error),
                 error.c_str());
        host.update(frame());
        const ScriptValue origin = scene.at(0, "origin");
        QCOMPARE(origin.count, 3);
        QCOMPARE(origin.vec[0], 11.0);
        QCOMPARE(origin.vec[1], 40.0);
        QCOMPARE(origin.vec[2], 5.0);
        QCOMPARE(scene.at(0, "alpha").number, 1.0);
        QVERIFY(!scene.at(0, "visible").boolean);
    }

    void aVectorValueScriptGetsAndReturnsVectors()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target(R"(
            export function update(value) { return value.multiply(2).add(new Vec3(1, 1, 1)); }
        )", "origin"),
                          ScriptValue{}, &error),
                 error.c_str());
        host.update(frame());
        const ScriptValue o = scene.at(0, "origin");
        QCOMPARE(o.vec[0], 21.0);
        QCOMPARE(o.vec[1], 41.0);
        QCOMPARE(o.vec[2], 1.0);
    }

    void scriptPropertiesTakeTheObjectsValuesOverTheirDefaults()
    {
        const char *source = R"(
            export var scriptProperties = createScriptProperties()
                .addCheckbox({ name: 'seconds', label: 'x', value: false })
                .addText({ name: 'delimiter', label: 'x', value: ':' })
                .addSlider({ name: 'size', label: 'x', value: 3, min: 0, max: 10 })
                .addColor({ name: 'tint', label: 'x', value: '1 0.5 0' })
                .finish();
            export function update(value) {
                const t = scriptProperties.tint;
                return String(scriptProperties.seconds) + scriptProperties.delimiter + scriptProperties.size + scriptProperties.delimiter + t.y;
            }
        )";
        FakeScene scene;
        ScriptHost defaults(scene);
        std::string error;
        QVERIFY2(defaults.add(target(source), ScriptValue{}, &error), error.c_str());
        defaults.update(frame());
        QCOMPARE(scene.at(0, "text").text, std::string("false:3:0.5"));

        ScriptTarget t = target(source);
        t.properties["seconds"] = ScriptValue::ofBool(true);
        t.properties["delimiter"] = ScriptValue::ofText(".");
        t.properties["size"] = ScriptValue::ofNumber(7);
        FakeScene other;
        ScriptHost custom(other);
        QVERIFY2(custom.add(t, ScriptValue{}, &error), error.c_str());
        custom.update(frame());
        QCOMPARE(other.at(0, "text").text, std::string("true.7.0.5"));
    }

    void scriptPropertiesBoundToUserPropertiesStartAtTheirValueAndFollowIt()
    {
        const char *source = R"(
            export var scriptProperties = createScriptProperties()
                .addSlider({ name: 'x', label: 'x', value: 3, min: 0, max: 10 })
                .addCheckbox({ name: 'on', label: 'x', value: false })
                .finish();
            export function update(value) { return scriptProperties.x + (scriptProperties.on ? 'on' : 'off'); }
        )";
        std::string error;
        const auto run = [&](const ScriptTarget &t, const std::map<std::string, ScriptValue> &changes = {}) {
            FakeScene scene;
            ScriptHost host(scene);
            if (!host.add(t, ScriptValue{}, &error))
                qFatal("%s", error.c_str());
            if (!changes.empty())
                host.applyScriptPropertyChanges(changes);
            host.update(frame());
            return scene.at(0, "text").text;
        };
        ScriptTarget bound = target(source);
        bound.propertyUsers = {{"x", "lens"}, {"on", "switch"}};
        // never set: the script's own default (or the file's value, when the object gave one)
        QCOMPARE(run(bound), std::string("3off"));
        bound.properties["x"] = ScriptValue::ofNumber(5);
        QCOMPARE(run(bound), std::string("5off"));
        // the start value the engine reads from the property
        bound.properties["x"] = ScriptValue::ofNumber(7);
        QCOMPARE(run(bound), std::string("7off"));
        // a value of an unexpected type (text for a slider) leaves the default
        ScriptTarget odd = target(source);
        odd.properties["x"] = ScriptValue::ofText("big");
        QCOMPARE(run(odd), std::string("3off"));
        // live: a change of the user property reaches scriptProperties; an unreadable one changes nothing
        QCOMPARE(run(bound, {{"lens", ScriptValue::ofNumber(9)}}), std::string("9off"));
        QCOMPARE(run(bound, {{"lens", ScriptValue::ofNumber(9)}, {"switch", ScriptValue::ofBool(true)}}), std::string("9on"));
        QCOMPARE(run(bound, {{"lens", ScriptValue::ofText("big")}}), std::string("7off"));
        QCOMPARE(run(bound, {{"other", ScriptValue::ofNumber(1)}}), std::string("7off")); // not bound
    }

    void aScriptHearsThatNothingIsPlaying()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target(R"(
            export function mediaPlaybackChanged(event) {
                thisLayer.alpha = event.state !== MediaPlaybackEvent.PLAYBACK_STOPPED ? 1 : 0;
            }
            export function mediaStatusChanged(event) { if (!event.enabled) thisLayer.visible = false; }
            // the events of a track do not come when there is none (the widgets play their intro from them)
            export function mediaPropertiesChanged(event) { thisLayer.text = 'heard'; }
            export function mediaThumbnailChanged(event) { thisLayer.text = 'heard'; }
            export function mediaTimelineChanged(event) { thisLayer.text = 'heard'; }
            export function update(v) { return v; }
        )", "text"), ScriptValue{}, &error), error.c_str());
        QCOMPARE(scene.at(0, "alpha").number, 0.0);
        QVERIFY(!scene.at(0, "visible").boolean);
        QCOMPARE(QString::fromStdString(scene.at(0, "text").text), QString("start"));
        QCOMPARE(host.disabledCount(), size_t{0});
    }

    static ScriptFrame at(double x, double y, bool down = false, double time = 1.0)
    {
        ScriptFrame f = frame();
        f.cursorX = static_cast<float>(x);
        f.cursorY = static_cast<float>(y);
        f.cursorDown = down;
        f.time = time;
        return f;
    }

    void pointerHooksFollowTheHoveredObject()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target(R"(
            const note = (c) => { thisLayer.text = thisLayer.text + c; };
            export function cursorEnter(e) { note('E'); }
            export function cursorLeave(e) { note('L'); }
            export function cursorMove(e) { note('M' + Math.round(e.worldPosition.x)); }
            export function cursorDown(e) { note('D'); }
            export function cursorUp(e) { note('U'); }
            export function cursorClick(e) { note('C'); }
        )", "text"), ScriptValue{}, &error), error.c_str());
        const auto text = [&] { return QString::fromStdString(scene.at(0, "text").text); };
        host.update(at(50, 50));
        QCOMPARE(text(), QString("start"));           // outside: nothing
        host.update(at(2, 5));
        QCOMPARE(text(), QString("startEM2"));        // in: enter, and the pointer moved to get there
        host.update(at(2, 5));
        QCOMPARE(text(), QString("startEM2"));        // still: no move
        host.update(at(4, 5));
        QCOMPARE(text(), QString("startEM2M4"));
        host.update(at(4, 5, true));
        QCOMPARE(text(), QString("startEM2M4D"));
        host.update(at(4, 5, false));
        QCOMPARE(text(), QString("startEM2M4DUC"));   // the press began on it: a click
        host.update(at(4, 5, true));
        host.update(at(40, 5, true));                  // dragged out while pressed: leave, and the release elsewhere is no click
        host.update(at(40, 5, false));
        QCOMPARE(text(), QString("startEM2M4DUCDL"));
        // pressed outside and released over it: up, but no click
        scene.at(0, "text") = ScriptValue::ofText("");
        host.update(at(40, 5, true));
        host.update(at(2, 5, true));
        host.update(at(2, 5, false));
        QCOMPARE(text(), QString("EM2U"));
    }

    void everyObjectUnderThePointerGetsItsOwnEvents()
    {
        // each script's own object is hit tested: two boxes that overlap are both entered
        FakeScene scene;
        scene.values[{1, "text"}] = ScriptValue::ofText("");
        ScriptHost host(scene);
        std::string error;
        const char *source = "export function cursorEnter(e) { thisLayer.text = thisLayer.text + 'in'; }\n"
                             "export function cursorLeave(e) { thisLayer.text = thisLayer.text + 'out'; }\n";
        QVERIFY2(host.add(target(source, "text", 0), ScriptValue{}, &error), error.c_str());
        QVERIFY2(host.add(target(source, "text", 1), ScriptValue{}, &error), error.c_str());
        scene.at(0, "text") = ScriptValue::ofText("");
        host.update(at(2, 5));  // only layer 0 is there
        host.update(at(7, 5));  // both
        host.update(at(12, 5)); // layer 1 only: layer 0 is left
        QCOMPARE(QString::fromStdString(scene.at(0, "text").text), QString("inout"));
        QCOMPARE(QString::fromStdString(scene.at(1, "text").text), QString("in"));
        host.update(at(50, 5));
        QCOMPARE(QString::fromStdString(scene.at(1, "text").text), QString("inout"));
    }

    void aThrowingPointerHookStopsOnlyThatScript()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target("export function cursorEnter(e) { throw new Error('no'); }", "text"), ScriptValue{}, &error), error.c_str());
        host.update(at(2, 5));
        QCOMPARE(host.disabledCount(), size_t{1});
        host.update(at(50, 5)); // nothing left to call
    }

    void timersRunOnTheSceneClockAndHaveACap()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        // engine.setTimeout / setInterval answer with a function that cancels the timer, as WE's do
        QVERIFY2(host.add(target(R"(
            const out = [];
            export function init() {
                engine.setTimeout(() => { out.push('t'); thisLayer.text = out.join(','); }, 500);
                const stop = engine.setInterval(() => { out.push('i'); thisLayer.text = out.join(','); }, 1000);
                const dropped = engine.setTimeout(() => { out.push('never'); }, 100);
                dropped();
                engine.setTimeout(() => stop(), 3500);
            }
        )", "text"), ScriptValue{}, &error), error.c_str());
        QVERIFY(host.hasUpdates()); // pending timers keep the scene moving though the script has no update
        const auto text = [&] { return QString::fromStdString(scene.at(0, "text").text); };
        const auto tick = [&](double t) { ScriptFrame f = frame(); f.time = t; host.update(f); };
        tick(0.0);
        QCOMPARE(text(), QString("start"));
        tick(0.4);
        QCOMPARE(text(), QString("start"));
        tick(0.5);
        QCOMPARE(text(), QString("t"));
        tick(0.9);
        tick(1.0);
        QCOMPARE(text(), QString("t,i"));
        tick(1.4);                                  // an interval fires at most once a frame and then waits a whole period
        tick(2.0);
        QCOMPARE(text(), QString("t,i,i"));
        tick(10.0);                                 // a long stall: one more (and the stop timer runs with it), not seven
        QCOMPARE(text(), QString("t,i,i,i"));
        tick(11.0);
        QCOMPARE(text(), QString("t,i,i,i"));
        QVERIFY(!host.hasUpdates());                // nothing pending any more
        // the number of pending timers is capped
        FakeScene other;
        ScriptHost capped(other);
        QVERIFY2(capped.add(target(R"(
            let fired = 0;
            export function init() {
                for (let i = 0; i < 400; ++i) engine.setTimeout(() => { ++fired; thisLayer.text = String(fired); }, 10);
            }
        )", "text"), ScriptValue{}, &error), error.c_str());
        ScriptFrame later = frame();
        later.time = 5.0;
        capped.update(later);
        QCOMPARE(QString::fromStdString(other.at(0, "text").text), QString("256"));
    }

    void aTimerThatNeverReturnsCostsBoundedTime()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target("export function init() { engine.setInterval(() => { for (;;) {} }, 0); }", "text"), ScriptValue{}, &error), error.c_str());
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 4; ++i) {
            host.update(at(50, 50, false, 1.0 + i));
        }
        QVERIFY2(timer.elapsed() < 2000, "the budget must stop it");
        QCOMPARE(host.disabledCount(), size_t{1}); // three frames over budget in a row stop the script
    }

    void applyUserPropertiesHearsEveryChange()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target(R"(
            const heard = [];
            export function applyUserProperties(changed) {
                heard.push(Object.keys(changed).sort().join('+') + ':' + String(changed.speed) + ':' + (changed.tint ? changed.tint.constructor.name : '-'));
                thisLayer.text = heard.join(';') + '|' + engine.userProperties.speed;
            }
        )", "text"), ScriptValue{}, &error), error.c_str());
        ScriptValue tint;
        tint.type = ScriptValue::Type::Vector;
        tint.count = 3;
        tint.vec = {1, 0.5, 0, 0};
        std::map<std::string, ScriptValue> all{{"speed", ScriptValue::ofNumber(2)}, {"tint", tint}};
        host.applyUserProperties(all, all);
        QCOMPARE(QString::fromStdString(scene.at(0, "text").text), QString("speed+tint:2:Vec3|2"));
        all["speed"] = ScriptValue::ofNumber(3);
        host.applyUserProperties({{"speed", ScriptValue::ofNumber(3)}}, all);
        QCOMPARE(QString::fromStdString(scene.at(0, "text").text), QString("speed+tint:2:Vec3;speed:3:-|3"));
    }

    void objectsKnowTheirParentMatrixAndSpriteFrame()
    {
        FakeScene scene;
        scene.values[{1, "name"}] = ScriptValue::ofText("parent");
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target(R"(
            export function init() {
                const p = thisLayer.getParent();
                const m = thisLayer.getTransformMatrix().m;
                thisLayer.getTextureAnimation().setFrame(3.7);
                thisLayer.text = p.name + ':' + m[12] + ',' + m[13] + ':' + (thisLayer.getParent().getParent() === null)
                    + ':' + thisLayer.getTextureAnimation().getFrame();
            }
        )", "text"), ScriptValue{}, &error), error.c_str());
        QCOMPARE(QString::fromStdString(scene.at(0, "text").text), QString("parent:100,200:true:3"));
        QVERIFY(std::find(scene.sets.begin(), scene.sets.end(), std::make_pair(0, std::string("textureFrame"))) != scene.sets.end());
    }

    void userPropertiesReadAsUndefinedInsteadOfThrowing()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target("export function update(v) { return String(engine.userProperties.name) + ':' + typeof engine.userProperties; }", "text"), ScriptValue{}, &error), error.c_str());
        host.update(frame());
        QCOMPARE(QString::fromStdString(scene.at(0, "text").text), QString("undefined:object"));
    }

    void aScriptHearsThatNothingIsPlayingOnlyWhenItAsks()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target("export function update(v) { return v; }"), ScriptValue{}, &error), error.c_str());
        QCOMPARE(scene.at(0, "alpha").number, 0.5);
    }

    void aClockScriptFormatsTheTime()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target(R"(
            export function update(value) {
                const t = new Date();
                return ('00' + t.getHours()).slice(-2) + ':' + ('00' + t.getMinutes()).slice(-2) + ':' + ('00' + t.getSeconds()).slice(-2);
            }
        )"),
                          ScriptValue{}, &error),
                 error.c_str());
        host.update(frame());
        QVERIFY2(QRegularExpression(R"(^\d\d:\d\d:\d\d$)").match(QString::fromStdString(scene.at(0, "text").text)).hasMatch(),
                 scene.at(0, "text").text.c_str());
    }

    void layersCanBeCreatedSortedAndFound()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target(R"(
            export function init() {
                const index = thisScene.getLayerIndex(thisLayer);
                const bar = thisScene.createLayer('models/bar.json');
                thisScene.sortLayer(bar, index);
                bar.parallaxDepth = new Vec2(0, 0);
                const shadow = thisScene.createLayer({ text: 'shadow', pointsize: thisLayer.pointsize });
                shared.count = (shared.count || 0) + 2;
                if (thisScene.getLayer('zero') === null || thisScene.getLayer('missing') !== null) throw new Error('find');
            }
        )",
                                 "visible"),
                          ScriptValue{}, &error),
                 error.c_str());
        QCOMPARE(scene.created.size(), size_t{2});
        QCOMPARE(scene.created[0], std::string("model:models/bar.json"));
        QCOMPARE(scene.created[1].substr(0, 11), std::string("json:{\"text"));
        QVERIFY(scene.created[1].find("\"pointsize\":32") != std::string::npos);
        QCOMPARE(scene.sorted.size(), size_t{1});
        QVERIFY(scene.sorted[0] == std::make_pair(101, 7));
        QCOMPARE(scene.at(101, "parallaxDepth").count, 2);
    }

    void audioBuffersCarryTheSpectrum()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target(R"(
            const audio = engine.registerAudioBuffers(16);
            export function update(value) { return audio.average[3] + audio.left[3] * 10 + audio.right[3] * 100; }
        )",
                                 "alpha"),
                          ScriptValue{}, &error),
                 error.c_str());
        QVERIFY(host.usesAudio());
        AudioSpectrum spectrum;
        spectrum.left16[3] = 0.5f;
        spectrum.right16[3] = 0.25f;
        ScriptFrame f = frame();
        f.audio = &spectrum;
        host.update(f);
        // average 0.375 + left 5 + right 25
        QVERIFY(std::fabs(scene.at(0, "alpha").number - 30.375) < 1e-4);
    }

    void frameInputsAndTheCursorAreVisible()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target(R"(
            export function update(value) {
                const c = input.cursorWorldPosition;
                return engine.frametime + ',' + engine.canvasSize.x + ',' + c.x + ',' + c.y + ',' + input.cursorDown;
            }
        )"),
                          ScriptValue{}, &error),
                 error.c_str());
        ScriptFrame f = frame();
        f.canvasWidth = 1280;
        f.cursorX = 100;
        f.cursorY = 50;
        f.cursorDown = true;
        host.update(f);
        QCOMPARE(scene.at(0, "text").text, std::string("0.05,1280,100,50,true"));
    }

    void scriptsAreIsolatedButShareTheSharedObject()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target("var mine = 1; shared.value = 'from a'; export function update(v) { return 'a'; }", "text"), ScriptValue{}, &error), error.c_str());
        QVERIFY2(host.add(target("export function update(v) { return typeof mine + ':' + shared.value; }", "origin", 0), ScriptValue{}, &error), error.c_str());
        // the second script returns a string for a vector property: ignored by the scene, but the call must work
        QVERIFY2(host.add(target("export function update(v) { return typeof mine + ':' + shared.value; }", "text", 5), ScriptValue{}, &error), error.c_str());
        host.update(frame());
        QCOMPARE(scene.at(5, "text").text, std::string("undefined:from a"));
    }

    void modulesAreTheOnesWeProvide()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target(R"(
            import * as WEMath from 'WEMath';
            import { rgb2hsv } from 'WEColor';
            export function update(value) { return WEMath.mix(10, 20, 0.5) + ':' + WEMath.clamp(5, 0, 1) + ':' + rgb2hsv(new Vec3(1, 0, 0)).y; }
        )"),
                          ScriptValue{}, &error),
                 error.c_str());
        host.update(frame());
        QCOMPARE(scene.at(0, "text").text, std::string("15:1:1"));
        QVERIFY(!host.add(target("import fs from 'fs'; export function update() {}"), ScriptValue{}, &error));
        QVERIFY(error.find("fs") != std::string::npos);
    }

    void aBrokenScriptIsRefusedOrDisabledOnceWhileOthersRun()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY(!host.add(target("export function update( { oops"), ScriptValue{}, &error));
        QVERIFY(!error.empty());
        QVERIFY(!host.add(target("export function init() { throw new Error('nope'); }"), ScriptValue{}, &error));
        QVERIFY(error.find("nope") != std::string::npos);
        QCOMPARE(host.scriptCount(), size_t{0});

        QVERIFY(host.add(target("export function update(v) { throw new TypeError('boom'); }"), ScriptValue{}, &error));
        QVERIFY(host.add(target("export function update(v) { return 'fine'; }", "text", 3), ScriptValue{}, &error));
        host.update(frame());
        host.update(frame());
        QCOMPARE(host.disabledCount(), size_t{1});
        QCOMPARE(scene.at(3, "text").text, std::string("fine")); // the other carried on
        int stopped = 0;
        for (const std::string &m : host.messages())
            stopped += m.find("stopped") != std::string::npos ? 1 : 0;
        QCOMPARE(stopped, 1); // logged once
    }

    void scriptsThatThrowNonObjectsAreStoppedAndTheHostTearsDownCleanly()
    {
        std::string error;
        {
            FakeScene scene;
            ScriptHost host(scene);
            QVERIFY(host.add(target("export function update(v) { throw null; }"), ScriptValue{}, &error));
            QVERIFY(host.add(target("export function update(v) { throw 42; }", "text", 3), ScriptValue{}, &error));
            QVERIFY(host.add(target("export function applyUserProperties(p) { throw undefined; }", "text", 4), ScriptValue{}, &error));
            host.update(frame());
            host.applyUserProperties({{"a", ScriptValue::ofNumber(1.0)}}, {{"a", ScriptValue::ofNumber(1.0)}});
            host.update(frame());
            QCOMPARE(host.disabledCount(), size_t{3});
        } // the destructor runs the runtime's own collection: a TypeError left pending for a stack nobody asked for made it assert
    }

    void aThousandScriptsLoadAndUpdateWithinTheFrameBudgetAndMemory()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        // the library's biggest scenes have 944 and 1382 scripts: in the shared realm a script costs its module and its names
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 1000; ++i)
            QVERIFY2(host.add(target("let n = 0; export function update(v) { n += engine.frametime; thisLayer.alpha = n; return v; }", "text", i % 50), ScriptValue{}, &error),
                     error.c_str());
        const qint64 loadMs = timer.elapsed();
        QCOMPARE(host.ownRealmCount(), size_t{0});
        host.update(frame());
        qint64 best = 1 << 30;
        for (int k = 0; k < 5; ++k) {
            timer.restart();
            host.update(frame());
            best = std::min(best, timer.elapsed());
        }
        QCOMPARE(host.disabledCount(), size_t{0});
        const double mib = host.memoryUsed() / 1048576.0;
        qInfo("1000 scripts: load %lld ms, frame %lld ms, %.1f MiB", static_cast<long long>(loadMs), static_cast<long long>(best), mib);
        QVERIFY2(mib < 32.0, qPrintable(QString::number(mib)));        // a realm each was about 0.4 MiB a script
        QVERIFY2(best < 250, qPrintable(QString::number(best))); // under the frame's ceiling (a few ms on a desktop; coarse for busy machines)
    }

    void scriptsOfTheSharedRealmKeepTheirOwnNamesAndState()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        const char *source = R"(
            export var scriptProperties = createScriptProperties().addSlider({ name: 'k', value: 1 }).finish();
            let calls = 0;
            export function update(v) {
                ++calls;
                return [thisLayer.name, scriptProperties.k, calls, typeof shared.seen, engine.userProperties.who].join(':');
            }
            export function applyUserProperties(p) { shared.seen = 1; }
        )";
        ScriptTarget a = target(source, "text", 1);
        a.properties["k"] = ScriptValue::ofNumber(5);
        ScriptTarget b = target(source, "text", 2);
        scene.at(1, "name") = ScriptValue::ofText("one");
        scene.at(2, "name") = ScriptValue::ofText("two");
        QVERIFY2(host.add(a, ScriptValue{}, &error), error.c_str());
        QVERIFY2(host.add(b, ScriptValue{}, &error), error.c_str());
        QCOMPARE(host.ownRealmCount(), size_t{0});
        host.applyUserProperties({{"who", ScriptValue::ofText("x")}}, {{"who", ScriptValue::ofText("x")}});
        host.update(frame());
        QCOMPARE(scene.at(1, "text").text, std::string("one:5:1:number:x"));
        QCOMPARE(scene.at(2, "text").text, std::string("two:1:1:number:x"));
        host.update(frame());
        QCOMPARE(scene.at(2, "text").text, std::string("two:1:2:number:x")); // each module counts its own calls
    }

    void timersAndAudioBelongToTheScriptThatAsked()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY(host.add(target("export function init() { engine.setTimeout(() => { thisLayer.text = 'fired'; }, 0); }", "text", 1), ScriptValue{}, &error));
        QVERIFY(host.add(target("export function update(v) { return 'still'; }", "text", 2), ScriptValue{}, &error));
        QVERIFY(host.active());
        QVERIFY(!host.usesAudio());
        host.update(frame());
        QCOMPARE(scene.at(1, "text").text, std::string("fired"));
        QCOMPARE(scene.at(2, "text").text, std::string("still"));
        QVERIFY(host.add(target("const later = setTimeout; const stop = later(() => {}, 1e9); export function update(v) { clearTimeout(stop); return 'ok'; }", "text", 3),
                         ScriptValue{}, &error));
        host.update(frame());
        QCOMPARE(scene.at(3, "text").text, std::string("ok"));
    }

    void aScriptThatChangesBuiltinsGetsARealmOfItsOwn()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target("Math.twice = (x) => 2 * x; Array.prototype.last = function () { return this[this.length - 1]; };"
                                 "export function update(v) { return Math.twice(2) + ':' + [1, 9].last(); }", "text", 1),
                          ScriptValue{}, &error),
                 error.c_str());
        QVERIFY2(host.add(target("export function update(v) { return typeof Math.twice + ':' + typeof [].last; }", "text", 2), ScriptValue{}, &error), error.c_str());
        QCOMPARE(host.ownRealmCount(), size_t{1});
        host.update(frame());
        QCOMPARE(scene.at(1, "text").text, std::string("4:9"));
        QCOMPARE(scene.at(2, "text").text, std::string("undefined:undefined")); // the shared realm never saw them
        // a script that declares one of its own names itself works in a realm of its own, where they are globals
        QVERIFY2(host.add(target("const engine = { frametime: 7 }; export function update(v) { return String(engine.frametime) + typeof thisLayer; }", "text", 3),
                          ScriptValue{}, &error),
                 error.c_str());
        QCOMPARE(host.ownRealmCount(), size_t{2});
        host.update(frame());
        QCOMPARE(scene.at(3, "text").text, std::string("7object"));
    }

    void theScreenForTheSharedRealm()
    {
        const auto own = [](const char *source) { return !scriptNeedsOwnRealm(source).empty(); };
        QVERIFY(!own("export function update(v) { return Math.max(v, 1) + Vec3.fromString('1 2 3').x; }"));
        QVERIFY(!own("const a = Object.prototype.hasOwnProperty.call(engine.userProperties, 'k'); let x = Math.PI <= 3;"));
        QVERIFY(!own("Object.defineProperty(thisLayer, 'clicked', { value: false, writable: true }); shared.n = 1; thisLayer.alpha = 0.5;"));
        QVERIFY(!own("// eval the Function type\nexport function update(v) { return v; }"));
        QVERIFY(own("const s = 'globalThis';")); // the source is not parsed: a word in a string is a false alarm (a realm more), never a miss
        QVERIFY(!own("const o = { engine: 1 }; o.engine = 2; let thisLayerAlpha = 3; function f(engine) { return engine; }"));
        QVERIFY(own("Math.helper = 1;"));
        QVERIFY(own("console.log = () => {};"));
        QVERIFY(own("Vec3 . prototype.extra = function () {};"));
        QVERIFY(own("Object.assign(Vec2.prototype, { x2() {} });"));
        QVERIFY(own("Object.defineProperty(Math, 'tau', { value: 6.28 });"));
        QVERIFY(own("JSON['parse'] = null;"));
        QVERIFY(own("delete Math.max;"));
        QVERIFY(own("globalThis.counter = 0;"));
        QVERIFY(own("const f = new Function('return 1');"));
        QVERIFY(own("thisLayer = thisScene.getLayer('x');"));
        QVERIFY(own("let v = []; v.__proto__.extra = 1;"));
        QVERIFY(own("Object.setPrototypeOf(a, b);"));
        // what a review found going around the first version
        QVERIFY(own("Vec3 = function () {};"));
        QVERIFY(own("shared = {};"));
        QVERIFY(own("console = { log() {} };"));
        QVERIFY(own("localStorage = null;"));
        QVERIFY(own("input = 1;"));
        QVERIFY(own("const P = Vec3.prototype; P.extra = 1;"));
        QVERIFY(own("const p = [].__proto__; p.extra = 1;"));
        QVERIFY(own("const g = (() => {}).constructor('return this')();"));
        QVERIFY(own("Object['assign'](Math, { tau: 6.28 });"));
        QVERIFY(own("Math [ 'max' ] = null;"));
        QVERIFY(own("Object.assign(console, { log() {} });"));
        QVERIFY(own("Object.freeze(shared);"));
        QVERIFY(own(("Math[" + std::string(400, ' ') + "'max'] = null;").c_str())); // a bracket too long to follow counts as a member
        // ... and what stays in the shared realm
        QVERIFY(!own("if (Object.prototype.hasOwnProperty.call(o, 'k')) shared['k' + 1] = Array.prototype.slice.call(a, 1);"));
        QVERIFY(!own("shared.n = 1; const s = shared; s.m = 2; let x = Vec3.fromString('1 2 3'); x.x = Math.max(1, 2);"));
    }

    void aScriptCannotLoadAnotherScriptsModuleOrNames()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target("export const secret = 1; export function update(v) { return 'a'; }", "text", 1), ScriptValue{}, &error), error.c_str());
        QVERIFY(!host.add(target("import { secret } from 'script:test-text'; export function update(v) { return String(secret); }", "text", 2), ScriptValue{}, &error));
        QVERIFY2(error.find("script:test-text") != std::string::npos, error.c_str());
        QVERIFY(!host.add(target("import { thisLayer as other } from '__scope/0'; export function update(v) { return other.name; }", "text", 3), ScriptValue{}, &error));
        QVERIFY(!host.add(target(std::string((1u << 20) + 1, ' ').c_str(), "text", 4), ScriptValue{}, &error));
        QVERIFY2(error.find("1 MiB") != std::string::npos, error.c_str());
    }

    void oneScriptsEndlessLoopDoesNotStopItsNeighbour()
    {
        FakeScene scene;
        ScriptHost::Limits limits;
        limits.frameSeconds = 0.06;
        limits.frameTotalSeconds = 0.09;
        ScriptHost host(scene, limits);
        std::string error;
        QVERIFY(host.add(target("export function update(v) { for (;;) {} }", "text", 1), ScriptValue{}, &error));
        // a neighbour that needs half the ceiling: after the loop it is cut by the ceiling, which is not its own budget
        QVERIFY(host.add(target("let n = 0; export function update(v) { const end = Date.now() + 45; while (Date.now() < end) {} return String(++n); }", "text", 2),
                         ScriptValue{}, &error));
        for (int i = 0; i < 9; ++i)
            host.update(frame());
        QCOMPARE(host.disabledCount(), size_t{1});
        bool loopStopped = false;
        for (const std::string &m : host.messages())
            loopStopped = loopStopped || (m.find("test-text") != std::string::npos && m.find("over its time budget") != std::string::npos);
        QVERIFY(loopStopped);
        QVERIFY(std::atoi(scene.at(2, "text").text.c_str()) >= 3); // it ran (first, after a frame the ceiling cut it)
    }

    void eventsThatDoNotFitInTheCeilingComeWithTheNextUpdate()
    {
        FakeScene scene;
        ScriptHost::Limits limits;
        limits.frameSeconds = 1.0;
        limits.frameTotalSeconds = 0.1;
        ScriptHost host(scene, limits);
        std::string error;
        const char *slow = "let heard = ''; export function applyUserProperties(p) { heard = 'k=' + p.k; const end = Date.now() + 80; while (Date.now() < end) {} }"
                           "export function update(v) { return heard; }";
        for (int i = 0; i < 3; ++i)
            QVERIFY(host.add(target(slow, "text", i), ScriptValue{}, &error));
        host.applyUserProperties({{"k", ScriptValue::ofNumber(1)}}, {{"k", ScriptValue::ofNumber(1)}});
        QCOMPARE(scene.at(2, "text").text, std::string()); // not reached within the ceiling
        for (int i = 0; i < 4; ++i)
            host.update(frame());
        for (int i = 0; i < 3; ++i)
            QCOMPARE(scene.at(i, "text").text, std::string("k=1"));
        QCOMPARE(host.disabledCount(), size_t{0});
    }

    void everyScriptHearsTheScreenSize()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        for (int i = 1; i <= 2; ++i)
            QVERIFY(host.add(target("export function resizeScreen(s) { thisLayer.text = s.x + 'x' + s.y; }", "text", i), ScriptValue{}, &error));
        ScriptFrame f = frame();
        f.screenWidth = 100;
        f.screenHeight = 50;
        host.update(f);
        f.screenWidth = 200;
        host.update(f);
        QCOMPARE(scene.at(1, "text").text, std::string("200x50"));
        QCOMPARE(scene.at(2, "text").text, std::string("200x50"));
    }

    void theHostHandsOutTheRealmsOwnVectors()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        // a script of its own realm that replaces Vec3 still gets the prelude's vectors as values
        QVERIFY(host.add(target("globalThis.Vec3 = function () { throw new Error('mine'); }; export function update(v) { return typeof v.x + ':' + typeof v.length; }",
                                "origin", 0),
                         ScriptValue{}, &error));
        host.update(frame());
        QCOMPARE(host.disabledCount(), size_t{0});
        QCOMPARE(scene.at(0, "origin").text, std::string("number:function"));
    }

    void objectsAreTheSameForEveryScriptOfTheRealm()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        // the library's icon launchers define `clicked` on their own layer and other scripts read it from thisScene.getLayer
        QVERIFY2(host.add(target("export function init() { Object.defineProperty(thisLayer, 'clicked', { enumerable: true, writable: true, value: true }); }", "text", 0),
                          ScriptValue{}, &error),
                 error.c_str());
        QVERIFY2(host.add(target("export function update(v) { const l = thisScene.getLayer('zero'); return String(l.clicked) + ':' + (l === thisScene.getLayer('zero')); }", "text", 4),
                          ScriptValue{}, &error),
                 error.c_str());
        host.update(frame());
        QCOMPARE(scene.at(4, "text").text, std::string("true:true"));
    }

    void errorsNameTheScriptsOwnLines()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY(host.add(target("export function update(v) {\n  const a = 1;\n  throw new Error('line three');\n}", "text", 1), ScriptValue{}, &error));
        host.update(frame());
        QCOMPARE(host.disabledCount(), size_t{1});
        bool found = false;
        for (const std::string &m : host.messages())
            found = found || (m.find("line three") != std::string::npos && m.find(":3") != std::string::npos);
        QVERIFY(found);
        // a script that does not compile is reported with its own error and line
        QVERIFY(!host.add(target("let a = 1;\nlet b = ;\n"), ScriptValue{}, &error));
        QVERIFY2(error.find(":2") != std::string::npos, error.c_str());
    }

    void tooManyScriptsAreRefusedNotRunOutOfMemory()
    {
        FakeScene scene;
        ScriptHost::Limits limits;
        limits.maxScripts = 3;
        ScriptHost host(scene, limits);
        std::string error;
        for (int i = 0; i < 3; ++i)
            QVERIFY(host.add(target("export function update(v) { return v; }"), ScriptValue{}, &error));
        QVERIFY(!host.add(target("export function update(v) { return v; }"), ScriptValue{}, &error));
        QCOMPARE(error, std::string("too many scripts"));
    }

    void aComboOrSliderWithoutAValueStartsAtItsFirstOptionOrMinimum()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target(R"(
            export var scriptProperties = createScriptProperties()
                .addCombo({ name: 'pick', label: 'Pick', options: [{ label: 'A', value: 'first' }, { label: 'B', value: 'second' }] })
                .addSlider({ name: 'amount', label: 'Amount', min: 2, max: 9 })
                .addSlider({ name: 'plain', label: 'Plain' })
                .finish();
            export function update(v) { return scriptProperties.pick + ':' + scriptProperties.amount + ':' + scriptProperties.plain; }
        )", "text"), ScriptValue{}, &error), error.c_str());
        host.update(frame());
        QCOMPARE(scene.at(0, "text").text, std::string("first:2:0"));
    }

    void aRunawayScriptIsStoppedByTheBudget()
    {
        FakeScene scene;
        ScriptHost::Limits limits;
        limits.frameSeconds = 0.05;
        limits.initSeconds = 0.2;
        ScriptHost host(scene, limits);
        std::string error;
        // an endless init is refused
        QElapsedTimer timer;
        timer.start();
        QVERIFY(!host.add(target("export function init() { for (;;) {} }"), ScriptValue{}, &error));
        QVERIFY2(timer.elapsed() < 3000, "init is bounded");
        // an endless update is disabled, and the next script still runs in the same frame
        QVERIFY(host.add(target("export function update(v) { while (true) {} }", "text", 1), ScriptValue{}, &error));
        QVERIFY(host.add(target("export function update(v) { return 'ok'; }", "text", 2), ScriptValue{}, &error));
        timer.restart();
        host.update(frame());
        QVERIFY2(timer.elapsed() < 3000, "a frame is bounded");
        QCOMPARE(scene.at(2, "text").text, std::string("ok")); // the script after it ran in the same frame
        QCOMPARE(host.disabledCount(), size_t{0});             // one slow frame is forgiven
        host.update(frame());
        host.update(frame());
        QCOMPARE(host.disabledCount(), size_t{1});             // three in a row are not
    }

    void aSlowFrameIsForgivenWhenTheScriptRecovers()
    {
        FakeScene scene;
        ScriptHost::Limits limits;
        limits.frameSeconds = 0.03;
        ScriptHost host(scene, limits);
        std::string error;
        QVERIFY(host.add(target("let n = 0; export function update(v) { if (++n % 2 === 1) { for (;;) {} } return 'fine'; }"), ScriptValue{}, &error));
        for (int i = 0; i < 8; ++i)
            host.update(frame()); // slow, fine, slow, fine...: never three in a row
        QCOMPARE(host.disabledCount(), size_t{0});
        QCOMPARE(scene.at(0, "text").text, std::string("fine"));
    }

    void eachScriptHasItsOwnBudgetAndTheFrameACeiling()
    {
        FakeScene scene;
        ScriptHost::Limits limits;
        limits.frameSeconds = 0.04;
        limits.frameTotalSeconds = 0.15;
        ScriptHost host(scene, limits);
        std::string error;
        for (int i = 0; i < 12; ++i)
            QVERIFY(host.add(target("export function update(v) { for (;;) {} }", "text", 10 + i), ScriptValue{}, &error));
        QElapsedTimer timer;
        timer.start();
        host.update(frame());
        QVERIFY2(timer.elapsed() < 1500, qPrintable(QString::number(timer.elapsed()))); // 12 x 40 ms would be 480 ms: the ceiling cuts it
        QVERIFY(timer.elapsed() >= 100);
    }

    void theHostApiIsNotReachableAndAudioTakesOnlyKnownBands()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target(R"(
            export function update(v) {
                const seen = [typeof __host, typeof __frame, typeof __layerOf, typeof makeScope];
                try { __frame = 1; seen.push('wrote'); } catch (e) { seen.push('blocked'); }
                const a = engine.registerAudioBuffers(7); // not a resolution: falls back to 16
                seen.push(a.resolution, Object.getPrototypeOf(shared) === null);
                return seen.join(',');
            }
        )"), ScriptValue{}, &error), error.c_str());
        // the host's entry points are the prelude's value, not globals: a script in a realm of its own that writes the global object
        // changes nothing the host calls
        QVERIFY(host.add(target("export function update(v) { globalThis.__host = null; globalThis.Vec2 = null; return typeof globalThis.__frame; }", "text", 4),
                         ScriptValue{}, &error));
        QCOMPARE(host.ownRealmCount(), size_t{2}); // both: the first asks for a prototype
        host.update(frame());
        host.update(frame());
        QCOMPARE(scene.at(0, "text").text, std::string("undefined,undefined,undefined,undefined,blocked,16,true"));
        QCOMPARE(scene.at(4, "text").text, std::string("undefined"));
        QCOMPARE(host.disabledCount(), size_t{0});
        // the native itself refuses other band counts (a script that got hold of it still cannot read past the arrays)
        ScriptFrame f = frame();
        AudioSpectrum spectrum;
        f.audio = &spectrum;
        QVERIFY(host.add(target("export function update(v) { return String(engine.registerAudioBuffers(64).left.length); }", "text", 3), ScriptValue{}, &error));
        host.update(f);
        QCOMPARE(scene.at(3, "text").text, std::string("64"));
    }

    void logMessagesAndTotalsAreCapped()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY(host.add(target(R"(
            export function update(v) {
                console.log('x'.repeat(1000000));
                for (let i = 0; i < 100000; ++i) console.log('line ' + i);
            }
        )"), ScriptValue{}, &error));
        host.update(frame());
        host.update(frame());
        size_t bytes = 0;
        for (const std::string &m : host.messages())
            bytes += m.size();
        QVERIFY2(bytes < 400000, qPrintable(QString::number(bytes)));
        for (const std::string &m : host.messages())
            QVERIFY(m.size() < 5000);
        QVERIFY(!host.messages().empty());
    }

    void aFailedFirstScriptDoesNotBreakSharedForTheNext()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY(!host.add(target("export function init() { shared.broken = 1; throw new Error('no'); }"), ScriptValue{}, &error));
        QVERIFY(host.add(target("export function update(v) { shared.n = (shared.n || 0) + 1; return String(shared.n + shared.broken); }", "text", 4), ScriptValue{}, &error));
        host.update(frame());
        host.update(frame());
        QCOMPARE(scene.at(4, "text").text, std::string("3")); // shared kept what the failed one set
    }

    void memoryIsLimitedAndThereIsNoFileOrNetworkAccess()
    {
        FakeScene scene;
        ScriptHost::Limits limits;
        limits.memoryBytes = 16u << 20;
        ScriptHost host(scene, limits);
        std::string error;
        QVERIFY(!host.add(target("export function init() { const a = []; for (;;) a.push(new Array(100000).fill(1)); }"), ScriptValue{}, &error));
        QVERIFY(!error.empty());
        // nothing that reaches the machine exists
        QVERIFY2(host.add(target(R"(
            export function update(v) {
                return [typeof std, typeof os, typeof require, typeof fetch, typeof XMLHttpRequest, typeof process, typeof scriptArgs].join(',');
            }
        )"),
                          ScriptValue{}, &error),
                 error.c_str());
        host.update(frame());
        QCOMPARE(scene.at(0, "text").text, std::string("undefined,undefined,undefined,undefined,undefined,undefined,undefined"));
    }

    void hostileScriptsCostBoundedTimeAndMemory()
    {
        FakeScene scene;
        ScriptHost::Limits limits;
        limits.memoryBytes = 16u << 20;
        limits.frameSeconds = 0.05;
        limits.initSeconds = 0.2;
        ScriptHost host(scene, limits);
        std::string error;
        QElapsedTimer timer;
        timer.start();
        // unbounded recursion: the stack limit turns it into an exception
        QVERIFY(!host.add(target("function f(n) { return f(n + 1) + 1; } export function init() { f(0); }"), ScriptValue{}, &error));
        // a string that doubles until memory runs out
        QVERIFY(!host.add(target("export function init() { let s = 'x'; for (;;) s += s; }"), ScriptValue{}, &error));
        // a promise that keeps scheduling itself: the job queue must not run forever
        host.add(target("export function init() { const again = () => Promise.resolve().then(again); again(); }"), ScriptValue{}, &error); // must not hang
        // regular expression backtracking
        QVERIFY(host.add(target("export function update(v) { return /(a+)+$/.test('a'.repeat(60) + 'b') ? 'yes' : 'no'; }", "text", 5), ScriptValue{}, &error));
        host.update(frame());
        QVERIFY2(timer.elapsed() < 6000, qPrintable(QString::number(timer.elapsed())));
        // a property read as a huge or hostile value does not crash the host
        QVERIFY(host.add(target("export function update(v) { return 'y'.repeat(1 << 20); }", "text", 6), ScriptValue{}, &error));
        host.update(frame());
        host.update(frame());
    }

    void aWorkshopScriptNamesFilesThroughItsId()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY(host.add(target("export let __workshopId = '2127844565';\nexport function init() { thisScene.createLayer('models/bar.json'); }"), ScriptValue{}, &error));
        QVERIFY(host.add(target("export function init() { thisScene.createLayer('models/bar.json'); }"), ScriptValue{}, &error));
        QVERIFY(host.add(target("export let __workshopId = 'not digits';\nexport function init() { thisScene.createLayer('models/bar.json'); }"), ScriptValue{}, &error));
        QCOMPARE(scene.created.size(), size_t{3});
        QCOMPARE(scene.created[0], std::string("model:models/workshop/2127844565/bar.json"));
        QCOMPARE(scene.created[1], std::string("model:models/bar.json"));   // no id: as written
        QCOMPARE(scene.created[2], std::string("model:models/bar.json"));   // an id that is not digits is ignored
    }

    void oneScriptMayHaveOnlyTwoHundredAndFiftySixLayersAlive()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY(host.add(target("export function init() { for (let i = 0; i < 1000; ++i) thisScene.createLayer('models/x.json'); }"), ScriptValue{}, &error));
        QCOMPARE(scene.created.size(), size_t{256});
        // another script has its own allowance
        QVERIFY(host.add(target("export function init() { for (let i = 0; i < 10; ++i) thisScene.createLayer('models/y.json'); }"), ScriptValue{}, &error));
        QCOMPARE(scene.created.size(), size_t{266});
    }

    void localStorageIsKeptBetweenRunsInTheItemsFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::string file = dir.filePath(QStringLiteral("script-storage/123.json")).toStdString();
        std::string error;
        {
            FakeScene scene;
            ScriptHost host(scene);
            host.setStorageFile(file);
            QVERIFY(host.add(target("export function init() { localStorage.set('seen', {n: 3}); localStorage.set('__proto__', 1, localStorage.LOCATION_GLOBAL); }"),
                             ScriptValue{}, &error));
        } // written when the host ends (and a second after a change while it runs)
        QVERIFY(QFile::exists(QString::fromStdString(file)));
        FakeScene scene;
        ScriptHost host(scene);
        host.setStorageFile(file);
        QVERIFY(host.add(target("export function update(v) { return JSON.stringify(localStorage.get('seen')) + ':' + localStorage.get('__proto__', localStorage.LOCATION_GLOBAL); }"),
                         ScriptValue{}, &error));
        host.update(frame());
        QCOMPARE(scene.at(0, "text").text, std::string("{\"n\":3}:1"));
        // a file that is not storage is ignored; a place that cannot be written keeps the storage for the run, said once
        QFile junk(dir.filePath(QStringLiteral("junk.json")));
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write("[1, 2, {\"global\": 5}");
        junk.close();
        FakeScene other;
        {
            ScriptHost again(other);
            again.setStorageFile(junk.fileName().toStdString());
            QVERIFY(again.add(target("export function update(v) { return String(localStorage.get('seen')); }"), ScriptValue{}, &error));
            again.update(frame());
            QCOMPARE(other.at(0, "text").text, std::string("undefined"));
            QVERIFY(QFile::exists(junk.fileName() + QStringLiteral(".bad"))); // kept aside, not overwritten
            again.setStorageFile("/proc/no/such/place/1.json");
            QVERIFY(again.add(target("export function init() { localStorage.set('x', 1); }", "text", 2), ScriptValue{}, &error));
        }
        QVERIFY(!QFile::exists(junk.fileName() + QStringLiteral(".tmp")));
        // the 100 KB are what is written: UTF-8
        FakeScene wide;
        ScriptHost quota(wide);
        QVERIFY(quota.add(target("export function update(v) { localStorage.set('a', 'é'.repeat(30000)); localStorage.set('b', 'é'.repeat(25000));"
                                 "return [localStorage.get('a') !== undefined, localStorage.get('b') !== undefined].join(' '); }"),
                          ScriptValue{}, &error));
        quota.update(frame());
        QCOMPARE(wide.at(0, "text").text, std::string("true false"));
    }

    void anItemsStorageFileIsInTheEnginesCache()
    {
        const QByteArray before = qgetenv("XDG_CACHE_HOME");
        qputenv("XDG_CACHE_HOME", "/home/someone/.cache");
        QCOMPARE(scriptStorageFile("/steam/steamapps/workshop/content/431960/3662790108/"),
                 std::string("/home/someone/.cache/murasaki/shader-cache/script-storage/3662790108.json"));
        QCOMPARE(scriptStorageFile("/items/my wallpaper"), std::string("/home/someone/.cache/murasaki/shader-cache/script-storage/my_wallpaper.json"));
        QCOMPARE(scriptStorageFile("/"), std::string());
        qputenv("XDG_CACHE_HOME", before);
    }

    void consoleOutputIsKept()
    {
        FakeScene scene;
        ScriptHost host(scene);
        std::string error;
        QVERIFY2(host.add(target("export function init() { console.log('hello', 3, 'world'); }"), ScriptValue{}, &error), error.c_str());
        QVERIFY(!host.messages().empty());
        QVERIFY(host.messages().back().find("hello 3 world") != std::string::npos);
    }
};

QTEST_GUILESS_MAIN(TstSceneVkScript)
#include "tst_scenevk_script.moc"
