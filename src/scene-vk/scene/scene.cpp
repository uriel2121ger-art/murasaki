#include "scene/scene.h"

#include "assets/pkg.h"
#include "assets/tex.h"
#include "assets/texfile.h"
#include "assets/videodecoder.h"
#include "util/json.h"

#include <algorithm>
#include <limits>
#include <cmath>
#include <map>
#include <set>

namespace murasaki::scene {

namespace {

// Bounds that keep a hostile or broken scene from asking for absurd sizes or work.
constexpr double kMaxSceneSize = 65536.0;
constexpr size_t kMaxObjects = 5000;
constexpr size_t kMaxInstanceTextureSlots = 32; // texture slots of an `instance` block that are read

class Loader
{
public:
    Loader(const Vfs &vfs, Scene &scene) : m_vfs(vfs), m_scene(scene) {}

    void run(const Json &root);

private:
    bool m_autoProjection = false;
    bool m_cameraFade = false; // general.camerafade
    void reject(const std::string &reason)
    {
        if (std::find(m_scene.unsupported.begin(), m_scene.unsupported.end(), reason) == m_scene.unsupported.end()) {
            m_scene.unsupported.push_back(reason);
        }
    }

    // The scene still loads and draws; the feature is named so the gap is visible (--check, the log, the compat harness).
    void degrade(const std::string &reason)
    {
        if (std::find(m_scene.degraded.begin(), m_scene.degraded.end(), reason) == m_scene.degraded.end()) {
            m_scene.degraded.push_back(reason);
        }
    }

    void general(const Json *g);
    void fog(const Json *g);
    void camera(const Json *c);
    void object(const Json &o);
    void autoProjection();
    void imageKeys(const Json &o);
    void resolveAttachments();
    void layerDegradations();
    void imageLayer(const Json &o, size_t node);
    void effectChain(const Json &o, ImageLayer &layer);
    void shapeLayer(const Json &o, size_t node);
    void layerLook(const Json &o, ImageLayer &layer);
    int blendModeOf(const Json &o);
    void particleLayer(const Json &o, size_t node);
    void meshObject(const Json &o, size_t node);
    void lightObject(const Json &o, size_t node);
    std::vector<AnimationLayer> animationLayers(const Json &o);
    void soundObject(const Json &o, size_t node);
    void cameraObject(const Json &o, size_t node);
    CameraPath cameraPath(const Json &p);
    bool material(const std::string &modelPath, ImageLayer &layer, const std::string &objectName);
    enum class InstanceBase { Picture, FlatColor }; // a material with a picture to replace, or the flat shader without one
    void applyInstance(const Json &instance, ImageLayer &layer, InstanceBase base);
    void texture(const std::string &path);

    // Property readers. A value can be a literal, an object {"value": ...} or {"user": ..., "value": ...}; scripts
    // are rejected.
    const Json *valueOf(const Json *j, std::string *user, const char *what);
    void recordScript(const Json &j, const char *what);
    void addScript(const Json &j, const std::string &property);
    void animation(const Json *property, PropertyAnimation::Target target, size_t index, int components);
    Vec3 vec3(const Json *j, Vec3 fallback, const char *what, std::string *user = nullptr);
    Vec2 vec2(const Json *j, Vec2 fallback, const char *what, std::string *user = nullptr);
    std::string boundUser(const Json *j);
    Bound<bool> boolean(const Json *j, bool fallback, const char *what);
    Bound<float> number(const Json *j, float fallback, const char *what);
    Bound<Vec3> color(const Json *j, Vec3 fallback, const char *what);

    const Vfs &m_vfs;
    Scene &m_scene;
    std::map<std::string, bool> m_textures; // checked already
    struct MeshCacheEntry {
        std::shared_ptr<const MeshModel> model; // null: `error` says why
        std::string error;
    };
    std::map<std::string, MeshCacheEntry> m_meshModels; // 3D models by path, read once
    std::string *m_condition = nullptr;     // where a condition read by valueOf goes (set while a boolean is read)
    const Json *m_object = nullptr;         // the object being read (an animation's `parent` key names another of its properties)
    struct PendingAttachment {
        size_t node;
        std::string name;
    };
    std::vector<PendingAttachment> m_pendingAttachments;
    int m_scriptNode = -1;                  // the object being read: scripts on its properties attach to it
    int m_animationLayer = -1;              // the puppet animation layer being read: a script on its visibility drives "alayer.<n>.visible"

public:
    // for the functions that add layers to a loaded scene
    bool imageFromModel(const std::string &modelPath, ImageLayer &layer, const std::string &name)
    {
        return material(modelPath, layer, name);
    }
    size_t addNode(const std::string &name);
    void textFrom(const Json &o, size_t node);
};

const Json *Loader::valueOf(const Json *j, std::string *user, const char *what)
{
    if (!j || j->isNull()) {
        return nullptr;
    }
    if (!j->isObject()) {
        return j;
    }
    if (const Json *script = j->find("script"); script && script->isString()) {
        recordScript(*j, what);
    }
    if (const Json *u = j->find("user")) {
        if (u->isString()) {
            if (user) {
                *user = u->asString();
            }
        } else if (u->isObject()) {
            // {"name": ..., "condition": ...}: a property compared against a value, which a value read as a boolean can take (a
            // visibility, the camera parallax: Bound<bool> and resolve() keep the condition)
            const Json *name = u->find("name");
            const Json *condition = u->find("condition");
            if (name && name->isString() && condition && m_condition) {
                if (user) {
                    *user = name->asString();
                }
                *m_condition = condition->isString() ? condition->asString() : std::to_string(static_cast<long long>(condition->asNumber()));
            } else {
                reject(std::string("property condition (") + what + "): M4");
            }
        }
    }
    const Json *value = j->find("value");
    return value && !value->isNull() ? value : nullptr;
}

// A script on a property of the object being read. The properties the engine can drive from a script are the object's
// transform, visibility, tint, size and the text; anything else keeps the scene refused.
void Loader::recordScript(const Json &j, const char *what)
{
    static const std::map<std::string, std::string> kProperties = {
        {"origin", "origin"},         {"scale", "scale"},           {"angles", "angles"},         {"visibility", "visible"},
        {"alpha", "alpha"},           {"brightness", "brightness"}, {"colour", "color"},          {"size", "size"},
        {"parallax depth", "parallaxDepth"},                        {"particle rate", "rate"},    {"particle alpha", "alpha"},
        {"particle size", "size"},    {"particle lifetime", "lifetime"},                          {"particle speed", "speed"},
        {"particle count", "count"},  {"particle colour", "color"}, {"text", "text"},           {"text size", "size"},
        {"sound volume", "volume"},   {"material colour", "material.color"}, {"material alpha", "material.alpha"},
        {"material brightness", "material.brightness"}};
    // the scene's own properties, which scripts reach as "scene.<key>" (the keys of general in scene.json)
    static const std::map<std::string, std::string> kSceneProperties = {
        {"camera parallax", "cameraparallax"},        {"camera parallax amount", "cameraparallaxamount"},
        {"camera parallax delay", "cameraparallaxdelay"}, {"camera parallax mouse influence", "cameraparallaxmouseinfluence"},
        {"camera shake", "camerashake"},              {"camera shake amplitude", "camerashakeamplitude"},
        {"camera shake roughness", "camerashakeroughness"}, {"camera shake speed", "camerashakespeed"},
        {"bloom", "bloom"},                           {"bloom strength", "bloomstrength"}, {"bloom threshold", "bloomthreshold"},
        {"clear colour", "clearcolor"},               {"scene zoom", "zoom"}};
    // the values of a light object (the script's `value` is the property being driven; the handle's get and set know these names)
    static const std::map<std::string, std::string> kLightProperties = {
        {"light colour", "light.color"},      {"light intensity", "light.intensity"},   {"light radius", "light.radius"},
        {"light exponent", "light.exponent"}, {"light inner cone", "light.innercone"}, {"light outer cone", "light.outercone"},
        {"light density", "light.density"},   {"light volumetrics exponent", "light.volumetricsexponent"}};
    if (m_scriptNode >= 0) {
        if (const auto light = kLightProperties.find(what); light != kLightProperties.end()) {
            addScript(j, light->second);
            return;
        }
    }
    if (m_scriptNode < 0) {
        if (const auto scene = kSceneProperties.find(what); scene != kSceneProperties.end()) {
            addScript(j, "scene." + scene->second);
            return;
        }
    }
    // a puppet animation layer's visibility, rate or blend: "alayer.<n>.<field>", and the script's thisObject is the animation layer
    // (script/scripthost.cpp)
    static const std::map<std::string, std::string> kAnimationLayerProperties = {
        {"animation layer visibility", "visible"}, {"animation rate", "rate"}, {"animation blend", "blend"}};
    if (const auto layerProperty = kAnimationLayerProperties.find(what);
        layerProperty != kAnimationLayerProperties.end() && m_scriptNode >= 0 && m_animationLayer >= 0) {
        addScript(j, "alayer." + std::to_string(m_animationLayer) + "." + layerProperty->second);
        return;
    }
    const auto it = kProperties.find(what);
    if (m_scriptNode < 0 || it == kProperties.end()) {
        reject(std::string("SceneScript (") + what + "): M4");
        return;
    }
    addScript(j, it->second);
}

// Some scene files hold two scripts in one string, one after the other (a template's empty `update`, then the script itself), each
// starting with `'use strict';` at the beginning of a line: two modules in one would export `update` twice, so the last one is
// the script (the first is the boilerplate in every file seen).
std::string effectiveScript(const std::string &source)
{
    size_t last = 0;
    for (const char *marker : {"\n'use strict';", "\n\"use strict\";"}) {
        for (size_t at = source.find(marker); at != std::string::npos; at = source.find(marker, at + 1)) {
            last = std::max(last, at + 1);
        }
    }
    return last > 0 ? source.substr(last) : source;
}

// The binding of a script on `property` of the object being read.
void Loader::addScript(const Json &j, const std::string &property)
{
    ScriptBinding b;
    b.scene = m_scriptNode < 0;
    b.node = b.scene ? 0 : static_cast<size_t>(m_scriptNode);
    b.property = property;
    b.source = effectiveScript(j.stringOr("script"));
    b.name = b.property; // the object's name is put in front when the object is complete (Loader::object)
    if (const Json *props = j.find("scriptproperties"); props && props->isObject()) {
        for (const auto &[key, value] : props->members()) {
            if (value.isBool()) {
                b.properties[key] = ScriptValue::ofBool(value.asBool());
            } else if (value.isNumber()) {
                b.properties[key] = ScriptValue::ofNumber(value.asNumber());
            } else if (value.isString()) {
                b.properties[key] = ScriptValue::ofText(value.asString());
            } else if (value.isObject()) {
                // a script property bound to a user property: the file's value stands in until the property is set
                const Json *user = value.find("user");
                const Json *fallback = value.find("value");
                if (user && user->isString()) {
                    b.propertyUsers[key] = user->asString();
                }
                if (fallback && fallback->isBool()) {
                    b.properties[key] = ScriptValue::ofBool(fallback->asBool());
                } else if (fallback && fallback->isNumber()) {
                    b.properties[key] = ScriptValue::ofNumber(fallback->asNumber());
                } else if (fallback && fallback->isString()) {
                    b.properties[key] = ScriptValue::ofText(fallback->asString());
                }
            }
        }
    }
    m_scene.scripts.push_back(std::move(b));
}

// The user property a vector is bound to, "" when the value is a literal. A script or keyframes on the property own it (as today,
// they win over the property), so the binding is dropped then.
std::string Loader::boundUser(const Json *j)
{
    if (!j || !j->isObject() || j->find("script") || j->find("animation")) {
        return {};
    }
    const Json *u = j->find("user");
    return u && u->isString() ? u->asString() : std::string();
}

Vec3 Loader::vec3(const Json *j, Vec3 fallback, const char *what, std::string *user)
{
    if (user) {
        *user = boundUser(j);
    }
    const Json *v = valueOf(j, nullptr, what);
    if (!v) {
        return fallback;
    }
    float f[3] = {fallback.x, fallback.y, fallback.z};
    if (v->isString()) {
        parseFloats(v->asString(), f, 3);
    } else if (v->isNumber()) {
        f[0] = f[1] = f[2] = static_cast<float>(v->asNumber());
    }
    // A Windows "-nan(ind)" (an editor wrote a broken value) reads as the property's default for that component:
    // the likeliest reading of what WE does, and the only one that does not wipe the object out.
    const float defaults[3] = {fallback.x, fallback.y, fallback.z};
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(f[i])) {
            f[i] = defaults[i];
        }
    }
    return {f[0], f[1], f[2]};
}

Vec2 Loader::vec2(const Json *j, Vec2 fallback, const char *what, std::string *user)
{
    if (user) {
        *user = boundUser(j);
    }
    const Json *v = valueOf(j, nullptr, what);
    if (!v) {
        return fallback;
    }
    float f[2] = {fallback.x, fallback.y};
    if (v->isString()) {
        parseFloats(v->asString(), f, 2);
    } else if (v->isNumber()) {
        f[0] = f[1] = static_cast<float>(v->asNumber());
    }
    const float defaults[2] = {fallback.x, fallback.y};
    for (int i = 0; i < 2; ++i) {
        if (!std::isfinite(f[i])) {
            f[i] = defaults[i];
        }
    }
    return {f[0], f[1]};
}

