#include "webapiscript.h"

#include "pathutil.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

// Wallpaper Engine's page API, reimplemented from its documented behavior. __MURASAKI_PROPERTIES__ is replaced
// with a JS string literal holding the JSON text.
constexpr char16_t kScriptTemplate[] = uR"JS((function () {
  'use strict';
  if (window.__murasaki) return;
  var propertiesJson = __MURASAKI_PROPERTIES__;
  var audioListeners = [];
  var latest = new Array(128).fill(0); // the newest spectrum pushed by the host; silence until the first push
  var started = false, paused = false, timer = 0, currentFps = 0;
  var contexts = [], audios = [], suspendedContexts = [], pausedMedia = [];
  function safely(fn) { try { fn(); } catch (e) { console.error('murasaki: ' + e); } }
  function listener() { return window.wallpaperPropertyListener || null; }
  function tick() {
    audioListeners.forEach(function (cb) { safely(function () { cb(latest.slice()); }); });
  }
  // The page-facing timers are wrapped further down; the audio tick keeps the native ones (it stops on pause itself).
  var nativeSetInterval = window.setInterval, nativeClearInterval = window.clearInterval;
  function schedule() {
    if (timer) { nativeClearInterval(timer); timer = 0; }
    if (started && !paused && audioListeners.length) timer = nativeSetInterval(tick, 33);
  }
  // Pages that play sound themselves must go quiet with the wallpaper: track every AudioContext and Audio they
  // create (weakly where the engine has WeakRef) and, when pausing, the <audio>/<video> elements in the document.
  function ref(o) { return typeof WeakRef === 'function' ? new WeakRef(o) : { deref: function () { return o; } }; }
  function liveItems(list, keep) {
    var items = [];
    for (var i = list.length - 1; i >= 0; --i) {
      var o = list[i].deref();
      if (o && (!keep || keep(o))) items.unshift(o); else list.splice(i, 1);
    }
    return items;
  }
  function notClosed(ctx) { return ctx.state !== 'closed'; }
  function ignoreRejection(result) { if (result && typeof result.catch === 'function') result.catch(function () {}); }
  function pauseContext(ctx) {
    if (ctx.state !== 'running') return;
    safely(function () { ignoreRejection(ctx.suspend()); });
    suspendedContexts.push(ctx);
  }
  function named(Tracked, Original) {
    Object.defineProperty(Tracked, 'name', { value: Original.name, configurable: true });
    return Tracked;
  }
  function trackAudioContext(name) {
    var Original = window[name];
    if (typeof Original !== 'function') return null;
    window[name] = named(class extends Original {
      constructor() {
        super(...arguments);
        if (contexts.length > 32) liveItems(contexts, notClosed);
        contexts.push(ref(this));
        if (paused) pauseContext(this); // created while the wallpaper is paused: stays silent until the unpause
      }
    }, Original);
    return Original;
  }
  var originalContext = trackAudioContext('AudioContext');
  if (originalContext && window.webkitAudioContext === originalContext) window.webkitAudioContext = window.AudioContext;
  else trackAudioContext('webkitAudioContext');
  if (typeof window.Audio === 'function') {
    var OriginalAudio = window.Audio;
    // Audio() is a legacy factory: wrap it with a plain function that shares its prototype.
    var TrackedAudio = function () {
      var el = new OriginalAudio(...arguments);
      if (audios.length > 32) liveItems(audios);
      audios.push(ref(el));
      return el;
    };
    TrackedAudio.prototype = OriginalAudio.prototype;
    window.Audio = named(TrackedAudio, OriginalAudio);
  }
  function mediaElements() {
    var found = liveItems(audios);
    if (typeof document !== 'undefined' && document && typeof document.querySelectorAll === 'function') {
      safely(function () {
        Array.prototype.forEach.call(document.querySelectorAll('audio,video'), function (el) {
          if (found.indexOf(el) < 0) found.push(el);
        });
      });
    }
    return found;
  }
  function pauseMedia() {
    liveItems(contexts, notClosed).forEach(pauseContext);
    mediaElements().forEach(function (el) {
      if (el.paused) return;
      safely(function () {
        el.pause();
        pausedMedia.push({ el: el, attached: el.isConnected !== false });
      });
    });
  }
  function resumeMedia() {
    var ctxs = suspendedContexts, media = pausedMedia;
    suspendedContexts = []; pausedMedia = [];
    ctxs.forEach(function (ctx) {
      if (ctx.state === 'suspended') safely(function () { ignoreRejection(ctx.resume()); });
    });
    media.forEach(function (entry) {
      var el = entry.el;
      if (!el.paused) return; // the page already resumed it
      if (entry.attached && el.isConnected === false) return; // removed from the document meanwhile
      safely(function () { ignoreRejection(el.play()); });
    });
  }
  // A paused wallpaper stays visible, so Chromium does not throttle it like a hidden page. CSS animations,
  // transitions, Web Animations and SMIL are paused here instead; only the ones that were running are resumed.
  var pausedAnimations = [], pausedSvgs = [];
  function pauseAnimations() {
    if (typeof document === 'undefined' || !document) return;
    if (typeof document.getAnimations === 'function') {
      safely(function () {
        document.getAnimations().forEach(function (animation) {
          if (animation.playState !== 'running') return;
          safely(function () { animation.pause(); pausedAnimations.push(animation); });
        });
      });
    }
    if (typeof document.querySelectorAll === 'function') {
      safely(function () {
        Array.prototype.forEach.call(document.querySelectorAll('svg'), function (svg) {
          if (typeof svg.pauseAnimations !== 'function') return;
          if (typeof svg.animationsPaused === 'function' && svg.animationsPaused()) return;
          safely(function () { svg.pauseAnimations(); pausedSvgs.push(svg); });
        });
      });
    }
  }
  function resumeAnimations() {
    var animations = pausedAnimations, svgs = pausedSvgs;
    pausedAnimations = []; pausedSvgs = [];
    animations.forEach(function (animation) {
      if (animation.playState === 'paused') safely(function () { animation.play(); }); // not cancelled meanwhile
    });
    svgs.forEach(function (svg) {
      if (typeof svg.unpauseAnimations === 'function') safely(function () { svg.unpauseAnimations(); });
    });
  }
  // Timers: while paused a callback runs at most once per kThrottleMs, like Chromium's hidden-page throttling. The
  // wrappers hand out their own ids (from kFirstTimerId up, clear of the native ones) so a timer can be re-armed
  // when the pause changes without its id changing; clearTimeout/clearInterval take either kind.
  var kThrottleMs = 1000, kFirstTimerId = 1000000000;
  var nativeSetTimeout = window.setTimeout, nativeClearTimeout = window.clearTimeout;
  var timers = new Map(), nextTimerId = kFirstTimerId;
  var rearmTimers = function () {};
  if (typeof nativeSetTimeout === 'function' && typeof nativeClearTimeout === 'function'
      && typeof nativeSetInterval === 'function' && typeof nativeClearInterval === 'function') {
    var fireTimer = null;
    var armTimer = function (t) {
      if (t.native) nativeClearTimeout(t.native);
      var wait = Math.max(0, t.due - Date.now());
      if (paused) wait = Math.max(wait, kThrottleMs);
      t.native = nativeSetTimeout(function () { fireTimer(t); }, wait);
    };
    fireTimer = function (t) {
      t.native = 0;
      if (!timers.has(t.id)) return;
      if (t.repeat) { t.due = Date.now() + t.delay; armTimer(t); } else timers.delete(t.id);
      try { t.fn.apply(window, t.args); } catch (e) { nativeSetTimeout(function () { throw e; }); }
    };
    var addTimer = function (repeat, fn, delay, args) {
      if (typeof fn !== 'function') return null; // a code string: left to the native timer
      var d = Number(delay);
      d = isFinite(d) && d > 0 ? d : 0;
      var t = { id: nextTimerId++, fn: fn, args: args, repeat: repeat, delay: repeat ? Math.max(d, 4) : d, native: 0 };
      t.due = Date.now() + t.delay;
      timers.set(t.id, t);
      armTimer(t);
      return t.id;
    };
    var clearTimer = function (native) {
      return function (id) {
        var t = timers.get(id);
        if (!t) { native(id); return; }
        if (t.native) nativeClearTimeout(t.native);
        timers.delete(t.id);
      };
    };
    window.setTimeout = named(function setTimeout(fn, delay) {
      var id = addTimer(false, fn, delay, Array.prototype.slice.call(arguments, 2));
      return id === null ? nativeSetTimeout.apply(window, arguments) : id;
    }, nativeSetTimeout);
    window.setInterval = named(function setInterval(fn, delay) {
      var id = addTimer(true, fn, delay, Array.prototype.slice.call(arguments, 2));
      return id === null ? nativeSetInterval.apply(window, arguments) : id;
    }, nativeSetInterval);
    window.clearTimeout = named(clearTimer(nativeClearTimeout), nativeClearTimeout);
    window.clearInterval = named(clearTimer(nativeClearInterval), nativeClearInterval);
    rearmTimers = function () { timers.forEach(armTimer); };
  }
  // WE caps a web wallpaper at its FPS setting; pages that draw on every requestAnimationFrame otherwise render at
  // the display rate. Callbacks queue up and run together on the first native frame once frameMs has passed.
  // While the wallpaper is paused no frame runs: the queued callbacks wait for the unpause, so canvas animations cost
  // nothing although the view stays visible (hiding it would shrink the page's viewport to 0x0).
  var frameMs = 0, lastFrame = -Infinity, nextFrameId = 1, pendingFrames = new Map(), nativeFrame = 0;
  var wakeFrames = function () {};
  if (typeof window.requestAnimationFrame === 'function' && typeof window.cancelAnimationFrame === 'function') {
    var nativeRequest = window.requestAnimationFrame.bind(window);
    var pumpFrames = function (now) {
      nativeFrame = 0;
      if (paused) return; // the unpause calls wakeFrames()
      if (now - lastFrame < frameMs - 1) { nativeFrame = nativeRequest(pumpFrames); return; } // 1 ms vsync jitter
      // Step on a fixed grid so a 60 Hz display yields 30 fps, not 20; after a stall restart from now.
      lastFrame = frameMs && now - lastFrame < 2 * frameMs ? lastFrame + frameMs : now;
      var batch = pendingFrames;
      pendingFrames = new Map();
      batch.forEach(function (cb) {
        try { cb(now); } catch (e) { setTimeout(function () { throw e; }); } // reported like a native callback
      });
    };
    window.requestAnimationFrame = function requestAnimationFrame(cb) {
      var id = nextFrameId++;
      pendingFrames.set(id, cb);
      if (!nativeFrame && !paused) nativeFrame = nativeRequest(pumpFrames);
      return id;
    };
    wakeFrames = function () {
      if (!nativeFrame && pendingFrames.size) nativeFrame = nativeRequest(pumpFrames);
    };
    window.cancelAnimationFrame = function cancelAnimationFrame(id) { pendingFrames.delete(id); };
  }
  window.wallpaperRegisterAudioListener = function (cb) {
    if (typeof cb === 'function') { audioListeners.push(cb); schedule(); }
  };
  ['wallpaperRegisterMediaStatusListener', 'wallpaperRegisterMediaPropertiesListener',
   'wallpaperRegisterMediaThumbnailListener', 'wallpaperRegisterMediaPlaybackListener',
   'wallpaperRegisterMediaTimelineListener', 'wallpaperRequestRandomFileForProperty'].forEach(function (name) {
    if (typeof window[name] !== 'function') window[name] = function () {};
  });
  Object.defineProperty(window, '__murasaki', { value: Object.freeze({
    start: function (fps) {
      if (started) return;
      started = true;
      if (fps > 0) { frameMs = 1000 / fps; currentFps = fps; }
      var l = listener();
      if (l && typeof l.applyUserProperties === 'function')
        safely(function () { l.applyUserProperties(JSON.parse(propertiesJson)); }); // fresh copy: the page may mutate it
      if (l && typeof l.applyGeneralProperties === 'function')
        safely(function () { l.applyGeneralProperties({ fps: fps }); });
      if (paused && l && typeof l.setPaused === 'function') safely(function () { l.setPaused(true); });
      schedule();
    },
    // Host change of the FPS limit while the page runs. Before start() the limit comes from start() itself.
    setFps: function (fps) {
      if (!started || typeof fps !== 'number' || !isFinite(fps) || fps <= 0 || fps === currentFps) return;
      currentFps = fps;
      frameMs = 1000 / fps;
      var l = listener();
      if (l && typeof l.applyGeneralProperties === 'function')
        safely(function () { l.applyGeneralProperties({ fps: fps }); });
    },
    // Host push of changed user properties, {key: {value}} like the initial call. Ignored before start().
    applyProperties: function (props) {
      if (!started || !props || typeof props !== 'object' || Array.isArray(props)) return;
      var l = listener();
      if (l && typeof l.applyUserProperties === 'function') safely(function () { l.applyUserProperties(props); });
    },
    // Host push of the 128-value spectrum. Anything but an array(-like) of 128 finite numbers is ignored.
    audio: function (samples) {
      if (!samples || samples.length !== 128) return;
      var copy = new Array(128);
      for (var i = 0; i < 128; ++i) {
        var v = samples[i];
        if (typeof v !== 'number' || !isFinite(v)) return;
        copy[i] = v;
      }
      latest = copy;
    },
    listening: function () { return audioListeners.length > 0; },
    setPaused: function (value) {
      paused = !!value;
      var l = listener();
      if (l && typeof l.setPaused === 'function') safely(function () { l.setPaused(paused); });
      // After the page's own handler, which may pause or resume by itself.
      if (paused) { pauseMedia(); pauseAnimations(); } else { resumeMedia(); resumeAnimations(); }
      rearmTimers();
      if (!paused) wakeFrames();
      schedule();
    }
  }) });
})();
)JS";

