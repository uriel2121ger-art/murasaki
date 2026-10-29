#include "testutil.h"
#include "webapiscript.h"

#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestWebApiScript : public QObject
{
    Q_OBJECT

private slots:
    void valuesFollowPrecedence();
    void skipsLabelsAndGroups();
    void scriptDrivesTheListener();
    void scriptEmbedsHostileTextInertly();
    void protoKeyIsAnOwnProperty();
    void startWhilePausedReplaysThePause();
    void ticksNeverStartBeforeStart();
    void apiObjectCannotBeReplaced();
    void itemWithoutPropertiesGivesEmptyObject();
    void pathValuesResolveInsideItemDirs();
    void pushedSpectrumReachesListenersOnTheNextTick();
    void malformedSpectrumKeepsThePreviousOne();
    void listeningFlipsOnRegistration();
    void pauseSuspendsAudioContextsAndMedia();
    void pauseToleratesMissingDocumentAndContexts();
    void resumeSkipsRemovedAndPageResumedElements();
    void audioInstancesArePausedAndDeduped();
    void wrappersKeepNameArgumentsAndStatics();
    void webkitAudioContextIsTracked();
    void closedContextsAreSkipped();
    void contextCreatedWhilePausedStaysSilent();
    void noTicksWhilePaused();
    void audioFunctionCannotBeReplaced();
    void animationFramesAreCappedAtStartFps();
    void animationFramesWaitWhilePaused();
    void cancelledFramesStayCancelledWhilePaused();
    void framesRequestedWhilePausedRunAfterTheUnpause();
    void setFpsWhilePausedAppliesAfterTheUnpause();
    void animationFramesBeforeStartAreUncapped();
    void pauseStopsAnimationsAndSvg();
    void pausedTimersAreThrottledAndKeepTheirIds();
    void audioTickKeepsItsCadenceUnthrottled();
    void setFpsRetunesTheFrameCap();
    void applyPropertiesReachesTheListenerAfterStart();
    void applyPropertiesRejectsNonObjects();

private:
    static QJsonObject item();
    static QJsonObject runPage(const QString &script, const QString &afterLoad);
};

QJsonObject TestWebApiScript::item()
{
    return testutil::json(
        "{\"presetValues\": {\"speed\": 7, \"nullkey\": null},"
        " \"properties\": ["
        "  {\"key\": \"speed\", \"type\": \"slider\", \"value\": 1},"
        "  {\"key\": \"tint\", \"type\": \"color\", \"value\": \"1 0 0\"},"
        "  {\"key\": \"nullkey\", \"type\": \"bool\", \"value\": true},"
        "  {\"key\": \"title\", \"type\": \"textinput\", \"value\": \"hi\"},"
        "  {\"key\": \"credits\", \"type\": \"label\", \"text\": \"by someone\"},"
        "  {\"key\": \"g\", \"type\": \"group\"}"
        " ]}");
}

// Evaluates script in a fake window whose wallpaperPropertyListener records every call, runs afterLoad, and
// returns what was recorded.
QJsonObject TestWebApiScript::runPage(const QString &script, const QString &afterLoad)
{
    QJSEngine engine;
    engine.installExtensions(QJSEngine::ConsoleExtension);
    engine.globalObject().setProperty(u"window"_s, engine.globalObject());
    const QJSValue setup = engine.evaluate(
        u"var recorded = {intervals: 0};"
        " var tickFn = null; function fireTick() { if (tickFn) tickFn(); }"
        " function setInterval(fn, ms) { recorded.intervals += 1; recorded.intervalMs = ms; tickFn = fn; return 1; }"
        " function clearInterval(id) { tickFn = null; }"
        " window.wallpaperPropertyListener = {"
        "   applyUserProperties: function (p) { recorded.user = p; },"
        "   applyGeneralProperties: function (p) { recorded.general = p; },"
        "   setPaused: function (p) { recorded.paused = p; } };"_s);
    if (setup.isError())
        qFatal("setup failed: %s", qPrintable(setup.toString()));
    const QJSValue injected = engine.evaluate(script);
    if (injected.isError())
        qFatal("injected script failed: %s", qPrintable(injected.toString()));
    const QJSValue result = engine.evaluate(afterLoad + u"; JSON.stringify(recorded)"_s);
    if (result.isError())
        qFatal("page script failed: %s", qPrintable(result.toString()));
    return QJsonDocument::fromJson(result.toString().toUtf8()).object();
}

// Stub media for the pause tests: contexts and elements that log what is done to them.
static const QString kMediaStubs = uR"JS(var log = [];
 function Ctx(a, b) { this.state = 'running'; this.args = [a, b]; }
 Ctx.prototype.suspend = function () { this.state = 'suspended'; log.push('suspend'); return Promise.resolve(); };
 Ctx.prototype.resume = function () { this.state = 'running'; log.push('resume'); return Promise.resolve(); };
 Ctx.staticValue = 42;
 var OriginalCtx = Ctx; window.AudioContext = Ctx;
 function media(name, paused) { return {paused: paused, isConnected: true,
   pause: function () { this.paused = true; log.push('pause ' + name); },
   play: function () { this.paused = false; log.push('play ' + name); return Promise.resolve(); }}; }
 var docElements = [];
 window.document = {querySelectorAll: function () { return docElements; }};)JS"_s;

