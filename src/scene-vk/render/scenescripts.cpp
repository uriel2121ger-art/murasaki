#include "render/scenescripts.h"

#include "assets/tex.h"
#include "assets/texfile.h"
#include "puppet/mdl.h"
#include "shaders/glslcompiler.h"
#include "util/json.h"
#include "util/log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>

namespace murasaki::scene {

namespace {

constexpr double kDegrees = 180.0 / 3.14159265358979323846;

ScriptValue vec3(Vec3 v)
{
    return ScriptValue::ofVector({v.x, v.y, v.z});
}
ScriptValue vec2(Vec2 v)
{
    return ScriptValue::ofVector({v.x, v.y});
}

bool same(const ScriptValue &a, const ScriptValue &b)
{
    if (a.type != b.type) {
        return false;
    }
    switch (a.type) {
    case ScriptValue::Type::Bool:
        return a.boolean == b.boolean;
    case ScriptValue::Type::Number:
        return a.number == b.number;
    case ScriptValue::Type::String:
        return a.text == b.text;
    case ScriptValue::Type::Vector:
        return a.count == b.count && std::equal(a.vec.begin(), a.vec.begin() + a.count, b.vec.begin());
    default:
        return true;
    }
}

float asFloat(const ScriptValue &v, float fallback)
{
    return v.type == ScriptValue::Type::Number ? static_cast<float>(v.number) : v.type == ScriptValue::Type::Bool ? (v.boolean ? 1.0f : 0.0f) : fallback;
}

Vec3 asVec3(const ScriptValue &v, Vec3 fallback)
{
    if (v.type == ScriptValue::Type::Vector && v.count >= 3) {
        return {static_cast<float>(v.vec[0]), static_cast<float>(v.vec[1]), static_cast<float>(v.vec[2])};
    }
    if (v.type == ScriptValue::Type::Number) {
        const float f = static_cast<float>(v.number);
        return {f, f, f};
    }
    return fallback;
}

Vec2 asVec2(const ScriptValue &v, Vec2 fallback)
{
    if (v.type == ScriptValue::Type::Vector && v.count >= 2) {
        return {static_cast<float>(v.vec[0]), static_cast<float>(v.vec[1])};
    }
    return fallback;
}

} // namespace

namespace {

ScriptValue toScriptValue(const PropertyValue &v)
{
    if (const auto *b = std::get_if<bool>(&v)) {
        return ScriptValue::ofBool(*b);
    }
    if (const auto *d = std::get_if<double>(&v)) {
        return ScriptValue::ofNumber(*d);
    }
    if (const auto *s = std::get_if<std::string>(&v)) {
        return ScriptValue::ofText(*s);
    }
    ScriptValue out;
    out.type = ScriptValue::Type::Vector;
    const auto &list = std::get<std::vector<double>>(v);
    for (size_t i = 0; i < list.size() && i < 4; ++i) {
        out.vec[i] = list[i];
        out.count = static_cast<int>(i) + 1;
    }
    return out.count >= 2 ? out : ScriptValue{};
}

// WE's language keys: "en-us", "de-de", "es-es": the locale's language and region.
std::string languageKey()
{
    for (const char *name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char *v = std::getenv(name);
        if (v && v[0] != '\0' && std::string_view(v) != "C" && std::string_view(v) != "POSIX") {
            std::string key(v);
            key = key.substr(0, key.find_first_of(".@"));
            std::replace(key.begin(), key.end(), '_', '-');
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
            return key.find('-') == std::string::npos ? key + "-" + key : key;
        }
    }
    return "en-us";
}

// The property a keyframe animation drives, as scripts name it (a property script's target, a linked track's key).
std::string animationProperty(const PropertyAnimation &a)
{
    switch (a.target) {
    case PropertyAnimation::Target::Origin:
        return "origin";
    case PropertyAnimation::Target::Scale:
        return "scale";
    case PropertyAnimation::Target::Angles:
        return "angles";
    case PropertyAnimation::Target::Alpha:
        return "alpha";
    case PropertyAnimation::Target::CameraZoom:
        return "zoom";
    case PropertyAnimation::Target::LightIntensity:
        return "intensity"; // (the file's keys: a linked track's `parent.key` names them; scripts reach the values as "light.<key>")
    case PropertyAnimation::Target::LightColor:
        return "color";
    case PropertyAnimation::Target::LightRadius:
        return "radius";
    case PropertyAnimation::Target::LightExponent:
        return "exponent";
    case PropertyAnimation::Target::LightInnerCone:
        return "innercone";
    case PropertyAnimation::Target::LightOuterCone:
        return "outercone";
    case PropertyAnimation::Target::LightDensity:
        return "density";
    case PropertyAnimation::Target::LightVolumetricsExponent:
        return "volumetricsexponent";
    case PropertyAnimation::Target::EffectConstant:
        return "fx:" + std::to_string(a.effect) + ":" + std::to_string(a.pass) + ":" + a.key;
    }
    return {};
}

} // namespace

std::string scriptStorageFile(const std::string &itemDir)
{
    const std::string dir = shaderCacheDirectory();
    std::string name = std::filesystem::path(itemDir).lexically_normal().filename().string();
    if (name.empty()) {
        name = std::filesystem::path(itemDir).lexically_normal().parent_path().filename().string();
    }
    for (char &c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') {
            c = '_';
        }
    }
    if (dir.empty() || name.empty() || name.find_first_not_of('.') == std::string::npos) {
        return {};
    }
    return dir + "/script-storage/" + name + ".json";
}

SceneScripts::SceneScripts(Scene &scene, const Vfs &vfs, const PropertyStore &props, ScriptFlags &flags)
    : m_scene(scene), m_vfs(vfs), m_props(props), m_flags(flags)
{
}

SceneScripts::~SceneScripts()
{
    if (m_host && m_updates > 0) {
        logInfo("scripts: {} ({} in realms of their own), {:.1f} MiB, updates {:.2f} ms a frame on average and {:.2f} at most over {} frames",
                m_host->scriptCount(), m_host->ownRealmCount(), static_cast<double>(m_host->memoryUsed()) / 1048576.0, 1000.0 * m_updateTotal / static_cast<double>(m_updates),
                1000.0 * m_updateMax, m_updates);
    }
}

size_t SceneScripts::memoryUsed() const
{
    return m_host ? m_host->memoryUsed() : 0;
}

size_t SceneScripts::ownRealms() const
{
    return m_host ? m_host->ownRealmCount() : 0;
}

int SceneScripts::handleFor(Handle::Kind kind, size_t index)
{
    for (size_t i = 0; i < m_handles.size(); ++i) {
        if (m_handles[i].kind == kind && m_handles[i].index == index) {
            return static_cast<int>(i);
        }
    }
    m_handles.push_back({kind, index});
    m_initial.push_back(initialConfig(m_handles.back()));
    return static_cast<int>(m_handles.size()) - 1;
}

int SceneScripts::handleForNode(size_t node)
{
    for (size_t l = 0; l < m_scene.layers.size(); ++l) {
        if (m_scene.layers[l].node == node) {
            return handleFor(Handle::Kind::Image, l);
        }
    }
    for (size_t l = 0; l < m_scene.particles.size(); ++l) {
        if (m_scene.particles[l].node == node) {
            return handleFor(Handle::Kind::Particle, l);
        }
    }
    return handleFor(Handle::Kind::Node, node);
}

SoundObject *SceneScripts::soundOf(size_t node)
{
    for (SoundObject &s : m_scene.sounds) {
        if (s.node == node) {
            return &s;
        }
    }
    return nullptr;
}

namespace {
std::string numberText(double v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}
std::string vectorText(Vec3 v)
{
    return numberText(v.x) + " " + numberText(v.y) + " " + numberText(v.z);
}
std::string quoted(const std::string &s)
{
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += static_cast<unsigned char>(c) < 0x20 ? ' ' : c;
    }
    return out + "\"";
}
} // namespace

// The object as the file had it, in the file's own notation (getInitialLayerConfig).
std::string SceneScripts::initialConfig(const Handle &h) const
{
    const SceneNode &node = m_scene.nodes[nodeOf(h)];
    std::string out = "{\"id\":" + std::to_string(node.id) + ",\"name\":" + quoted(node.name) + ",\"origin\":" + quoted(vectorText(node.origin))
        + ",\"scale\":" + quoted(vectorText(node.scale))
        + ",\"angles\":" + quoted(vectorText({static_cast<float>(node.angles.x * kDegrees), static_cast<float>(node.angles.y * kDegrees), static_cast<float>(node.angles.z * kDegrees)}))
        + ",\"visible\":" + (resolve(node.visible, m_props) ? "true" : "false");
    if (h.kind == Handle::Kind::Image) {
        const ImageLayer &l = m_scene.layers[h.index];
        if (!l.model.empty()) {
            out += ",\"image\":" + quoted(l.model);
        }
        out += ",\"size\":" + quoted(numberText(l.size.x) + " " + numberText(l.size.y)) + ",\"alpha\":" + numberText(resolve(l.alpha, m_props))
            + ",\"color\":" + quoted(vectorText(resolve(l.color, m_props))) + ",\"solid\":" + (l.solid ? "true" : "false");
        if (l.text) {
            out += ",\"text\":" + quoted(l.text->value) + ",\"pointsize\":" + numberText(l.text->pointSize) + ",\"font\":" + quoted(l.text->font);
        }
    }
    return out + "}";
}