Bound<bool> Loader::boolean(const Json *j, bool fallback, const char *what)
{
    Bound<bool> b{fallback, {}};
    m_condition = &b.condition;
    const Json *v = valueOf(j, &b.user, what);
    m_condition = nullptr;
    if (v && v->isBool()) {
        b.value = v->asBool();
    } else if (v && v->isNumber()) {
        b.value = v->asNumber() != 0.0;
    }
    return b;
}

Bound<float> Loader::number(const Json *j, float fallback, const char *what)
{
    Bound<float> b{fallback, {}};
    const Json *v = valueOf(j, &b.user, what);
    if (v && v->isNumber()) {
        b.value = static_cast<float>(v->asNumber());
    } else if (v && v->isString()) {
        float f = fallback;
        parseFloats(v->asString(), &f, 1);
        b.value = f;
    }
    if (!std::isfinite(b.value)) {
        b.value = fallback;
    }
    return b;
}

Bound<Vec3> Loader::color(const Json *j, Vec3 fallback, const char *what)
{
    Bound<Vec3> b{fallback, {}};
    const Json *v = valueOf(j, &b.user, what);
    if (v && v->isString()) {
        float f[3] = {fallback.x, fallback.y, fallback.z};
        parseFloats(v->asString(), f, 3);
        const float defaults[3] = {fallback.x, fallback.y, fallback.z};
        for (int i = 0; i < 3; ++i) {
            b.value = {std::isfinite(f[0]) ? f[0] : defaults[0], std::isfinite(f[1]) ? f[1] : defaults[1], std::isfinite(f[2]) ? f[2] : defaults[2]};
        }
    }
    return b;
}

// The keyframes of one component (`[{"frame": 0, "value": 1, "back": {...}, "front": {...}}, ...]`), sorted by frame.
void readKeys(const Json &keys, std::vector<AnimationKey> &out)
{
    for (const Json &k : keys.items()) {
        if (!k.isObject() || out.size() >= kMaxAnimationKeyframes) {
            continue;
        }
        AnimationKey key;
        key.frame = static_cast<float>(k.numberOr("frame", 0.0));
        key.value = static_cast<float>(k.numberOr("value", 0.0));
        if (const Json *back = k.find("back"); back && back->isObject()) {
            key.backOn = back->boolOr("enabled", false);
            key.backX = static_cast<float>(back->numberOr("x", -1.0));
            key.backY = static_cast<float>(back->numberOr("y", 0.0));
        }
        if (const Json *front = k.find("front"); front && front->isObject()) {
            key.frontOn = front->boolOr("enabled", false);
            key.frontX = static_cast<float>(front->numberOr("x", 1.0));
            key.frontY = static_cast<float>(front->numberOr("y", 0.0));
        }
        if (std::isfinite(key.frame) && std::isfinite(key.value) && std::isfinite(key.backX) && std::isfinite(key.backY) && std::isfinite(key.frontX) && std::isfinite(key.frontY)) {
            out.push_back(key);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const AnimationKey &l, const AnimationKey &r) { return l.frame < r.frame; });
}

void Loader::animation(const Json *property, PropertyAnimation::Target target, size_t index, int components)
{
    if (!property || !property->isObject()) {
        return;
    }
    const Json *anim = property->find("animation");
    if (!anim || !anim->isObject()) {
        return;
    }
    if (m_scene.animations.size() >= kMaxPropertyAnimations) {
        reject("more than " + std::to_string(kMaxPropertyAnimations) + " animated properties");
        return;
    }
    PropertyAnimation a;
    a.target = target;
    a.index = index;
    a.components = components;
    a.node = static_cast<size_t>(std::max(0, m_scriptNode));
    if (const Json *options = anim->find("options"); options && options->isObject()) {
        a.name = options->stringOr("name");
        a.playback.playing = !options->boolOr("startpaused", false);
        if (const Json *link = options->find("parent"); link && link->isObject()) {
            a.parentKey = link->stringOr("key");
        }
        a.fps = static_cast<float>(std::clamp(options->numberOr("fps", 30.0), 1.0, 240.0));
        a.length = static_cast<float>(std::clamp(options->numberOr("length", 1.0), 1.0, 1.0e6));
        const std::string mode = options->stringOr("mode", "single");
        a.mode = mode == "loop" ? PropertyAnimation::Mode::Loop : mode == "mirror" ? PropertyAnimation::Mode::Mirror : PropertyAnimation::Mode::Single;
        // A linked track (`"parent": {"key": "zoom"}` here, `"children": [...]` on the parent) plays on its parent's clock: the
        // parent's fps, length and mode replace its own (in the library the two always agree; the rule is a choice, not a finding)
        if (const Json *link = options->find("parent"); link && link->isObject() && m_object) {
            const Json *parent = m_object->find(link->stringOr("key"));
            const Json *parentAnimation = parent && parent->isObject() ? parent->find("animation") : nullptr;
            const Json *parentOptions = parentAnimation && parentAnimation->isObject() ? parentAnimation->find("options") : nullptr;
            if (parentOptions && parentOptions->isObject() && parent != property) {
                a.fps = static_cast<float>(std::clamp(parentOptions->numberOr("fps", 30.0), 1.0, 240.0));
                a.length = static_cast<float>(std::clamp(parentOptions->numberOr("length", 1.0), 1.0, 1.0e6));
                const std::string parentMode = parentOptions->stringOr("mode", "single");
                a.mode = parentMode == "loop" ? PropertyAnimation::Mode::Loop : parentMode == "mirror" ? PropertyAnimation::Mode::Mirror : PropertyAnimation::Mode::Single;
            }
        }
    }
    a.relative = anim->boolOr("relative", false);
    if (a.relative) {
        // the offsets are added to the property's own value
        if (const Json *base = property->find("value")) {
            if (base->isNumber()) {
                a.base = {static_cast<float>(base->asNumber()), static_cast<float>(base->asNumber()), static_cast<float>(base->asNumber())};
            } else if (base->isString()) {
                parseFloats(base->asString(), a.base.data(), 3);
            }
            for (float &b : a.base) {
                b = std::isfinite(b) ? b : 0.0f;
            }
        }
    }
    for (int c = 0; c < components; ++c) {
        const Json *keys = anim->find("c" + std::to_string(c));
        if (!keys || !keys->isArray()) {
            continue;
        }
        readKeys(*keys, a.curves[static_cast<size_t>(c)]);
    }
    if (std::any_of(a.curves.begin(), a.curves.end(), [](const auto &c) { return !c.empty(); })) {
        m_scene.animations.push_back(std::move(a));
    }
}

float sampleCurve(const PropertyAnimation &a, int component, double timeSeconds, float fallback)
{
    if (component < 0 || component >= 3) {
        return fallback;
    }
    const auto &keys = a.curves[static_cast<size_t>(component)];
    if (keys.empty()) {
        return fallback;
    }
    double frame = timeSeconds * static_cast<double>(a.fps);
    const double length = static_cast<double>(a.length);
    switch (a.mode) {
    case PropertyAnimation::Mode::Loop:
        frame = std::fmod(std::max(0.0, frame), length);
        break;
    case PropertyAnimation::Mode::Mirror: {
        const double m = std::fmod(std::max(0.0, frame), 2.0 * length);
        frame = m <= length ? m : 2.0 * length - m;
        break;
    }
    case PropertyAnimation::Mode::Single:
        frame = std::clamp(frame, 0.0, length);
        break;
    }
    if (!std::isfinite(frame) || frame <= static_cast<double>(keys.front().frame)) {
        return keys.front().value;
    }
    if (frame >= static_cast<double>(keys.back().frame)) {
        return keys.back().value;
    }
    const auto next = std::upper_bound(keys.begin(), keys.end(), frame, [](double f, const AnimationKey &k) { return f < static_cast<double>(k.frame); });
    const AnimationKey &k0 = *(next - 1);
    const AnimationKey &k1 = *next;
    const double span = static_cast<double>(k1.frame - k0.frame);
    const double u = span > 0.0 ? (frame - static_cast<double>(k0.frame)) / span : 0.0;
    // the segment in its own units (frames 0..1, value difference 0..1): the control points are the handles, or the straight line's thirds
    const double x1 = k0.frontOn ? std::clamp(static_cast<double>(k0.frontX), 0.0, 1.0) : 1.0 / 3.0;
    const double y1 = k0.frontOn ? static_cast<double>(k0.frontY) : 1.0 / 3.0;
    const double x2 = k1.backOn ? std::clamp(1.0 + static_cast<double>(k1.backX), 0.0, 1.0) : 2.0 / 3.0;
    const double y2 = k1.backOn ? 1.0 + static_cast<double>(k1.backY) : 2.0 / 3.0;
    const auto bezier = [](double p1, double p2, double t) {
        const double s = 1.0 - t;
        return 3.0 * s * s * t * p1 + 3.0 * s * t * t * p2 + t * t * t;
    };
    double lo = 0.0, hi = 1.0; // x is monotonic in t for control x within 0..1
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (lo + hi);
        (bezier(x1, x2, mid) < u ? lo : hi) = mid;
    }
    const double fraction = bezier(y1, y2, 0.5 * (lo + hi));
    return static_cast<float>(static_cast<double>(k0.value) + (static_cast<double>(k1.value) - static_cast<double>(k0.value)) * fraction);
}