// Stub timers on a manual clock for the throttle tests: advance(ms) runs everything due, in order.
static const QString kFakeTimers = uR"JS(var now = 0, timerSeq = 0, fakeTimers = [];
 Date.now = function () { return now; };
 function addFake(repeat, fn, ms) {
   var t = {id: ++timerSeq, fn: fn, ms: ms, at: now + ms, repeat: repeat}; fakeTimers.push(t); return t.id; }
 function setTimeout(fn, ms) { return addFake(false, fn, ms); }
 function setInterval(fn, ms) { return addFake(true, fn, ms); }
 function clearTimeout(id) { fakeTimers = fakeTimers.filter(function (t) { return t.id !== id; }); }
 function clearInterval(id) { clearTimeout(id); }
 function advance(ms) {
   var target = now + ms;
   for (;;) {
     var due = fakeTimers.filter(function (t) { return t.at <= target; })
       .sort(function (a, b) { return a.at - b.at || a.id - b.id; })[0];
     if (!due) break;
     now = Math.max(now, due.at);
     if (due.repeat) due.at = now + due.ms; else clearTimeout(due.id);
     due.fn();
   }
   now = target;
 })JS"_s;

// Stub animation frames driven by vsync(t), as the other frame tests do.
static const QString kFakeFrames = uR"JS(var nativeCbs = [];
 window.requestAnimationFrame = function (cb) { nativeCbs.push(cb); return 1; };
 window.cancelAnimationFrame = function () {};
 function vsync(t) { var cbs = nativeCbs; nativeCbs = []; cbs.forEach(function (cb) { cb(t); }); })JS"_s;

void TestWebApiScript::valuesFollowPrecedence()
{
    const QJsonObject props = webUserProperties(item(), testutil::json("{\"tint\": \"0 0 1\", \"title\": null}"));
    QCOMPARE(props.value(u"speed"_s).toObject().value(u"value"_s).toInt(), 7);           // preset over default
    QCOMPARE(props.value(u"tint"_s).toObject().value(u"value"_s).toString(), u"0 0 1"_s); // override over default
    QCOMPARE(props.value(u"nullkey"_s).toObject().value(u"value"_s).toBool(), true);    // null preset keeps default
    QCOMPARE(props.value(u"title"_s).toObject().value(u"value"_s).toString(), u"hi"_s); // null override keeps value
}

void TestWebApiScript::skipsLabelsAndGroups()
{
    const QJsonObject props = webUserProperties(item(), {});
    QVERIFY(!props.contains(u"credits"_s));
    QVERIFY(!props.contains(u"g"_s));
    QCOMPARE(props.size(), 4);
}

void TestWebApiScript::scriptDrivesTheListener()
{
    const QString script = buildWebApiScript(item(), {});
    const QJsonObject recorded = runPage(script,
        u"var audio = 0; window.wallpaperRegisterAudioListener(function (a) { audio = a.length; });"
        " window.__murasaki.start(120); window.__murasaki.setPaused(true);"
        " window.wallpaperRegisterMediaStatusListener(function () {});"
        " recorded.stubs = typeof window.wallpaperRequestRandomFileForProperty"_s);

    QCOMPARE(recorded.value(u"user"_s).toObject().value(u"speed"_s).toObject().value(u"value"_s).toInt(), 7);
    QCOMPARE(recorded.value(u"general"_s).toObject().value(u"fps"_s).toInt(), 120);
    QCOMPARE(recorded.value(u"paused"_s).toBool(), true);
    QCOMPARE(recorded.value(u"intervals"_s).toInt(), 1); // the silent audio feed started once, before the pause
    QCOMPARE(recorded.value(u"intervalMs"_s).toInt(), 33);
    QCOMPARE(recorded.value(u"stubs"_s).toString(), u"function"_s);
}

void TestWebApiScript::scriptEmbedsHostileTextInertly()
{
    QJsonObject hostile = item();
    hostile.insert(u"properties"_s, QJsonArray{QJsonObject{
        {u"key"_s, u"title"_s}, {u"type"_s, u"textinput"_s},
        {u"value"_s, u"\"]}); recorded.pwned = true; ({\""_s + QString(QChar(0x2028)) + QString(QChar(0x2029)) + u"</script>"_s}}});
    const QString script = buildWebApiScript(hostile, {});
    QVERIFY(!script.contains(u"</script>"_s));
    QVERIFY(!script.contains(QChar(0x2028)));
    const QJsonObject recorded = runPage(script, u"window.__murasaki.start(60)"_s);
    QVERIFY(!recorded.contains(u"pwned"_s));
    QVERIFY(recorded.value(u"user"_s).toObject().value(u"title"_s).toObject().value(u"value"_s).toString()
                .endsWith(u"</script>"_s));
}

void TestWebApiScript::protoKeyIsAnOwnProperty()
{
    QJsonObject proto = item();
    proto.insert(u"properties"_s, QJsonArray{QJsonObject{
        {u"key"_s, u"__proto__"_s}, {u"type"_s, u"bool"_s}, {u"value"_s, true}}});
    const QJsonObject recorded = runPage(buildWebApiScript(proto, {}),
        u"window.__murasaki.start(60);"
        " recorded.own = Object.prototype.hasOwnProperty.call(recorded.user, '__proto__');"
        " recorded.value = recorded.user['__proto__'].value"_s);
    QCOMPARE(recorded.value(u"own"_s).toBool(), true);
    QCOMPARE(recorded.value(u"value"_s).toBool(), true);
}

