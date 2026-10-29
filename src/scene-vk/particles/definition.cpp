#include "particles/definition.h"

#include "assets/texfile.h"
#include "util/json.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace murasaki::scene {

namespace {

constexpr int kMaxChildDepth = 4;
constexpr size_t kMaxListItems = 256; // entries kept of an array member (`colors`)

std::string lowered(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

const std::set<std::string> kEmitters = {"boxrandom", "sphererandom"};
const std::set<std::string> kInitializers = {"lifetimerandom", "sizerandom",  "colorrandom",  "hsvcolorrandom",  "colorlist", "velocityrandom",
                                             "alpharandom",    "rotationrandom", "angularvelocityrandom", "turbulentvelocityrandom", "mapsequencearoundcontrolpoint", "mapsequencebetweencontrolpoints",
                                             "inheritcontrolpointvelocity", "positionoffsetrandom", "remapinitialvalue", "inheritinitialvaluefromevent"};
const std::set<std::string> kOperators = {"movement",     "alphafade",   "sizechange",          "alphachange",        "colorchange",
                                          "angularmovement", "turbulence", "controlpointattract", "vortex", "vortex_v2", "oscillatealpha",     "oscillatesize",
                                          "oscillateposition", "remapvalue", "boids", "capvelocity", "maintaindistancetocontrolpoint",
                                          "maintaindistancebetweencontrolpoints", "reducemovementnearcontrolpoint", "collisionplane", "collisionsphere",
                                          "collisionbox", "collisionquad", "collisionbounds", "inheritvaluefromevent"};
// The operators that read a lifetime blend window (`blendinstart`.. `blendoutend`); on the others it is reported.
const std::set<std::string> kBlendedOperators = {"turbulence", "controlpointattract", "vortex", "vortex_v2", "oscillatealpha", "oscillatesize",
                                                 "oscillateposition", "remapvalue", "capvelocity", "maintaindistancetocontrolpoint",
                                                 "maintaindistancebetweencontrolpoints", "reducemovementnearcontrolpoint"};
const std::set<std::string> kRenderers = {"sprite", "spritetrail", "rope", "ropetrail"};

const PropertyValue *boundValue(const Constant &c, const PropertyStore *props)
{
    return !c.user.empty() && props ? props->find(c.user) : nullptr;
}

} // namespace

bool particleEmitterSupported(const std::string &name)
{
    return kEmitters.count(name) != 0;
}
bool particleInitializerSupported(const std::string &name)
{
    return kInitializers.count(name) != 0;
}
bool particleOperatorSupported(const std::string &name)
{
    return kOperators.count(name) != 0;
}
bool particleRendererSupported(const std::string &name)
{
    return kRenderers.count(name) != 0;
}

namespace {

template <typename T>
std::optional<T> lookup(const std::map<std::string, T> &words, const std::string &word)
{
    const auto it = words.find(lowered(word));
    return it == words.end() ? std::nullopt : std::optional<T>(it->second);
}

} // namespace

std::optional<ParticleRemapValue> particleRemapValue(const std::string &word)
{
    using V = ParticleRemapValue;
    static const std::map<std::string, V> words = {
        {"lifetimefraction", V::LifetimeFraction}, {"maxlifetime", V::MaxLifetime}, {"size", V::Size}, {"opacity", V::Opacity},
        {"speed", V::Speed}, {"rotation", V::Rotation}, {"angularspeed", V::AngularSpeed}, {"distancetocontrolpoint", V::DistanceToControlPoint},
        {"positionbetweentwocontrolpoints", V::PositionBetweenControlPoints}, {"runtime", V::Runtime}, {"timeofday", V::TimeOfDay},
        {"particlesystemtime", V::SystemTime}, {"layertime", V::LayerTime}, {"color", V::Color}, {"position", V::Position},
        {"velocity", V::Velocity}, {"controlpoint", V::ControlPoint}, {"deltatocontrolpoint", V::DeltaToControlPoint},
        {"directiontocontrolpoint", V::DirectionToControlPoint}, {"layerorigin", V::LayerOrigin}};
    return lookup(words, word);
}

bool particleRemapIsVector(ParticleRemapValue v)
{
    return v >= ParticleRemapValue::Color;
}

bool particleRemapWritable(ParticleRemapValue v)
{
    using V = ParticleRemapValue;
    return v == V::MaxLifetime || v == V::Size || v == V::Opacity || v == V::Speed || v == V::Rotation || v == V::AngularSpeed || v == V::Color
        || v == V::Position || v == V::Velocity;
}

std::optional<ParticleRemapOperation> particleRemapOperation(const std::string &word)
{
    using O = ParticleRemapOperation;
    static const std::map<std::string, O> words = {{"remap", O::Assign}, {"multiply", O::Multiply}, {"add", O::Add}, {"subtract", O::Subtract}};
    return lookup(words, word);
}

std::optional<ParticleTransformFunction> particleTransformFunction(const std::string &word)
{
    using F = ParticleTransformFunction;
    static const std::map<std::string, F> words = {{"none", F::None},         {"sine", F::Sine},
                                                   {"square", F::Square},     {"saw", F::Saw},
                                                   {"triangle", F::Triangle}, {"simplexnoise", F::SimplexNoise},
                                                   {"fbmnoise", F::FbmNoise}};
    return lookup(words, word);
}

std::optional<ParticleCollisionBehavior> particleCollisionBehavior(const std::string &word)
{
    using B = ParticleCollisionBehavior;
    static const std::map<std::string, B> words = {{"bounce", B::Bounce}, {"stop", B::Stop}, {"slide", B::Slide}, {"delete", B::Delete}};
    return lookup(words, word);
}

std::optional<ParticleEventValue> particleEventValue(const std::string &word)
{
    using E = ParticleEventValue;
    static const std::map<std::string, E> words = {
        {"setcolor", E::SetColor}, {"multiplycolor", E::MultiplyColor}, {"setopacity", E::SetOpacity}, {"multiplyopacity", E::MultiplyOpacity},
        {"setcoloropacity", E::SetColorOpacity}, {"multiplycoloropacity", E::MultiplyColorOpacity}, {"setvelocity", E::SetVelocity},
        {"addvelocity", E::AddVelocity}, {"setsize", E::SetSize}, {"multiplysize", E::MultiplySize}, {"setrotation", E::SetRotation},
        {"addrotation", E::AddRotation}, {"setangularvelocity", E::SetAngularVelocity}, {"addangularvelocity", E::AddAngularVelocity}};
    return lookup(words, word);
}

ParticleEventValue particleEventValueOf(const ParticleComponent &c)
{
    for (const auto &[member, text] : c.strings) {
        if (const auto v = particleEventValue(text)) {
            return *v;
        }
    }
    return ParticleEventValue::SetColor;
}

MaterialPass particleMaterialPass(const ParticleDefinition &definition, bool spriteSheet)
{
    MaterialPass pass = definition.materialPass;
    pass.combos["THICKFORMAT"] = 1;
    if (spriteSheet) {
        pass.combos["SPRITESHEET"] = 1;
    }
    for (const ParticleComponent &c : definition.renderers) {
        if (c.name == "spritetrail") {
            pass.combos["TRAILRENDERER"] = 1;
        } else if (c.name == "rope" || c.name == "ropetrail") {
            pass.shader = "genericropeparticle"; // a ribbon through the particles, its own shader pair
            if (c.name == "ropetrail") {
                pass.combos["TRAILRENDERER"] = 1;
            }
        }
    }
    if (const int mode = particleOrientation(definition).mode; mode != 0) {
        pass.combos["ORIENTATION"] = mode; // (mode 0 stays undeclared: the shader default)
    }
    return pass;
}

namespace {

// The orientation one renderer asks for (see ParticleOrientation).
ParticleOrientation orientationOf(const ParticleComponent &c)
{
    ParticleOrientation o;
    const std::string text = c.text("orientation");
    o.mode = text == "upright" ? 1 : text == "fixed" ? 2 : 0;
    if (o.mode == 0) {
        return o;
    }
    Vec3 a = c.vector("axis", {0, 0, 0});
    float length = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    if (!(length > 1e-6f) || !std::isfinite(length)) {
        // no axis, or all zero: WE's own default A = (0, 1, 0) (read from its renderer reader), so B = (1, 0, 0), C = (0, 0, -1) below
        a = {0.0f, 1.0f, 0.0f};
        length = 1.0f;
    }
    a = {a.x / length, a.y / length, a.z / length};
    Vec3 b{1.0f, 0.0f, 0.0f};
    if (std::abs(a.x) > 1e-6f || std::abs(a.z) > 1e-6f) {
        b = {a.z, 0.0f, -a.x}; // cross((0, 1, 0), A)
        const float bl = std::sqrt(b.x * b.x + b.z * b.z);
        b = {b.x / bl, 0.0f, b.z / bl};
    }
    const Vec3 cross{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    const float cl = std::sqrt(cross.x * cross.x + cross.y * cross.y + cross.z * cross.z);
    o.right = b;
    o.up = {cross.x / cl, cross.y / cl, cross.z / cl};
    o.forward = a;
    return o;
}

bool sameOrientation(const ParticleOrientation &a, const ParticleOrientation &b)
{
    const auto close = [](Vec3 x, Vec3 y) { return std::abs(x.x - y.x) + std::abs(x.y - y.y) + std::abs(x.z - y.z) < 1e-5f; };
    return a.mode == b.mode && close(a.right, b.right) && close(a.up, b.up) && close(a.forward, b.forward);
}

} // namespace

ParticleOrientation particleOrientation(const ParticleDefinition &definition)
{
    // one pass draws the whole system, so one orientation: the first renderer that asks for one (load() reports the others)
    for (const ParticleComponent &c : definition.renderers) {
        if (const ParticleOrientation o = orientationOf(c); o.mode != 0) {
            return o;
        }
    }
    return {};
}

bool particleUsesAudio(const ParticleDefinition &definition)
{
    for (const ParticleComponent &e : definition.emitters) {
        if (e.integer("audioprocessingmode", 0) > 0) {
            return true;
        }
    }
    for (const ParticleComponent &o : definition.operators) {
        if ((o.name == "vortex" || o.name == "turbulence") && o.integer("audioprocessingmode", 0) > 0) {
            return true;
        }
    }
    return std::any_of(definition.children.begin(), definition.children.end(),
                       [](const ParticleChild &c) { return c.definition && particleUsesAudio(*c.definition); });
}

std::vector<int> particleSlots(const MaterialPass &pass)
{
    std::vector<int> slots{0};
    if (pass.textures.size() > 1 && !pass.textures[1].empty()) {
        slots.push_back(1);
    }
    return slots;
}

namespace {

// A number out of a user property or a file: finite and bounded, so it can feed ranges and float to integer casts.
float sane(double v, float fallback)
{
    if (!std::isfinite(v)) {
        return fallback;
    }
    return static_cast<float>(std::clamp(v, -1.0e9, 1.0e9));
}

} // namespace

int ParticleComponent::integer(const std::string &key, int fallback) const
{
    return static_cast<int>(number(key, static_cast<float>(fallback)));
}

float ParticleComponent::number(const std::string &key, float fallback, const PropertyStore *props) const
{
    const auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }
    const Constant &c = it->second;
    if (const PropertyValue *p = boundValue(c, props)) {
        if (const bool *b = std::get_if<bool>(p)) {
            return *b ? 1.0f : 0.0f;
        }
        if (const double *d = std::get_if<double>(p)) {
            return sane(*d, fallback);
        }
        if (const auto *v = std::get_if<std::vector<double>>(p); v && !v->empty()) {
            return sane((*v)[0], fallback);
        }
    }
    return c.count > 0 ? sane(c.value[0], fallback) : fallback;
}

Vec3 ParticleComponent::vector(const std::string &key, Vec3 fallback, const PropertyStore *props) const
{
    const auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }
    const Constant &c = it->second;
    std::array<float, 4> v = c.value;
    int count = c.count;
    if (const PropertyValue *p = boundValue(c, props)) {
        if (const auto *vec = std::get_if<std::vector<double>>(p)) {
            count = static_cast<int>(std::min<size_t>(3, vec->size()));
            for (int i = 0; i < count; ++i) {
                v[static_cast<size_t>(i)] = sane((*vec)[static_cast<size_t>(i)], 0.0f);
            }
        } else if (const double *d = std::get_if<double>(p)) {
            v[0] = sane(*d, 0.0f);
            count = 1;
        }
    }
    if (count <= 0) {
        return fallback;
    }
    Vec3 out{v[0], count > 1 ? v[1] : v[0], count > 2 ? v[2] : v[0]};
    out = {sane(out.x, 0.0f), sane(out.y, 0.0f), sane(out.z, 0.0f)};
    if (!std::isfinite(out.x) || !std::isfinite(out.y) || !std::isfinite(out.z)) {
        return fallback;
    }
    return out;
}