void Loader::general(const Json *g)
{
    if (!g) {
        return;
    }
    if (const Json *ortho = g->find("orthogonalprojection"); ortho && ortho->isObject()) {
        const double w = ortho->numberOr("width", 1920);
        const double h = ortho->numberOr("height", 1080);
        // `"auto": true` with no width and height: the size follows the pictures (see run())
        const Json *wj = ortho->find("width"), *hj = ortho->find("height");
        m_autoProjection = ortho->boolOr("auto", false) && !(wj && wj->isNumber()) && !(hj && hj->isNumber());
        if (!(w > 0.0 && h > 0.0 && w <= kMaxSceneSize && h <= kMaxSceneSize)) {
            reject("projection size out of range");
        } else {
            m_scene.orthoSize = {static_cast<float>(w), static_cast<float>(h)};
        }
    } else {
        // no orthogonal projection: a perspective camera (formats.md, "Camera"). The camera's own keys are `general`'s: `fov`
        // (vertical, degrees; a user property may own it), `nearz` and `farz`. `perspectiveoverridefov` is not read: no
        // perspective scene of the library has it, and in the orthographic ones that do it has no perspective camera to act on.
        m_scene.perspective = true;
        m_scene.orthoSize = kPerspectiveCanvas;
        const Bound<float> fov = number(g->find("fov"), 50.0f, "fov");
        m_scene.camera.fov = clampFov(fov.value);
        m_scene.camera.fovUser = fov.user;
        const float nearZ = number(g->find("nearz"), 0.01f, "near z").value;
        m_scene.camera.nearZ = nearZ > 0.0f ? nearZ : 0.01f;
        const float farZ = number(g->find("farz"), 10000.0f, "far z").value;
        m_scene.camera.farZ = farZ > m_scene.camera.nearZ ? farZ : m_scene.camera.nearZ + 1.0f;
    }
    {
        // may be bound to a user property (`{"user": ..., "value": bool}`): followed live
        m_scene.camera.fade = boolean(g->find("camerafade"), false, "camera fade");
        m_cameraFade = m_scene.camera.fade.value || !m_scene.camera.fade.user.empty();
    }
    // `clearenabled` off: the scene target is not cleared between frames, what is drawn stays under the next frame (no library item
    // turns it off; the plain reading of the switch, see formats.md)
    m_scene.clearEnabled = boolean(g->find("clearenabled"), true, "clear enabled");
    const Bound<Vec3> clear = color(g->find("clearcolor"), {0, 0, 0}, "clear colour");
    m_scene.clearColor = clear.value;
    // 0.7 grey is the editor's default clear colour, and what almost every scene still has: where such a scene draws nothing the
    // preview images and Wallpaper Engine show black, not grey (checked on 2280451215, 1370984500 and 1885306489, whose pictures do not
    // cover the whole scene), so the untouched default reads as black. A colour somebody chose, or one bound to a user property, is used as it is.
    {
        const Vec3 c = m_scene.clearColor;
        if (clear.user.empty() && std::fabs(c.x - 0.7f) < 0.003f && std::fabs(c.y - 0.7f) < 0.003f && std::fabs(c.z - 0.7f) < 0.003f) {
            m_scene.clearColor = {0.0f, 0.0f, 0.0f};
        }
    }
    // the lit materials' ambient terms, as written: a script or a user property on them is not followed (scripts read and write
    // scene.ambientcolor as a stored value, render/scenescripts.cpp)
    const auto written = [&](const char *key) {
        const Json *j = g->find(key);
        return j && j->isObject() ? j->find("value") : j;
    };
    m_scene.ambientColor = color(written("ambientcolor"), {0.2f, 0.2f, 0.2f}, "ambient colour").value;
    // `transparentsorting`: the draw order of formats.md, "Draw order" (6 perspective scenes of the library turn it on); a
    // `customsortorder` (in none) is not read
    if (const Json *sorting = written("transparentsorting"); sorting && sorting->isBool()) {
        m_scene.transparentSorting = sorting->asBool();
    }
    if (const Json *custom = g->find("customsortorder"); custom && !custom->isNull()) {
        degrade("custom sort order");
    }
    // gravity and wind: read by the bone physics of puppets (puppet/bonephysics.h); particles and everything else ignore them
    m_scene.boneWorld.gravityDirection = vec3(written("gravitydirection"), {0, -1, 0}, "gravity direction");
    m_scene.boneWorld.gravityStrength = number(written("gravitystrength"), 1.0f, "gravity strength").value;
    m_scene.boneWorld.windEnabled = boolean(written("windenabled"), false, "wind").value;
    m_scene.boneWorld.windDirection = vec3(written("winddirection"), {0.707f, 0.707f, 0}, "wind direction");
    m_scene.boneWorld.windStrength = number(written("windstrength"), 1.0f, "wind strength").value;
    {
        // bounded: a hostile scene must not hand the simulation huge forces (the spring step stays stable whatever they are)
        BoneWorld &w = m_scene.boneWorld;
        const auto bounded = [](float v, float limit, float fallback) { return std::isfinite(v) ? std::clamp(v, -limit, limit) : fallback; };
        const auto direction = [&](Vec3 d, Vec3 fallback) {
            return Vec3{bounded(d.x, 1e3f, fallback.x), bounded(d.y, 1e3f, fallback.y), bounded(d.z, 1e3f, fallback.z)};
        };
        w.gravityStrength = bounded(w.gravityStrength, 1e3f, 1.0f);
        w.windStrength = bounded(w.windStrength, 1e5f, 1.0f);
        w.gravityDirection = direction(w.gravityDirection, {0, -1, 0});
        w.windDirection = direction(w.windDirection, {0.707f, 0.707f, 0});
    }
    if (m_scene.boneWorld.windEnabled) {
        degrade("scene wind (bones only)"); // the wind moves the spring bones of puppets; particles and the rest do not feel it
    }
    m_scene.skylightColor = color(written("skylightcolor"), {0.3f, 0.3f, 0.3f}, "skylight colour").value;
    m_scene.parallax.enabled = boolean(g->find("cameraparallax"), false, "camera parallax");
    m_scene.parallax.amount = number(g->find("cameraparallaxamount"), 1.0f, "camera parallax amount");
    m_scene.parallax.delay = number(g->find("cameraparallaxdelay"), 0.0f, "camera parallax delay");
    m_scene.parallax.mouseInfluence = number(g->find("cameraparallaxmouseinfluence"), 1.0f, "camera parallax mouse influence");
    m_scene.shake.enabled = boolean(g->find("camerashake"), false, "camera shake");
    m_scene.shake.amplitude = number(g->find("camerashakeamplitude"), 0.5f, "camera shake amplitude");
    m_scene.shake.roughness = number(g->find("camerashakeroughness"), 1.0f, "camera shake roughness");
    m_scene.shake.speed = number(g->find("camerashakespeed"), 3.0f, "camera shake speed");
    m_scene.bloom.enabled = boolean(g->find("bloom"), false, "bloom");
    m_scene.bloom.strength = number(g->find("bloomstrength"), 2.0f, "bloom strength");
    m_scene.bloom.threshold = number(g->find("bloomthreshold"), 0.65f, "bloom threshold");
    m_scene.bloom.tint = vec3(g->find("bloomtint"), {1, 1, 1}, "bloom tint");
    if (g->boolOr("hdr", false)) {
        // An HDR scene keeps its buffers in floating point and blooms with the `bloomhdr*` settings: the shipped HDR chain, halved
        // `iterations` times and added back up with `scatter`, the threshold softened by `feather` (formats.md, "HDR scenes")
        m_scene.hdr = true;
        m_scene.bloom.strength = number(g->find("bloomhdrstrength"), m_scene.bloom.strength.value, "bloom strength");
        m_scene.bloom.threshold = number(g->find("bloomhdrthreshold"), m_scene.bloom.threshold.value, "bloom threshold");
        m_scene.bloom.iterations = number(g->find("bloomhdriterations"), m_scene.bloom.iterations.value, "bloom iterations");
        m_scene.bloom.scatter = number(g->find("bloomhdrscatter"), m_scene.bloom.scatter.value, "bloom scatter");
        m_scene.bloom.feather = number(g->find("bloomhdrfeather"), m_scene.bloom.feather.value, "bloom feather");
    }
    // `displayhdr` asks WE for an HDR display's output; the output here is always 8 bit (SDR), tone-mapped as on any display
    if (const Json *display = written("displayhdr"); display && display->isBool() && display->asBool()) {
        degrade("display hdr");
    }
    fog(g);
    // keys WE reads and this engine does not: named when they are set to something
    // the scene's own zoom (not a camera object's, which has its own); a script on it (setCameraTransforms writes the same value) runs
    m_scene.camera.zoom = number(g->find("zoom"), 1.0f, "scene zoom");
    if (!(m_scene.camera.zoom.value > 0.0f)) {
        m_scene.camera.zoom.value = 1.0f;
    }
    // `lightconfig` ({"point": 2, "spot": 1, "pointshadow": 1, ...}) is how many lights of each kind (and how many casting shadows) the
    // scene has: in every library item it matches the light objects, which is what the shaders are compiled for here
    // (render/lights.h), so it is not read
}

// `general.fogdistance*` and `general.fogheight*` (formats.md, "Fog"): each band is a switch, a colour, where it starts and ends
// (a distance from the eye, or a world height) and the density at both ends. Under the orthographic camera nothing has a
// distance from an eye or a height to measure, so the fog is not drawn there and is named.
void Loader::fog(const Json *g)
{
    const auto band = [&](const std::string &prefix, FogBand &out) {
        out.enabled = boolean(g->find(prefix), false, "fog");
        out.color = color(g->find(prefix + "color"), out.color.value, "fog colour");
        out.start = number(g->find(prefix + "start"), out.start.value, "fog start");
        out.end = number(g->find(prefix + "end"), out.end.value, "fog end");
        out.startDensity = number(g->find(prefix + "startdensity"), out.startDensity.value, "fog density");
        out.endDensity = number(g->find(prefix + "enddensity"), out.endDensity.value, "fog density");
        return out.enabled.value || !out.enabled.user.empty();
    };
    const bool distance = band("fogdistance", m_scene.fog.distance);
    const bool height = band("fogheight", m_scene.fog.height);
    // any other `fog*` switch that is on is a key this reader does not know: named, not dropped silently
    for (const auto &[key, value] : g->members()) {
        const Json *v = value.isObject() ? value.find("value") : &value;
        if (key.rfind("fog", 0) == 0 && key.rfind("fogdistance", 0) != 0 && key.rfind("fogheight", 0) != 0 && v && v->isBool() && v->asBool()) {
            degrade("general fog");
        }
    }
    if ((distance || height) && !m_scene.perspective) {
        degrade("general fog");
        m_scene.fog = SceneFog{};
    }
}

// The `camera` block: where the scene's own camera is and which way it looks (any orientation; formats.md, "Camera"). Old files
// write fov, nearz and farz here too; `general`'s win in a perspective scene.
void Loader::camera(const Json *c)
{
    if (!c) {
        return;
    }
    m_scene.camera.eye = vec3(c->find("eye"), {0, 0, 0}, "camera");
    m_scene.camera.center = vec3(c->find("center"), {0, 0, -1}, "camera");
    m_scene.camera.up = vec3(c->find("up"), {0, 1, 0}, "camera");
    if (!m_scene.perspective) {
        m_scene.camera.fov = clampFov(static_cast<float>(c->numberOr("fov", 50.0)));
        m_scene.camera.nearZ = std::max(0.001f, static_cast<float>(c->numberOr("nearz", 0.01)));
        m_scene.camera.farZ = std::max(m_scene.camera.nearZ + 1.0f, static_cast<float>(c->numberOr("farz", 10000.0)));
    }
}

void Loader::texture(const std::string &path)
{
    if (m_textures.count(path)) {
        return;
    }
    m_textures[path] = true;
    if (!m_vfs.exists(path)) {
        reject("texture '" + path + "' not found");
        return;
    }
    std::string error;
    const auto tex = readTexFile(m_vfs, path, TexReadOptions{false}, &error);
    if (!tex) {
        reject("texture '" + path + "': " + error);
        return;
    }
    if (tex->hasVideo()) {
        if (!videoSupported()) {
            reject("video texture: the video plugin (libmurasaki-scene-vk-video.so) was not found next to the engine");
        } else if (tex->videoSize == 0) {
            reject("video texture without a stream");
        }
        return;
    }
    if (tex->freeImageFormat == -1 && !texFormatSupported(tex->rawFormat)) {
        reject(std::string("texture format ") + texFormatName(tex->rawFormat) + ": M2");
    }
    if (tex->droppedPages > 0) {
        degrade("sprite sheet pages"); // images of the sheet that would take it over the size limit; their frames show the last one kept
    }
}

bool Loader::material(const std::string &modelPath, ImageLayer &layer, const std::string &objectName)
{
    const auto modelText = m_vfs.readText(modelPath);
    std::string error;
    const auto model = modelText ? parseJson(*modelText, &error) : std::nullopt;
    if (!model) {
        reject("model '" + modelPath + "' " + (modelText ? "is not valid JSON" : "not found"));
        return false;
    }
    layer.model = modelPath;
    // The model file can declare the layer's size: `width` and `height` (every library item that has them also has the same size on
    // the object, which wins), `autosize` says the size follows the picture, which is what a layer without any size does anyway.
    if (layer.size.x <= 0.0f || layer.size.y <= 0.0f) {
        const double w = model->numberOr("width", 0.0), h = model->numberOr("height", 0.0);
        if (w > 0.0 && h > 0.0 && w <= kMaxSceneSize && h <= kMaxSceneSize) {
            layer.size = {static_cast<float>(w), static_cast<float>(h)};
        } else if (model->boolOr("projectlayer", false)) {
            layer.projectLayer = true; // covers the whole wallpaper: sized once the scene's size is final (run())
        }
    }
    // `nopadding` asks for no margin around the picture for effects to spill into, which is how every layer is drawn here
    if (const Json *ref = model->find("usertexturereference"); ref && ((ref->isBool() && ref->asBool()) || (ref->isString() && !ref->asString().empty()))) {
        degrade("usertexturereference");
    }
    if (const Json *puppetPath = model->find("puppet"); puppetPath && puppetPath->isString()) {
        const auto bytes = m_vfs.read(puppetPath->asString());
        std::string puppetError;
        std::optional<Puppet> puppet;
        if (bytes) {
            puppet = parsePuppet(std::span<const uint8_t>(bytes->data(), bytes->size()), &puppetError);
        } else {
            // the file is not in the item (2955378002 names `models/1x1_puppet.mdl` fifty times and does not carry it, nor does the
            // install): the layer is drawn as the plain picture it also is, with no skeleton and no deformation. Assumption: WE
            // draws the rest of the scene without what it cannot load; what it does with the layer itself is not known
            degrade("puppet model missing");
        }
        if (!bytes) {
            // (nothing more to read of it)
        } else if (puppet) {
            layer.puppet = std::make_shared<const Puppet>(std::move(*puppet));
            if (!layer.puppet->attachments.empty()) {
                layer.attachmentPoses = std::make_shared<AttachmentPoses>(); // the bind pose until the renderer moves the bones
                puppetAttachmentPoses(*layer.puppet, {}, 0.0, layer.attachmentPoses->matrices);
            }
            if (layer.puppet->simulatedBones > 0) {
                layer.bonePhysics = std::make_shared<BonePhysicsState>(layer.puppet->bones.size(), bonePhysicsSeed());
            }
            if (layer.puppet->ikBones > 0) {
                // an IK chain's end is read and not evaluated (it keeps the animated pose): the models carry no goal for it
                // (docs/scene-vk/puppets.md, "IK chains")
                degrade(layer.puppet->ikGoalBones > 0 ? "puppet bone IK (control point goal)" : "puppet bone IK (no goal in the data)");
            }
        } else {
            reject("puppet: " + puppetError);
        }
    }
    const std::string materialPath = model->stringOr("material");
    const auto materialText = materialPath.empty() ? std::nullopt : m_vfs.readText(materialPath);
    const auto mat = materialText ? parseJson(*materialText, &error) : std::nullopt;
    if (!mat) {
        reject("material of '" + objectName + "' " + (materialText ? "is not valid JSON" : "not found"));
        return false;
    }
    const Json *passes = mat->find("passes");
    if (!passes || !passes->isArray() || passes->items().empty()) {
        reject("material of '" + objectName + "' has no passes");
        return false;
    }
    if (passes->items().size() > 1) {
        reject("multi-pass material: M2");
    }
    const Json &pass = passes->items()[0];
    layer.base = parseMaterialPass(pass);
    ImageMaterial &m = layer.material;
    m.shader = pass.stringOr("shader");
    // Any shader the translator can compile is fine: render/analysis.h checks that, with the effects' shaders.
    const std::string blending = pass.stringOr("blending", "translucent");
    if (blending == "translucent") {
        m.blending = Blending::Translucent;
    } else if (blending == "additive") {
        m.blending = Blending::Additive;
    } else if (blending == "normal" || blending == "disabled" || blending == "alphatocoverage") {
        // alphatocoverage: normal blending; layer.base (parseMaterialPass) carries the ALPHATOCOVERAGE combo the shipped shaders read
        m.blending = Blending::Normal;
    } else {
        reject("blending '" + blending + "': M2");
    }
    if (const Json *combos = pass.find("combos"); combos && combos->isObject()) {
        for (const auto &[name, value] : combos->members()) {
            // VERSION only switches which colour uniform the shader reads; SPRITESHEET is fine for a single frame.
            // Material files write the names in lower case, shaders in upper case.
            const std::string folded = foldCase(name);
            if (folded == "version" || folded == "spritesheet") {
                continue;
            }
            // lighting and fog are drawn: LIGHTING reads the scene's lights and ambient colour, which the engine gives it
            // (render/lights.h); the scene's fog (if any) reaches a FOG shader as FOG_DIST / FOG_HEIGHT (render/shaderlib.h)
            if (folded == "lighting" || folded == "fog") {
                continue;
            }
            // the shader's own options (2244339517's `fullcloudsbg` declares BLENDMODE and SHADING as combos, reads neither, and includes
            // common_blending.h): any shader the translator compiles takes its combos, which parseMaterialPass carries; the analysis names
            // the ones a shader does read (`combo blendmode`, `combo shading`)
            if (folded == "blendmode" || folded == "shading") {
                continue;
            }
            // reflection is the shader's own: it is translated with the combo and reads the scene drawn so far (with its mip chain). A user
            // binding ({"user": .., "value": ..}) is not followed: the combo stays at the material's value
            if (folded == "reflection") {
                if (value.isObject() && value.find("user")) {
                    degrade("combo " + folded);
                }
                continue;
            }
            if (value.isNumber() && value.asNumber() == 0.0) {
                continue;
            }
            reject("shader option " + name + ": M2");
        }
    }
    if (const Json *constants = pass.find("constantshadervalues"); constants && constants->isObject()) {
        m.alpha = number(constants->find("alpha"), 1.0f, "material alpha");
        m.brightness = number(constants->find("brightness"), 1.0f, "material brightness");
        m.color = color(constants->find("color"), {1, 1, 1}, "material colour");
    }
    // An instanced model's object carries an `instance` block: its constants, combos and textures replace the material's (the model
    // says `instanced`; the editor writes it for a solid placeholder, a layer whose picture is a plain texture)
    // (a first texture that is empty, `[""]` as well as `[null]`, is no picture: the flat shader then makes a flat colour)
    const bool flatMaterial = (layer.base.textures.empty() || layer.base.textures[0].empty()) && (model->boolOr("solidlayer", false) || m.shader == "flat");
    if (const Json *instance = m_object ? m_object->find("instance") : nullptr; instance && instance->isObject()) {
        if (model->boolOr("instanced", false)) {
            applyInstance(*instance, layer, flatMaterial ? InstanceBase::FlatColor : InstanceBase::Picture);
        } else {
            degrade("instance values"); // a block on a model that does not take one
        }
    }
    const std::vector<std::string> &textures = layer.base.textures;
    if (textures.empty() || textures[0].empty()) {
        if (flatMaterial) {
            layer.flatColor = true; // a flat colour: the object's colour and alpha, over the object's size
            return true;
        }
        reject("material of '" + objectName + "' has no texture");
        return false;
    }
    if (const std::string &first = textures[0]; first.rfind("_rt_", 0) == 0) {
        // the scene drawn so far is the one render target a layer can start from (`fullscreen` and `compose` layers)
        if (first != "_rt_FullFrameBuffer" && first != "_rt_MipMappedFrameBuffer") {
            reject("render target texture '" + first + "': M2");
            return false;
        }
        layer.fullscreen = model->boolOr("fullscreen", false);
        layer.passthrough = model->boolOr("passthrough", false);
        if (layer.fullscreen) {
            layer.size = m_scene.orthoSize;
        }
        m.texturePath = first;
        // the other slots are files bound beside it (a shader that reads `g_Texture1` as its own picture; slot 0 is then only the scene)
        for (size_t i = 1; i < textures.size(); ++i) {
            if (!textures[i].empty()) {
                if (textures[i].rfind("_rt_", 0) == 0) {
                    reject("render target '" + textures[i] + "' as a base texture: M5");
                } else {
                    texture("materials/" + textures[i] + ".tex");
                }
            }
        }
        return true;
    }
    // the other slots of the pass (masks, normal maps: `g_Texture1` and on) are files bound beside the first
    for (size_t i = 1; i < textures.size(); ++i) {
        if (!textures[i].empty()) {
            if (textures[i].rfind("_rt_", 0) == 0) {
                reject("render target '" + textures[i] + "' as a base texture: M5");
            } else {
                texture("materials/" + textures[i] + ".tex");
            }
        }
    }
    m.texturePath = "materials/" + textures[0] + ".tex";
    texture(m.texturePath);
    return true;
}