void TestWebApiScript::startWhilePausedReplaysThePause()
{
    const QJsonObject recorded = runPage(buildWebApiScript(item(), {}),
        u"window.wallpaperRegisterAudioListener(function () {});"
        " window.__murasaki.setPaused(true); recorded.pausedBeforeStart = recorded.paused; recorded.paused = null;"
        " window.__murasaki.start(60)"_s);
    QCOMPARE(recorded.value(u"pausedBeforeStart"_s).toBool(), true);
    QCOMPARE(recorded.value(u"paused"_s).toBool(), true); // replayed after the properties
    QCOMPARE(recorded.value(u"intervals"_s).toInt(), 0);
    QVERIFY(recorded.contains(u"user"_s));
}

void TestWebApiScript::ticksNeverStartBeforeStart()
{
    const QJsonObject recorded = runPage(buildWebApiScript(item(), {}),
        u"window.wallpaperRegisterAudioListener(function () {});"
        " window.__murasaki.setPaused(false); window.__murasaki.setPaused(true); window.__murasaki.setPaused(false);"
        " recorded.before = recorded.intervals; window.__murasaki.start(60)"_s);
    QCOMPARE(recorded.value(u"before"_s).toInt(), 0);
    QCOMPARE(recorded.value(u"intervals"_s).toInt(), 1);
}

void TestWebApiScript::apiObjectCannotBeReplaced()
{
    const QJsonObject recorded = runPage(buildWebApiScript(item(), {}),
        u"try { window.__murasaki = {start: function () { recorded.hijacked = true; }}; } catch (e) {}"
        " try { delete window.__murasaki; } catch (e) {}"
        " window.__murasaki.start(60)"_s);
    QVERIFY(!recorded.contains(u"hijacked"_s));
    QVERIFY(recorded.contains(u"user"_s));
}

void TestWebApiScript::itemWithoutPropertiesGivesEmptyObject()
{
    QVERIFY(webUserProperties(QJsonObject{}, {}).isEmpty());
    const QJsonObject recorded = runPage(buildWebApiScript(QJsonObject{}, {}), u"window.__murasaki.start(60)"_s);
    QVERIFY(recorded.contains(u"user"_s));
    QVERIFY(recorded.value(u"user"_s).toObject().isEmpty());
}

void TestWebApiScript::pathValuesResolveInsideItemDirs()
{
    QTemporaryDir tmp;
    const QString preset = tmp.filePath(u"preset"_s);
    const QString base = tmp.filePath(u"base"_s);
    testutil::writeFile(preset + u"/files/a b.jpg"_s, "x");
    testutil::writeFile(base + u"/img/default.png"_s, "x");
    testutil::writeFile(base + u"/slides/1.jpg"_s, "x");
    testutil::writeFile(tmp.filePath(u"secret.txt"_s), "x");
    QJsonObject item = testutil::json(
        "{\"properties\": ["
        "  {\"key\": \"fromPreset\", \"type\": \"file\", \"value\": \"files/a b.jpg\"},"
        "  {\"key\": \"fromBase\", \"type\": \"file\", \"value\": \"img/default.png\"},"
        "  {\"key\": \"folder\", \"type\": \"directory\", \"value\": \"slides\"},"
        "  {\"key\": \"escape\", \"type\": \"file\", \"value\": \"../secret.txt\"},"
        "  {\"key\": \"missing\", \"type\": \"file\", \"value\": \"files/none.jpg\"},"
        "  {\"key\": \"unset\", \"type\": \"file\", \"value\": null},"
        "  {\"key\": \"picked\", \"type\": \"file\", \"value\": null},"
        "  {\"key\": \"caption\", \"type\": \"textinput\", \"value\": \"files/a b.jpg\"}"
        " ]}");
    item.insert(u"dir"_s, preset);
    item.insert(u"contentDir"_s, base);
    const QJsonObject props = webUserProperties(item, QJsonObject{{u"picked"_s, u"/home/me/pic.jpg"_s}});
    const auto value = [&](const char *key) { return props.value(QLatin1String(key)).toObject().value(u"value"_s); };
    // Absolute paths without the leading slash, so "file:///" + value is the file's URL.
    QCOMPARE(value("fromPreset").toString(), testutil::canonical(preset + u"/files/a b.jpg"_s).mid(1));
    QCOMPARE(value("fromBase").toString(), testutil::canonical(base + u"/img/default.png"_s).mid(1));
    QCOMPARE(value("folder").toString(), testutil::canonical(base + u"/slides"_s).mid(1));
    QCOMPARE(value("escape").toString(), QString());
    QCOMPARE(value("missing").toString(), QString());
    QVERIFY(value("unset").isNull());
    QCOMPARE(value("picked").toString(), u"home/me/pic.jpg"_s);
    QCOMPARE(value("caption").toString(), u"files/a b.jpg"_s); // only file and directory values are paths
}