size_t SceneScripts::nodeOf(const Handle &h) const
{
    switch (h.kind) {
    case Handle::Kind::Image:
        return m_scene.layers[h.index].node;
    case Handle::Kind::Particle:
        return m_scene.particles[h.index].node;
    default:
        return h.index;
    }
}

void SceneScripts::start()
{
    if (m_scene.scripts.empty()) {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    m_host = std::make_unique<ScriptHost>(*this, m_limits);
    m_host->setStorageFile(m_scene.scriptStorageFile);
    // scripts are added in file order, every module first and then every init (ScriptHost::setDeferredInits); a script's init may add
    // layers and the inits after it see them
    m_host->setDeferredInits(true);
    const std::vector<ScriptBinding> bindings = m_scene.scripts;
    for (const ScriptBinding &b : bindings) {
        // the handle of the object the script is attached to: its layer when it has one, else the bare node
        Handle::Kind kind = Handle::Kind::Node;
        size_t index = b.node;
        for (size_t i = 0; i < m_scene.layers.size(); ++i) {
            if (m_scene.layers[i].node == b.node) {
                kind = Handle::Kind::Image;
                index = i;
            }
        }
        for (size_t i = 0; i < m_scene.particles.size(); ++i) {
            if (m_scene.particles[i].node == b.node) {
                kind = Handle::Kind::Particle;
                index = i;
            }
        }
        ScriptTarget t;
        t.layer = b.scene ? -1 : handleFor(kind, index);
        t.property = b.property;
        t.source = b.source;
        t.properties = b.properties;
        t.propertyUsers = b.propertyUsers;
        for (const auto &[key, user] : b.propertyUsers) { // a script property bound to a user property starts at its value
            if (const PropertyValue *p = m_props.find(user)) {
                if (const ScriptValue v = toScriptValue(*p); v.type != ScriptValue::Type::Undefined) {
                    t.properties[key] = v;
                }
            }
        }
        t.name = b.name;
        std::string error;
        if (!m_host->add(t, get(t.layer, b.property), &error)) {
            ++m_failed;
            m_failures.push_back(b.name + ": " + error);
            logError("script '{}' failed: {}", b.name, error);
        }
    }
    for (const auto &[name, error] : m_host->initAll()) {
        ++m_failed;
        m_failures.push_back(name + ": " + error);
        logError("script '{}' failed: {}", name, error);
    }
    m_host->setDeferredInits(false); // scripts added later (by a running script's createLayer) start at once
    announceProperties();
    m_host->applyGeneralSettings({{"language", ScriptValue::ofText(languageKey())}});
    m_startSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    logInfo("scripts: {} started in {:.0f} ms ({} in realms of their own), {:.1f} MiB", m_host->scriptCount(), m_startSeconds * 1000.0,
            m_host->ownRealmCount(),
            static_cast<double>(m_host->memoryUsed()) / 1048576.0);
}

void SceneScripts::announceProperties()
{
    if (!m_host || m_revision == m_props.revision()) {
        return;
    }
    m_revision = m_props.revision();
    std::map<std::string, ScriptValue> changed;
    std::map<std::string, ScriptValue> all;
    for (const auto &[key, value] : m_props.values()) {
        const ScriptValue v = toScriptValue(value);
        if (v.type == ScriptValue::Type::Undefined) {
            continue;
        }
        all[key] = v;
        const auto it = m_announced.find(key);
        if (it == m_announced.end() || it->second != value) {
            changed[key] = v;
        }
    }
    m_announced = m_props.values();
    m_host->applyScriptPropertyChanges(changed);
    m_host->applyUserProperties(changed, all);
}

void SceneScripts::update(const ScriptFrame &frame)
{
    m_time = frame.time;
    if (m_host) {
        const auto started = std::chrono::steady_clock::now();
        announceProperties();
        m_host->update(frame);
        m_lastUpdate = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        m_updateTotal += m_lastUpdate;
        m_updateMax = std::max(m_updateMax, m_lastUpdate);
        ++m_updates;
    }
}

// The cursor events go to solid image layers: the point in the layer's own units (from its origin), and a hit when it is inside the
// layer's quad.
bool SceneScripts::hitTest(int layer, double x, double y, double *localX, double *localY)
{
    if (layer < 0 || static_cast<size_t>(layer) >= m_handles.size()) {
        return false;
    }
    const Handle h = m_handles[static_cast<size_t>(layer)];
    if (h.kind != Handle::Kind::Image) {
        return false;
    }
    const ImageLayer &l = m_scene.layers[h.index];
    if (!l.solid || l.size.x <= 0.0f || l.size.y <= 0.0f || !resolve(m_scene.nodes[l.node].visible, m_props)) {
        return false;
    }
    float ax = 0.5f, ay = 0.5f;
    if (l.alignment.find("left") != std::string::npos) {
        ax = 0.0f;
    } else if (l.alignment.find("right") != std::string::npos) {
        ax = 1.0f;
    }
    if (l.alignment.find("top") != std::string::npos) {
        ay = 1.0f;
    } else if (l.alignment.find("bottom") != std::string::npos) {
        ay = 0.0f;
    }
    const Mat4 inv = inverse(nodeWorldMatrix(m_scene, l.node));
    const Vec3 p{static_cast<float>(inv.at(0, 0) * x + inv.at(0, 1) * y + inv.at(0, 3)), static_cast<float>(inv.at(1, 0) * x + inv.at(1, 1) * y + inv.at(1, 3)), 0.0f};
    if (localX) {
        *localX = p.x;
    }
    if (localY) {
        *localY = p.y;
    }
    const float u = p.x + ax * l.size.x;
    const float v = p.y + ay * l.size.y;
    return u >= 0.0f && u <= l.size.x && v >= 0.0f && v <= l.size.y;
}

bool SceneScripts::hasUpdates() const
{
    return m_host && m_host->hasUpdates();
}

bool SceneScripts::active() const
{
    return m_host && m_host->active();
}

bool SceneScripts::usesAudio() const
{
    return m_host && m_host->usesAudio();
}

const std::vector<std::string> &SceneScripts::messages() const
{
    static const std::vector<std::string> none;
    return m_host ? m_host->messages() : none;
}

ScriptValue SceneScripts::sceneGet(const std::string &p)
{
    const auto stored = [&](const char *key, const ScriptValue &fallback) {
        const auto it = m_stored.find(key);
        return it == m_stored.end() ? fallback : it->second;
    };
    if (p == "bloom") return ScriptValue::ofBool(resolve(m_scene.bloom.enabled, m_props));
    if (p == "bloomstrength") return ScriptValue::ofNumber(resolve(m_scene.bloom.strength, m_props));
    if (p == "bloomthreshold") return ScriptValue::ofNumber(resolve(m_scene.bloom.threshold, m_props));
    if (p == "clearcolor") return vec3(m_scene.clearColor);
    if (p == "fov") return ScriptValue::ofNumber(m_scene.camera.fov);
    if (p == "nearz") return ScriptValue::ofNumber(m_scene.camera.nearZ);
    if (p == "farz") return ScriptValue::ofNumber(m_scene.camera.farZ);
    if (p == "camerashake") return ScriptValue::ofBool(resolve(m_scene.shake.enabled, m_props));
    if (p == "camerashakespeed") return ScriptValue::ofNumber(resolve(m_scene.shake.speed, m_props));
    if (p == "camerashakeamplitude") return ScriptValue::ofNumber(resolve(m_scene.shake.amplitude, m_props));
    if (p == "camerashakeroughness") return ScriptValue::ofNumber(resolve(m_scene.shake.roughness, m_props));
    if (p == "cameraparallax") return ScriptValue::ofBool(resolve(m_scene.parallax.enabled, m_props));
    if (p == "cameraparallaxamount") return ScriptValue::ofNumber(resolve(m_scene.parallax.amount, m_props));
    if (p == "cameraparallaxdelay") return ScriptValue::ofNumber(resolve(m_scene.parallax.delay, m_props));
    if (p == "cameraparallaxmouseinfluence") return ScriptValue::ofNumber(resolve(m_scene.parallax.mouseInfluence, m_props));
    if (p == "camera.eye") return vec3(m_scene.camera.eye);
    if (p == "camera.center") return vec3(m_scene.camera.center);
    if (p == "camera.up") return vec3(m_scene.camera.up);
    // kept as they are set: there is no light to apply the colours to, and nothing applies the scene's own fade (the loader names
    // `camera fade`). The scene's zoom is applied; a camera object's zoom is the object's, not this one
    if (p == "zoom" || p == "camera.zoom") return ScriptValue::ofNumber(resolve(m_scene.camera.zoom, m_props));
    if (p == "clearenabled") return ScriptValue::ofBool(resolve(m_scene.clearEnabled, m_props));
    if (p == "ambientcolor") return stored("ambientcolor", ScriptValue::ofVector({0.2, 0.2, 0.2}));
    if (p == "skylightcolor") return stored("skylightcolor", ScriptValue::ofVector({0.3, 0.3, 0.3}));
    if (p == "camerafade") return stored("camerafade", ScriptValue::ofBool(false));
    return {};
}

bool SceneScripts::sceneSet(const std::string &p, const ScriptValue &v)
{
    const auto flag = [&](Bound<bool> &b) { b = {v.type == ScriptValue::Type::Bool ? v.boolean : v.number != 0.0, {}}; };
    const auto number = [&](Bound<float> &b) { b = {asFloat(v, b.value), {}}; };
    if (p == "bloom") flag(m_scene.bloom.enabled);
    else if (p == "bloomstrength") number(m_scene.bloom.strength);
    else if (p == "bloomthreshold") number(m_scene.bloom.threshold);
    else if (p == "clearcolor") m_scene.clearColor = asVec3(v, m_scene.clearColor);
    else if (p == "fov") m_scene.camera.fov = clampFov(asFloat(v, m_scene.camera.fov));
    else if (p == "nearz") m_scene.camera.nearZ = asFloat(v, m_scene.camera.nearZ);
    else if (p == "farz") m_scene.camera.farZ = asFloat(v, m_scene.camera.farZ);
    else if (p == "camerashake") flag(m_scene.shake.enabled);
    else if (p == "camerashakespeed") number(m_scene.shake.speed);
    else if (p == "camerashakeamplitude") number(m_scene.shake.amplitude);
    else if (p == "camerashakeroughness") number(m_scene.shake.roughness);
    else if (p == "cameraparallax") flag(m_scene.parallax.enabled);
    else if (p == "cameraparallaxamount") number(m_scene.parallax.amount);
    else if (p == "cameraparallaxdelay") number(m_scene.parallax.delay);
    else if (p == "cameraparallaxmouseinfluence") number(m_scene.parallax.mouseInfluence);
    else if (p == "camera.eye") m_scene.camera.eye = asVec3(v, m_scene.camera.eye);
    else if (p == "camera.center") m_scene.camera.center = asVec3(v, m_scene.camera.center);
    else if (p == "camera.up") m_scene.camera.up = asVec3(v, m_scene.camera.up);
    else if (p == "clearenabled") flag(m_scene.clearEnabled);
    else if (p == "zoom" || p == "camera.zoom") {
        // only a finite zoom above zero is taken (the loader and the renderer treat anything else as 1)
        if (const float z = asFloat(v, 0.0f); std::isfinite(z) && z > 0.0f) {
            m_scene.camera.zoom = {z, {}};
        }
    }
    else if (p == "ambientcolor" || p == "skylightcolor" || p == "camerafade") m_stored[p] = v;
    else return false;
    return true;
}

bool SceneScripts::shortcutLaunch(const std::string &name)
{
    if (!m_shortcutLaunch || name.empty() || name.size() > 63) {
        return false;
    }
    for (const char c : name) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
            return false;
        }
    }
    const PropertyValue *value = m_props.find(name);
    if (!value || !std::holds_alternative<std::string>(*value) || std::get<std::string>(*value).empty()) {
        return false; // no application chosen for it
    }
    return m_shortcutLaunch(name);
}