// The `instance` block of an instanced model's object (the editor's "instance" of a material, `textures`, `usertextures`, `combos`
// and `constantshadervalues` of pass 0, plus an `id`): a texture replaces the material's in its slot, a constant or combo replaces
// the material's value of that key (the others stay). A flat-colour material has no picture to replace: its textures stay as they are.
void Loader::applyInstance(const Json &instance, ImageLayer &layer, InstanceBase base)
{
    for (const std::string &reason : userBindings(instance)) {
        degrade(reason);
    }
    for (const MediaBinding &b : mediaBindings(instance)) {
        layer.base.media.push_back(b);
    }
    for (const std::string &name : userTextureProperties(instance)) {
        auto &list = layer.base.userTextureProperties;
        if (std::find(list.begin(), list.end(), name) == list.end()) {
            list.push_back(name);
        }
    }
    if (const Json *constants = instance.find("constantshadervalues"); constants && constants->isObject()) {
        parseConstants(constants, layer.base.constants);
        ImageMaterial &m = layer.material;
        if (const Json *v = constants->find("alpha")) {
            m.alpha = number(v, m.alpha.value, "instance alpha");
        }
        if (const Json *v = constants->find("brightness")) {
            m.brightness = number(v, m.brightness.value, "instance brightness");
        }
        if (const Json *v = constants->find("color")) {
            m.color = color(v, m.color.value, "instance colour");
        }
    }
    parseUserShaderValues(instance.find("usershadervalues"), layer.base.constants); // after the literals: it keeps them
    if (const Json *combos = instance.find("combos"); combos && combos->isObject()) {
        for (const auto &[name, value] : combos->members()) {
            const std::string folded = foldCase(name);
            const bool plain = value.isNumber() || value.isBool();
            const bool set = plain && (value.isNumber() ? value.asNumber() != 0.0 : value.asBool());
            // the options the material loader takes: version and spritesheet apply, a zero is nothing, and lighting, reflection and fog
            // are set and named like the material's. Any other value (or a user binding) is not applied
            if (plain && (folded == "version" || folded == "spritesheet" || !set)) {
                parseCombo(name, value, layer.base.combos);
            } else if (plain && (folded == "lighting" || folded == "fog")) {
                parseCombo(name, value, layer.base.combos); // drawn, as the material's
            } else if (plain && folded == "reflection") {
                parseCombo(name, value, layer.base.combos); // drawn, as the material's
            } else {
                degrade("instance values");
            }
        }
    }
    if (base == InstanceBase::FlatColor) {
        return;
    }
    const std::vector<std::string> names = parseTextureList(instance.find("textures"));
    for (size_t slot = 0; slot < names.size(); ++slot) {
        if (names[slot].empty()) {
            continue;
        }
        if (slot >= kMaxInstanceTextureSlots) {
            degrade("instance texture slot"); // the engine binds no more slots than that
            continue;
        }
        if (layer.base.textures.size() <= slot) {
            layer.base.textures.resize(slot + 1);
        }
        layer.base.textures[slot] = names[slot];
    }
}

void Loader::object(const Json &o)
{
    m_scriptNode = static_cast<int>(m_scene.nodes.size());
    m_object = &o;
    const size_t scriptsBefore = m_scene.scripts.size();
    const size_t layersBefore = m_scene.layers.size();
    SceneNode node;
    node.id = static_cast<int>(o.numberOr("id", -1));
    node.parentId = static_cast<int>(o.numberOr("parent", -1));
    node.name = o.stringOr("name");
    node.origin = vec3(o.find("origin"), {0, 0, 0}, "origin", &node.originUser);
    node.scale = vec3(o.find("scale"), {1, 1, 1}, "scale", &node.scaleUser);
    node.angles = vec3(o.find("angles"), {0, 0, 0}, "angles", &node.anglesUser);
    const size_t nodeIndex = m_scene.nodes.size();
    using Kind = Scene::UserVector::Kind;
    if (!node.originUser.empty()) {
        m_scene.userVectors.push_back({Kind::Origin, nodeIndex, node.origin});
    }
    if (!node.scaleUser.empty()) {
        m_scene.userVectors.push_back({Kind::Scale, nodeIndex, node.scale});
    }
    if (!node.anglesUser.empty()) {
        m_scene.userVectors.push_back({Kind::Angles, nodeIndex, node.angles});
    }
    animation(o.find("origin"), PropertyAnimation::Target::Origin, m_scene.nodes.size(), 3);
    animation(o.find("scale"), PropertyAnimation::Target::Scale, m_scene.nodes.size(), 3);
    animation(o.find("angles"), PropertyAnimation::Target::Angles, m_scene.nodes.size(), 3);
    node.visible = boolean(o.find("visible"), true, "visibility");
    const size_t index = m_scene.nodes.size();
    m_scene.nodes.push_back(node);
    // `attachment` on any object (images, and the groups that carry the layers standing on a point): resolved once every object is
    // read, the parent may come later
    if (const Json *a = o.find("attachment"); a && a->isString() && !a->asString().empty()) {
        m_pendingAttachments.push_back({index, a->asString()});
    }

    if (o.find("particle") && !o.find("particle")->isNull()) {
        particleLayer(o, index);
    } else if (const Json *image = o.find("image"); image && image->isString()) {
        imageLayer(o, index);
        imageKeys(o);
    } else if (o.find("text") && !o.find("text")->isNull()) {
        textFrom(o, index);
    } else if (o.find("sound") && !o.find("sound")->isNull()) {
        soundObject(o, index);
    } else if (const Json *camera = o.find("camera"); camera && camera->isString()) {
        cameraObject(o, index);
    } else if (o.find("light") && !o.find("light")->isNull()) {
        lightObject(o, index);
    } else if (const Json *model = o.find("model"); model && !model->isNull()) {
        meshObject(o, index);
    } else if (const Json *shape = o.find("shape"); shape && !shape->isNull()) {
        shapeLayer(o, index);
    } else {
        // the object kinds WE knows besides the ones above (`camera` and `shape` have their own readers); a bare group has none
        for (const char *kind : {"reflected", "sprite"}) {
            if (const Json *k = o.find(kind); k && !k->isNull()) {
                degrade(std::string("object '") + kind + "'");
            }
        }
    }
    for (size_t i = scriptsBefore; i < m_scene.scripts.size(); ++i) {
        m_scene.scripts[i].name = node.name + "." + m_scene.scripts[i].property;
    }
    // An image layer's size and parallax depth may be bound to user properties too (a text layer's size is its text's; a
    // fullscreen layer covers the scene whatever the object says)
    if (m_scene.layers.size() > layersBefore && m_scene.layers.back().node == index) {
        ImageLayer &layer = m_scene.layers.back();
        const size_t layerIndex = m_scene.layers.size() - 1;
        layer.parallaxDepthUser = boundUser(o.find("parallaxDepth"));
        if (!layer.parallaxDepthUser.empty()) {
            m_scene.userVectors.push_back({Kind::ParallaxDepth, layerIndex, {layer.parallaxDepth.x, layer.parallaxDepth.y, 0.0f}});
        }
        if (!layer.text && !layer.fullscreen) {
            layer.sizeUser = boundUser(o.find("size"));
            if (!layer.sizeUser.empty()) {
                m_scene.userVectors.push_back({Kind::Size, layerIndex, {layer.size.x, layer.size.y, 0.0f}});
            }
        }
    }
    // Anything left is a group: a transform other objects hang on.
    m_scriptNode = -1;
    m_object = nullptr;
}