namespace {

std::vector<ParticleComponent> components(const Json &root, const char *key)
{
    std::vector<ParticleComponent> out;
    const Json *array = root.find(key);
    if (!array || !array->isArray()) {
        return out;
    }
    for (const Json &entry : array->items()) {
        if (!entry.isObject()) {
            continue;
        }
        ParticleComponent c;
        c.name = entry.stringOr("name");
        for (const auto &[member, value] : entry.members()) {
            if (member != "name" && member != "id") {
                c.values[lowered(member)] = parseConstant(value);
                if (value.isString()) {
                    c.strings[lowered(member)] = value.asString();
                } else if (value.isArray()) {
                    std::vector<Vec3> &list = c.lists[lowered(member)];
                    for (const Json &item : value.items()) {
                        if (list.size() >= kMaxListItems) {
                            c.cutLists.push_back(lowered(member));
                            break;
                        }
                        const Constant v = parseConstant(item);
                        if (v.count > 0) {
                            list.push_back({v.value[0], v.count > 1 ? v.value[1] : v.value[0], v.count > 2 ? v.value[2] : v.value[0]});
                        }
                    }
                }
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

Vec3 vec3Of(const Json *j, Vec3 fallback)
{
    if (!j) {
        return fallback;
    }
    const Constant c = parseConstant(*j);
    if (c.count <= 0) {
        return fallback;
    }
    return {c.value[0], c.count > 1 ? c.value[1] : c.value[0], c.count > 2 ? c.value[2] : c.value[0]};
}

void degrade(ParticleDefinition &d, const std::string &reason)
{
    if (std::find(d.degraded.begin(), d.degraded.end(), reason) == d.degraded.end()) {
        d.degraded.push_back(reason);
    }
}

void reject(ParticleDefinition &d, const std::string &reason)
{
    if (std::find(d.unsupported.begin(), d.unsupported.end(), reason) == d.unsupported.end()) {
        d.unsupported.push_back(reason);
    }
}

// What a remap element names that the simulation does not do: the element still runs with the default for that word.
void checkRemap(ParticleDefinition &d, const ParticleComponent &c)
{
    if (const std::string w = c.text("input"); !w.empty() && !particleRemapValue(w)) {
        degrade(d, "particle remap input " + w);
    }
    if (const std::string w = c.text("output"); !w.empty()) {
        if (const auto v = particleRemapValue(w); !v || !particleRemapWritable(*v)) {
            degrade(d, "particle remap output " + w); // left alone
        }
    }
    if (const std::string w = c.text("operation"); !w.empty() && !particleRemapOperation(w)) {
        degrade(d, "particle remap operation " + w);
    }
    if (const std::string w = c.text("transformfunction"); !w.empty() && !particleTransformFunction(w)) {
        degrade(d, "particle remap transform " + w);
    }
}

// Definitions already read, by file and depth (the depth decides whether the children are followed), so a file listed
// many times as a child is parsed once: the tree is shared, only its size is counted.
using DefinitionCache = std::map<std::pair<std::string, int>, std::shared_ptr<const ParticleDefinition>>;

std::optional<ParticleDefinition> load(const Vfs &vfs, const std::string &file, int depth, std::string *error, DefinitionCache &cache)
{
    const auto text = vfs.readText(file);
    if (!text) {
        if (error) {
            *error = "particle file '" + file + "' not found";
        }
        return std::nullopt;
    }
    std::string parseError;
    const auto root = parseJson(*text, &parseError);
    if (!root || !root->isObject()) {
        if (error) {
            *error = "particle file '" + file + "': " + (root ? "not a JSON object" : parseError);
        }
        return std::nullopt;
    }
    ParticleDefinition d;
    d.file = file;
    d.material = root->stringOr("material");
    d.maxCount = static_cast<uint32_t>(std::clamp(root->numberOr("maxcount", 100.0), 0.0, 1.0e6));
    d.startTime = static_cast<float>(std::clamp(root->numberOr("starttime", 0.0), 0.0, 3600.0));
    d.flags = static_cast<uint32_t>(std::max(0.0, root->numberOr("flags", 0.0)));
    d.animationMode = root->stringOr("animationmode", "sequence");
    d.sequenceMultiplier = static_cast<float>(root->numberOr("sequencemultiplier", 1.0));
    d.emitters = components(*root, "emitter");
    d.initializers = components(*root, "initializer");
    d.operators = components(*root, "operator");
    d.renderers = components(*root, "renderer");
    if (d.renderers.empty()) {
        d.renderers.push_back({"sprite", {}});
    }
    if (d.material.empty()) {
        reject(d, "particles: no material");
    } else {
        std::string materialError;
        const auto material = loadMaterial(vfs, d.material, &materialError);
        if (!material || material->passes.empty()) {
            reject(d, "particles: material '" + d.material + "' " + (material ? "has no passes" : "cannot be read"));
        } else {
            d.materialPass = material->passes[0];
            if (material->passes.size() > 1) {
                reject(d, "particles: multi-pass material");
            }
            if (d.materialPass.textures.empty()) {
                d.materialPass.textures.resize(1);
            }
            if (d.materialPass.textures[0].empty()) {
                // a sprite without a picture is WE's white one (the presets' `vapor` and `snowflat` leave it out)
                d.materialPass.textures[0] = "util/white";
            }
            if (d.materialPass.textures[0] != "util/white") {
                // The particle shader lays a sheet's frames out on one image (a grid of `sheetCols` x `sheetRows`), so a sheet that spreads
                // over several images draws everything from the first one; only the header is needed to know.
                const std::string texPath = "materials/" + d.materialPass.textures[0] + ".tex";
                if (vfs.exists(texPath)) {
                    if (const auto tex = readTexFile(vfs, texPath, TexReadOptions{false}); tex && tex->animated() && (!tex->pages.empty() || tex->droppedPages > 0)) {
                        degrade(d, "particle sprite sheet pages");
                    }
                }
            }
        }
    }

    for (const ParticleComponent &c : d.emitters) {
        if (!particleEmitterSupported(c.name)) {
            reject(d, "particles: emitter " + c.name);
        }
        if (const int mode = c.integer("audioprocessingmode", 0); mode > 3) { // (negative is off)
            degrade(d, "particle emitter audio mode " + std::to_string(mode)); // read as both channels
        }
        for (const auto &[member, value] : c.values) {
            if (value.scripted) {
                reject(d, "SceneScript (particle " + c.name + "." + member + "): M4");
            }
        }
    }
    for (const ParticleComponent &c : d.initializers) {
        if (!particleInitializerSupported(c.name)) {
            reject(d, "particles: initializer " + c.name);
        }
        if (c.name == "remapinitialvalue") {
            checkRemap(d, c);
        }
        if (const auto it = c.lists.find("colors"); c.name == "colorlist" && (it == c.lists.end() || it->second.empty())) {
            degrade(d, "particle colorlist without colors"); // the colour stays what earlier initializers made it
        }
        for (const std::string &member : c.cutLists) {
            degrade(d, "particle list " + member + " cut to " + std::to_string(kMaxListItems));
        }
        for (const auto &[member, value] : c.values) {
            if (value.scripted) {
                reject(d, "SceneScript (particle " + c.name + "." + member + "): M4");
            }
        }
    }
    for (const ParticleComponent &c : d.operators) {
        if (!particleOperatorSupported(c.name)) {
            reject(d, "particles: operator " + c.name);
        }
        if (const int mode = c.integer("audioprocessingmode", 0); mode > 0) {
            if (c.name != "vortex" && c.name != "turbulence") {
                degrade(d, "particle operator audio response"); // only these two (the library's) follow the spectrum
            } else if (mode > 3) {
                degrade(d, "particle operator audio mode " + std::to_string(mode)); // read as both channels
            }
        }
        if (c.name == "remapvalue") {
            checkRemap(d, c);
            if (c.integer("flags", 0) != 0) {
                degrade(d, "particle remapvalue flags"); // meaning unknown, not read
            }
        }
        if (!kBlendedOperators.count(c.name)
            && (c.has("blendinstart") || c.has("blendinend") || c.has("blendoutstart") || c.has("blendoutend"))) {
            degrade(d, "particle operator blend window " + c.name); // works over the whole lifetime
        }
        if (c.name.rfind("collision", 0) == 0) {
            if (const std::string b = c.text("collisionbehavior"); !b.empty() && !particleCollisionBehavior(b)) {
                degrade(d, "particle collision behavior " + b); // bounces
            }
        }
        if (const int flags = c.integer("flags", 0); c.name == "vortex_v2" && (flags & ~7) != 0) {
            degrade(d, "particle vortex_v2 flags " + std::to_string(flags & ~7)); // meaning unknown, not read
        }
        for (const auto &[member, value] : c.values) {
            if (value.scripted) {
                reject(d, "SceneScript (particle " + c.name + "." + member + "): M4");
            }
        }
    }
    for (const ParticleComponent &c : d.renderers) {
        if (!particleRendererSupported(c.name)) {
            reject(d, "particles: renderer " + c.name);
        }
        if (const std::string o = c.text("orientation"); !o.empty() && o != "screen" && o != "upright" && o != "fixed") {
            degrade(d, "particle renderer orientation " + o); // drawn facing the screen
        }
        if (c.integer("flags", 0) != 0) {
            degrade(d, "particle renderer flags"); // meaning unknown, not read
        }
        for (const auto &[member, value] : c.values) {
            if (value.scripted) {
                reject(d, "SceneScript (particle " + c.name + "." + member + "): M4");
            }
        }
    }

    if (std::any_of(d.renderers.begin(), d.renderers.end(),
                    [&](const ParticleComponent &c) { return !sameOrientation(orientationOf(c), orientationOf(d.renderers[0])); })) {
        degrade(d, "particle renderers with different orientations"); // the first one that asks for an orientation draws them all
    }

    if (const Json *points = root->find("controlpoint"); points && points->isArray()) {
        for (const Json &p : points->items()) {
            ParticleControlPoint cp;
            cp.id = static_cast<int>(p.numberOr("id", -1));
            cp.flags = static_cast<uint32_t>(std::max(0.0, p.numberOr("flags", 0.0)));
            cp.offset = vec3Of(p.find("offset"), {0, 0, 0});
            if (cp.id >= 0 && cp.id < 8) {
                d.controlPoints.push_back(cp);
            }
        }
    }

    if (const Json *kids = root->find("children"); kids && kids->isArray()) {
        for (const Json &k : kids->items()) {
            ParticleChild child;
            child.type = k.stringOr("type", "static");
            child.file = k.stringOr("name");
            child.origin = vec3Of(k.find("origin"), {0, 0, 0});
            child.scale = vec3Of(k.find("scale"), {1, 1, 1});
            child.angles = vec3Of(k.find("angles"), {0, 0, 0});
            child.maxCount = static_cast<int>(k.numberOr("maxcount", 0));
            if (const double chance = k.numberOr("probability", 1.0); std::isfinite(chance)) {
                child.probability = static_cast<float>(std::clamp(chance, 0.0, 1.0));
            }
            if (child.type != "static" && child.type != "eventfollow" && child.type != "eventspawn" && child.type != "eventdeath") {
                reject(d, "particles: child " + child.type);
            }
            if (child.file.empty()) {
                continue;
            }
            if (depth >= kMaxChildDepth) {
                reject(d, "particles: children nested deeper than " + std::to_string(kMaxChildDepth));
                continue;
            }
            const auto key = std::make_pair(child.file, depth + 1);
            auto cached = cache.find(key);
            if (cached == cache.end()) {
                std::string childError;
                auto def = load(vfs, child.file, depth + 1, &childError, cache);
                if (!def) {
                    reject(d, childError);
                    continue;
                }
                cached = cache.emplace(key, std::make_shared<const ParticleDefinition>(std::move(*def))).first;
            }
            for (const std::string &r : cached->second->unsupported) {
                reject(d, r);
            }
            for (const std::string &r : cached->second->degraded) {
                degrade(d, r);
            }
            if (child.type == "eventfollow"
                && std::any_of(cached->second->children.begin(), cached->second->children.end(),
                               [](const ParticleChild &c) { return c.type == "eventfollow"; })) {
                reject(d, "particles: nested eventfollow"); // a follower of a follower is not drawn
            }
            if (d.systemCount + cached->second->systemCount > kMaxParticleSystems) {
                reject(d, "particles: more than " + std::to_string(kMaxParticleSystems) + " systems in one object");
                continue;
            }
            d.systemCount += cached->second->systemCount;
            child.definition = cached->second;
            d.children.push_back(std::move(child));
        }
    }
    return d;
}

} // namespace

std::optional<ParticleDefinition> loadParticleDefinition(const Vfs &vfs, const std::string &file, std::string *error)
{
    DefinitionCache cache;
    return load(vfs, file, 0, error, cache);
}

} // namespace murasaki::scene