// The video texture of an image layer (its albedo, the first texture of the material): the texture's name, empty when it is no video.
// A header read, kept by name.
std::string SceneScripts::videoNameOf(const ImageLayer &l)
{
    if (l.text || l.flatColor || l.base.textures.empty() || l.base.textures[0].empty()) {
        return {};
    }
    const std::string &name = l.base.textures[0];
    const auto known = m_videoTextures.find(name);
    if (known != m_videoTextures.end()) {
        return known->second ? name : std::string();
    }
    bool video = false;
    if (name.rfind("_rt_", 0) != 0 && name.rfind("__text:", 0) != 0 && m_vfs.exists("materials/" + name + ".tex")) {
        const auto tex = readTexFile(m_vfs, "materials/" + name + ".tex", TexReadOptions{false});
        video = tex && tex->hasVideo();
    }
    m_videoTextures.emplace(name, video);
    return video ? name : std::string();
}

ScriptValue SceneScripts::videoGet(const ImageLayer &l, const std::string &p)
{
    const std::string name = videoNameOf(l);
    if (p == "present") {
        return ScriptValue::ofBool(!name.empty());
    }
    if (name.empty()) {
        return {};
    }
    const auto it = m_scene.videos.find(name);
    const VideoControl c = it == m_scene.videos.end() ? VideoControl{} : it->second;
    if (p == "duration") return ScriptValue::ofNumber(c.duration);
    if (p == "rate") return ScriptValue::ofNumber(c.rate);
    if (p == "loop") return ScriptValue::ofBool(c.loop);
    if (p == "playing") return ScriptValue::ofBool(c.playing);
    if (p == "time") return ScriptValue::ofNumber(c.seek ? *c.seek : c.position);
    if (p == "ended") return ScriptValue::ofNumber(c.ended);
    return {};
}

bool SceneScripts::videoSet(const ImageLayer &l, const std::string &p, const ScriptValue &v)
{
    const std::string name = videoNameOf(l);
    if (name.empty()) {
        return false;
    }
    VideoControl &c = m_scene.videos[name];
    if (p == "rate") {
        // within 16 times either way: a script's 1e300 would be an infinite float, and a fast one keeps the decoder flat out
        c.rate = v.type == ScriptValue::Type::Number && std::isfinite(v.number) ? static_cast<float>(std::clamp(v.number, -16.0, 16.0)) : 1.0f;
    } else if (p == "loop") {
        c.loop = v.type == ScriptValue::Type::Bool ? v.boolean : asFloat(v, 1.0f) != 0.0f;
    } else if (p == "play") {
        c.playing = true;
    } else if (p == "pause") {
        c.playing = false;
    } else if (p == "stop") {
        c.playing = false;
        c.seek = 0.0;
    } else if (p == "seek") {
        const double t = v.type == ScriptValue::Type::Number ? v.number : 0.0;
        c.seek = std::isfinite(t) ? std::max(0.0, t) : 0.0;
    } else {
        return false;
    }
    return true;
}