// A 3D mesh object: the model's parts and the material each part names (docs/scene-vk/formats.md, meshes). The node (origin,
// angles, scale, parent, visibility) was read with the object.
void Loader::meshObject(const Json &o, size_t node)
{
    const Json *modelKey = o.find("model");
    std::string path = modelKey->isString() ? modelKey->asString() : std::string();
    // a model JSON that points to the mesh as a `puppet` (the form image objects use) is read the same way
    if (path.size() > 5 && path.compare(path.size() - 5, 5, ".json") == 0) {
        const auto text = m_vfs.readText(path);
        const auto json = text ? parseJson(*text) : std::nullopt;
        const Json *puppet = json && json->isObject() ? json->find("puppet") : nullptr;
        path = puppet && puppet->isString() ? puppet->asString() : std::string();
        if (path.empty()) {
            reject("3D model '" + (modelKey->isString() ? modelKey->asString() : std::string("?")) + "' " + (text ? "names no mesh" : "not found"));
            return;
        }
    }
    // a model several objects use is read once: they share it (and the renderer shares its GPU buffers)
    auto cached = m_meshModels.find(path);
    if (cached == m_meshModels.end()) {
        MeshCacheEntry entry;
        if (const auto bytes = path.empty() ? std::nullopt : m_vfs.read(path); !bytes) {
            entry.error = "3D model '" + path + "' not found";
        } else {
            std::string error;
            if (auto parsed = parseMeshModel(std::span<const uint8_t>(bytes->data(), bytes->size()), &error)) {
                entry.model = std::make_shared<const MeshModel>(std::move(*parsed));
            } else {
                entry.error = "3D model: " + error;
            }
        }
        cached = m_meshModels.emplace(path, std::move(entry)).first;
    }
    if (!cached->second.model) {
        reject(cached->second.error);
        return;
    }
    const std::shared_ptr<const MeshModel> model = cached->second.model;
    if (model->morphTargets) {
        degrade("morph targets"); // drawn at rest
    }
    if (model->attachments) {
        degrade("mesh attachments");
    }
    if (model->skinned && model->rig.simulatedBones > 0) {
        degrade("bone physics of a 3D mesh"); // the spring bones of a puppet layer are simulated; a mesh keeps the animated pose
    }
    if (model->skinned && model->rig.ikBones > 0) {
        degrade("mesh bone IK");
    }
    if (const Json *fx = o.find("effects"); fx && fx->isArray() && !fx->items().empty()) {
        degrade("effects on a 3D mesh");
    }
    MeshLayer mesh;
    mesh.node = node;
    mesh.path = path;
    mesh.solid = o.boolOr("solid", false);
    // casts unless the file says otherwise: the library's mesh objects never write `"castshadow": true`, 24 write false and 211 leave it
    // out, among them every mesh of the scenes whose lights cast shadows (formats.md, "Shadows")
    mesh.castShadow = o.boolOr("castshadow", true);
    if (model->skinned) {
        // the puppet's skeleton on a mesh: it plays its animation layers like a puppet layer; without any it stands in its bind pose,
        // which is the mesh as the file has it (drawn unskinned)
        mesh.animationLayers = animationLayers(o);
        mesh.skinning = !mesh.animationLayers.empty() && model->rig.usedBones > 0 && !model->rig.animations.empty();
    }
    for (const MeshPart &part : model->parts) {
        std::string materialError;
        const auto material = loadMaterial(m_vfs, part.material, &materialError);
        if (!material || material->passes.empty()) {
            reject("material of 3D model '" + path + "': " + (material ? std::string("no passes") : materialError));
            return;
        }
        if (material->passes.size() > 1) {
            reject("multi-pass material: M2");
        }
        MaterialPass pass = material->passes[0];
        if (!pass.blendingKnown) {
            reject("blending '" + pass.blendingName + "': M2");
        }
        for (const std::string &d : pass.degraded) {
            degrade(d);
        }
        for (const std::string &t : pass.textures) {
            if (t.empty()) {
                continue;
            }
            if (t.rfind("_rt_", 0) == 0) {
                // the scene so far, or another layer's composite (the analysis checks that the object is an image layer: it may come
                // later in the file)
                if (t == "_rt_Reflection") {
                    // what a `reflected` layer would draw (none in the library does): the slot holds transparent black (drawMesh)
                    degrade("reflection target");
                } else if (t != "_rt_FullFrameBuffer" && t != "_rt_MipMappedFrameBuffer" && !layerCompositeId(t)) {
                    reject("render target '" + t + "' on a 3D mesh: M5");
                }
                continue;
            }
            texture("materials/" + t + ".tex");
        }
        if (mesh.skinning) {
            pass.combos["SKINNING"] = 1;
            pass.combos["BONECOUNT"] = static_cast<int>(model->rig.boneSlots); // the bones the vertices name: g_Bones holds those
        }
        mesh.materials.push_back(std::move(pass));
    }
    mesh.model = model;
    m_scene.order.push_back({DrawEntry::Kind::Mesh, m_scene.meshes.size()});
    m_scene.meshes.push_back(std::move(mesh));
}

// A light object (formats.md, "Lights"). The node (origin, angles, scale, parent, visibility, keyframes and scripts on the transform)
// was read with the object. Its values may be bound to user properties (the control point may not: named), the intensity may also
// be keyframed (the keyframes own it then, as for any property: the binding is dropped); a script on one of its values is not run
// (the file's value stays) and keyframes on anything but the intensity are not played: both are named.
void Loader::lightObject(const Json &o, size_t node)
{
    const Json *kind = o.find("light");
    static const std::map<std::string, LightType> kTypes = {{"lpoint", LightType::Point},
                                                            {"lspot", LightType::Spot},
                                                            {"ltube", LightType::Tube},
                                                            {"ldirectional", LightType::Directional},
                                                            {"point", LightType::LegacyPoint}};
    const auto type = kind->isString() ? kTypes.find(kind->asString()) : kTypes.end();
    if (type == kTypes.end()) {
        reject("light type '" + (kind->isString() ? kind->asString() : std::string("?")) + "'");
        return;
    }
    SceneLight light;
    light.node = node;
    light.type = type->second;
    const size_t index = m_scene.lights.size();
    // a light's property as a plain value: what a script or keyframes on it start from. A script on one of the values that have a handle
    // (colour, intensity, radius, exponent, the cones, density, the volumetrics exponent) runs; on anything else it is named
    const auto plain = [&](const char *key, bool animatable, bool scriptable = false) -> const Json * {
        const Json *j = o.find(key);
        if (j && j->isObject()) {
            if (j->find("script") && !scriptable) {
                degrade("light script");
                const Json *v = j->find("value");
                return v && !v->isNull() ? v : nullptr;
            }
            if (j->find("animation") && !animatable) {
                degrade("light animation");
            }
        }
        return j;
    };
    // keyframes on a value that has a target: they own it (the binding is dropped), as for any keyframed property
    const auto keyframed = [&](const char *key, PropertyAnimation::Target target, int components, auto &value) {
        if (const Json *j = o.find(key); j && j->isObject() && j->find("animation")) { // (with a script too: the keyframes play first)
            animation(j, target, index, components);
            value.user.clear();
        }
    };
    light.color = color(plain("color", true, true), {1, 1, 1}, "light colour");
    keyframed("color", PropertyAnimation::Target::LightColor, 3, light.color);
    light.intensity = number(plain("intensity", true, true), 1.0f, "light intensity");
    keyframed("intensity", PropertyAnimation::Target::LightIntensity, 1, light.intensity);
    light.radius = number(plain("radius", true, true), 1000.0f, "light radius");
    keyframed("radius", PropertyAnimation::Target::LightRadius, 1, light.radius);
    light.exponent = number(plain("exponent", true, true), 2.0f, "light exponent");
    keyframed("exponent", PropertyAnimation::Target::LightExponent, 1, light.exponent);
    light.innerCone = number(plain("innercone", true, true), 20.0f, "light inner cone");
    keyframed("innercone", PropertyAnimation::Target::LightInnerCone, 1, light.innerCone);
    light.outerCone = number(plain("outercone", true, true), 30.0f, "light outer cone");
    keyframed("outercone", PropertyAnimation::Target::LightOuterCone, 1, light.outerCone);
    light.density = number(plain("density", true, true), 2.0f, "light density");
    keyframed("density", PropertyAnimation::Target::LightDensity, 1, light.density);
    light.volumetricsExponent = number(plain("volumetricsexponent", true, true), 1.0f, "light volumetrics exponent");
    keyframed("volumetricsexponent", PropertyAnimation::Target::LightVolumetricsExponent, 1, light.volumetricsExponent);
    light.lightSourceSize = number(plain("lightsourcesize", false), 0.0f, "light source size");
    for (size_t i = 0; i < light.cascadeDistances.size(); ++i) {
        const std::string key = "cascadedistance" + std::to_string(i);
        light.cascadeDistances[i] = number(plain(key.c_str(), false), 0.0f, "light cascade distance");
    }
    const auto flag = [&](const char *key) {
        const Bound<bool> b = boolean(plain(key, false), false, "light switch");
        return b.value || !b.user.empty();
    };
    light.castShadow = flag("castshadow");
    light.castVolumetrics = flag("castvolumetrics");
    light.useCookie = flag("usecookie");
    if (const Json *cookie = o.find("cookie"); cookie && cookie->isString()) {
        light.cookie = cookie->asString();
    }
    if (const Json *cp = plain("controlpoint", false); cp && !cp->isNull()) {
        std::string user;
        const Vec3 p = vec3(cp, {0, 0, 0}, "light control point", &user);
        if (light.type == LightType::Tube) {
            light.controlPoint = p;
            if (!user.empty()) {
                degrade("light binding"); // the file's end stays
            }
        } else {
            degrade("light control point"); // what binding a control point does to the other kinds is not known
        }
    }
    if (light.type == LightType::Tube && !light.controlPoint) {
        degrade("tube without end"); // drawn as a point light at its origin
    }
    // the node's scale moves a tube's end (a point of the light's own space) but is not applied to the radius: what WE does with it is
    // not known (3429481890 scales a point light by 2.7 x 1.5 x 1.5)
    if (const Vec3 sc = m_scene.nodes[node].scale; !m_scene.nodes[node].scaleUser.empty() || std::fabs(sc.x - 1.0f) > 1e-4f
        || std::fabs(sc.y - 1.0f) > 1e-4f || std::fabs(sc.z - 1.0f) > 1e-4f) {
        degrade("light scale");
    }
    // what is not drawn yet. Shadows are drawn for point, spot and directional lights (render/shadows.h), as many per type as the
    // atlas has room for; the editor offers no shadow for the other kinds
    if (light.castShadow && (light.type == LightType::Tube || light.type == LightType::LegacyPoint)) {
        degrade(light.type == LightType::Tube ? "tube light shadows" : "older point light shadows");
    }
    if (light.castShadow && (light.type == LightType::Point || light.type == LightType::Spot || light.type == LightType::Directional)) {
        const auto shadowed = std::count_if(m_scene.lights.begin(), m_scene.lights.end(), [&](const SceneLight &l) { return l.type == light.type && l.castShadow; });
        const int cap = light.type == LightType::Point ? kMaxShadowPointLights
            : light.type == LightType::Spot            ? kMaxShadowSpotLights
                                                       : kMaxShadowDirectionalLights;
        if (shadowed >= cap) {
            degrade("too many shadowed lights"); // drawn without its shadow
        }
    }
    // the beam is drawn for point and spot lights (render/scenerenderer.cpp); the editor offers castvolumetrics for no other kind
    if (light.castVolumetrics && light.type != LightType::Point && light.type != LightType::Spot) {
        degrade(light.type == LightType::Tube ? "tube light volumetrics" : light.type == LightType::Directional ? "directional light volumetrics"
                                                                                                               : "older point light volumetrics");
    }
    if (light.useCookie) {
        degrade("light cookie");
    }
    const auto sameType = std::count_if(m_scene.lights.begin(), m_scene.lights.end(), [&](const SceneLight &l) { return l.type == light.type; });
    if (static_cast<size_t>(sameType) >= (light.type == LightType::LegacyPoint ? kMaxLegacyLights : kMaxLightsPerType)) {
        degrade("too many lights"); // loaded, but it does not reach the shaders (render/lights.h)
    }
    m_scene.lights.push_back(std::move(light));
}

// A camera object. Its origin, angles and visibility were read with the node; the zoom is a number, a user property or keyframes.
void Loader::cameraObject(const Json &o, size_t node)
{
    SceneCameraObject camera;
    camera.node = node;
    const Json *zoom = o.find("zoom");
    if (zoom && zoom->isObject() && zoom->find("script")) {
        degrade("camera zoom script"); // (not run: the zoom stays at its value)
        camera.zoom.value = static_cast<float>(zoom->numberOr("value", 1.0));
    } else {
        camera.zoom = number(zoom, 1.0f, "camera zoom");
    }
    if (!(camera.zoom.value > 0.0f)) {
        camera.zoom.value = 1.0f;
    }
    if (camera.zoom.user.empty()) { // a user property owns the value it is bound to; keyframes play only on a free one
        animation(zoom, PropertyAnimation::Target::CameraZoom, m_scene.cameraObjects.size(), 1);
    }
    camera.fov = number(o.find("fov"), m_scene.camera.fov, "camera fov");
    camera.fov.value = clampFov(camera.fov.value);
    camera.queueMode = o.stringOr("queuemode");
    camera.path = o.stringOr("path");
    if (!camera.path.empty()) {
        bool readable = false;
        if (const auto text = m_vfs.readText(camera.path)) {
            if (const auto json = parseJson(*text); json && json->isObject()) {
                if (const Json *paths = json->find("paths"); paths && paths->isArray()) {
                    camera.pathCount = paths->items().size();
                    readable = true;
                    for (const Json &p : paths->items()) {
                        if (p.isObject() && camera.paths.size() < kMaxCameraPaths) {
                            camera.paths.push_back(cameraPath(p));
                        }
                    }
                }
            }
        }
        // `camerafade`: the picture fades when a path ends and the next starts. With no path there is nothing to fade (the key is
        // written true by default). The paths play in a perspective scene only, so an orthographic one has no fade to draw: its
        // paths are named (`camera paths`) and the fade goes with them
        if (m_cameraFade && (!readable || camera.pathCount > 0)) {
            if (!readable || !m_scene.perspective) {
                degrade("camera fade");
            }
        }
        // a perspective scene plays the paths; an orthographic one has no camera to move along them (none in the library has
        // any). A file that cannot be read may hold some, so that is named too
        if (!readable || (camera.pathCount > 0 && !m_scene.perspective)) {
            degrade("camera paths");
        }
    }
    m_scene.cameraObjects.push_back(std::move(camera));
}