void TestWebApiScript::pushedSpectrumReachesListenersOnTheNextTick()
{
    const QJsonObject recorded = runPage(buildWebApiScript(item(), {}),
        u"var got = [];"
        " window.wallpaperRegisterAudioListener(function (a) { got.push(a); });"
        " window.__murasaki.start(60); fireTick(); recorded.silence = got[0].every(function (v) { return v === 0; });"
        " var pushed = []; for (var i = 0; i < 128; ++i) pushed.push(i / 128);"
        " window.__murasaki.audio(pushed); pushed[0] = 99;" // the host's array is copied
        " fireTick(); got[1][1] = 5; fireTick();" // listeners get their own copy each tick
        " recorded.len = got[1].length; recorded.first = got[1][0]; recorded.second = got[2][1];"_s);
    QCOMPARE(recorded.value(u"silence"_s).toBool(), true);
    QCOMPARE(recorded.value(u"len"_s).toInt(), 128);
    QCOMPARE(recorded.value(u"first"_s).toDouble(), 0.0);
    QCOMPARE(recorded.value(u"second"_s).toDouble(), 1.0 / 128);
}

void TestWebApiScript::malformedSpectrumKeepsThePreviousOne()
{
    const QJsonObject recorded = runPage(buildWebApiScript(item(), {}),
        u"var last = null; window.wallpaperRegisterAudioListener(function (a) { last = a; });"
        " window.__murasaki.start(60);"
        " var good = new Array(128).fill(0.5); window.__murasaki.audio(good);"
        " var short = new Array(127).fill(1), nan = good.slice(), inf = good.slice(), str = good.slice();"
        " nan[3] = NaN; inf[4] = Infinity; str[5] = '1';"
        " [short, nan, inf, str, null, undefined, 7, 'abc', {}, {length: 128}].forEach(function (bad) {"
        "   window.__murasaki.audio(bad); });"
        " fireTick(); recorded.sum = last.reduce(function (a, b) { return a + b; }, 0)"_s);
    QCOMPARE(recorded.value(u"sum"_s).toDouble(), 64.0);
}

void TestWebApiScript::listeningFlipsOnRegistration()
{
    const QJsonObject recorded = runPage(buildWebApiScript(item(), {}),
        u"recorded.before = window.__murasaki.listening();"
        " window.wallpaperRegisterAudioListener(42); recorded.notFunction = window.__murasaki.listening();"
        " window.wallpaperRegisterAudioListener(function () {}); recorded.after = window.__murasaki.listening()"_s);
    QCOMPARE(recorded.value(u"before"_s).toBool(), false);
    QCOMPARE(recorded.value(u"notFunction"_s).toBool(), false);
    QCOMPARE(recorded.value(u"after"_s).toBool(), true);
}

void TestWebApiScript::pauseSuspendsAudioContextsAndMedia()
{
    const QJsonObject recorded = runPage(u"var log = [];"
        " window.AudioContext = function () { this.state = 'running'; };"
        " window.AudioContext.prototype.suspend = function () { this.state = 'suspended'; log.push('suspend'); return Promise.resolve(); };"
        " window.AudioContext.prototype.resume = function () { this.state = 'running'; log.push('resume'); return Promise.resolve(); };"
        " var Original = window.AudioContext;"
        " function media(name, paused) { return {paused: paused,"
        "   pause: function () { this.paused = true; log.push('pause ' + name); },"
        "   play: function () { this.paused = false; log.push('play ' + name); return Promise.resolve(); }}; }"
        " var playing = media('a', false), idle = media('b', true), video = media('v', false);"
        " window.document = {querySelectorAll: function (sel) { if (sel !== 'svg') recorded.selector = sel; return [playing, idle, video]; }};"_s
        + buildWebApiScript(item(), {}),
        u"var running = new window.AudioContext(), stopped = new window.AudioContext();"
        " stopped.state = 'suspended'; recorded.isInstance = running instanceof Original;"
        " window.__murasaki.setPaused(true); window.__murasaki.setPaused(true);"
        " recorded.whilePaused = log.join(',');"
        " window.__murasaki.setPaused(false); window.__murasaki.setPaused(false);"
        " recorded.all = log.join(','); recorded.ctxState = running.state + '/' + stopped.state"_s);
    QCOMPARE(recorded.value(u"isInstance"_s).toBool(), true);
    QCOMPARE(recorded.value(u"selector"_s).toString(), u"audio,video"_s);
    QCOMPARE(recorded.value(u"whilePaused"_s).toString(), u"suspend,pause a,pause v"_s);
    // Only what the wallpaper paused comes back, once.
    QCOMPARE(recorded.value(u"all"_s).toString(), u"suspend,pause a,pause v,resume,play a,play v"_s);
    QCOMPARE(recorded.value(u"ctxState"_s).toString(), u"running/suspended"_s);
}

void TestWebApiScript::pauseToleratesMissingDocumentAndContexts()
{
    const QJsonObject recorded = runPage(buildWebApiScript(item(), {}),
        u"window.__murasaki.setPaused(true); window.__murasaki.setPaused(false); recorded.ok = true;"
        " recorded.hasContext = typeof window.AudioContext"_s);
    QCOMPARE(recorded.value(u"ok"_s).toBool(), true);
    QCOMPARE(recorded.value(u"hasContext"_s).toString(), u"undefined"_s);
}

void TestWebApiScript::resumeSkipsRemovedAndPageResumedElements()
{
    const QJsonObject recorded = runPage(kMediaStubs + buildWebApiScript(item(), {}),
        u"var kept = media('kept', false), removed = media('removed', false), self = media('self', false);"
        " docElements = [kept, removed, self];"
        " window.__murasaki.setPaused(true);"
        " removed.isConnected = false; self.play(); log.length = 0;" // the page restarted one of them itself
        " window.__murasaki.setPaused(false); recorded.log = log.join(',')"_s);
    QCOMPARE(recorded.value(u"log"_s).toString(), u"play kept"_s);
}

