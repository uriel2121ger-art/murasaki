#include "script/prelude.h"

namespace murasaki::scene {

const char *const kScriptPrelude =
#include "script/prelude_math.inc"
R"JS(
'use strict';
(function (global, __host) {
  function num(v, d) { return typeof v === 'number' && isFinite(v) ? v : d; }
  const __storage = global.__storage; // the host's store of localStorage, shared by every script
  delete global.__storage;
  const { Vec2, Vec3, Vec4, Mat3, Mat4, WEMath, WEColor, WEVector } = global; // defined by the math part above

  global.console = {
    log(...a) { __host.log('log', a.map(String).join(' ')); },
    warn(...a) { __host.log('warn', a.map(String).join(' ')); },
    error(...a) { __host.log('error', a.map(String).join(' ')); },
    info(...a) { __host.log('log', a.map(String).join(' ')); },
    debug(...a) { __host.log('log', a.map(String).join(' ')); },
  };

  // ---- layers: proxies of the host's objects ----
  const names = ['origin', 'scale', 'angles', 'color', 'size', 'parallaxDepth', 'alpha', 'visible', 'text', 'pointsize', 'font',
                 'alignment', 'brightness', 'name', 'rate', 'count', 'lifetime', 'speed', 'solid', 'perspective', 'opaquebackground',
                 'backgroundcolor', 'padding', 'horizontalalign', 'verticalalign', 'anchor', 'limitrows', 'maxrows', 'limitwidth',
                 'maxwidth', 'volume'];
  const fromHost = (v) => {
    if (v === undefined) return undefined;
    if (Array.isArray(v)) return v.length === 2 ? new Vec2(v[0], v[1]) : v.length === 3 ? new Vec3(v[0], v[1], v[2]) : new Vec4(v[0], v[1], v[2], v[3]);
    return v;
  };
  const toHost = (value) => {
    if (value !== null && typeof value === 'object') {
      if (value instanceof Vec2) return [value.x, value.y];
      if (value instanceof Vec3) return [value.x, value.y, value.z];
      if (value instanceof Vec4) return [value.x, value.y, value.z, value.w];
    }
    return value;
  };
  const asId = (x) => {
    if (x === null || x === undefined) return -1;
    if (typeof x === 'object' && x.__id !== undefined) return x.__id;
    if (typeof x === 'number') { const all = __host.enumerate(); return x >= 0 && x < all.length ? all[x] : -1; }
    return __host.findLayer(String(x));
  };

  class Effect {
    constructor(layer, index) { Object.defineProperty(this, '__layer', { value: layer }); Object.defineProperty(this, '__index', { value: index }); }
    get name() { return __host.effectGet(this.__layer, this.__index, 'name'); }
    set name(v) {}
    get visible() { return !!__host.effectGet(this.__layer, this.__index, 'visible'); }
    set visible(v) { __host.effectSet(this.__layer, this.__index, 'visible', !!v); }
    getMaterialCount() { return __host.effectGet(this.__layer, this.__index, 'passes') | 0; }
    // a pass's material: its properties (the constants a scene sets by their `material` key) read and write through
    getMaterial(index) {
      if (typeof index !== 'number' || index < 0 || index >= this.getMaterialCount()) return null;
      const layer = this.__layer, fx = this.__index;
      return new Proxy({}, {
        get(t, key) { return typeof key === 'string' ? fromHost(__host.effectGet(layer, fx, 'constant:' + index + ':' + key)) : undefined; },
        set(t, key, value) { if (typeof key === 'string') __host.effectSet(layer, fx, 'constant:' + index + ':' + key, toHost(value)); return true; },
      });
    }
    setMaterialProperty(name, value) { __host.effectSet(this.__layer, this.__index, 'constant:*:' + name, toHost(value)); }
    executeMaterialFunction(name) {}
  }

  // an animation layer of a puppet: weight, speed and visibility are the object's own animationlayers entries
  class AnimationLayer {
    constructor(layer, index) { Object.defineProperty(this, '__layer', { value: layer }); Object.defineProperty(this, '__index', { writable: true, value: index }); this.__callbacks = []; }
    __get(f) { return __host.get(this.__layer, 'alayer.' + this.__index + '.' + f); }
    __set(f, v) { __host.set(this.__layer, 'alayer.' + this.__index + '.' + f, v); }
    get fps() { return this.__get('fps'); }
    get frameCount() { return this.__get('frameCount'); }
    get duration() { return this.__get('duration'); }
    get name() { return this.__get('name'); }
    get rate() { return this.__get('rate'); } set rate(v) { this.__set('rate', v); }
    get blend() { return this.__get('blend'); } set blend(v) { this.__set('blend', v); }
    get visible() { return this.__get('visible'); } set visible(v) { this.__set('visible', !!v); }
    play() { this.visible = true; if (this.rate === 0) this.rate = 1; }
    stop() { this.visible = false; }
    pause() { this.rate = 0; }
    isPlaying() { return this.visible && this.rate !== 0; }
    // the frame runs on the engine's clock; setting one moves the layer's animation to it. Animations loop, so they never end
    getFrame() { return this.__get('frame'); }
    setFrame(frame) { this.__set('frame', num(frame, 0)); }
    addEndedCallback(callback) { if (typeof callback === 'function') this.__callbacks.push(callback); }
  }

  // a keyframe animation of the object's properties (the editor's timeline): it plays on the scene clock unless the file starts it
  // paused; play, pause, stop, rate and setFrame steer its own clock, and a track linked to it (`options.parent`) follows
  class Animation {
    constructor(layer, index) { Object.defineProperty(this, '__layer', { value: layer }); Object.defineProperty(this, '__index', { value: index }); }
    __get(f) { return __host.get(this.__layer, 'animation.' + this.__index + '.' + f); }
    __set(f, v) { __host.set(this.__layer, 'animation.' + this.__index + '.' + f, v); }
    get fps() { return this.__get('fps'); }
    get frameCount() { return this.__get('frameCount'); }
    get duration() { return this.__get('duration'); }
    get name() { return this.__get('name'); }
    get rate() { return this.__get('rate'); }
    set rate(v) { this.__set('rate', num(v, 1)); }
    play() { this.__set('play', true); }
    stop() { this.__set('stop', true); }
    pause() { this.__set('pause', true); }
    isPlaying() { return !!this.__get('playing'); }
    getFrame() { return this.__get('frame'); }
    setFrame(frame) { this.__set('frame', num(frame, 0)); }
  }
  const animationOf = (id, key) => { const i = __host.get(id, key); return typeof i === 'number' && i >= 0 ? new Animation(id, i) : null; };

  class TextureAnimation {
    constructor(id) { this.rate = 1; this.__playing = true; this.__frame = 0; Object.defineProperty(this, '__id', { value: id }); }
    get frameCount() { return __host.get(this.__id, 'textureFrameCount') || 1; }
    get duration() { return 0; }
    play() { this.__playing = true; }
    stop() { this.__playing = false; this.__frame = 0; }
    pause() { this.__playing = false; }
    isPlaying() { return this.__playing; }
    getFrame() { return this.__frame; }
    // the sprite sheet then shows that frame and stays on it
    setFrame(frame) { this.__frame = Math.max(0, num(frame, 0) | 0); __host.set(this.__id, 'textureFrame', this.__frame); }
    join() {}
  }

  // a video texture's player (IVideoTexture): the object is the layer's own, made when the script first asks for it. The engine's player
  // does the work (the host's "video.*" properties of the layer); the callbacks of addEndedCallback are called from the frame of the scripts
  // that tick, once for every time the video reached its end since the callback was added.
  const videoWatchers = [];
  class VideoTexture {
    constructor(id) { Object.defineProperty(this, '__id', { value: id }); }
    get duration() { return num(__host.get(this.__id, 'video.duration'), 0); }
    get rate() { return num(__host.get(this.__id, 'video.rate'), 1); }
    set rate(v) { __host.set(this.__id, 'video.rate', num(v, 1)); }
    get loop() { return !!__host.get(this.__id, 'video.loop'); }
    set loop(v) { __host.set(this.__id, 'video.loop', !!v); }
    play() { __host.set(this.__id, 'video.play', true); }
    pause() { __host.set(this.__id, 'video.pause', true); }
    stop() { __host.set(this.__id, 'video.stop', true); }
    isPlaying() { return !!__host.get(this.__id, 'video.playing'); }
    getCurrentTime() { return num(__host.get(this.__id, 'video.time'), 0); }
    setCurrentTime(time) { __host.set(this.__id, 'video.seek', num(time, 0)); }
    addEndedCallback(callback) {
      if (typeof callback !== 'function' || videoWatchers.length >= 256) return;
      videoWatchers.push({ id: this.__id, callback, seen: num(__host.get(this.__id, 'video.ended'), 0) });
      __host.wake();
    }
  }
  const dispatchVideoEnds = () => {
    for (const w of videoWatchers.slice()) {
      const ended = num(__host.get(w.id, 'video.ended'), 0);
      for (let fired = 0; w.seen < ended && fired < 4; ++fired) {
        ++w.seen;
        try { w.callback(); } catch (e) { console.warn('video ended callback: ' + e); }
      }
      w.seen = Math.max(w.seen, ended);
    }
  };

  class Instance {
    constructor(id) { Object.defineProperty(this, '__id', { value: id }); }
  }
  for (const n of ['alpha', 'size', 'count', 'speed', 'lifetime', 'rate', 'colorn', 'controlpoint0', 'controlpoint1', 'controlpoint2', 'controlpoint3',
                   'controlpoint4', 'controlpoint5', 'controlpoint6', 'controlpoint7']) {
    Object.defineProperty(Instance.prototype, n, { get() { return fromHost(__host.get(this.__id, n)); }, set(v) { __host.set(this.__id, n, toHost(v)); }, enumerable: true });
  }

  // a scene object: one per object in a realm, so a property one script defines on its `thisLayer` is seen by the scripts that get the
  // same object from thisScene.getLayer (the library's icon launchers set `clicked` and `hovered` that way)
  class Layer {
    constructor(id) { Object.defineProperty(this, '__id', { value: id }); }
    // a keyframe animation of the object by its name, or its first one; null when it has none (as WE)
    getAnimation(name) { return animationOf(this.__id, 'animation:' + (name === undefined || name === null ? '' : String(name))); }
    // the object's world matrix: the position is in m[12], m[13], m[14]
    getTransformMatrix() { return new Mat4(__host.matrix(this.__id)); }
    // turn the layer so that its +z axis points at `center` (a world position), `up` (default +y) being the way its +y leans; the layer's
    // own angles are set, so a parent's rotation is not compensated. lookAtYaw only turns it around `up`: it never faces up or down
    lookAt(center, up) { this.__face(center, up, false); }
    lookAtYaw(center, up) { this.__face(center, up, true); }
    __face(center, up, yawOnly) {
      if (center === null || typeof center !== 'object') return;
      const u0 = up === null || typeof up !== 'object' ? new Vec3(0, 1, 0) : new Vec3(up);
      if (!(u0.length() > 0)) return;
      const upn = u0.normalize();
      let f = new Vec3(center).subtract(new Vec3(this.origin));
      if (yawOnly) f = f.subtract(upn.multiply(f.dot(upn)));
      if (!(f.length() > 1e-9)) return;
      f = f.normalize();
      const r = upn.cross(f);
      if (!(r.length() > 1e-9)) return;
      const right = r.normalize();
      this.angles = Mat4.fromBasis(right, f.cross(right), f).extractEuler();
    }
    rotateObjectSpace(angles) { this.angles = Mat4.fromEuler(this.angles).multiply(Mat4.fromEuler(angles)).extractEuler(); }
    setParent(parent, a, b) {
      const keep = typeof a === 'boolean' ? a : b === true;
      __host.setParent(this.__id, asId(parent), keep);
    }
    getParent() { const id = __host.parent(this.__id); return id < 0 ? null : layerOf(id); }
    getChildren() { return __host.children(this.__id).map((id) => layerOf(id)); }
    // attachments belong to puppet and 3D models; the models this engine draws have none
    getAttachmentIndex(name) { return -1; }
    getAttachmentMatrix(attachment) { return new Mat4(); }
    getAttachmentOrigin(attachment) { return new Vec3(0, 0, 0); }
    getAttachmentAngles(attachment) { return new Vec3(0, 0, 0); }
    // image and text layers
    getEffectCount() { return __host.effectCount(this.__id); }
    getEffect(which) {
      const n = this.getEffectCount();
      let i = typeof which === 'number' ? which : -1;
      if (typeof which === 'string') for (let k = 0; k < n; ++k) if (new Effect(this.__id, k).name === which) { i = k; break; }
      return i >= 0 && i < n ? new Effect(this.__id, i) : null;
    }
    transformAttachmentToTexture(attachmentLayer, attachmentName) { return new Mat3(); }
    // texture animations (sprite sheets) run on the engine's clock and cannot be steered: the object keeps its own state so scripts
    // that drive it (play, rate, setFrame) run on without effect
    getTextureAnimation() { return this.__textureAnimation || (Object.defineProperty(this, '__textureAnimation', { value: new TextureAnimation(this.__id) }), this.__textureAnimation); }
    // the video texture of the layer's albedo, the same object every time; null when the albedo is no video (as WE)
    getVideoTexture() {
      if (this.__videoTexture) return this.__videoTexture;
      if (!__host.get(this.__id, 'video.present')) return null;
      Object.defineProperty(this, '__videoTexture', { value: new VideoTexture(this.__id) });
      return this.__videoTexture;
    }
    // puppet animation layers
    getAnimationLayerCount() { return __host.get(this.__id, 'alayers') | 0; }
    getAnimationLayer(which) {
      const n = this.getAnimationLayerCount();
      let i = typeof which === 'number' ? which : -1;
      if (typeof which === 'string') for (let k = 0; k < n; ++k) if (new AnimationLayer(this.__id, k).name === which) { i = k; break; }
      return i >= 0 && i < n ? new AnimationLayer(this.__id, i) : null;
    }
    createAnimationLayer(animation) {
      const before = this.getAnimationLayerCount();
      const config = typeof animation === 'object' && animation !== null && !(animation instanceof AnimationLayer) ? animation : { animation };
      __host.set(this.__id, 'alayer.create', JSON.stringify(config));
      return this.getAnimationLayerCount() > before ? new AnimationLayer(this.__id, before) : null;
    }
    playSingleAnimation(animation, config) {
      return this.createAnimationLayer(Object.assign({}, typeof animation === 'object' && animation !== null ? animation : { animation }, config || {}));
    }
    destroyAnimationLayer(layer) {
      const before = this.getAnimationLayerCount();
      let i = layer instanceof AnimationLayer ? layer.__index : (this.getAnimationLayer(layer) || {}).__index;
      if (i === undefined) return false;
      __host.set(this.__id, 'alayer.destroy', i);
      return this.getAnimationLayerCount() < before;
    }
    // bones: the skeleton is readable; poses are the animations', so the setters change nothing
    getBoneCount() { return __host.get(this.__id, 'boneCount') | 0; }
    getBoneIndex(name) { const i = __host.get(this.__id, 'boneIndex:' + name); return i === undefined ? -1 : i; }
    getBoneParentIndex(child) {
      const i = typeof child === 'number' ? child : this.getBoneIndex(child);
      const p = i < 0 ? undefined : __host.get(this.__id, 'bone.' + i + '.parent');
      return p === undefined ? -1 : p;
    }
    getBoneTransform(bone) { return new Mat4(); }
    setBoneTransform(bone, transform) {}
    getLocalBoneTransform(bone) { return new Mat4(); }
    setLocalBoneTransform(bone, transform) {}
    getLocalBoneAngles(bone) { return new Vec3(0, 0, 0); }
    setLocalBoneAngles(bone, angles) {}
    getLocalBoneOrigin(bone) { return new Vec3(0, 0, 0); }
    setLocalBoneOrigin(bone, origin) {}
    // the spring bones of a puppet (its constraints' `r` and `t` flags) take the impulses; a bone that does not simulate ignores them
    applyBonePhysicsImpulse(bone, directionalImpulse, angularImpulse) {
      const i = typeof bone === 'number' ? bone : this.getBoneIndex(bone);
      const v = (a) => a ? [num(a.x, 0), num(a.y, 0), num(a.z, 0)] : [0, 0, 0];
      if (i >= 0) __host.set(this.__id, 'bonePhysics.impulse', JSON.stringify([i, v(directionalImpulse), v(angularImpulse)]));
    }
    // one bone back to the animated pose at rest, or all of them without an argument
    resetBonePhysicsSimulation(bone) {
      const i = bone === undefined ? -1 : typeof bone === 'number' ? bone : this.getBoneIndex(bone);
      if (i < -1 || (bone !== undefined && i < 0) || i !== i) return; // a bone that does not resolve resets nothing
      __host.set(this.__id, 'bonePhysics.reset', i);
    }
    getBlendShapeIndex(name) { return -1; }
    getBlendShapeWeight(blendShape) { return 0; }
    setBlendShapeWeight(blendShape, weight) {}
    // particle systems and sound objects
    get instance() { return new Instance(this.__id); }
    play() { if (__host.get(this.__id, 'volume') !== undefined) __host.set(this.__id, 'playing', true); else __host.set(this.__id, 'psystem.play', true); }
    pause() { if (__host.get(this.__id, 'volume') !== undefined) __host.set(this.__id, 'playing', false); else __host.set(this.__id, 'psystem.pause', true); }
    stop() { if (__host.get(this.__id, 'volume') !== undefined) __host.set(this.__id, 'playing', false); else __host.set(this.__id, 'psystem.stop', true); }
    isPlaying() { return !!__host.get(this.__id, 'playing'); }
    emitParticles(count) { __host.set(this.__id, 'psystem.emit', typeof count === 'number' ? count : 1); }
  }
  for (const n of names) {
    Object.defineProperty(Layer.prototype, n, {
      get() { return fromHost(__host.get(this.__id, n)); },
      set(value) { __host.set(this.__id, n, toHost(value)); },
      enumerable: true,
    });
  }
  class CameraTransforms {
    constructor(eye, center, up, zoom) { this.eye = eye; this.center = center; this.up = up; this.zoom = zoom; }
  }
  global.CameraTransforms = CameraTransforms;
  // a custom model's geometry: this engine draws no such models, so the data is only kept
  class ModelData {
    applyData(shapes) { this.__shapes = shapes; }
    replaceData(shapes) { this.__shapes = shapes; }
  }
  const vertexAttributes = { POSITION: 'position', NORMAL: 'normal', UV: 'uv', TANGENT_SIGNED: 'tangentsigned', COLOR: 'color' };
  Object.assign(ModelData.prototype, vertexAttributes);
  // scripts name the attributes on the class too (IModelData.POSITION, as the reference's own example does)
  global.IModelData = Object.freeze(Object.assign({}, vertexAttributes));

  const layers = new Map();
  function layerOf(id) {
    let l = layers.get(id);
    if (!l) { l = new Layer(id); layers.set(id, l); }
    return l;
  }

  class Scene {
    getLayer(which) { const id = asId(which); return id < 0 || __host.get(id, 'destroyed') ? null : layerOf(id); }
    // by the object's id in the scene file, as a string ('92001') or a number; null when there is none (or it was destroyed)
    getLayerByID(id) {
      const n = typeof id === 'string' && /^\s*\d+\s*$/.test(id) ? Number(id) : typeof id === 'number' ? id : -1;
      const handle = Number.isInteger(n) ? __host.findLayerById(n) : -1;
      return handle < 0 || __host.get(handle, 'destroyed') ? null : layerOf(handle);
    }
    // the animation of that name on the first layer that has one; a name is needed (a scene is not bound to a property)
    getAnimation(name) {
      if (name === undefined || name === null) return null;
      for (const id of __host.enumerate()) { const a = animationOf(id, 'animation:' + String(name)); if (a) return a; }
      return null;
    }
    getLayerCount() { return __host.enumerate().length; }
    enumerateLayers() { return __host.enumerate().map((id) => layerOf(id)); }
    destroyLayer(which) { const id = asId(which); return id < 0 ? false : __host.destroy(id); }
    createLayer(spec) {
      if (spec !== null && typeof spec === 'object' && spec.__asset !== undefined) spec = spec.__asset;
      const id = __host.createLayer(typeof spec === 'string' ? spec : JSON.stringify(spec), typeof spec !== 'string');
      return id < 0 ? null : layerOf(id);
    }
    getLayerIndex(layer) { const id = asId(layer); return id < 0 ? -1 : __host.layerIndex(id); }
    sortLayer(layer, index) { const id = asId(layer); if (id < 0) return false; __host.sortLayer(id, index); return true; }
    getInitialLayerConfig(layer) {
      const id = asId(layer);
      const text = id < 0 ? undefined : __host.get(id, 'initialConfig');
      return text === undefined ? null : JSON.parse(text);
    }
    getCameraTransforms() {
      const g = (n) => fromHost(__host.get(-1, 'scene.' + n));
      return new CameraTransforms(g('camera.eye'), g('camera.center'), g('camera.up'), g('camera.zoom'));
    }
    setCameraTransforms(t) {
      if (!t) return;
      for (const [k, v] of [['eye', t.eye], ['center', t.center], ['up', t.up], ['zoom', t.zoom]]) if (v !== undefined) __host.set(-1, 'scene.camera.' + k, toHost(v));
    }
    createModelData(configuration) { const d = new ModelData(); d.__config = configuration; return d; }
    destroyModelData(handle) {}
  }
  for (const n of ['bloom', 'bloomstrength', 'bloomthreshold', 'clearenabled', 'clearcolor', 'ambientcolor', 'skylightcolor', 'fov', 'nearz', 'farz',
                   'camerafade', 'camerashake', 'camerashakespeed', 'camerashakeamplitude', 'camerashakeroughness', 'cameraparallax',
                   'cameraparallaxamount', 'cameraparallaxdelay', 'cameraparallaxmouseinfluence']) {
    Object.defineProperty(Scene.prototype, n, { get() { return fromHost(__host.get(-1, 'scene.' + n)); }, set(v) { __host.set(-1, 'scene.' + n, toHost(v)); }, enumerable: true });
  }
  const scene = new Scene();

  // ---- engine: one per script (its timers, audio buffers and user properties), its methods on the prototype ----
  // the clock, the canvas and the cursor are read from the host when a script asks, not copied into the realm every frame (a scene
  // has a thousand scripts): the host keeps one set of numbers per frame
  const F = (i) => __host.frame(i);
  class Engine {
    constructor(scope) {
      this.userProperties = {};
      // these three are called detached (`const later = engine.setTimeout`, the global setTimeout): closures over the script's scope
      this.registerAudioBuffers = function (resolution) {
        const n = resolution === 64 ? 64 : resolution === 32 ? 32 : 16;
        const buf = { resolution: n, left: new Float32Array(n), right: new Float32Array(n), average: new Float32Array(n) };
        scope.audioBuffers.push(buf);
        __host.wake(true);
        return buf;
      };
      this.setTimeout = function (callback, delay) { return scope.addTimer(callback, delay, false); };
      this.setInterval = function (callback, delay) { return scope.addTimer(callback, delay, true); };
    }
    isDesktopDevice() { return true; }
    isMobileDevice() { return false; }
    isWallpaper() { return true; }
    isScreensaver() { return false; }
    isRunningInEditor() { return false; }
    isPortrait() { return F(9) > F(8); }
    isLandscape() { return F(8) >= F(9); }
    // asks the host to start the application the user chose for the property `name`: true when the request was sent, false when no action has
    // been performed (not a property, no application chosen, no host to ask)
    openUserShortcut(name) { return typeof name === 'string' && name.length > 0 && __host.get(-1, 'shortcut.launch:' + name) === true; }
    // the file is only a note for the Workshop export; the handle is what createLayer takes
    registerAsset(file) { return { __asset: String(file), file: String(file) }; }
  }
  Object.assign(Engine.prototype, { AUDIO_RESOLUTION_16: 16, AUDIO_RESOLUTION_32: 32, AUDIO_RESOLUTION_64: 64 });
  for (const [name, read] of [['frametime', () => F(0)], ['runtime', () => F(1)], ['timeOfDay', () => F(2)],
                              ['canvasSize', () => new Vec2(F(3), F(4))], ['screenResolution', () => new Vec2(F(8), F(9))]]) {
    Object.defineProperty(Engine.prototype, name, { get: read, set(v) {}, enumerable: true });
  }
  const clearTimer = function (cancel) { if (typeof cancel === 'function') cancel(); };

  // ---- input: scene units for the world position, pixels of the screen for the screen position ----
  global.input = {};
  for (const [name, read] of [['cursorWorldPosition', () => new Vec3(F(5), F(6), 0)],
                              ['cursorScreenPosition', () => new Vec2(F(3) > 0 ? F(5) / F(3) * F(8) : 0, F(4) > 0 ? (F(4) - F(6)) / F(4) * F(9) : 0)],
                              ['cursorLeftDown', () => F(7) !== 0], ['cursorDown', () => F(7) !== 0]]) {
    Object.defineProperty(global.input, name, { get: read, set(v) {}, enumerable: true });
  }
  global.shared = global.shared || {};

  // ---- localStorage: shared by every script (the host's store, which it keeps on disk per item), JSON text per key, 100 KB in all ----
  const LOCATIONS = { LOCATION_GLOBAL: 'global', LOCATION_SCREEN: 'screen' };
  const kStorageLimit = 100 * 1000;
  const loc = (l) => (l === LOCATIONS.LOCATION_GLOBAL ? 'global' : 'screen');
  // the quota is on what is written to disk: UTF-8 bytes (counted by the host)
  const utf8 = (s) => __host.utf8(s);
  function storageBytes() { let n = 0; for (const l of ['global', 'screen']) for (const k of Object.keys(__storage[l])) n += utf8(k) + utf8(__storage[l][k]); return n; }
  global.localStorage = {
    LOCATION_GLOBAL: LOCATIONS.LOCATION_GLOBAL, LOCATION_SCREEN: LOCATIONS.LOCATION_SCREEN,
    get(key, location) {
      const v = __storage[loc(location)][String(key)];
      if (v === undefined) return undefined;
      try { return JSON.parse(v); } catch (e) { return undefined; }
    },
    set(key, value, location) {
      const l = loc(location), k = String(key);
      const text = value === undefined ? undefined : JSON.stringify(value);
      if (text === undefined) { delete __storage[l][k]; __host.stored(); return; }
      const old = __storage[l][k];
      if (storageBytes() - (old === undefined ? 0 : utf8(k) + utf8(old)) + utf8(k) + utf8(text) > kStorageLimit) {
        console.error('localStorage: the 100 KB of a wallpaper are used up; ' + k + ' was not stored');
        return;
      }
      __storage[l][k] = text;
      __host.stored();
    },
    clear(location) { for (const k of Object.keys(__storage[loc(location)])) delete __storage[loc(location)][k]; __host.stored(); },
    delete(key, location) {
      const l = loc(location), k = String(key);
      if (!(k in __storage[l])) return false;
      delete __storage[l][k];
      __host.stored();
      return true;
    },
  };

  // colours arrive as arrays of three numbers (the host cannot tell a colour from a vector): user properties of a colour
  // are Vec3 for scripts, so are vectors
  function toProperty(v) {
    if (Array.isArray(v)) return v.length === 2 ? new Vec2(v[0], v[1]) : v.length === 3 ? new Vec3(v[0], v[1], v[2]) : new Vec4(v[0], v[1], v[2], v[3]);
    return v;
  }

  // ---- event arguments and the media hooks ----
  class CursorEvent { constructor(world, local, hit) { this.worldPosition = world; this.localPosition = local; this.hitBox = hit; } }
  class AnimationEvent { constructor(name, frame) { this.name = name; this.frame = frame; } }
  global.CursorEvent = CursorEvent; global.AnimationEvent = AnimationEvent;
  global.MediaPlaybackEvent = { PLAYBACK_STOPPED: 0, PLAYBACK_PLAYING: 1, PLAYBACK_PAUSED: 2 };
  // the media events' colours are Vec3 (r, g, b in 0..1), everything else as the host wrote it
  const mediaEvent = (o) => {
    for (const k of ['primaryColor', 'secondaryColor', 'tertiaryColor', 'textColor', 'highContrastColor']) {
      if (Array.isArray(o[k])) o[k] = new Vec3(o[k][0], o[k][1], o[k][2]);
    }
    return o;
  };

  // ---- script properties ----
  // a value as the property's kind holds it; undefined when it cannot be one (a slider given text, a colour given a number)
  const coerce = (d, v) => {
    if (d.kind === 'Color') { if (typeof v === 'string') return new Vec3(v); return Array.isArray(v) && v.length >= 3 ? new Vec3(v[0], v[1], v[2]) : (v instanceof Vec3 ? v : undefined); }
    if (d.kind === 'Checkbox') return v === true || v === 1 || v === 'true';
    if (d.kind === 'Slider') { const n = typeof v === 'number' ? v : Number(v); return typeof v === 'boolean' || !Number.isFinite(n) ? undefined : n; }
    return v;
  };
  function makeScriptProperties(scope) {
    const defs = [];
    const builder = {};
    const add = (kind) => (o) => { defs.push(Object.assign({ kind }, o)); return builder; };
    for (const k of ['Checkbox', 'Text', 'Slider', 'Combo', 'Color', 'File', 'TextInput', 'Scene', 'Directory', 'Image']) builder['add' + k] = add(k);
    builder.finish = function () {
      const values = {};
      const given = scope.given;
      for (const d of defs) {
        let v = d.value;
        if (Object.prototype.hasOwnProperty.call(given, d.name) && given[d.name] !== null) {
          const c = coerce(d, given[d.name]);
          if (c !== undefined) v = given[d.name];
        }
        // a combo or a slider the script gives no value starts at its first option or its minimum (the editor does the same)
        if ((v === undefined || v === null) && d.kind === 'Combo' && Array.isArray(d.options) && d.options.length) v = d.options[0].value;
        if ((v === undefined || v === null) && d.kind === 'Slider') v = typeof d.min === 'number' ? d.min : 0;
        if (d.kind === 'Color' && typeof v === 'string') v = new Vec3(v);
        else if (d.kind === 'Checkbox') v = v === true || v === 1 || v === 'true';
        else if (d.kind === 'Slider') v = typeof v === 'number' ? v : Number(v);
        values[d.name] = v;
      }
      // properties the object sets without the script declaring them stay readable
      for (const k of Object.keys(given)) if (!(k in values)) values[k] = given[k];
      scope.scriptValues = { values, defs };
      return values;
    };
    return builder;
  }

  // ---- what one script sees: its objects, its engine, its timers and its script properties ----
  // A script's names (thisLayer, thisObject, thisScene, engine, the timer functions, createScriptProperties) are its; the host
  // makes them the globals of a context of its own, or the imports of a module in the realm the scripts share. `given` is the object's
  // own settings of the script's properties; `animationLayer` (>= 0) makes thisObject that animation layer of the object; `property` is
  // the property a property script drives.
  function makeScope(layerId, animationLayer, given, property) {
    const scope = { timers: [], timerSeq: 0, audioBuffers: [], given: given || {}, scriptValues: null };
    // timers run when the host announces a frame (frame below): a callback is the script's own code and runs inside its time budget
    scope.addTimer = function (callback, delay, repeat) {
      // at most 256 timers at a time a script has pending; a timer past that is refused (it never fires)
      if (typeof callback !== 'function' || scope.timers.length >= 256) return function () {};
      const ms = Math.max(0, num(delay, 0));
      const t = { id: ++scope.timerSeq, due: F(1) * 1000 + ms, ms, repeat, dead: false, callback };
      scope.timers.push(t);
      __host.wake();
      return function () { t.dead = true; };
    };
    const engine = new Engine(scope);
    const thisLayer = layerOf(layerId);
    let thisObject = thisLayer;
    if (animationLayer >= 0) {
      thisObject = new AnimationLayer(layerId, animationLayer);
    } else if (property) {
      // a property script's object: the layer, whose getAnimation() without a name is the animation of the script's own property
      thisObject = Object.create(thisLayer, {
        getAnimation: { value(name) { return name === undefined || name === null ? animationOf(layerId, 'animation@' + property) : thisLayer.getAnimation(name); } },
      });
    }
    const names = {
      thisLayer,
      thisObject,
      thisScene: scene,
      engine,
      setTimeout: engine.setTimeout, setInterval: engine.setInterval, clearTimeout: clearTimer, clearInterval: clearTimer,
      createScriptProperties() { return makeScriptProperties(scope); },
    };
    return {
      names,
      // before every update of a script with audio buffers or timers
      frame() {
        const runtime = F(1);
        for (const b of scope.audioBuffers) {
          const l = __host.audio(b.resolution, 0), r = __host.audio(b.resolution, 1);
          for (let i = 0; i < b.resolution; ++i) { b.left[i] = l[i]; b.right[i] = r[i]; b.average[i] = (l[i] + r[i]) * 0.5; }
        }
        if (videoWatchers.length) dispatchVideoEnds();
        // timers whose time has come, oldest first; an interval does not catch up on frames it missed
        const timers = scope.timers;
        if (timers.length) {
          const now = runtime * 1000;
          for (const t of timers.slice()) {
            if (t.dead || t.due > now) continue;
            if (t.repeat) t.due = now + Math.max(1, t.ms); else t.dead = true;
            t.callback();
          }
          for (let i = timers.length - 1; i >= 0; --i) if (timers[i].dead) timers.splice(i, 1);
        }
      },
      usesAudio() { return scope.audioBuffers.length > 0; },
      hasTimers() { return scope.timers.length > 0 || videoWatchers.length > 0; },
      setUserProperties(changed, all) {
        for (const k of Object.keys(all)) engine.userProperties[k] = toProperty(all[k]);
        const o = {};
        for (const k of Object.keys(changed)) o[k] = toProperty(changed[k]);
        return o;
      },
      // a user property the object binds a script property to changed: the script's `scriptProperties` object follows
      setScriptProperties(changed) {
        const held = scope.scriptValues;
        if (!held) return;
        for (const k of Object.keys(changed)) {
          const d = held.defs.find((x) => x.name === k);
          const c = d ? coerce(d, changed[k]) : toProperty(changed[k]);
          if (c !== undefined) held.values[k] = c;
        }
      },
    };
  }

  // ---- the host's entry points: the value of the prelude, never on the global object ----
  return {
    makeScope,
    // a script that exports only some of the cursor hooks is called only for those
    cursorEvent(name, ns, wx, wy, lx, ly) {
      if (typeof ns[name] === 'function') return ns[name](new CursorEvent(new Vec3(wx, wy, 0), new Vec3(lx, ly, 0), null));
    },
    eventArg(kind, a, b) { return kind === 'resize' ? new Vec2(a, b) : kind === 'media' ? mediaEvent(JSON.parse(a)) : undefined; },
    // what plays (or that nothing does) is told once, after a script's init: the status and the playback, and with a player the track's
    // properties, thumbnail and timeline (docs/scene-vk/scripts.md)
    announceMedia(ns, snapshot) {
      // a hook that throws is logged and the script carries on: it was not asked for by a track, only by the state of the player
      const send = (name, event) => {
        if (typeof ns[name] !== 'function' || event === null) return;
        try { ns[name](mediaEvent(event)); } catch (e) { console.warn('media hook ' + name + ': ' + e); }
      };
      const s = JSON.parse(snapshot);
      send('mediaStatusChanged', s.status);
      send('mediaPlaybackChanged', s.playback);
      send('mediaPropertiesChanged', s.properties);
      send('mediaThumbnailChanged', s.thumbnail);
      send('mediaTimelineChanged', s.timeline);
    },
  };
})(globalThis, (() => { const h = globalThis.__host; delete globalThis.__host; return h; })()); // only the prelude's own closures reach the natives
)JS";

const char *const kModuleWEMath = "export const mix = WEMath.mix, clamp = WEMath.clamp, smoothStep = WEMath.smoothStep, deg2rad = WEMath.deg2rad, "
                                  "rad2deg = WEMath.rad2deg, PI = WEMath.PI; export default WEMath;";
const char *const kModuleWEVector = "export const angleVector2 = WEVector.angleVector2, vectorAngle2 = WEVector.vectorAngle2; export default WEVector;";
const char *const kModuleWEColor = "export const rgb2hsv = WEColor.rgb2hsv, hsv2rgb = WEColor.hsv2rgb, normalizeColor = WEColor.normalizeColor, "
                                   "expandColor = WEColor.expandColor; export default WEColor;";

} // namespace murasaki::scene