// One entry of a camera object's path file: the `eye`, `center` and `up` curves (c0..c2) and `fov` and `zoom` (a list of keys, or
// c0), played once for `options.length` frames at `options.fps`.
CameraPath Loader::cameraPath(const Json &p)
{
    CameraPath path;
    float fps = 30.0f, length = 1.0f;
    if (const Json *options = p.find("options"); options && options->isObject()) {
        fps = static_cast<float>(std::clamp(options->numberOr("fps", 30.0), 1.0, 240.0));
        length = static_cast<float>(std::clamp(options->numberOr("length", 1.0), 1.0, 1.0e6));
    }
    path.visible = p.boolOr("visible", true);
    const auto track = [&](PropertyAnimation &a, const char *key, int components) {
        a.fps = fps;
        a.length = length;
        a.mode = PropertyAnimation::Mode::Single;
        a.components = components;
        const Json *j = p.find(key);
        if (!j) {
            return;
        }
        if (j->isArray()) {
            readKeys(*j, a.curves[0]);
            return;
        }
        if (j->isObject()) {
            for (int c = 0; c < components; ++c) {
                if (const Json *keys = j->find("c" + std::to_string(c)); keys && keys->isArray()) {
                    readKeys(*keys, a.curves[static_cast<size_t>(c)]);
                }
            }
        }
    };
    track(path.eye, "eye", 3);
    track(path.center, "center", 3);
    track(path.up, "up", 3);
    track(path.fov, "fov", 1);
    track(path.zoom, "zoom", 1);
    return path;
}

size_t Loader::addNode(const std::string &name)
{
    SceneNode node;
    int id = 0;
    for (const SceneNode &n : m_scene.nodes) {
        id = std::max(id, n.id);
    }
    node.id = id + 1;
    node.name = name;
    m_scene.nodeIndexById.emplace(node.id, m_scene.nodes.size());
    m_scene.nodes.push_back(node);
    return m_scene.nodes.size() - 1;
}

// alpha (with its keyframes), brightness and colour: what image, text and shape layers share. Call before the layer is pushed
// (the keyframes are registered by the layer's index).
void Loader::layerLook(const Json &o, ImageLayer &layer)
{
    layer.alpha = number(o.find("alpha"), 1.0f, "alpha");
    if (layer.alpha.user.empty()) { // a user property owns the value it is bound to; keyframes play only on a free one
        animation(o.find("alpha"), PropertyAnimation::Target::Alpha, m_scene.layers.size(), 1);
    }
    layer.brightness = number(o.find("brightness"), 1.0f, "brightness");
    layer.color = color(o.find("color"), {1, 1, 1}, "colour");
}

int Loader::blendModeOf(const Json &o)
{
    const int mode = static_cast<int>(o.numberOr("colorBlendMode", 0));
    if (mode < 0 || mode > 32) {
        reject("colour blend mode: " + std::to_string(mode)); // common_blending.h knows 1..32
    }
    return mode;
}

// The effects of an object, shared by image, text and shape layers. It must run before the layer is pushed (the keyframes and
// the scripts it registers use m_scene.layers.size() as the layer index) and while m_scriptNode is set (Loader::object does).
void Loader::effectChain(const Json &o, ImageLayer &layer)
{
    if (const Json *effects = o.find("effects"); effects && effects->isArray()) {
        for (const Json &fx : effects->items()) {
            std::string effectError;
            std::vector<std::string> reasons;
            auto instance = loadEffectInstance(m_vfs, fx, &effectError, &reasons);
            for (const std::string &r : reasons) {
                reject(r);
            }
            if (!instance) {
                reject(effectError);
                continue;
            }
            // a script on the effect's visibility: "fxv:<effect>"
            if (const Json *visible = fx.find("visible"); visible && visible->isObject() && visible->find("script") && visible->find("script")->isString() && m_scriptNode >= 0) {
                addScript(*visible, "fxv:" + std::to_string(layer.effects.size()));
            }
            // scripts on the constants of an effect pass: "fx:<effect>:<pass>:<constant>" is their property
            if (const Json *passes = fx.find("passes"); passes && passes->isArray()) {
                for (size_t p = 0; p < passes->items().size(); ++p) {
                    const Json *constants = passes->items()[p].find("constantshadervalues");
                    if (!constants || !constants->isObject()) {
                        continue;
                    }
                    for (const auto &[key, value] : constants->members()) {
                        if (value.isObject() && value.find("script") && value.find("script")->isString()) {
                            if (m_scriptNode < 0) {
                                continue;
                            }
                            std::string lowered = key;
                            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            addScript(value, "fx:" + std::to_string(layer.effects.size()) + ":" + std::to_string(p) + ":" + lowered);
                        } else if (value.isObject() && value.find("animation")) {
                            // keyframes on the constant (an intro fade is a black layer whose `alpha` runs from 1 to 0)
                            std::string lowered = key;
                            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            int components = 0;
                            for (int c = 0; c < 3; ++c) {
                                if (const Json *keys = value.find("animation")->find("c" + std::to_string(c)); keys && keys->isArray()) {
                                    components = c + 1;
                                }
                            }
                            const size_t before = m_scene.animations.size();
                            animation(&value, PropertyAnimation::Target::EffectConstant, m_scene.layers.size(), std::max(1, components));
                            if (m_scene.animations.size() > before) {
                                PropertyAnimation &added = m_scene.animations.back();
                                added.effect = layer.effects.size();
                                added.pass = p;
                                added.key = lowered;
                            }
                        }
                    }
                }
            }
            layer.effects.push_back(std::move(*instance));
        }
    }
}

// A text object becomes an image layer whose texture the engine draws from the string (render/scenerenderer.cpp).
void Loader::textFrom(const Json &o, size_t node)
{
    ImageLayer layer;
    layer.node = node;
    layer.solid = o.boolOr("solid", false);
    TextSpec t;
    if (const Json *v = valueOf(o.find("text"), &t.user, "text"); v && v->isString()) {
        t.value = v->asString();
    } else if (const Json *plain = o.find("text"); plain && plain->isString()) {
        t.value = plain->asString();
    }
    t.font = o.stringOr("font");
    t.pointSize = number(o.find("pointsize"), 32.0f, "text size").value;
    const std::string legacy = o.stringOr("alignment", "center");
    t.horizontal = o.stringOr("horizontalalign", legacy.find("left") != std::string::npos ? "left" : legacy.find("right") != std::string::npos ? "right" : "center");
    // older scenes only carry `alignment` ("topleft", "bottomright", ...) for both axes
    t.vertical = o.stringOr("verticalalign", legacy.find("top") != std::string::npos ? "top" : legacy.find("bottom") != std::string::npos ? "bottom" : "center");
    t.padding = static_cast<float>(o.numberOr("padding", 0.0));
    // `maxwidth` is the width wrapping uses only while `limitwidth` is on (3796026374 keeps a stale 500 with it off and a 2095 wide box)
    t.maxWidth = o.boolOr("limitwidth", true) ? static_cast<float>(o.numberOr("maxwidth", 0.0)) : 0.0f;
    layer.alignment = std::string(t.vertical == "top" ? "top" : t.vertical == "bottom" ? "bottom" : "") + (t.horizontal == "left" ? "left" : t.horizontal == "right" ? "right" : "");
    if (layer.alignment.empty()) {
        layer.alignment = "center";
    }
    layer.parallaxDepth = vec2(o.find("parallaxDepth"), {1.0f, 1.0f}, "parallax depth");
    layerLook(o, layer);
    effectChain(o, layer);
    layer.colorBlendMode = blendModeOf(o);
    layer.material.shader = "genericimage2";
    layer.material.blending = Blending::Translucent;
    layer.base.shader = "genericimage2";
    layer.base.blending = Blending::Translucent;
    layer.base.blendingName = "translucent";
    layer.base.textures = {"__text:" + std::to_string(node)};
    layer.text = std::move(t);
    m_scene.order.push_back({DrawEntry::Kind::Image, m_scene.layers.size()});
    m_scene.layers.push_back(std::move(layer));
}

// A `"shape": "quad"` object is a layer of its own in WE, not an image file: a rectangle that only its effects give a look
// (the library's are light shafts). Assumptions, see docs/scene-vk/formats.md: a white translucent base picture, and
// kShapeSize scene units a side at scale 1 when the object has no `size`.
void Loader::shapeLayer(const Json &o, size_t node)
{
    const Json *shape = o.find("shape");
    if (!shape->isString() || shape->asString() != "quad") {
        reject("shape '" + (shape->isString() ? shape->asString() : std::string("?")) + "'");
        return;
    }
    ImageLayer layer;
    layer.node = node;
    layer.size = vec2(o.find("size"), {kShapeSize, kShapeSize}, "size");
    layer.alignment = o.stringOr("alignment", "center");
    layer.parallaxDepth = vec2(o.find("parallaxDepth"), {1.0f, 1.0f}, "parallax depth");
    if (const Json *cast = o.find("castshadow"); cast && cast->isBool() && cast->asBool()) {
        degrade("shadows from 2D layers"); // only meshes cast (none of the library's image layers asks to)
    }
    layerLook(o, layer);
    effectChain(o, layer);
    layer.colorBlendMode = blendModeOf(o);
    layer.material.shader = "genericimage2";
    layer.material.blending = Blending::Translucent;
    layer.material.texturePath = "materials/util/white.tex";
    layer.base.shader = "genericimage2";
    layer.base.blending = Blending::Translucent;
    layer.base.blendingName = "translucent";
    layer.base.textures = {"util/white"};
    texture(layer.material.texturePath);
    m_scene.order.push_back({DrawEntry::Kind::Image, m_scene.layers.size()});
    m_scene.layers.push_back(std::move(layer));
}

// An object's `animationlayers`: the puppet animations it plays (an image layer's puppet or a skinned 3D mesh).
std::vector<AnimationLayer> Loader::animationLayers(const Json &o)
{
    std::vector<AnimationLayer> out;
    if (const Json *layers = o.find("animationlayers"); layers && layers->isArray()) {
        if (layers->items().size() > kMaxAnimationLayers) {
            reject("puppet: more than " + std::to_string(kMaxAnimationLayers) + " animation layers on one object");
        }
        for (const Json &l : layers->items()) {
            if (!l.isObject() || out.size() >= kMaxAnimationLayers) {
                continue;
            }
            AnimationLayer a;
            a.animation = static_cast<uint32_t>(std::clamp(l.numberOr("animation", 0.0), 0.0, 4.0e9));
            m_animationLayer = static_cast<int>(out.size());
            a.blend = number(l.find("blend"), 1.0f, "animation blend");
            a.rate = number(l.find("rate"), 1.0f, "animation rate");
            a.visible = boolean(l.find("visible"), true, "animation layer visibility");
            m_animationLayer = -1;
            a.additive = l.boolOr("additive", false);
            out.push_back(std::move(a));
        }
    }
    return out;
}

void Loader::imageLayer(const Json &o, size_t node)
{
    ImageLayer layer;
    const size_t animationsBefore = m_scene.animations.size(); // the alpha's keyframes are registered by the layer's index: no layer, no keyframes
    layer.node = node;
    layer.solid = o.boolOr("solid", false);
    layer.size = vec2(o.find("size"), {0, 0}, "size");
    layer.alignment = o.stringOr("alignment", "center");
    layer.parallaxDepth = vec2(o.find("parallaxDepth"), {1.0f, 1.0f}, "parallax depth");
    if (const Json *cast = o.find("castshadow"); cast && cast->isBool() && cast->asBool()) {
        degrade("shadows from 2D layers"); // only meshes cast (none of the library's image layers asks to)
    }
    layerLook(o, layer);
    const std::string name = m_scene.nodes[node].name;

    layer.animationLayers = animationLayers(o);
    effectChain(o, layer);
    layer.colorBlendMode = blendModeOf(o);
    if (material(o.stringOr("image"), layer, name)) {
        m_scene.order.push_back({DrawEntry::Kind::Image, m_scene.layers.size()});
        m_scene.layers.push_back(std::move(layer));
    } else {
        m_scene.animations.resize(animationsBefore);
    }
}

void Loader::soundObject(const Json &o, size_t node)
{
    SoundObject s;
    s.node = node;
    s.name = o.stringOr("name");
    if (const Json *files = o.find("sound"); files && files->isArray()) {
        for (const Json &f : files->items()) {
            if (f.isString()) {
                s.files.push_back(f.asString());
            }
        }
    }
    const std::string mode = o.stringOr("playbackmode", "loop");
    if (mode != "loop" && mode != "random" && mode != "single") {
        reject("audio: playback mode '" + mode + "'");
        return;
    }
    s.random = mode == "random";
    s.loop = mode == "loop"; // single: the playlist's first file once (assumption: WE's "single" plays once and stops)
    s.minTime = std::clamp(static_cast<float>(o.numberOr("mintime", 1.0)), 0.0f, 3600.0f);
    s.maxTime = std::clamp(static_cast<float>(o.numberOr("maxtime", 5.0)), s.minTime, 3600.0f);
    s.volume = number(o.find("volume"), 1.0f, "sound volume");
    s.startSilent = o.boolOr("startsilent", false);
    std::vector<std::string> found;
    for (const std::string &f : s.files) {
        if (!m_vfs.exists(f)) {
            continue;
        }
        // by the extension here, by the bytes when it plays: mp3, ogg, flac and wav
        std::string extension = f.substr(f.find_last_of('.') == std::string::npos ? f.size() : f.find_last_of('.'));
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension != ".mp3" && extension != ".ogg" && extension != ".flac" && extension != ".wav") {
            reject("audio: only mp3, ogg, flac and wav sounds ('" + f + "')");
            return;
        }
        if (found.size() >= kMaxSoundFiles) {
            reject("audio: more than " + std::to_string(kMaxSoundFiles) + " files in one sound object");
            return;
        }
        found.push_back(f);
    }
    if (found.empty()) {
        return; // a sound with no file, or none that exists, is a sound object that plays nothing (WE logs it and goes on)
    }
    s.files = std::move(found);
    m_scene.sounds.push_back(std::move(s));
}