void TestWebApiScript::audioInstancesArePausedAndDeduped()
{
    const QJsonObject recorded = runPage(kMediaStubs
        + u"function Aud(src) { this.src = src; this.paused = false; this.isConnected = false;"
          "  this.pause = function () { this.paused = true; log.push('pause ' + this.src); };"
          "  this.play = function () { this.paused = false; log.push('play ' + this.src); return Promise.resolve(); }; }"
          " var OriginalAudio = Aud; window.Audio = Aud;"_s
        + buildWebApiScript(item(), {}),
        u"var loose = new window.Audio('x'), inDom = new window.Audio('y'); docElements = [inDom];"
        " recorded.instance = loose instanceof OriginalAudio && loose instanceof window.Audio;"
        " window.__murasaki.setPaused(true); recorded.pausedLog = log.join(',');"
        " window.__murasaki.setPaused(false); recorded.all = log.join(',')"_s);
    QCOMPARE(recorded.value(u"instance"_s).toBool(), true);
    // y is both tracked and in the document; it is paused and played once. A never-attached Audio resumes too.
    QCOMPARE(recorded.value(u"pausedLog"_s).toString(), u"pause x,pause y"_s);
    QCOMPARE(recorded.value(u"all"_s).toString(), u"pause x,pause y,play x,play y"_s);
}

void TestWebApiScript::wrappersKeepNameArgumentsAndStatics()
{
    const QJsonObject recorded = runPage(kMediaStubs
        + u"function Aud(src) { this.src = src; this.paused = true; } window.Audio = Aud;"_s
        + buildWebApiScript(item(), {}),
        u"var ctx = new window.AudioContext('a', 'b');"
        " recorded.args = ctx.args.join(); recorded.name = window.AudioContext.name;"
        " recorded.stat = window.AudioContext.staticValue; recorded.instance = ctx instanceof OriginalCtx;"
        " recorded.audioName = window.Audio.name; recorded.audioSrc = new window.Audio('s').src;"
        " recorded.wrapped = window.AudioContext !== OriginalCtx"_s);
    QCOMPARE(recorded.value(u"args"_s).toString(), u"a,b"_s);
    QCOMPARE(recorded.value(u"name"_s).toString(), u"Ctx"_s);
    QCOMPARE(recorded.value(u"stat"_s).toInt(), 42);
    QCOMPARE(recorded.value(u"instance"_s).toBool(), true);
    QCOMPARE(recorded.value(u"audioName"_s).toString(), u"Aud"_s);
    QCOMPARE(recorded.value(u"audioSrc"_s).toString(), u"s"_s);
    QCOMPARE(recorded.value(u"wrapped"_s).toBool(), true);
}

void TestWebApiScript::webkitAudioContextIsTracked()
{
    const QJsonObject recorded = runPage(u"var log = [];"
        " function Legacy() { this.state = 'running'; }"
        " Legacy.prototype.suspend = function () { this.state = 'suspended'; log.push('suspend'); };"
        " Legacy.prototype.resume = function () { this.state = 'running'; log.push('resume'); };"
        " window.webkitAudioContext = Legacy;"_s + buildWebApiScript(item(), {}),
        u"var ctx = new window.webkitAudioContext(); recorded.noModern = typeof window.AudioContext;"
        " window.__murasaki.setPaused(true); window.__murasaki.setPaused(false); recorded.log = log.join(',')"_s);
    QCOMPARE(recorded.value(u"noModern"_s).toString(), u"undefined"_s);
    QCOMPARE(recorded.value(u"log"_s).toString(), u"suspend,resume"_s);
}

void TestWebApiScript::closedContextsAreSkipped()
{
    const QJsonObject recorded = runPage(kMediaStubs + buildWebApiScript(item(), {}),
        u"var closed = new window.AudioContext(); closed.state = 'closed';"
        " var open = new window.AudioContext();"
        " window.__murasaki.setPaused(true); window.__murasaki.setPaused(false); recorded.log = log.join(',')"_s);
    QCOMPARE(recorded.value(u"log"_s).toString(), u"suspend,resume"_s); // one context only
}

void TestWebApiScript::contextCreatedWhilePausedStaysSilent()
{
    const QJsonObject recorded = runPage(kMediaStubs + buildWebApiScript(item(), {}),
        u"window.__murasaki.setPaused(true); var late = new window.AudioContext();"
        " recorded.whilePaused = log.join(','); window.__murasaki.setPaused(false); recorded.all = log.join(',')"_s);
    QCOMPARE(recorded.value(u"whilePaused"_s).toString(), u"suspend"_s);
    QCOMPARE(recorded.value(u"all"_s).toString(), u"suspend,resume"_s);
}

void TestWebApiScript::noTicksWhilePaused()
{
    const QJsonObject recorded = runPage(buildWebApiScript(item(), {}),
        u"var count = 0; window.wallpaperRegisterAudioListener(function () { count += 1; });"
        " window.__murasaki.start(60); fireTick(); window.__murasaki.setPaused(true); fireTick(); fireTick();"
        " recorded.whilePaused = count; window.__murasaki.setPaused(false); fireTick(); recorded.after = count"_s);
    QCOMPARE(recorded.value(u"whilePaused"_s).toInt(), 1);
    QCOMPARE(recorded.value(u"after"_s).toInt(), 2);
}