ScriptValue SceneScripts::get(int layer, const std::string &p)
{
    if (layer == -1 && p.rfind("shortcut.launch:", 0) == 0) {
        return ScriptValue::ofBool(shortcutLaunch(p.substr(16)));
    }
    if (layer == -1 && p.rfind("scene.", 0) == 0) {
        return sceneGet(p.substr(6));
    }
    if (layer < 0 || static_cast<size_t>(layer) >= m_handles.size()) {
        return {};
    }
    if (p == "destroyed") {
        return ScriptValue::ofBool(m_destroyed.count(layer) != 0);
    }
    // a script on an effect's visibility: "fxv:<effect>"
    if (p.rfind("fxv:", 0) == 0) {
        return effectGet(layer, std::atoi(p.c_str() + 4), "visible");
    }
    // a script on an effect constant: "fx:<effect>:<pass>:<constant>" is its property
    if (p.rfind("fx:", 0) == 0) {
        unsigned effect = 0, pass = 0;
        int consumed = 0;
        if (std::sscanf(p.c_str(), "fx:%u:%u:%n", &effect, &pass, &consumed) >= 2 && consumed > 0) {
            return effectGet(layer, static_cast<int>(effect), "constant:" + std::to_string(pass) + ":" + p.substr(static_cast<size_t>(consumed)));
        }
        return {};
    }
    if (p == "initialConfig") {
        return ScriptValue::ofText(m_initial[static_cast<size_t>(layer)]);
    }
    const Handle h = m_handles[static_cast<size_t>(layer)];
    if (p.rfind("animation", 0) == 0) {
        return animationGet(h, p);
    }
    if (p == "alayers" || p.rfind("alayer.", 0) == 0 || p == "boneCount" || p.rfind("bone", 0) == 0) {
        return rigGet(rigOf(h), p);
    }
    const SceneNode &node = m_scene.nodes[nodeOf(h)];
    if (p == "origin") {
        return vec3(node.origin);
    }
    if (p == "scale") {
        return vec3(node.scale);
    }
    if (p == "angles") {
        return ScriptValue::ofVector({node.angles.x * kDegrees, node.angles.y * kDegrees, node.angles.z * kDegrees});
    }
    if (p == "visible") {
        return ScriptValue::ofBool(resolve(node.visible, m_props));
    }
    if (p == "name") {
        return ScriptValue::ofText(node.name);
    }
    if (p.rfind("light.", 0) == 0) {
        for (const SceneLight &light : m_scene.lights) {
            if (light.node != nodeOf(h)) {
                continue;
            }
            if (p == "light.color") return vec3(resolve(light.color, m_props));
            if (p == "light.intensity") return ScriptValue::ofNumber(resolve(light.intensity, m_props));
            if (p == "light.radius") return ScriptValue::ofNumber(resolve(light.radius, m_props));
            if (p == "light.exponent") return ScriptValue::ofNumber(resolve(light.exponent, m_props));
            if (p == "light.innercone") return ScriptValue::ofNumber(resolve(light.innerCone, m_props));
            if (p == "light.outercone") return ScriptValue::ofNumber(resolve(light.outerCone, m_props));
            if (p == "light.density") return ScriptValue::ofNumber(resolve(light.density, m_props));
            if (p == "light.volumetricsexponent") return ScriptValue::ofNumber(resolve(light.volumetricsExponent, m_props));
        }
        return {};
    }
    if (h.kind == Handle::Kind::Image) {
        const ImageLayer &l = m_scene.layers[h.index];
        if (p == "alpha") {
            return ScriptValue::ofNumber(resolve(l.alpha, m_props));
        }
        if (p == "brightness") {
            return ScriptValue::ofNumber(resolve(l.brightness, m_props));
        }
        // the material's own constants (a script on `constantshadervalues`), multiplied with the object's values at draw time
        if (p == "material.alpha") {
            return ScriptValue::ofNumber(resolve(l.material.alpha, m_props));
        }
        if (p == "material.brightness") {
            return ScriptValue::ofNumber(resolve(l.material.brightness, m_props));
        }
        if (p == "material.color") {
            return vec3(resolve(l.material.color, m_props));
        }
        if (p == "color") {
            return vec3(resolve(l.color, m_props));
        }
        if (p == "size") {
            return vec2(l.size);
        }
        if (p == "parallaxDepth") {
            return vec2(l.parallaxDepth);
        }
        if (p == "textureFrame") {
            return ScriptValue::ofNumber(std::max(0, l.textureFrame));
        }
        if (p.rfind("video.", 0) == 0) {
            return videoGet(l, p.substr(6));
        }
        if (p == "alignment") {
            return ScriptValue::ofText(l.alignment);
        }
        if (l.text) {
            if (p == "text") {
                std::string value = l.text->value;
                if (!l.text->user.empty()) {
                    if (const PropertyValue *pv = m_props.find(l.text->user); pv && std::holds_alternative<std::string>(*pv)) {
                        value = std::get<std::string>(*pv);
                    }
                }
                return ScriptValue::ofText(value);
            }
            if (p == "pointsize") {
                return ScriptValue::ofNumber(l.text->pointSize);
            }
            if (p == "font") {
                return ScriptValue::ofText(l.text->font);
            }
        }
    } else if (h.kind == Handle::Kind::Particle) {
        const ParticleLayer &l = m_scene.particles[h.index];
        if (p == "rate") {
            return ScriptValue::ofNumber(resolve(l.rate, m_props));
        }
        if (p == "count") {
            return ScriptValue::ofNumber(resolve(l.count, m_props));
        }
        if (p == "lifetime") {
            return ScriptValue::ofNumber(resolve(l.lifetime, m_props));
        }
        if (p == "speed") {
            return ScriptValue::ofNumber(resolve(l.speed, m_props));
        }
        if (p == "size") {
            return ScriptValue::ofNumber(resolve(l.size, m_props));
        }
        if (p == "alpha") {
            return ScriptValue::ofNumber(resolve(l.alpha, m_props));
        }
        if (p == "color") {
            return vec3(resolve(l.color, m_props));
        }
        if (p == "parallaxDepth") {
            return vec2(l.parallaxDepth);
        }
        if (p == "colorn") {
            return vec3(resolve(l.color, m_props));
        }
        if (p == "playing") {
            return ScriptValue::ofBool(!l.paused && !l.stopped);
        }
        if (p.rfind("controlpoint", 0) == 0 && p.size() == 13 && p[12] >= '0' && p[12] <= '7') {
            const auto &cp = l.controlPoints[static_cast<size_t>(p[12] - '0')];
            return vec3(cp ? *cp : Vec3{0, 0, 0});
        }
    }
    // ---- what only some objects have ----
    if (p == "solid" || p == "perspective") {
        if (h.kind == Handle::Kind::Image) {
            const ImageLayer &l = m_scene.layers[h.index];
            return ScriptValue::ofBool(p == "solid" ? l.solid : l.perspective);
        }
    }
    if (h.kind == Handle::Kind::Image) {
        const ImageLayer &l = m_scene.layers[h.index];
        if (l.text) {
            const TextSpec &t = *l.text;
            if (p == "horizontalalign") return ScriptValue::ofText(t.horizontal);
            if (p == "verticalalign") return ScriptValue::ofText(t.vertical);
            if (p == "padding") return ScriptValue::ofNumber(t.padding);
            if (p == "maxwidth") return ScriptValue::ofNumber(t.maxWidth);
            if (p == "limitwidth") return ScriptValue::ofBool(t.maxWidth > 0.0f);
        }
        // kept as they are set: the text engine has no background box, row limit or anchor
        const std::string key = "h" + std::to_string(layer) + ":" + p;
        if (const auto it = m_stored.find(key); it != m_stored.end()) {
            return it->second;
        }
        if (l.text) {
            if (p == "opaquebackground" || p == "limitrows") return ScriptValue::ofBool(false);
            if (p == "backgroundcolor") return ScriptValue::ofVector({0, 0, 0});
            if (p == "maxrows") return ScriptValue::ofNumber(0);
            if (p == "anchor") return ScriptValue::ofText("center");
        }
    }
    if (h.kind == Handle::Kind::Node || h.kind == Handle::Kind::Image || h.kind == Handle::Kind::Particle) {
        if (const SoundObject *sound = soundOf(nodeOf(h)); sound) {
            if (p == "volume") return ScriptValue::ofNumber(resolve(sound->volume, m_props));
            if (p == "playing") return ScriptValue::ofBool(sound->playing);
        }
    }
    return {};
}