void Loader::particleLayer(const Json &o, size_t node)
{
    const Json *file = o.find("particle");
    if (!file || !file->isString()) {
        reject("particles: an object with an inline definition");
        return;
    }
    ParticleLayer layer;
    layer.node = node;
    layer.parallaxDepth = vec2(o.find("parallaxDepth"), {0.0f, 0.0f}, "parallax depth");
    if (const Json *over = o.find("instanceoverride"); over && over->isObject()) {
        layer.alpha = number(over->find("alpha"), 1.0f, "particle alpha");
        layer.size = number(over->find("size"), 1.0f, "particle size");
        layer.lifetime = number(over->find("lifetime"), 1.0f, "particle lifetime");
        layer.rate = number(over->find("rate"), 1.0f, "particle rate");
        layer.speed = number(over->find("speed"), 1.0f, "particle speed");
        layer.count = number(over->find("count"), 1.0f, "particle count");
        for (size_t n = 0; n < layer.controlPoints.size(); ++n) {
            if (const Json *cp = over->find("controlpoint" + std::to_string(n)); cp && !cp->isNull()) {
                layer.controlPoints[n] = vec3(cp, {0, 0, 0}, "particle control point");
            }
        }
        if (over->find("colorn")) {
            layer.color = color(over->find("colorn"), {1, 1, 1}, "particle colour");
        } else if (over->find("color")) {
            layer.color = color(over->find("color"), {1, 1, 1}, "particle colour"); // 0..255 is scaled by color()
        }
    }
    std::string error;
    auto def = loadParticleDefinition(m_vfs, file->asString(), &error);
    if (!def) {
        reject("particles: " + error);
        return;
    }
    for (const std::string &r : def->unsupported) {
        reject(r);
    }
    for (const std::string &r : def->degraded) {
        degrade(r);
    }
    layer.definition = std::make_shared<const ParticleDefinition>(std::move(*def));
    m_scene.order.push_back({DrawEntry::Kind::Particle, m_scene.particles.size()});
    m_scene.particles.push_back(std::move(layer));
}

// What an image object carries that is not followed: its own keys (its `instance` block is applied with its material).
void Loader::imageKeys(const Json &o)
{
    if (const Json *over = o.find("instanceoverride"); over && over->isObject() && !over->members().empty()) {
        degrade("image instanceoverride");
    }
    if (const Json *a = o.find("attachments"); a && ((a->isArray() && !a->items().empty()) || (a->isObject() && !a->members().empty()) || (a->isString() && !a->asString().empty()))) {
        degrade("attachments");
    }
}

// What the layers' materials and effects named while they loaded.
void Loader::layerDegradations()
{
    const auto property = [this](const std::string &name) {
        auto &list = m_scene.userTextureProperties;
        if (std::find(list.begin(), list.end(), name) == list.end()) {
            list.push_back(name);
        }
    };
    for (const ImageLayer &layer : m_scene.layers) {
        for (const std::string &r : layer.base.degraded) {
            degrade(r);
        }
        for (const std::string &name : layer.base.userTextureProperties) {
            property(name);
        }
        for (const EffectInstance &fx : layer.effects) {
            for (const std::string &r : fx.degraded) {
                degrade(r);
            }
            for (const std::string &name : fx.userTextureProperties) {
                property(name);
            }
        }
    }
}

void Loader::run(const Json &root)
{
    general(root.find("general"));
    camera(root.find("camera"));
    const Json *objects = root.find("objects");
    if (objects && objects->isArray()) {
        if (objects->items().size() > kMaxObjects) {
            reject("too many objects");
        } else {
            for (const Json &o : objects->items()) {
                if (o.isObject()) {
                    object(o);
                }
            }
        }
    }
    for (size_t i = 0; i < m_scene.nodes.size(); ++i) {
        m_scene.nodeIndexById.emplace(m_scene.nodes[i].id, i); // the first object with an id keeps it
    }
    resolveAttachments();
    if (m_autoProjection) {
        autoProjection();
    }
    for (ImageLayer &layer : m_scene.layers) {
        if (layer.projectLayer && (layer.size.x <= 0.0f || layer.size.y <= 0.0f)) {
            layer.size = m_scene.orthoSize;
        }
    }
    layerDegradations();
}

// An object with an `attachment` stands on that named point of its parent puppet's rig (an MDAT entry of the model: no bone of the
// 13 scenes' models has such a name). What does not resolve is named in the report with its reason.
void Loader::resolveAttachments()
{
    // a loop of parents (A under B under A, an object under itself) is reported: the transform walk stops at 32 levels, the picture is
    // not what the file means
    {
        const size_t count = m_scene.nodes.size();
        std::vector<uint8_t> state(count, 0); // 0 not seen, 1 on the walk now, 2 known to end
        for (size_t start = 0; start < count; ++start) {
            std::vector<size_t> walk;
            size_t at = start;
            bool ended = false; // the walk reached an object with no parent (or one that is not there)
            while (state[at] == 0) {
                state[at] = 1;
                walk.push_back(at);
                const int up = m_scene.nodes[at].parentId;
                const auto it = up < 0 ? m_scene.nodeIndexById.end() : m_scene.nodeIndexById.find(up);
                if (it == m_scene.nodeIndexById.end()) {
                    ended = true;
                    break;
                }
                at = it->second;
            }
            if (!ended && state[at] == 1) { // back on an object of this very walk
                degrade("parent loop at object '" + m_scene.nodes[at].name.substr(0, 48) + "'");
            }
            for (const size_t n : walk) {
                state[n] = 2;
            }
        }
    }
    std::unordered_map<size_t, size_t> layerOfNode;
    for (size_t i = 0; i < m_scene.layers.size(); ++i) {
        layerOfNode.emplace(m_scene.layers[i].node, i);
    }
    std::set<std::string> said;
    for (const PendingAttachment &p : m_pendingAttachments) {
        std::string name = p.name.substr(0, 48);
        std::string why;
        SceneNode &node = m_scene.nodes[p.node];
        const auto parent = node.parentId < 0 ? m_scene.nodeIndexById.end() : m_scene.nodeIndexById.find(node.parentId);
        const auto layer = parent == m_scene.nodeIndexById.end() ? layerOfNode.end() : layerOfNode.find(parent->second);
        if (parent == m_scene.nodeIndexById.end()) {
            why = "the object has no parent";
        } else if (layer == layerOfNode.end() || !m_scene.layers[layer->second].puppet) {
            why = "the parent is not a puppet";
        } else {
            const ImageLayer &puppetLayer = m_scene.layers[layer->second];
            const auto &points = puppetLayer.puppet->attachments;
            const auto found = std::find_if(points.begin(), points.end(), [&](const PuppetAttachment &a) { return a.name == p.name; });
            if (found == points.end()) {
                why = points.empty() ? "the parent's model has no attachment points" : "the parent's model has no such point";
            } else if (parent->second == p.node) {
                why = "the object is its own parent";
            } else {
                node.attachPoses = puppetLayer.attachmentPoses;
                ++puppetLayer.attachmentPoses->users;
                node.attachIndex = static_cast<size_t>(found - points.begin());
            }
        }
        if (!why.empty() && said.insert(name + "/" + why).second) {
            degrade("attachment '" + name + "': " + why);
        }
    }
}

// An `auto` orthogonal projection has no size of its own (assumption: nothing documents it). The scene is taken to be the
// picture that starts at the origin: of the visible image layers, the largest one whose lower left corner, after its world
// transform (parents included), sits on (0, 0), so a scene made of a 1500 x 1000 picture at (750, 500) is 1500 x 1000, however
// large the screen. Failing that, the largest visible layer.
void Loader::autoProjection()
{
    Vec2 anchoredSize{0.0f, 0.0f}, largestSize{0.0f, 0.0f};
    float anchoredArea = 0.0f, largestArea = 0.0f;
    const PropertyStore noProperties; // a layer is visible here when its own default says so
    for (const ImageLayer &layer : m_scene.layers) {
        if (layer.size.x <= 0.0f || layer.size.y <= 0.0f || layer.fullscreen || layer.text || !nodeVisible(m_scene, layer.node, noProperties)) {
            continue;
        }
        const Mat4 world = nodeWorldMatrix(m_scene, layer.node);
        const auto column = [&](int c) { return std::sqrt(world.at(0, c) * world.at(0, c) + world.at(1, c) * world.at(1, c) + world.at(2, c) * world.at(2, c)); };
        const float w = layer.size.x * column(0), h = layer.size.y * column(1);
        const Vec2 anchor{layer.alignment.find("left") != std::string::npos ? 0.0f : layer.alignment.find("right") != std::string::npos ? 1.0f : 0.5f,
                          layer.alignment.find("bottom") != std::string::npos ? 0.0f : layer.alignment.find("top") != std::string::npos ? 1.0f : 0.5f};
        const float left = world.at(0, 3) - anchor.x * w, bottom = world.at(1, 3) - anchor.y * h;
        if (w * h > largestArea) {
            largestArea = w * h;
            largestSize = {w, h};
        }
        if (std::fabs(left) < 1.0f && std::fabs(bottom) < 1.0f && w * h > anchoredArea) {
            anchoredArea = w * h;
            anchoredSize = {w, h};
        }
    }
    const Vec2 chosen = anchoredArea > 0.0f ? anchoredSize : largestSize;
    if (chosen.x > 0.0f && chosen.y > 0.0f && chosen.x <= kMaxSceneSize && chosen.y <= kMaxSceneSize) {
        m_scene.orthoSize = chosen;
    }
    for (ImageLayer &layer : m_scene.layers) {
        if (layer.fullscreen) {
            layer.size = m_scene.orthoSize;
        }
    }
}

} // namespace

double PropertyAnimation::endFrame() const
{
    double frame = 0.0;
    for (const auto &keys : curves) {
        if (!keys.empty()) {
            frame = std::max(frame, std::min(static_cast<double>(length), static_cast<double>(keys.back().frame)));
        }
    }
    return frame;
}

double PropertyAnimation::endSeconds() const
{
    if (!playback.playing || playback.rate <= 0.0) {
        return playback.from;
    }
    if (mode != Mode::Single) {
        return std::numeric_limits<double>::infinity();
    }
    return playback.from + std::max(0.0, endFrame() / static_cast<double>(fps) - playback.anchor) / playback.rate;
}

double PropertyAnimation::frameAt(double sceneSeconds) const
{
    double frame = clock(sceneSeconds) * static_cast<double>(fps);
    const double l = static_cast<double>(length);
    if (!std::isfinite(frame)) {
        return 0.0;
    }
    switch (mode) {
    case Mode::Loop:
        return std::fmod(std::max(0.0, frame), l);
    case Mode::Mirror: {
        const double m = std::fmod(std::max(0.0, frame), 2.0 * l);
        return m <= l ? m : 2.0 * l - m;
    }
    case Mode::Single:
        break;
    }
    return std::clamp(frame, 0.0, l);
}

bool PropertyAnimation::finishedAt(double sceneSeconds) const
{
    return mode == Mode::Single && clock(sceneSeconds) * static_cast<double>(fps) >= endFrame();
}

float sampleAnimation(const PropertyAnimation &animation, int component, double timeSeconds, float fallback)
{
    const double t = animation.clock(timeSeconds);
    if (animation.relative) {
        return component >= 0 && component < 3 ? animation.base[static_cast<size_t>(component)] + sampleCurve(animation, component, t, 0.0f) : fallback;
    }
    return sampleCurve(animation, component, t, fallback);
}

double CameraPath::seconds() const
{
    return static_cast<double>(eye.length) / static_cast<double>(eye.fps);
}

bool CameraPath::drivesCamera() const
{
    const auto driven = [](const PropertyAnimation &a) { return std::any_of(a.curves.begin(), a.curves.end(), [](const auto &c) { return !c.empty(); }); };
    return driven(eye) && driven(center);
}

CameraPathPose sampleCameraPath(const CameraPath &path, double seconds)
{
    const auto driven = [](const PropertyAnimation &a) { return std::any_of(a.curves.begin(), a.curves.end(), [](const auto &c) { return !c.empty(); }); };
    const auto vector = [&](const PropertyAnimation &a, Vec3 fallback) -> std::optional<Vec3> {
        if (!driven(a)) {
            return std::nullopt;
        }
        return Vec3{sampleAnimation(a, 0, seconds, fallback.x), sampleAnimation(a, 1, seconds, fallback.y), sampleAnimation(a, 2, seconds, fallback.z)};
    };
    CameraPathPose pose;
    pose.eye = vector(path.eye, {0.0f, 0.0f, 0.0f});
    pose.center = vector(path.center, {0.0f, 0.0f, -1.0f});
    pose.up = vector(path.up, {0.0f, 1.0f, 0.0f});
    if (driven(path.fov)) {
        pose.fov = sampleAnimation(path.fov, 0, seconds, 50.0f);
    }
    if (driven(path.zoom)) {
        pose.zoom = sampleAnimation(path.zoom, 0, seconds, 1.0f);
    }
    return pose;
}

int addImageLayerFromModel(Scene &scene, const Vfs &vfs, const std::string &modelPath, std::string *error)
{
    Loader loader(vfs, scene);
    const size_t node = loader.addNode("script layer");
    ImageLayer layer;
    layer.node = node;
    layer.alignment = "center";
    const size_t unsupportedBefore = scene.unsupported.size();
    if (!loader.imageFromModel(modelPath, layer, "script layer")) {
        scene.nodeIndexById.erase(scene.nodes.back().id);
        scene.nodes.pop_back();
        if (error) {
            *error = scene.unsupported.size() > unsupportedBefore ? scene.unsupported.back() : "cannot read the model '" + modelPath + "'";
        }
        return -1;
    }
    scene.order.push_back({DrawEntry::Kind::Image, scene.layers.size()});
    scene.layers.push_back(std::move(layer));
    return static_cast<int>(scene.layers.size()) - 1;
}