void TestWebApiScript::audioFunctionCannotBeReplaced()
{
    const QJsonObject recorded = runPage(buildWebApiScript(item(), {}),
        u"var last = null; window.wallpaperRegisterAudioListener(function (a) { last = a; });"
        " window.__murasaki.start(60);"
        " try { window.__murasaki.audio = function () { recorded.hijacked = true; }; } catch (e) {}"
        " window.__murasaki.audio(new Array(128).fill(1)); fireTick(); recorded.value = last[0]"_s);
    QVERIFY(!recorded.contains(u"hijacked"_s));
    QCOMPARE(recorded.value(u"value"_s).toDouble(), 1.0);
}


void TestWebApiScript::animationFramesAreCappedAtStartFps()
{
    // A 60 Hz display (16.67 ms vsync) and a page that redraws on every frame, capped at 30 fps after start().
    const QJsonObject recorded = runPage(u"var nativeCbs = []; recorded.native = 0;"
        " window.requestAnimationFrame = function (cb) { recorded.native += 1; nativeCbs.push(cb); return 1; };"
        " window.cancelAnimationFrame = function () {};"
        " function vsync(t) { var cbs = nativeCbs; nativeCbs = []; cbs.forEach(function (cb) { cb(t); }); }"_s
            + buildWebApiScript(item(), {}),
        u"recorded.draws = 0; recorded.cancelled = 0;"
        " function draw(t) { recorded.draws += 1; requestAnimationFrame(draw); }"
        " requestAnimationFrame(draw);"
        " var id = requestAnimationFrame(function () { recorded.cancelled += 1; }); cancelAnimationFrame(id);"
        " vsync(0); vsync(16.67); recorded.uncapped = recorded.draws;"
        " window.__murasaki.start(30); recorded.draws = 0;"
        " for (var i = 2; i < 62; ++i) vsync(i * 16.67);"
        " recorded.name = requestAnimationFrame.name"_s);
    QCOMPARE(recorded.value(u"uncapped"_s).toInt(), 2); // no cap before start()
    QCOMPARE(recorded.value(u"draws"_s).toInt(), 30); // 60 vsyncs at 30 fps
    QCOMPARE(recorded.value(u"cancelled"_s).toInt(), 0);
    QCOMPARE(recorded.value(u"name"_s).toString(), u"requestAnimationFrame"_s);
}

void TestWebApiScript::setFpsRetunesTheFrameCap()
{
    const QJsonObject recorded = runPage(u"var nativeCbs = []; recorded.native = 0;"
        " window.requestAnimationFrame = function (cb) { recorded.native += 1; nativeCbs.push(cb); return 1; };"
        " window.cancelAnimationFrame = function () {};"
        " function vsync(t) { var cbs = nativeCbs; nativeCbs = []; cbs.forEach(function (cb) { cb(t); }); }"_s
            + buildWebApiScript(item(), {}),
        u"recorded.draws = 0; function draw(t) { recorded.draws += 1; requestAnimationFrame(draw); }"
        " requestAnimationFrame(draw);"
        " window.__murasaki.setFps(10); recorded.beforeStart = recorded.general === undefined;" // ignored before start
        " window.__murasaki.start(60);"
        " for (var i = 0; i < 60; ++i) vsync(i * 16.67); recorded.at60 = recorded.draws; recorded.draws = 0;"
        " window.__murasaki.setFps(30);"
        " for (var i = 60; i < 120; ++i) vsync(i * 16.67); recorded.at30 = recorded.draws;"
        " window.__murasaki.setFps(0); window.__murasaki.setFps('x'); window.__murasaki.setFps(NaN);"
        " recorded.draws = 0; for (var i = 120; i < 180; ++i) vsync(i * 16.67); recorded.still30 = recorded.draws"_s);
    QVERIFY(recorded.value(u"beforeStart"_s).toBool());
    QVERIFY(qAbs(recorded.value(u"at60"_s).toInt() - 60) <= 1);
    QCOMPARE(recorded.value(u"at30"_s).toInt(), 30);
    QCOMPARE(recorded.value(u"still30"_s).toInt(), 30); // invalid values are ignored
    QCOMPARE(recorded.value(u"general"_s).toObject().value(u"fps"_s).toInt(), 30);
}

void TestWebApiScript::applyPropertiesReachesTheListenerAfterStart()
{
    const QJsonObject recorded = runPage(buildWebApiScript(item(), {}),
        u"window.__murasaki.applyProperties({speed: {value: 99}}); recorded.early = recorded.user === undefined;"
        " window.__murasaki.start(30); recorded.initial = recorded.user.speed.value;"
        " window.__murasaki.applyProperties({tint: {value: '0 1 0'}});"_s);
    QVERIFY(recorded.value(u"early"_s).toBool());
    QCOMPARE(recorded.value(u"initial"_s).toInt(), 7);
    const QJsonObject user = recorded.value(u"user"_s).toObject();
    QCOMPARE(user.keys(), QStringList{u"tint"_s}); // only what was pushed
    QCOMPARE(user.value(u"tint"_s).toObject().value(u"value"_s).toString(), u"0 1 0"_s);
}