void SceneScripts::set(int layer, const std::string &p, const ScriptValue &v)
{
    if (layer == -1 && p.rfind("scene.", 0) == 0 && v.type != ScriptValue::Type::Undefined) {
        const ScriptValue before = sceneGet(p.substr(6));
        if (!same(before, v) && sceneSet(p.substr(6), v)) {
            m_flags.dirty = true;
        }
        return;
    }
    if (layer < 0 || static_cast<size_t>(layer) >= m_handles.size() || v.type == ScriptValue::Type::Undefined) {
        return;
    }
    if (p.rfind("animation.", 0) == 0) {
        animationSet(m_handles[static_cast<size_t>(layer)], p, v);
        return;
    }
    const ScriptValue before = get(layer, p);
    if (same(before, v)) {
        return;
    }
    if (p.rfind("fxv:", 0) == 0) {
        effectSet(layer, std::atoi(p.c_str() + 4), "visible", v);
        return;
    }
    if (p.rfind("fx:", 0) == 0) {
        unsigned effect = 0, pass = 0;
        int consumed = 0;
        if (std::sscanf(p.c_str(), "fx:%u:%u:%n", &effect, &pass, &consumed) >= 2 && consumed > 0) {
            effectSet(layer, static_cast<int>(effect), "constant:" + std::to_string(pass) + ":" + p.substr(static_cast<size_t>(consumed)), v);
        }
        return;
    }
    const Handle h = m_handles[static_cast<size_t>(layer)];
    SceneNode &node = m_scene.nodes[nodeOf(h)];
    bool known = true;
    if (p == "origin") {
        node.origin = asVec3(v, node.origin);
    } else if (p == "scale") {
        node.scale = asVec3(v, node.scale);
        m_flags.textChanged = true; // a text's pixel size follows its scale
    } else if (p == "angles") {
        const Vec3 d = asVec3(v, {0, 0, 0});
        node.angles = {static_cast<float>(d.x / kDegrees), static_cast<float>(d.y / kDegrees), static_cast<float>(d.z / kDegrees)};
    } else if (p == "visible") {
        node.visible = {v.type == ScriptValue::Type::Bool ? v.boolean : v.number != 0.0, {}};
    } else if (p.rfind("light.", 0) == 0) {
        known = false;
        for (SceneLight &light : m_scene.lights) {
            if (light.node != nodeOf(h)) {
                continue;
            }
            known = true;
            if (p == "light.color") light.color = {asVec3(v, light.color.value), {}};
            else if (p == "light.intensity") light.intensity = {asFloat(v, light.intensity.value), {}};
            else if (p == "light.radius") light.radius = {asFloat(v, light.radius.value), {}};
            else if (p == "light.exponent") light.exponent = {asFloat(v, light.exponent.value), {}};
            else if (p == "light.innercone") light.innerCone = {asFloat(v, light.innerCone.value), {}};
            else if (p == "light.outercone") light.outerCone = {asFloat(v, light.outerCone.value), {}};
            else if (p == "light.density") light.density = {asFloat(v, light.density.value), {}};
            else if (p == "light.volumetricsexponent") light.volumetricsExponent = {asFloat(v, light.volumetricsExponent.value), {}};
            else known = false;
        }
    } else if (h.kind == Handle::Kind::Image) {
        ImageLayer &l = m_scene.layers[h.index];
        if (p == "alpha") {
            l.alpha = {asFloat(v, 1.0f), {}};
        } else if (p == "material.alpha") {
            l.material.alpha = {asFloat(v, 1.0f), {}};
        } else if (p == "material.brightness") {
            l.material.brightness = {asFloat(v, 1.0f), {}};
        } else if (p == "material.color") {
            l.material.color = {asVec3(v, {1, 1, 1}), {}};
        } else if (p == "brightness") {
            l.brightness = {asFloat(v, 1.0f), {}};
        } else if (p == "color") {
            l.color = {asVec3(v, {1, 1, 1}), {}};
        } else if (p == "size") {
            l.size = asVec2(v, l.size);
        } else if (p == "parallaxDepth") {
            l.parallaxDepth = asVec2(v, l.parallaxDepth);
        } else if (p == "textureFrame") {
            l.textureFrame = static_cast<int>(std::clamp(asFloat(v, 0.0f), 0.0f, 100000.0f));
        } else if (p.rfind("video.", 0) == 0) {
            known = videoSet(l, p.substr(6), v);
        } else if (p == "alignment" && v.type == ScriptValue::Type::String) {
            l.alignment = v.text;
            if (l.text) {
                l.text->horizontal = v.text.find("left") != std::string::npos ? "left" : v.text.find("right") != std::string::npos ? "right" : "center";
                l.text->vertical = v.text.find("top") != std::string::npos ? "top" : v.text.find("bottom") != std::string::npos ? "bottom" : "center";
            }
        } else if (l.text && p == "text" && v.type == ScriptValue::Type::String) {
            l.text->value = v.text;
            l.text->user.clear();
            m_flags.textChanged = true;
        } else if (l.text && p == "pointsize") {
            l.text->pointSize = asFloat(v, l.text->pointSize);
            m_flags.textChanged = true;
        } else if (l.text && p == "font" && v.type == ScriptValue::Type::String) {
            l.text->font = v.text;
            m_flags.textChanged = true;
        } else {
            known = false;
        }
    } else if (h.kind == Handle::Kind::Particle) {
        ParticleLayer &l = m_scene.particles[h.index];
        if (p == "rate") {
            l.rate = {asFloat(v, 1.0f), {}};
        } else if (p == "count") {
            l.count = {asFloat(v, 1.0f), {}};
        } else if (p == "lifetime") {
            l.lifetime = {asFloat(v, 1.0f), {}};
        } else if (p == "speed") {
            l.speed = {asFloat(v, 1.0f), {}};
        } else if (p == "size") {
            l.size = {asFloat(v, 1.0f), {}};
        } else if (p == "alpha") {
            l.alpha = {asFloat(v, 1.0f), {}};
        } else if (p == "color") {
            l.color = {asVec3(v, {1, 1, 1}), {}};
        } else if (p == "parallaxDepth") {
            l.parallaxDepth = asVec2(v, l.parallaxDepth);
        } else {
            known = false;
        }
        if (known && p != "origin" && p != "scale" && p != "angles" && p != "visible") {
            m_flags.particlesChanged = true;
        }
    } else {
        known = false;
    }
    if (!known) {
        known = setExtra(layer, h, p, v);
    }
    if (known) {
        m_flags.dirty = true;
    }
}