// The payload is embedded as a JS string literal that start() hands to JSON.parse: JSON.parse keeps a "__proto__"
// key as an own property (an object literal would set the prototype). The JSON text is serialized as a JSON
// string, which escapes backslash, quote and control characters; U+2028/2029 are line terminators in older
// engines, and "</" must never close a <script> element if this text is ever inlined into HTML.
QString jsLiteral(const QJsonObject &object)
{
    const QString jsonText = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
    QString literal =
        QString::fromUtf8(QJsonDocument(QJsonArray{jsonText}).toJson(QJsonDocument::Compact)); // ["..."]
    literal = literal.mid(1, literal.size() - 2);
    literal.replace(QChar(0x2028), u"\\u2028"_s);
    literal.replace(QChar(0x2029), u"\\u2029"_s);
    literal.replace(u"</"_s, u"<\\/"_s);
    return literal;
}

// A "file" or "directory" value as Wallpaper Engine hands it to pages: an absolute path without the leading
// slash (Windows' "C:/..." shape), since pages build URLs with "file:///" + value. Relative values (presets ship
// them, e.g. "files/image.jpg") resolve against the preset's dir, then the base item's; one that resolves
// nowhere, or escapes those dirs, becomes "". Absolute values are the user's own choice and are kept.
QJsonValue resolvePathValue(const QJsonObject &item, const QString &type, const QJsonValue &value)
{
    const QString text = value.toString();
    if (text.isEmpty())
        return value;
    if (QDir::isAbsolutePath(text))
        return text.mid(1);
    for (const QString &base : {item.value(u"dir"_s).toString(), item.value(u"contentDir"_s).toString()}) {
        if (base.isEmpty())
            continue;
        QString resolved;
        if (type == u"file") {
            resolved = containedExistingFile(base, text);
        } else {
            const QString canonicalBase = QFileInfo(base).canonicalFilePath();
            const QString lexical = canonicalBase.isEmpty() ? QString() : confinedPath(canonicalBase, text);
            const QFileInfo info(lexical);
            if (!lexical.isEmpty() && info.isDir() && info.canonicalFilePath().startsWith(canonicalBase + u'/'))
                resolved = info.canonicalFilePath();
        }
        if (!resolved.isEmpty())
            return resolved.mid(1);
    }
    return QString();
}

} // namespace

QJsonObject webUserProperties(const QJsonObject &item, const QJsonObject &overrides)
{
    const QJsonObject preset = item.value(u"presetValues"_s).toObject();
    QJsonObject result;
    for (const QJsonValue &entry : item.value(u"properties"_s).toArray()) {
        const QJsonObject property = entry.toObject();
        const QString key = property.value(u"key"_s).toString();
        const QString type = property.value(u"type"_s).toString();
        if (key.isEmpty() || type == u"label" || type == u"group")
            continue;
        QJsonValue value = property.value(u"value"_s);
        for (const QJsonObject *layer : {&preset, &overrides}) {
            const QJsonValue candidate = layer->value(key);
            if (!candidate.isUndefined() && !candidate.isNull())
                value = candidate;
        }
        if (type == u"file" || type == u"directory")
            value = resolvePathValue(item, type, value);
        result.insert(key, QJsonObject{{u"value"_s, value}});
    }
    return result;
}

QString buildWebApiScript(const QJsonObject &item, const QJsonObject &overrides)
{
    QString script = QString::fromUtf16(kScriptTemplate);
    script.replace(u"__MURASAKI_PROPERTIES__"_s, jsLiteral(webUserProperties(item, overrides)));
    return script;
}

} // namespace murasaki