void TestWebApiScript::applyPropertiesRejectsNonObjects()
{
    const QJsonObject recorded = runPage(buildWebApiScript(item(), {}),
        u"window.__murasaki.start(30); recorded.calls = 0;"
        " window.wallpaperPropertyListener.applyUserProperties = function () { recorded.calls += 1; };"
        " [null, undefined, 3, 'x', true, [1]].forEach(function (v) { window.__murasaki.applyProperties(v); });"
        " recorded.rejected = recorded.calls; window.__murasaki.applyProperties({a: {value: 1}});"
        " window.wallpaperPropertyListener.applyUserProperties = function () { throw new Error('boom'); };"
        " window.__murasaki.applyProperties({a: {value: 2}});" // a throwing page handler must not escape
        " window.wallpaperPropertyListener = null; window.__murasaki.applyProperties({a: {value: 3}})"_s);
    QCOMPARE(recorded.value(u"rejected"_s).toInt(), 0);
    QCOMPARE(recorded.value(u"calls"_s).toInt(), 1);
}

void TestWebApiScript::animationFramesWaitWhilePaused()
{
    // The view stays visible while paused, so the API itself must hold back animation frames.
    const QJsonObject recorded = runPage(u"var nativeCbs = []; recorded.native = 0;"
        " window.requestAnimationFrame = function (cb) { recorded.native += 1; nativeCbs.push(cb); return 1; };"
        " window.cancelAnimationFrame = function () {};"
        " function vsync(t) { var cbs = nativeCbs; nativeCbs = []; cbs.forEach(function (cb) { cb(t); }); }"_s
            + buildWebApiScript(item(), {}),
        u"recorded.draws = 0;"
        " function draw(t) { recorded.draws += 1; requestAnimationFrame(draw); }"
        " window.__murasaki.start(60); requestAnimationFrame(draw); vsync(0); recorded.before = recorded.draws;"
        " window.__murasaki.setPaused(true); vsync(20); vsync(40); recorded.drawsPaused = recorded.draws;"
        " recorded.nativeWhilePaused = nativeCbs.length;"
        " window.__murasaki.setPaused(false); vsync(60); recorded.after = recorded.draws"_s);
    QCOMPARE(recorded.value(u"before"_s).toInt(), 1);
    QCOMPARE(recorded.value(u"drawsPaused"_s).toInt(), 1); // no frames while paused
    QCOMPARE(recorded.value(u"nativeWhilePaused"_s).toInt(), 0); // and no native frame is requested
    QCOMPARE(recorded.value(u"after"_s).toInt(), 2); // the queued callback runs after the unpause
}

void TestWebApiScript::cancelledFramesStayCancelledWhilePaused()
{
    const QJsonObject recorded = runPage(kFakeFrames + buildWebApiScript(item(), {}),
        u"recorded.ran = 0; window.__murasaki.start(60); window.__murasaki.setPaused(true);"
        " var id = requestAnimationFrame(function () { recorded.ran += 1; }); cancelAnimationFrame(id);"
        " var keep = requestAnimationFrame(function () { recorded.kept = true; });"
        " window.__murasaki.setPaused(false); vsync(0); vsync(20); recorded.leftOver = nativeCbs.length"_s);
    QCOMPARE(recorded.value(u"ran"_s).toInt(), 0);
    QVERIFY(recorded.value(u"kept"_s).toBool()); // only the cancelled one is gone
    QCOMPARE(recorded.value(u"leftOver"_s).toInt(), 0);
}

void TestWebApiScript::framesRequestedWhilePausedRunAfterTheUnpause()
{
    const QJsonObject recorded = runPage(kFakeFrames + buildWebApiScript(item(), {}),
        u"recorded.times = []; window.__murasaki.start(60); window.__murasaki.setPaused(true);"
        " requestAnimationFrame(function (t) { recorded.times.push(t); });"
        " recorded.nativeWhilePaused = nativeCbs.length; vsync(5); recorded.whilePaused = recorded.times.length;"
        " window.__murasaki.setPaused(false); recorded.nativeAfter = nativeCbs.length; vsync(40)"_s);
    QCOMPARE(recorded.value(u"nativeWhilePaused"_s).toInt(), 0);
    QCOMPARE(recorded.value(u"whilePaused"_s).toInt(), 0);
    QCOMPARE(recorded.value(u"nativeAfter"_s).toInt(), 1);
    QCOMPARE(recorded.value(u"times"_s).toArray().size(), 1);
    QCOMPARE(recorded.value(u"times"_s).toArray().at(0).toInt(), 40);
}

void TestWebApiScript::setFpsWhilePausedAppliesAfterTheUnpause()
{
    const QJsonObject recorded = runPage(kFakeFrames + buildWebApiScript(item(), {}),
        u"recorded.draws = 0; function draw() { recorded.draws += 1; requestAnimationFrame(draw); }"
        " window.__murasaki.start(60); window.__murasaki.setPaused(true); requestAnimationFrame(draw);"
        " window.__murasaki.setFps(30); recorded.fps = recorded.general.fps;"
        " window.__murasaki.setPaused(false);"
        " for (var i = 0; i < 60; ++i) vsync(i * 16.67)"_s);
    QCOMPARE(recorded.value(u"fps"_s).toInt(), 30); // the page hears about it at once
    QVERIFY(qAbs(recorded.value(u"draws"_s).toInt() - 30) <= 1);
}

void TestWebApiScript::animationFramesBeforeStartAreUncapped()
{
    const QJsonObject recorded = runPage(kFakeFrames + buildWebApiScript(item(), {}),
        u"recorded.draws = 0; function draw() { recorded.draws += 1; requestAnimationFrame(draw); }"
        " requestAnimationFrame(draw); for (var i = 0; i < 10; ++i) vsync(i * 16.67)"_s);
    QCOMPARE(recorded.value(u"draws"_s).toInt(), 10);
}