namespace {

// A number list from the shapes a script's description holds a vector in: `{x, y, z}` (what JSON.stringify makes of Vec2 / Vec3), an array,
// the file's "x y z" text or a single number (all components).
bool descriptionFinite(const float *values, int count)
{
    return std::all_of(values, values + count, [](float f) { return std::isfinite(f); });
}

// (a value that is not finite, "nan" text included, is no value: the key is left as the model has it)
bool descriptionVectorRaw(const Json *j, float *out, int count);
bool descriptionVector(const Json *j, float *out, int count)
{
    float read[4];
    std::copy(out, out + count, read);
    if (!descriptionVectorRaw(j, read, count) || !descriptionFinite(read, count)) {
        return false;
    }
    std::copy(read, read + count, out);
    return true;
}

bool descriptionVectorRaw(const Json *j, float *out, int count)
{
    if (!j) {
        return false;
    }
    if (j->isNumber()) {
        for (int i = 0; i < count; ++i) {
            out[i] = static_cast<float>(j->asNumber());
        }
        return true;
    }
    if (j->isString()) {
        return parseFloats(j->asString(), out, count) > 0;
    }
    if (j->isArray()) {
        int n = 0;
        for (const Json &item : j->items()) {
            if (n < count && item.isNumber()) {
                out[n++] = static_cast<float>(item.asNumber());
            }
        }
        return n > 0;
    }
    if (j->isObject()) {
        static const char *const keys[4] = {"x", "y", "z", "w"};
        bool any = false;
        for (int i = 0; i < count; ++i) {
            if (const Json *v = j->find(keys[i]); v && v->isNumber()) {
                out[i] = static_cast<float>(v->asNumber());
                any = true;
            }
        }
        return any;
    }
    return false;
}

} // namespace

// `thisScene.createLayer({...})` without a `text`: an image layer from the description's `image` (a model file; a solid colour layer,
// `models/util/solidlayer.json`, when it names none) with the keys it gives: name, origin, angles (degrees), scale, size, alpha, color
// (0..1), brightness, visible, alignment and parallaxDepth. `type` is not read: "solid" is the layer the default model makes.
int addDescribedImageLayer(Scene &scene, const Vfs &vfs, const Json &description, std::string *error)
{
    std::string model = description.stringOr("image");
    if (model.empty()) {
        model = "models/util/solidlayer.json";
    }
    const int index = addImageLayerFromModel(scene, vfs, model, error);
    if (index < 0) {
        return -1;
    }
    ImageLayer &layer = scene.layers[static_cast<size_t>(index)];
    SceneNode &node = scene.nodes[layer.node];
    // sane ranges, as the Loader holds the file's values to (a size within what a scene may be, finite numbers)
    constexpr float kCoordinate = 1.0e6f, kScale = 1.0e4f, kAngle = 36000.0f, kLook = 100.0f;
    const auto held = [](float value, float low, float high) { return std::clamp(value, low, high); };
    if (std::string name = description.stringOr("name"); !name.empty()) {
        node.name = name.substr(0, 256);
    }
    if (float f[3] = {node.origin.x, node.origin.y, node.origin.z}; descriptionVector(description.find("origin"), f, 3)) {
        node.origin = {held(f[0], -kCoordinate, kCoordinate), held(f[1], -kCoordinate, kCoordinate), held(f[2], -kCoordinate, kCoordinate)};
    }
    if (float f[3] = {0, 0, 0}; descriptionVector(description.find("angles"), f, 3)) {
        constexpr float kRadians = 3.14159265358979f / 180.0f;
        node.angles = {held(f[0], -kAngle, kAngle) * kRadians, held(f[1], -kAngle, kAngle) * kRadians, held(f[2], -kAngle, kAngle) * kRadians};
    }
    if (float f[3] = {1, 1, 1}; descriptionVector(description.find("scale"), f, 3)) { // (a Vec2 leaves z at 1, a number is all three)
        node.scale = {held(f[0], -kScale, kScale), held(f[1], -kScale, kScale), held(f[2], -kScale, kScale)};
    }
    if (float f[2] = {layer.size.x, layer.size.y}; descriptionVector(description.find("size"), f, 2) && f[0] > 0.0f && f[1] > 0.0f && f[0] <= kMaxSceneSize && f[1] <= kMaxSceneSize) {
        layer.size = {f[0], f[1]};
    }
    if (float f[3] = {1, 1, 1}; descriptionVector(description.find("color"), f, 3)) {
        layer.color = {{held(f[0], 0.0f, kLook), held(f[1], 0.0f, kLook), held(f[2], 0.0f, kLook)}, {}};
    }
    if (const Json *a = description.find("alpha"); a && a->isNumber() && std::isfinite(a->asNumber())) {
        layer.alpha = {held(static_cast<float>(a->asNumber()), 0.0f, 1.0f), {}};
    }
    if (const Json *b = description.find("brightness"); b && b->isNumber() && std::isfinite(b->asNumber())) {
        layer.brightness = {held(static_cast<float>(b->asNumber()), 0.0f, kLook), {}};
    }
    if (const Json *visible = description.find("visible"); visible && visible->isBool()) {
        node.visible = {visible->asBool(), {}};
    }
    if (const std::string alignment = description.stringOr("alignment"); !alignment.empty() && alignment.size() <= 32) {
        layer.alignment = alignment;
    }
    if (float f[2] = {layer.parallaxDepth.x, layer.parallaxDepth.y}; descriptionVector(description.find("parallaxDepth"), f, 2)) {
        layer.parallaxDepth = {held(f[0], -kLook, kLook), held(f[1], -kLook, kLook)};
    }
    return index;
}

int addTextLayer(Scene &scene, const Vfs &vfs, const std::string &json, std::string *error)
{
    const auto parsed = parseJson(json);
    if (!parsed || !parsed->isObject()) {
        if (error) {
            *error = "the layer description is not a JSON object";
        }
        return -1;
    }
    if (!parsed->find("text")) {
        return addDescribedImageLayer(scene, vfs, *parsed, error);
    }
    Loader loader(vfs, scene);
    const size_t node = loader.addNode("script text");
    loader.textFrom(*parsed, node);
    return static_cast<int>(scene.layers.size()) - 1;
}

std::optional<ProjectInfo> readProject(const Vfs &vfs)
{
    const auto text = vfs.readText("project.json");
    if (!text) {
        return std::nullopt;
    }
    const auto json = parseJson(*text);
    if (!json || !json->isObject()) {
        return std::nullopt;
    }
    ProjectInfo info;
    info.type = json->stringOr("type");
    info.file = json->stringOr("file");
    info.dependency = json->stringOr("dependency");
    return info;
}

void applyProjectDefaults(const Vfs &vfs, PropertyStore &props)
{
    const auto text = vfs.readText("project.json");
    const auto project = text ? parseJson(*text) : std::nullopt;
    const Json *general = project ? project->find("general") : nullptr;
    const Json *list = general ? general->find("properties") : nullptr;
    if (!list || !list->isObject()) {
        return;
    }
    size_t taken = 0;
    for (const auto &[key, prop] : list->members()) {
        if (++taken > 4096 || props.find(key)) {
            continue;
        }
        const Json *value = prop.isObject() ? prop.find("value") : nullptr;
        if (value && value->isBool()) {
            props.set(key, value->asBool());
        } else if (value && value->isNumber()) {
            props.set(key, value->asNumber());
        } else if (value && value->isString()) {
            props.setFromText(key, value->asString());
        }
    }
}

std::string sceneFileName(const Vfs &vfs)
{
    if (const auto project = readProject(vfs); project && !project->file.empty()) {
        return project->file;
    }
    return "scene.json";
}

std::optional<Scene> loadScene(const Vfs &vfs, const std::string &sceneFile, std::string *error)
{
    const auto text = vfs.readText(sceneFile);
    if (!text) {
        if (error) {
            *error = "cannot read " + sceneFile;
        }
        return std::nullopt;
    }
    std::string parseError;
    const auto root = parseJson(*text, &parseError);
    if (!root || !root->isObject()) {
        if (error) {
            *error = sceneFile + ": " + (root ? "not a JSON object" : parseError);
        }
        return std::nullopt;
    }
    Scene scene;
    Loader(vfs, scene).run(*root);
    return scene;
}

namespace {
using UserKind = Scene::UserVector::Kind;

bool boundToNode(UserKind kind)
{
    return kind == UserKind::Origin || kind == UserKind::Scale || kind == UserKind::Angles;
}

// The user property a bound vector reads, "" when its node or layer is gone.
const std::string &userNameOf(const Scene &scene, const Scene::UserVector &u)
{
    static const std::string none;
    if (boundToNode(u.kind)) {
        if (u.index >= scene.nodes.size()) {
            return none;
        }
        const SceneNode &n = scene.nodes[u.index];
        return u.kind == UserKind::Origin ? n.originUser : u.kind == UserKind::Scale ? n.scaleUser : n.anglesUser;
    }
    if (u.index >= scene.layers.size()) {
        return none;
    }
    const ImageLayer &l = scene.layers[u.index];
    return u.kind == UserKind::Size ? l.sizeUser : l.parallaxDepthUser;
}

// Sets `components` of the field to `value`; true when one changed.
bool assign(float *field, const float *value, int components)
{
    bool changed = false;
    for (int i = 0; i < components; ++i) {
        changed = changed || field[i] != value[i];
        field[i] = value[i];
    }
    return changed;
}
} // namespace

bool applyUserVectors(Scene &scene, const PropertyStore &props)
{
    bool changed = false;
    for (const Scene::UserVector &u : scene.userVectors) {
        const std::string &name = userNameOf(scene, u);
        if (name.empty()) {
            continue;
        }
        const bool onNode = boundToNode(u.kind);
        const int count = onNode ? 3 : 2;
        float value[3] = {u.file.x, u.file.y, u.file.z};
        if (const auto given = props.getVector(name)) {
            for (int i = 0; i < count; ++i) {
                if (given->size() == 1) {
                    value[i] = static_cast<float>((*given)[0]);
                } else if (static_cast<size_t>(i) < given->size()) {
                    value[i] = static_cast<float>((*given)[static_cast<size_t>(i)]);
                }
            }
        }
        if (onNode) {
            SceneNode &n = scene.nodes[u.index];
            Vec3 &target = u.kind == UserKind::Origin ? n.origin : u.kind == UserKind::Scale ? n.scale : n.angles;
            float field[3] = {target.x, target.y, target.z};
            if (assign(field, value, 3)) {
                target = {field[0], field[1], field[2]};
                changed = true;
            }
        } else {
            ImageLayer &l = scene.layers[u.index];
            Vec2 &target = u.kind == UserKind::Size ? l.size : l.parallaxDepth;
            float field[2] = {target.x, target.y};
            if (assign(field, value, 2)) {
                target = {field[0], field[1]};
                changed = true;
            }
        }
    }
    return changed;
}

Mat4 nodeWorldMatrix(const Scene &scene, size_t node)
{
    Mat4 world = Mat4::identity();
    size_t current = node;
    for (int depth = 0; depth < 32; ++depth) {
        const SceneNode &n = scene.nodes[current];
        world = translation(n.origin) * rotationEuler(n.angles) * scaling(n.scale) * world;
        if (n.attachPoses && n.attachIndex < n.attachPoses->matrices.size()) {
            world = n.attachPoses->matrices[n.attachIndex] * world; // the parent puppet's point, in its mesh space
        }
        if (n.parentId < 0) {
            break;
        }
        const auto parent = scene.nodeIndexById.find(n.parentId);
        if (parent == scene.nodeIndexById.end()) {
            break;
        }
        current = parent->second;
    }
    return world;
}

std::optional<int> layerCompositeId(const std::string &name)
{
    const std::string prefix = "_rt_imageLayerComposite_";
    if (name.size() < prefix.size() + 3 || name.compare(0, prefix.size(), prefix) != 0) {
        return std::nullopt;
    }
    const std::string tail = name.substr(name.size() - 2);
    if (tail != "_a" && tail != "_b") {
        return std::nullopt;
    }
    const std::string digits = name.substr(prefix.size(), name.size() - prefix.size() - 2);
    if (digits.empty() || digits.size() > 9 || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    return std::stoi(digits);
}

std::optional<size_t> imageLayerOfObject(const Scene &scene, int id)
{
    for (size_t i = 0; i < scene.layers.size(); ++i) {
        const size_t node = scene.layers[i].node;
        if (node < scene.nodes.size() && scene.nodes[node].id == id) {
            return i;
        }
    }
    return std::nullopt;
}

bool nodeVisible(const Scene &scene, size_t node, const PropertyStore &props)
{
    size_t current = node;
    for (int depth = 0; depth < 32; ++depth) {
        const SceneNode &n = scene.nodes[current];
        if (!resolve(n.visible, props)) {
            return false;
        }
        if (n.parentId < 0) {
            return true;
        }
        const auto parent = scene.nodeIndexById.find(n.parentId);
        if (parent == scene.nodeIndexById.end()) {
            return true;
        }
        current = parent->second;
    }
    return true;
}

} // namespace murasaki::scene