// The properties only some objects have (see get): solid, text layout, IParticleSystem, sound objects, animation layers.
std::array<double, 16> SceneScripts::worldMatrix(int layer)
{
    std::array<double, 16> out{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (layer < 0 || static_cast<size_t>(layer) >= m_handles.size()) {
        return out;
    }
    const Mat4 m = nodeWorldMatrix(m_scene, nodeOf(m_handles[static_cast<size_t>(layer)]));
    for (size_t i = 0; i < 16; ++i) {
        out[i] = static_cast<double>(m.m[i]);
    }
    return out;
}

bool SceneScripts::setExtra(int layer, const Handle &h, const std::string &p, const ScriptValue &v)
{
    const bool truth = v.type == ScriptValue::Type::Bool ? v.boolean : v.number != 0.0;
    if (p.rfind("alayer.", 0) == 0 || p.rfind("bonePhysics.", 0) == 0) {
        return rigSet(rigOf(h), p, v);
    }
    if (h.kind == Handle::Kind::Image) {
        ImageLayer &l = m_scene.layers[h.index];
        if (p == "solid") {
            l.solid = truth;
            return true;
        }
        if (p == "perspective") {
            l.perspective = truth;
            return true;
        }
        if (l.text) {
            TextSpec &t = *l.text;
            if (p == "horizontalalign" && v.type == ScriptValue::Type::String) {
                t.horizontal = v.text;
                m_flags.textChanged = true;
                return true;
            }
            if (p == "verticalalign" && v.type == ScriptValue::Type::String) {
                t.vertical = v.text;
                m_flags.textChanged = true;
                return true;
            }
            if (p == "padding") {
                t.padding = asFloat(v, t.padding);
                m_flags.textChanged = true;
                return true;
            }
            if (p == "maxwidth") {
                t.maxWidth = std::max(0.0f, asFloat(v, t.maxWidth));
                m_flags.textChanged = true;
                return true;
            }
            if (p == "limitwidth") {
                if (!truth) {
                    m_stored["h" + std::to_string(layer) + ":maxwidth-kept"] = ScriptValue::ofNumber(t.maxWidth);
                    t.maxWidth = 0.0f;
                } else if (t.maxWidth <= 0.0f) {
                    const auto it = m_stored.find("h" + std::to_string(layer) + ":maxwidth-kept");
                    t.maxWidth = it != m_stored.end() ? static_cast<float>(it->second.number) : m_scene.orthoSize.x;
                }
                m_flags.textChanged = true;
                return true;
            }
            if (p == "opaquebackground" || p == "backgroundcolor" || p == "limitrows" || p == "maxrows" || p == "anchor") {
                m_stored["h" + std::to_string(layer) + ":" + p] = v;
                return true;
            }
        }
    } else if (h.kind == Handle::Kind::Particle) {
        ParticleLayer &l = m_scene.particles[h.index];
        if (p == "colorn") {
            l.color = {asVec3(v, {1, 1, 1}), {}};
            m_flags.particlesChanged = true;
            return true;
        }
        if (p.rfind("controlpoint", 0) == 0 && p.size() == 13 && p[12] >= '0' && p[12] <= '7') {
            l.controlPoints[static_cast<size_t>(p[12] - '0')] = asVec3(v, {0, 0, 0});
            m_flags.particlesChanged = true;
            return true;
        }
        if (p == "psystem.play") {
            l.paused = false;
            l.stopped = false;
            return true;
        }
        if (p == "psystem.pause") {
            l.paused = true;
            return true;
        }
        if (p == "psystem.stop") {
            l.stopped = true;
            l.paused = false;
            return true;
        }
        if (p == "psystem.emit") {
            l.burst += static_cast<uint32_t>(std::clamp(v.type == ScriptValue::Type::Number ? v.number : 1.0, 0.0, 10000.0));
            return true;
        }
    }
    if (SoundObject *sound = soundOf(nodeOf(h)); sound) {
        if (p == "volume") {
            sound->volume = {asFloat(v, 1.0f), {}};
            return true;
        }
        if (p == "playing") {
            sound->playing = truth;
            return true;
        }
    }
    return false;
}

ScriptValue SceneScripts::animationGet(const Handle &h, const std::string &p) const
{
    const size_t node = nodeOf(h);
    const auto &all = m_scene.animations;
    if (p.rfind("animation:", 0) == 0 || p.rfind("animation@", 0) == 0) {
        const bool byName = p[9] == ':';
        const std::string key = p.substr(10);
        for (size_t i = 0; i < all.size(); ++i) {
            if (all[i].node == node && (byName ? key.empty() || all[i].name == key : animationProperty(all[i]) == key || "light." + animationProperty(all[i]) == key)) {
                return ScriptValue::ofNumber(static_cast<double>(i));
            }
        }
        return ScriptValue::ofNumber(-1);
    }
    const size_t dot = p.find('.', 10);
    const size_t i = static_cast<size_t>(std::atoi(p.c_str() + 10));
    if (p.rfind("animation.", 0) != 0 || dot == std::string::npos || i >= all.size() || all[i].node != node) {
        return {};
    }
    const PropertyAnimation &a = all[i];
    const std::string field = p.substr(dot + 1);
    if (field == "fps") return ScriptValue::ofNumber(a.fps);
    if (field == "frameCount") return ScriptValue::ofNumber(a.length);
    if (field == "duration") return ScriptValue::ofNumber(static_cast<double>(a.length) / static_cast<double>(a.fps));
    if (field == "name") return ScriptValue::ofText(a.name);
    if (field == "rate") return ScriptValue::ofNumber(a.playback.rate);
    if (field == "playing") return ScriptValue::ofBool(a.playback.playing && a.playback.rate > 0.0 && !a.finishedAt(m_time));
    if (field == "frame") return ScriptValue::ofNumber(a.frameAt(m_time));
    return {};
}

void SceneScripts::animationSet(const Handle &h, const std::string &p, const ScriptValue &v)
{
    const size_t dot = p.find('.', 10);
    const size_t i = static_cast<size_t>(std::atoi(p.c_str() + 10));
    auto &all = m_scene.animations;
    if (dot == std::string::npos || i >= all.size() || all[i].node != nodeOf(h)) {
        return;
    }
    const std::string op = p.substr(dot + 1);
    const double t = m_time;
    const auto steer = [&](PropertyAnimation &a) {
        PropertyAnimation::Playback &pb = a.playback;
        const double now = a.clock(t);
        if (op == "play") {
            // a Single animation that has reached its end starts again (the media widgets stop and play it on every track)
            if (!pb.playing || a.finishedAt(t)) {
                pb.anchor = a.finishedAt(t) ? 0.0 : now;
                pb.from = t;
                pb.playing = true;
            }
        } else if (op == "pause") {
            pb = {false, t, now, pb.rate};
        } else if (op == "stop") {
            pb = {false, t, 0.0, pb.rate};
        } else if (op == "rate") {
            // 0 to 100 times the animation's speed: it does not run backwards
            const double rate = v.type == ScriptValue::Type::Number && std::isfinite(v.number) ? std::clamp(v.number, 0.0, 100.0) : 1.0;
            pb = {pb.playing, t, now, rate};
        } else if (op == "frame") {
            const double frame = v.type == ScriptValue::Type::Number && std::isfinite(v.number) ? std::clamp(v.number, 0.0, static_cast<double>(a.length)) : 0.0;
            pb.anchor = frame / static_cast<double>(a.fps);
            pb.from = t;
        }
    };
    // a linked track (`options.parent`) plays with its parent: the object's tracks that name it, or that it names, follow
    const PropertyAnimation &target = all[i];
    const std::string key = animationProperty(target);
    bool changed = false;
    for (PropertyAnimation &a : all) {
        if (&a == &target || (a.node == target.node && (a.parentKey == key || (!target.parentKey.empty() && (animationProperty(a) == target.parentKey || a.parentKey == target.parentKey))))) {
            const PropertyAnimation::Playback before = a.playback;
            steer(a);
            changed = changed || before.playing != a.playback.playing || before.from != a.playback.from || before.anchor != a.playback.anchor
                || before.rate != a.playback.rate;
        }
    }
    if (changed) {
        m_flags.dirty = true;
        m_flags.animationsChanged = true;
    }
}

// A script that will not run cannot play the animations it was given to play: one the file starts paused on the script's own property
// runs on the scene clock instead, as it would without the script (WE's behaviour is not known), and that is logged once.
void SceneScripts::scriptStopped(int layer, const std::string &property)
{
    if (layer < 0 || static_cast<size_t>(layer) >= m_handles.size() || property.empty()) {
        return;
    }
    const size_t node = nodeOf(m_handles[static_cast<size_t>(layer)]);
    for (PropertyAnimation &a : m_scene.animations) {
        if (a.node == node && (animationProperty(a) == property || "light." + animationProperty(a) == property) && !a.playback.playing && !a.released) {
            a.released = true;
            a.playback = {};
            m_flags.dirty = true;
            m_flags.animationsChanged = true;
            logInfo("the paused animation of {} plays on the scene clock: its script does not run", property);
        }
    }
}

// `config` is {"animation": <id or name>, "rate": 1, "blend": 1, "visible": true} as JSON text (what createAnimationLayer got).
bool SceneScripts::createAnimationLayer(const Rig &rig, const std::string &json)
{
    if (!rig.layers || !rig.puppet || rig.layers->size() >= kMaxAnimationLayers) {
        return false;
    }
    std::string error;
    const auto doc = parseJson(json, &error);
    if (!doc || !doc->isObject()) {
        return false;
    }
    AnimationLayer a;
    const Json *which = doc->find("animation");
    bool found = false;
    for (const PuppetAnimation &pa : rig.puppet->animations) {
        if (which && ((which->isString() && pa.name == which->asString()) || (which->isNumber() && pa.id == static_cast<uint32_t>(which->asNumber())))) {
            a.animation = pa.id;
            found = true;
        }
    }
    if (!found) {
        return false;
    }
    a.rate = {static_cast<float>(doc->numberOr("rate", 1.0)), {}};
    a.blend = {static_cast<float>(doc->numberOr("blend", 1.0)), {}};
    a.visible = {doc->boolOr("visible", true), {}};
    rig.layers->push_back(a);
    return true;
}

// The animation layers and skeleton of a puppet (an image layer with a rigged model) or of a skinned 3D mesh: both play the model's
// animations through `animationlayers` (docs/scene-vk/puppets.md).
SceneScripts::Rig SceneScripts::rigOf(const Handle &h)
{
    if (h.kind == Handle::Kind::Image) {
        ImageLayer &l = m_scene.layers[h.index];
        return {&l.animationLayers, l.puppet.get(), l.bonePhysics.get()};
    }
    const size_t node = nodeOf(h);
    for (MeshLayer &mesh : m_scene.meshes) {
        if (mesh.node == node && mesh.model) {
            return {&mesh.animationLayers, mesh.skinning ? &mesh.model->rig : nullptr};
        }
    }
    return {};
}

ScriptValue SceneScripts::rigGet(const Rig &rig, const std::string &p) const
{
    if (p == "alayers") {
        return ScriptValue::ofNumber(rig.layers ? static_cast<double>(rig.layers->size()) : 0.0);
    }
    if (p.rfind("alayer.", 0) == 0 && rig.layers) {
        const size_t dot = p.find('.', 7);
        const size_t i = static_cast<size_t>(std::atoi(p.c_str() + 7));
        if (dot == std::string::npos || i >= rig.layers->size()) {
            return {};
        }
        const AnimationLayer &a = (*rig.layers)[i];
        const std::string field = p.substr(dot + 1);
        if (field == "blend") return ScriptValue::ofNumber(resolve(a.blend, m_props));
        if (field == "rate") return ScriptValue::ofNumber(resolve(a.rate, m_props));
        if (field == "visible") return ScriptValue::ofBool(resolve(a.visible, m_props));
        if (rig.puppet) {
            for (const PuppetAnimation &pa : rig.puppet->animations) {
                if (pa.id == a.animation) {
                    if (field == "frame") {
                        const double frame = m_time * resolve(a.rate, m_props) * pa.fps + a.frameOffset;
                        return ScriptValue::ofNumber(pa.length > 0 && std::isfinite(frame) ? std::fmod(std::fmod(frame, pa.length) + pa.length, pa.length) : 0.0);
                    }
                    if (field == "name") return ScriptValue::ofText(pa.name);
                    if (field == "fps") return ScriptValue::ofNumber(pa.fps);
                    if (field == "frameCount") return ScriptValue::ofNumber(pa.length);
                    if (field == "duration") return ScriptValue::ofNumber(pa.fps > 0.0f ? pa.length / pa.fps : 0.0);
                }
            }
        }
        if (field == "name") return ScriptValue::ofText("");
        if (field == "fps" || field == "frameCount" || field == "duration" || field == "frame") return ScriptValue::ofNumber(0);
        return {};
    }
    const Puppet *puppet = rig.puppet;
    if (p == "boneCount") return ScriptValue::ofNumber(puppet ? static_cast<double>(puppet->bones.size()) : 0.0);
    if (!puppet) {
        return {};
    }
    if (p.rfind("boneIndex:", 0) == 0) {
        const std::string name = p.substr(10);
        for (size_t b = 0; b < puppet->bones.size(); ++b) {
            if (puppet->bones[b].name == name) return ScriptValue::ofNumber(static_cast<double>(b));
        }
        return ScriptValue::ofNumber(-1);
    }
    if (p.rfind("bone.", 0) == 0) {
        const size_t dot = p.find('.', 5);
        const size_t b = static_cast<size_t>(std::atoi(p.c_str() + 5));
        if (dot != std::string::npos && b < puppet->bones.size()) {
            const std::string field = p.substr(dot + 1);
            if (field == "parent") return ScriptValue::ofNumber(puppet->bones[b].parent);
            if (field == "name") return ScriptValue::ofText(puppet->bones[b].name);
        }
    }
    return {};
}

bool SceneScripts::rigSet(const Rig &rig, const std::string &p, const ScriptValue &v)
{
    if (!rig.layers) {
        return false;
    }
    std::vector<AnimationLayer> &layers = *rig.layers;
    if (p == "alayer.create" && v.type == ScriptValue::Type::String) {
        return createAnimationLayer(rig, v.text);
    }
    if (p == "bonePhysics.impulse" && v.type == ScriptValue::Type::String) {
        // [bone, [x, y, z], [x, y, z]] from the prelude: the directional and the angular impulse
        const auto json = parseJson(v.text);
        if (!rig.physics || !json || !json->isArray() || json->items().size() != 3) {
            return false;
        }
        const auto vec = [](const Json &j) {
            Vec3 out;
            if (j.isArray() && j.items().size() == 3) {
                out = {static_cast<float>(j.items()[0].asNumber()), static_cast<float>(j.items()[1].asNumber()), static_cast<float>(j.items()[2].asNumber())};
            }
            return out;
        };
        const double bone = json->items()[0].asNumber(-1.0);
        if (!std::isfinite(bone) || bone < 0.0 || bone >= static_cast<double>(rig.physics->size())) {
            return false;
        }
        rig.physics->applyImpulse(static_cast<size_t>(bone), vec(json->items()[1]), vec(json->items()[2]));
        return true;
    }
    if (p == "bonePhysics.reset" && v.type == ScriptValue::Type::Number) {
        if (!rig.physics) {
            return false;
        }
        // -1: every bone; otherwise a bone of the skeleton (a number that is not finite or not a bone resets nothing)
        if (!std::isfinite(v.number) || v.number < -1.0 || v.number >= static_cast<double>(rig.physics->size())) {
            return false;
        }
        rig.physics->reset(static_cast<int>(v.number));
        return true;
    }
    if (p == "alayer.destroy") {
        if (v.type != ScriptValue::Type::Number || !std::isfinite(v.number) || v.number < 0.0 || v.number >= static_cast<double>(layers.size())) {
            return false;
        }
        const size_t i = static_cast<size_t>(v.number);
        {
            layers.erase(layers.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
        return false;
    }
    const size_t dot = p.find('.', 7);
    const size_t i = static_cast<size_t>(std::atoi(p.c_str() + 7));
    if (dot == std::string::npos || i >= layers.size()) {
        return false;
    }
    AnimationLayer &a = layers[i];
    const std::string field = p.substr(dot + 1);
    if (field == "blend") {
        a.blend = {asFloat(v, 1.0f), {}};
        return true;
    }
    if (field == "rate") {
        a.rate = {asFloat(v, 1.0f), {}};
        return true;
    }
    if (field == "visible") {
        a.visible = {v.type == ScriptValue::Type::Bool ? v.boolean : v.number != 0.0, {}};
        return true;
    }
    if (field == "frame" && rig.puppet) {
        // the layer then shows that frame now and plays on from it: an offset from the clock
        for (const PuppetAnimation &pa : rig.puppet->animations) {
            if (pa.id == a.animation) {
                const double offset = asFloat(v, 0.0f) - m_time * resolve(a.rate, m_props) * pa.fps;
                a.frameOffset = std::isfinite(offset) && pa.length > 0 ? static_cast<float>(std::fmod(offset, pa.length)) : 0.0f;
                return true;
            }
        }
    }
    return false;
}

// `models/bar.json` from the script of Workshop item N is the package's `models/workshop/N/bar.json` (the editor files a
// Workshop script's resources under that folder and the script keeps the short name). The path is used as it is when it
// exists, so scripts of a scene's own package are not affected.
std::string SceneScripts::resolvePath(const std::string &path, const std::string &workshopId)
{
    if (m_vfs.exists(path)) {
        return path;
    }
    const size_t slash = path.find('/');
    if (slash == std::string::npos) {
        return path;
    }
    const std::string candidate = path.substr(0, slash + 1) + "workshop/" + workshopId + "/" + path.substr(slash + 1);
    return m_vfs.exists(candidate) ? candidate : path;
}

int SceneScripts::createLayer(const std::string &spec, bool json)
{
    // (the counters never pass their limits: the report is a flag of its own, and a layer destroyed later makes room again)
    if (m_created >= kMaxScriptLayers) {
        if (!m_layerLimitSaid) {
            m_layerLimitSaid = true;
            logError("createLayer: scripts may have at most {} layers they made alive", kMaxScriptLayers);
        }
        return -1;
    }
    if (m_totalCreated >= kMaxScriptLayerCreations) {
        if (!m_creationLimitSaid) {
            m_creationLimitSaid = true;
            logError("createLayer: scripts may create at most {} layers over the run (a destroyed layer is hidden, not freed)", kMaxScriptLayerCreations);
        }
        return -1;
    }
    std::string error;
    const int index = json ? addTextLayer(m_scene, m_vfs, spec, &error) : addImageLayerFromModel(m_scene, m_vfs, spec, &error);
    if (index < 0) {
        logError("createLayer: {}", error);
        return -1;
    }
    ++m_created;
    ++m_totalCreated;
    m_flags.dirty = true;
    m_flags.textChanged = true;
    const int handle = handleFor(Handle::Kind::Image, static_cast<size_t>(index));
    m_madeByScripts.insert(handle);
    return handle;
}

int SceneScripts::indexOf(int layer)
{
    if (layer < 0 || static_cast<size_t>(layer) >= m_handles.size()) {
        return -1;
    }
    const Handle h = m_handles[static_cast<size_t>(layer)];
    if (h.kind == Handle::Kind::Node) {
        return -1;
    }
    for (size_t i = 0; i < m_scene.order.size(); ++i) {
        const DrawEntry::Kind kind = h.kind == Handle::Kind::Particle ? DrawEntry::Kind::Particle : DrawEntry::Kind::Image;
        if (m_scene.order[i].kind == kind && m_scene.order[i].index == h.index) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void SceneScripts::sort(int layer, int index)
{
    const int from = indexOf(layer);
    if (from < 0 || m_scene.order.empty()) {
        return;
    }
    const DrawEntry entry = m_scene.order[static_cast<size_t>(from)];
    m_scene.order.erase(m_scene.order.begin() + from);
    const size_t to = static_cast<size_t>(std::clamp(index, 0, static_cast<int>(m_scene.order.size())));
    m_scene.order.insert(m_scene.order.begin() + static_cast<std::ptrdiff_t>(to), entry);
    m_flags.dirty = true;
}

int SceneScripts::find(const std::string &name)
{
    for (size_t i = 0; i < m_scene.nodes.size(); ++i) {
        if (m_scene.nodes[i].name == name) {
            for (size_t l = 0; l < m_scene.layers.size(); ++l) {
                if (m_scene.layers[l].node == i) {
                    return handleFor(Handle::Kind::Image, l);
                }
            }
            for (size_t l = 0; l < m_scene.particles.size(); ++l) {
                if (m_scene.particles[l].node == i) {
                    return handleFor(Handle::Kind::Particle, l);
                }
            }
            return handleFor(Handle::Kind::Node, i);
        }
    }
    return -1;
}

int SceneScripts::findById(int id)
{
    const auto it = m_scene.nodeIndexById.find(id);
    return id < 0 || it == m_scene.nodeIndexById.end() ? -1 : handleForNode(it->second);
}

// ---- the scene graph ----

int SceneScripts::parentOf(int layer)
{
    if (layer < 0 || static_cast<size_t>(layer) >= m_handles.size()) {
        return -1;
    }
    const SceneNode &n = m_scene.nodes[nodeOf(m_handles[static_cast<size_t>(layer)])];
    if (n.parentId < 0) {
        return -1;
    }
    const auto it = m_scene.nodeIndexById.find(n.parentId);
    return it == m_scene.nodeIndexById.end() ? -1 : handleForNode(it->second);
}

std::vector<int> SceneScripts::childrenOf(int layer)
{
    std::vector<int> out;
    if (layer < 0 || static_cast<size_t>(layer) >= m_handles.size()) {
        return out;
    }
    const int id = m_scene.nodes[nodeOf(m_handles[static_cast<size_t>(layer)])].id;
    for (size_t i = 0; i < m_scene.nodes.size(); ++i) {
        if (m_scene.nodes[i].parentId == id && id >= 0) {
            out.push_back(handleForNode(i));
        }
    }
    return out;
}

// A script can hang an object under another or take it out (parent -1). With `keepWorldTransform` the object stays where it is
// on the screen: its origin is recomputed in the new parent's space (rotation and scale of the parents are not compensated).
void SceneScripts::setParent(int layer, int parent, bool keepWorldTransform)
{
    if (layer < 0 || static_cast<size_t>(layer) >= m_handles.size()) {
        return;
    }
    const size_t child = nodeOf(m_handles[static_cast<size_t>(layer)]);
    int newParentId = -1;
    size_t parentNode = 0;
    if (parent >= 0) {
        if (static_cast<size_t>(parent) >= m_handles.size()) {
            return;
        }
        parentNode = nodeOf(m_handles[static_cast<size_t>(parent)]);
        // never under itself or one of its own descendants
        for (size_t n = parentNode, hops = 0; hops <= m_scene.nodes.size(); ++hops) {
            if (n == child) {
                return;
            }
            const int up = m_scene.nodes[n].parentId;
            const auto it = up < 0 ? m_scene.nodeIndexById.end() : m_scene.nodeIndexById.find(up);
            if (it == m_scene.nodeIndexById.end()) {
                break;
            }
            n = it->second;
        }
        newParentId = m_scene.nodes[parentNode].id;
    }
    const Mat4 world = nodeWorldMatrix(m_scene, child);
    SceneNode &n = m_scene.nodes[child];
    n.parentId = newParentId;
    n.attachPoses.reset(); // a new parent: the object no longer stands on a point of the old one
    n.attachIndex = 0;
    if (keepWorldTransform) {
        const Vec3 at{world.at(0, 3), world.at(1, 3), world.at(2, 3)};
        if (newParentId < 0) {
            n.origin = at;
        } else {
            const Mat4 inv = inverse(nodeWorldMatrix(m_scene, parentNode));
            n.origin = {inv.at(0, 0) * at.x + inv.at(0, 1) * at.y + inv.at(0, 2) * at.z + inv.at(0, 3),
                        inv.at(1, 0) * at.x + inv.at(1, 1) * at.y + inv.at(1, 2) * at.z + inv.at(1, 3),
                        inv.at(2, 0) * at.x + inv.at(2, 1) * at.y + inv.at(2, 2) * at.z + inv.at(2, 3)};
        }
    }
    m_flags.dirty = true;
}

std::vector<int> SceneScripts::enumerate()
{
    std::vector<int> out;
    for (const DrawEntry &e : m_scene.order) {
        if (e.kind == DrawEntry::Kind::Mesh) {
            continue; // (3D meshes have no script handle yet)
        }
        const int h = handleFor(e.kind == DrawEntry::Kind::Particle ? Handle::Kind::Particle : Handle::Kind::Image, e.index);
        if (!m_destroyed.count(h)) {
            out.push_back(h);
        }
    }
    return out;
}

// There is no freeing of an object's buffers mid-scene: a destroyed layer is hidden for good and leaves the enumeration.
bool SceneScripts::destroy(int layer)
{
    if (layer < 0 || static_cast<size_t>(layer) >= m_handles.size() || m_destroyed.count(layer)) {
        return false;
    }
    m_destroyed.insert(layer);
    if (m_madeByScripts.erase(layer) > 0 && m_created > 0) {
        --m_created; // the limit counts the layers that are alive: a grid that rebuilds itself destroys what it made
    }
    m_scene.nodes[nodeOf(m_handles[static_cast<size_t>(layer)])].visible = {false, {}};
    m_flags.dirty = true;
    return true;
}

// ---- effects ----

namespace {
Constant constantOf(const ScriptValue &v)
{
    Constant c;
    if (v.type == ScriptValue::Type::Vector) {
        c.count = v.count;
        for (int i = 0; i < v.count && i < 4; ++i) {
            c.value[static_cast<size_t>(i)] = static_cast<float>(v.vec[static_cast<size_t>(i)]);
        }
    } else {
        c.count = 1;
        c.value[0] = v.type == ScriptValue::Type::Bool ? (v.boolean ? 1.0f : 0.0f) : static_cast<float>(v.number);
    }
    return c;
}
ScriptValue valueOf(const Constant &c)
{
    if (c.count <= 1) {
        return ScriptValue::ofNumber(c.value[0]);
    }
    ScriptValue out;
    out.type = ScriptValue::Type::Vector;
    out.count = std::min(c.count, 4);
    for (int i = 0; i < out.count; ++i) {
        out.vec[static_cast<size_t>(i)] = c.value[static_cast<size_t>(i)];
    }
    return out;
}
} // namespace

int SceneScripts::effectCount(int layer)
{
    if (layer < 0 || static_cast<size_t>(layer) >= m_handles.size() || m_handles[static_cast<size_t>(layer)].kind != Handle::Kind::Image) {
        return 0;
    }
    return static_cast<int>(m_scene.layers[m_handles[static_cast<size_t>(layer)].index].effects.size());
}

ScriptValue SceneScripts::effectGet(int layer, int effect, const std::string &property)
{
    if (effectCount(layer) <= effect || effect < 0) {
        return {};
    }
    const EffectInstance &fx = m_scene.layers[m_handles[static_cast<size_t>(layer)].index].effects[static_cast<size_t>(effect)];
    if (property == "name") {
        return ScriptValue::ofText(fx.name);
    }
    if (property == "visible") {
        return ScriptValue::ofBool(resolve(fx.visible, m_props));
    }
    if (property == "passes") {
        return ScriptValue::ofNumber(static_cast<double>(fx.def.passes.size()));
    }
    // "constant:<pass>:<key>": the instance's override, else the material's own value
    if (property.rfind("constant:", 0) == 0) {
        const size_t colon = property.find(':', 9);
        if (colon == std::string::npos) {
            return {};
        }
        const size_t pass = static_cast<size_t>(std::atoi(property.c_str() + 9));
        const std::string key = property.substr(colon + 1);
        if (pass < fx.overrides.size()) {
            if (const auto it = fx.overrides[pass].constants.find(key); it != fx.overrides[pass].constants.end()) {
                return valueOf(it->second);
            }
        }
        if (pass < fx.def.passes.size()) {
            if (const auto it = fx.def.passes[pass].material.constants.find(key); it != fx.def.passes[pass].material.constants.end()) {
                return valueOf(it->second);
            }
        }
    }
    return {};
}

void SceneScripts::effectSet(int layer, int effect, const std::string &property, const ScriptValue &value)
{
    if (effectCount(layer) <= effect || effect < 0 || value.type == ScriptValue::Type::Undefined) {
        return;
    }
    EffectInstance &fx = m_scene.layers[m_handles[static_cast<size_t>(layer)].index].effects[static_cast<size_t>(effect)];
    if (property == "visible") {
        fx.visible = {value.type == ScriptValue::Type::Bool ? value.boolean : value.number != 0.0, {}};
    } else if (property.rfind("constant:", 0) == 0) {
        const size_t colon = property.find(':', 9);
        if (colon == std::string::npos) {
            return;
        }
        const std::string pass = property.substr(9, colon - 9);
        const std::string key = property.substr(colon + 1);
        if (fx.overrides.size() < fx.def.passes.size()) {
            fx.overrides.resize(fx.def.passes.size());
        }
        for (size_t i = 0; i < fx.overrides.size(); ++i) {
            if (pass == "*" || static_cast<size_t>(std::atoi(pass.c_str())) == i) {
                fx.overrides[i].constants[key] = constantOf(value);
            }
        }
    } else {
        return;
    }
    m_flags.dirty = true;
}

} // namespace murasaki::scene