void TestWebApiScript::pauseStopsAnimationsAndSvg()
{
    const QJsonObject recorded = runPage(
        u"var log = [];"
        " function anim(name, state) { return {playState: state, pause: function () { this.playState = 'paused';"
        "   log.push('pause ' + name); }, play: function () { this.playState = 'running'; log.push('play ' + name); }}; }"
        " var running = anim('running', 'running'), finished = anim('finished', 'finished'),"
        "   byPage = anim('byPage', 'paused'), cancelled = anim('cancelled', 'running');"
        " function svg(name, paused) { return {pauseAnimations: function () { paused = true; log.push('svgpause ' + name); },"
        "   unpauseAnimations: function () { paused = false; log.push('svgresume ' + name); },"
        "   animationsPaused: function () { return paused; }}; }"
        " var svgs = [svg('a', false), svg('b', true), {}];"
        " window.document = {getAnimations: function () { return [running, finished, byPage, cancelled]; },"
        "   querySelectorAll: function (sel) { return sel === 'svg' ? svgs : []; }};"_s
            + buildWebApiScript(item(), {}),
        u"window.__murasaki.start(30); window.__murasaki.setPaused(true); recorded.pausedLog = log.join();"
        " cancelled.playState = 'idle'; log.length = 0;"
        " window.__murasaki.setPaused(false); recorded.resumed = log.join()"_s);
    QCOMPARE(recorded.value(u"pausedLog"_s).toString(), u"pause running,pause cancelled,svgpause a"_s);
    QCOMPARE(recorded.value(u"resumed"_s).toString(), u"play running,svgresume a"_s);
}

void TestWebApiScript::pausedTimersAreThrottledAndKeepTheirIds()
{
    const QJsonObject recorded = runPage(kFakeTimers + buildWebApiScript(item(), {}),
        u"var ticks = 0, once = 0;"
        " var interval = setInterval(function () { ticks += 1; }, 100); window.__murasaki.start(30);"
        " advance(1000); recorded.running = ticks;"
        " window.__murasaki.setPaused(true); ticks = 0; advance(5000); recorded.throttled = ticks;"
        " setTimeout(function () { once += 1; }, 0); advance(500); recorded.earlyOnce = once;"
        " advance(600); recorded.lateOnce = once;"
        " window.__murasaki.setPaused(false); ticks = 0; advance(1000); recorded.resumed = ticks;"
        " window.__murasaki.setPaused(true); clearTimeout(interval); ticks = 0; advance(5000);"
        " recorded.cleared = ticks; recorded.sameIdKind = typeof interval;"
        " var args = null; setTimeout(function (a, b) { args = [a, b, this === window]; }, 0, 'x', 7);"
        " window.__murasaki.setPaused(false); advance(10); recorded.args = args;"
        " var gone = setTimeout(function () { recorded.gone = true; }, 5); clearInterval(gone); advance(20);"
        " var codeString = setTimeout('1', 1); recorded.nativeId = codeString < 1000000000;"
        " recorded.names = [setTimeout.name, setInterval.name, clearTimeout.name, clearInterval.name].join()"_s);
    QCOMPARE(recorded.value(u"running"_s).toInt(), 10);
    QCOMPARE(recorded.value(u"throttled"_s).toInt(), 5); // once per second
    QCOMPARE(recorded.value(u"earlyOnce"_s).toInt(), 0); // even a zero delay waits for the next throttle slot
    QCOMPARE(recorded.value(u"lateOnce"_s).toInt(), 1);
    QCOMPARE(recorded.value(u"resumed"_s).toInt(), 11); // the overdue run at once, then the normal cadence
    QCOMPARE(recorded.value(u"cleared"_s).toInt(), 0); // the id handed out before the pauses still clears it
    const QJsonArray args = recorded.value(u"args"_s).toArray();
    QCOMPARE(args.at(0).toString(), u"x"_s);
    QCOMPARE(args.at(1).toInt(), 7);
    QVERIFY(args.at(2).toBool());
    QVERIFY(!recorded.contains(u"gone"_s)); // clearInterval takes a timeout id too
    QVERIFY(recorded.value(u"nativeId"_s).toBool()); // code strings are left to the native timer
    QCOMPARE(recorded.value(u"names"_s).toString(), u"setTimeout,setInterval,clearTimeout,clearInterval"_s);
}

void TestWebApiScript::audioTickKeepsItsCadenceUnthrottled()
{
    const QJsonObject recorded = runPage(kFakeTimers + buildWebApiScript(item(), {}),
        u"var count = 0; window.wallpaperRegisterAudioListener(function () { count += 1; });"
        " window.__murasaki.start(30); advance(330); recorded.running = count;"
        " window.__murasaki.setPaused(true); advance(5000); recorded.whilePaused = count;"
        " window.__murasaki.setPaused(false); advance(330); recorded.resumed = count - recorded.whilePaused"_s);
    QCOMPARE(recorded.value(u"running"_s).toInt(), 10); // 33 ms, not the page-timer rules
    QCOMPARE(recorded.value(u"whilePaused"_s).toInt(), 10);
    QCOMPARE(recorded.value(u"resumed"_s).toInt(), 10);
}

QTEST_GUILESS_MAIN(TestWebApiScript)
#include "tst_webapiscript.moc"
