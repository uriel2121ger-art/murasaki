#include "render/particlerenderer.h"

#include "core/vk.h"
#include "util/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace murasaki::scene {

namespace {

// A system whose sprites add up to this many times the scene's area is drawn at half size (see ParticleRenderer::draw).
constexpr double kHalfSizeOverdraw = 1.5;

using Col = ParticleColumn;

constexpr uint32_t kFloatsPerVertex = 17;
constexpr uint32_t kStride = kFloatsPerVertex * sizeof(float);
constexpr size_t kInitialParticles = 512;
constexpr size_t kMaxDepth = 4;
// Memory limits. A system's vertices are rewritten every frame, so the budget is what the live particles may need at
// the worst: per system, over all systems, and in quads (the shared index buffer is sized by the largest).
constexpr size_t kSystemVertexBytes = 128u << 20;
constexpr size_t kGlobalVertexBytes = 256u << 20;
constexpr size_t kMaxQuads = 500000;
constexpr size_t kMaxFollowers = 256;
constexpr double kRebuildInterval = 0.25; // seconds between rebuilds a `count` override may cause

float fin(float v)
{
    return std::isfinite(v) ? v : 0.0f;
}

const GeometryAttribute kAttributes[] = {
    {"a_Position", VK_FORMAT_R32G32B32_SFLOAT, 0},
    {"a_TexCoordVec4", VK_FORMAT_R32G32B32A32_SFLOAT, 12},
    {"a_Color", VK_FORMAT_R32G32B32A32_SFLOAT, 28},
    {"a_TexCoordVec4C1", VK_FORMAT_R32G32B32A32_SFLOAT, 44},
    {"a_TexCoordC2", VK_FORMAT_R32G32_SFLOAT, 60},
};

// Rope vertices: a_PositionVec4 (start xyz, size), a_TexCoordVec4 (end xyz, trail length), a_TexCoordVec4C1 (control
// point xyz, trail position), a_TexCoordVec4C2 (control point xyz, end size), a_TexCoordVec4C3 (end colour),
// a_TexCoordC4 (corner), a_Color (start colour): 26 floats.
constexpr uint32_t kRopeFloats = 26;
constexpr uint32_t kRopeStride = kRopeFloats * sizeof(float);
const GeometryAttribute kRopeAttributes[] = {
    {"a_PositionVec4", VK_FORMAT_R32G32B32A32_SFLOAT, 0},
    {"a_TexCoordVec4", VK_FORMAT_R32G32B32A32_SFLOAT, 16},
    {"a_TexCoordVec4C1", VK_FORMAT_R32G32B32A32_SFLOAT, 32},
    {"a_TexCoordVec4C2", VK_FORMAT_R32G32B32A32_SFLOAT, 48},
    {"a_TexCoordVec4C3", VK_FORMAT_R32G32B32A32_SFLOAT, 64},
    {"a_TexCoordC4", VK_FORMAT_R32G32_SFLOAT, 80},
    {"a_Color", VK_FORMAT_R32G32B32A32_SFLOAT, 88},
};

Mat4 childMatrix(const ParticleChild &c)
{
    return translation(c.origin) * rotationEuler(c.angles) * scaling(c.scale);
}

PassTexture passTexture(const GpuTexture &t, PassRunner &runner)
{
    PassTexture p;
    p.view = t.view();
    p.sampler = runner.sampler(t.nearest(), t.clamp());
    p.info = {t.width(), t.height(), t.imageWidth(), t.imageHeight()};
    return p;
}

// The ribbon through the live particles of a rope system, in spawn order: a Catmull-Rom spline through their positions
// (`subdivision` pieces between neighbours, size and colour interpolated), four vertices for each piece. The shader gets
// each piece's two ends and the points around them, and finds the side of the ribbon from the view direction.
size_t buildRope(const ParticleSystem &s, int subdivision, float uvScale, bool uvScrolling, bool smoothing, double time, float *out)
{
    const size_t n = s.count();
    if (n < 2) {
        return 0;
    }
    const auto &X = s.column(Col::X);
    const auto &Y = s.column(Col::Y);
    const auto &Z = s.column(Col::Z);
    const auto &Size = s.column(Col::Size);
    const auto &R = s.column(Col::R);
    const auto &G = s.column(Col::G);
    const auto &B = s.column(Col::B);
    const auto &A = s.column(Col::Alpha);
    struct Point {
        Vec3 p;
        float size;
        float color[4];
    };
    const size_t pieces = (n - 1) * static_cast<size_t>(subdivision);
    std::vector<Point> points(pieces + 1);
    const auto at = [&](size_t i) { return Vec3{fin(X[i]), fin(Y[i]), fin(Z[i])}; };
    const auto spline = [](Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float t) {
        const float t2 = t * t;
        const float t3 = t2 * t;
        const auto one = [&](float a, float b, float c, float d) {
            return 0.5f * (2.0f * b + (-a + c) * t + (2.0f * a - 5.0f * b + 4.0f * c - d) * t2 + (-a + 3.0f * b - 3.0f * c + d) * t3);
        };
        return Vec3{one(p0.x, p1.x, p2.x, p3.x), one(p0.y, p1.y, p2.y, p3.y), one(p0.z, p1.z, p2.z, p3.z)};
    };
    for (size_t i = 0; i + 1 < n; ++i) {
        const Vec3 p1 = at(i);
        const Vec3 p2 = at(i + 1);
        const Vec3 p0 = i > 0 ? at(i - 1) : p1;
        const Vec3 p3 = i + 2 < n ? at(i + 2) : p2;
        for (int k = 0; k < subdivision; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(subdivision);
            Point &pt = points[i * static_cast<size_t>(subdivision) + static_cast<size_t>(k)];
            pt.p = spline(p0, p1, p2, p3, t);
            pt.size = fin(Size[i]) + (fin(Size[i + 1]) - fin(Size[i])) * t;
            const float c1[4] = {fin(R[i]), fin(G[i]), fin(B[i]), fin(A[i])};
            const float c2[4] = {fin(R[i + 1]), fin(G[i + 1]), fin(B[i + 1]), fin(A[i + 1])};
            for (int c = 0; c < 4; ++c) {
                pt.color[c] = c1[c] + (c2[c] - c1[c]) * t;
            }
        }
    }
    {
        Point &last = points[pieces];
        last.p = at(n - 1);
        last.size = fin(Size[n - 1]);
        last.color[0] = fin(R[n - 1]);
        last.color[1] = fin(G[n - 1]);
        last.color[2] = fin(B[n - 1]);
        last.color[3] = fin(A[n - 1]);
    }
    // texture coordinate along the rope: the shader divides the piece index by (trail length - 1), so the length is in
    // pieces; a UV scale above 1 repeats the texture. Smoothing spaces them by distance when the particles are alike.
    const float scale = uvScale > 0.0f ? uvScale : 1.0f;
    const float trailLength = static_cast<float>(pieces) / scale + 1.0f;
    std::vector<float> along(pieces + 1, 0.0f);
    float total = 0.0f;
    if (smoothing && !uvScrolling) {
        for (size_t i = 1; i <= pieces; ++i) {
            const Vec3 d{points[i].p.x - points[i - 1].p.x, points[i].p.y - points[i - 1].p.y, points[i].p.z - points[i - 1].p.z};
            total += std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            along[i] = total;
        }
    }
    const float scroll = uvScrolling ? static_cast<float>(std::fmod(time, 10000.0)) * (trailLength - 1.0f) : 0.0f;
    static const float corners[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    size_t vertex = 0;
    for (size_t k = 0; k < pieces; ++k) {
        const Point &a = points[k];
        const Point &b = points[k + 1];
        const Vec3 before = k > 0 ? points[k - 1].p : a.p;
        const Vec3 after = k + 2 <= pieces ? points[k + 2].p : b.p;
        const float position = (smoothing && !uvScrolling && total > 0.0f ? along[k] / total * static_cast<float>(pieces) : static_cast<float>(k)) + scroll;
        for (const auto &c : corners) {
            float *v = out + vertex * kRopeFloats;
            v[0] = a.p.x; v[1] = a.p.y; v[2] = a.p.z; v[3] = a.size;
            v[4] = b.p.x; v[5] = b.p.y; v[6] = b.p.z; v[7] = trailLength;
            v[8] = before.x; v[9] = before.y; v[10] = before.z; v[11] = position;
            v[12] = after.x; v[13] = after.y; v[14] = after.z; v[15] = b.size;
            v[16] = b.color[0]; v[17] = b.color[1]; v[18] = b.color[2]; v[19] = b.color[3];
            v[20] = c[0]; v[21] = c[1];
            v[22] = a.color[0]; v[23] = a.color[1]; v[24] = a.color[2]; v[25] = a.color[3];
            ++vertex;
        }
    }
    return vertex;
}

// The trails of a rope-trail system: one ribbon per particle through its current state and its history (newest
// first), a piece between neighbours. trailLength is the number of points of that particle's ribbon and the position
// the piece's index from the head, which is what the shader's TRAILRENDERER branch lays the texture out with.
size_t buildRopeTrail(const ParticleSystem &s, float *out)
{
    const auto &X = s.column(Col::X);
    const auto &Y = s.column(Col::Y);
    const auto &Z = s.column(Col::Z);
    const auto &Size = s.column(Col::Size);
    const auto &R = s.column(Col::R);
    const auto &G = s.column(Col::G);
    const auto &B = s.column(Col::B);
    const auto &A = s.column(Col::Alpha);
    static const float corners[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    size_t vertex = 0;
    std::vector<std::array<float, 8>> pts;
    for (size_t i = 0; i < s.count(); ++i) {
        const int history = s.trailCount(i);
        if (history < 1) {
            continue;
        }
        pts.clear();
        pts.push_back({fin(X[i]), fin(Y[i]), fin(Z[i]), fin(Size[i]), fin(R[i]), fin(G[i]), fin(B[i]), fin(A[i])});
        for (int k = 0; k < history; ++k) {
            const float *h = s.trailPoint(i, k);
            pts.push_back({h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]});
        }
        const float length = static_cast<float>(pts.size());
        for (size_t k = 0; k + 1 < pts.size(); ++k) {
            const auto &a = pts[k];
            const auto &b = pts[k + 1];
            const auto &before = k > 0 ? pts[k - 1] : a;
            const auto &after = k + 2 < pts.size() ? pts[k + 2] : b;
            for (const auto &c : corners) {
                float *v = out + vertex * kRopeFloats;
                v[0] = a[0]; v[1] = a[1]; v[2] = a[2]; v[3] = a[3];
                v[4] = b[0]; v[5] = b[1]; v[6] = b[2]; v[7] = length;
                v[8] = before[0]; v[9] = before[1]; v[10] = before[2]; v[11] = static_cast<float>(k);
                v[12] = after[0]; v[13] = after[1]; v[14] = after[2]; v[15] = b[3];
                v[16] = b[4]; v[17] = b[5]; v[18] = b[6]; v[19] = b[7];
                v[20] = c[0]; v[21] = c[1];
                v[22] = a[4]; v[23] = a[5]; v[24] = a[6]; v[25] = a[7];
                ++vertex;
            }
        }
    }
    return vertex;
}

} // namespace

bool particleAttributeKnown(const std::string &name)
{
    const auto known = [&](const GeometryAttribute &a) { return name == a.name; };
    return std::any_of(std::begin(kAttributes), std::end(kAttributes), known) || std::any_of(std::begin(kRopeAttributes), std::end(kRopeAttributes), known);
}

struct ParticleRenderer::Runtime {
    const ParticleDefinition *def = nullptr;
    Mat4 relative = Mat4::identity(); // the system's placement inside the object
    std::vector<std::unique_ptr<ParticleSystem>> instances; // a plain system has one; a follower child one per parent particle
    std::vector<uint32_t> owners;                           // followers: the serial of the parent particle each follows
    bool follower = false;
    size_t parent = 0;                                      // followers: index of the parent's runtime in the layer
    size_t maxFollowers = 0;
    int event = 0;                                          // event children: 1 a copy at every birth in the parent, 2 at every death
    float probability = 1.0f;                               // event children: the chance a parent particle gets a copy
    std::vector<std::pair<uint32_t, ParticleEventValues>> known; // event children: the parent's particles of the last frame, by serial
    std::vector<std::pair<uint32_t, ParticleEventValues>> current; // event children: this frame's list, kept to reuse its memory
    Mat4 toScene = Mat4::identity();                        // the system's space to the scene's and back (collisionbounds)
    Mat4 fromScene = Mat4::identity();
    bool sceneKnown = false;
    uint32_t salt = 0;
    MaterialPass pass;                // the material's pass plus the combos the renderer forces
    const TranslatedShader *shader = nullptr;
    const GpuTexture *texture = nullptr;
    const GpuTexture *normal = nullptr;
    bool trail = false;
    bool rope = false;
    bool ropeTrail = false; // a ribbon per particle through its history instead of one through all of them
    int trailSegments = 4;
    int ropeSubdivision = 4;
    float ropeUvScale = 1.0f;
    bool ropeUvScrolling = false;
    bool ropeUvSmoothing = true;
    bool uniformLifetimes = false;
    bool refract = false;
    ParticleOrientation orientation;  // renderer `orientation` / `axis`: g_OrientationRight / Up / Forward
    int sheetCols = 0;
    int sheetRows = 0;
    int sheetFrames = 0;
    float trailLength = 0.05f;
    float trailMax = 10.0f;
    float trailMin = 0.0f;
    GpuBuffer vertices;
    size_t vertexParticles = 0; // quads the buffer holds
    size_t vertexBytes = 0;
    bool warned = false;        // the budget or an allocation failure was logged
};

struct ParticleRenderer::Layer {
    ParticleOverrides overrides;
    std::vector<Runtime> systems;
    uint32_t seedSalt = 0;
    double lastRebuild = -1.0e9;
    uint32_t burstServed = 0; // ParticleLayer::burst already spawned
    bool warned = false;
};

ParticleRenderer::ParticleRenderer(const VkContext &ctx, PassRunner &runner, ShaderLibrary &shaders, const Scene &scene,
                                   const PropertyStore &props, TextureLookup textures)
    : m_ctx(ctx), m_runner(runner), m_shaders(shaders), m_scene(scene), m_props(props), m_textures(std::move(textures))
{
    for (size_t i = 0; i < scene.particles.size(); ++i) {
        auto layer = std::make_unique<Layer>();
        layer->seedSalt = static_cast<uint32_t>(i) * 31u;
        m_layers.push_back(std::move(layer));
        rebuild(*m_layers.back(), i);
        m_usesAudio = m_usesAudio || (scene.particles[i].definition && particleUsesAudio(*scene.particles[i].definition));
    }
}

ParticleRenderer::~ParticleRenderer() = default;

std::unique_ptr<ParticleSystem> ParticleRenderer::makeSystem(const Layer &layer, const Runtime &r, const ParticleDefinition &def, uint32_t seed)
{
    auto system = std::make_unique<ParticleSystem>(def, layer.overrides, seed, &m_props);
    if (r.sceneKnown) {
        system->setSceneSpace(m_scene.orthoSize, r.toScene, r.fromScene);
    }
    if (r.ropeTrail) {
        system->enableTrails(r.trailSegments, std::max(0.01f, r.trailLength) / static_cast<float>(r.trailSegments));
    }
    if (r.sheetFrames > 0) {
        float duration = 0.0f;
        for (const TexFrame &f : r.texture->frames()) {
            duration += f.duration;
        }
        system->setSpriteSheet(r.sheetFrames, duration);
    }
    return system;
}

ParticleOverrides ParticleRenderer::overridesOf(const ParticleLayer &p) const
{
    ParticleOverrides o;
    o.alpha = resolve(p.alpha, m_props);
    o.size = resolve(p.size, m_props);
    o.lifetime = resolve(p.lifetime, m_props);
    o.rate = resolve(p.rate, m_props);
    o.speed = resolve(p.speed, m_props);
    o.count = resolve(p.count, m_props);
    o.color = resolve(p.color, m_props);
    o.controlPoints = p.controlPoints;
    if (o.color.x > 1.0f || o.color.y > 1.0f || o.color.z > 1.0f) {
        o.color = {o.color.x / 255.0f, o.color.y / 255.0f, o.color.z / 255.0f}; // `color` is written 0..255
    }
    return sanitizeOverrides(o);
}

void ParticleRenderer::rebuild(Layer &layer, size_t index)
{
    const ParticleLayer &p = m_scene.particles[index];
    layer.overrides = overridesOf(p);
    layer.systems.clear();
    if (p.definition) {
        build(layer, *p.definition, Mat4::identity(), 0, layer.seedSalt);
    }
}

void ParticleRenderer::build(Layer &layer, const ParticleDefinition &def, const Mat4 &relative, size_t depth, uint32_t salt, const ParticleChild *follow,
                             size_t parent)
{
    if (layer.systems.size() >= kMaxParticleSystems) {
        if (!layer.warned) {
            layer.warned = true;
            logError("particle object with more than {} systems: the rest are left out", kMaxParticleSystems);
        }
        return;
    }
    Runtime r;
    r.def = &def;
    r.relative = relative;
    r.salt = salt;
    if (follow) {
        r.follower = true;
        r.parent = parent;
        r.maxFollowers = follow->maxCount > 0 ? std::min(static_cast<size_t>(follow->maxCount), kMaxFollowers) : 16;
        r.event = follow->type == "eventspawn" ? 1 : follow->type == "eventdeath" ? 2 : 0;
        r.probability = follow->probability;
    }
    r.texture = def.materialPass.textures.empty() ? nullptr : m_textures(def.materialPass.textures[0]);
    r.normal = def.materialPass.textures.size() > 1 && !def.materialPass.textures[1].empty() ? m_textures(def.materialPass.textures[1]) : nullptr;
    if (!r.texture) {
        throw std::runtime_error("particle texture '" + (def.materialPass.textures.empty() ? std::string() : def.materialPass.textures[0]) + "' is not loaded");
    }
    if (const auto it = def.materialPass.combos.find("REFRACT"); it != def.materialPass.combos.end() && it->second != 0) {
        r.refract = true;
    }
    bool sheet = false;
    if (const GpuTexture *t = r.texture; t->frames().size() > 1) {
        const TexFrame &f = t->frames()[0];
        const float fw = std::hypot(f.axisXx, f.axisXy);
        const float fh = std::hypot(f.axisYx, f.axisYy);
        if (fw > 0.0f && fh > 0.0f) {
            r.sheetCols = std::max(1, static_cast<int>(std::lround(t->width() / fw)));
            r.sheetRows = std::max(1, static_cast<int>(std::lround(t->height() / fh)));
            r.sheetFrames = static_cast<int>(t->frames().size());
            sheet = true;
        }
    }
    for (const ParticleComponent &c : def.renderers) {
        if (c.name == "spritetrail") {
            r.trail = true;
            r.trailLength = c.number("length", 0.05f, &m_props);
            r.trailMax = c.number("maxlength", 10.0f, &m_props);
            r.trailMin = c.number("minlength", 0.0f, &m_props);
        } else if (c.name == "rope" || c.name == "ropetrail") {
            r.rope = true;
            if (c.name == "ropetrail") {
                r.ropeTrail = true;
                r.trailLength = c.number("length", 1.0f, &m_props); // seconds of history the trail covers
                r.trailSegments = std::clamp(c.integer("segments", 4), 2, 64);
            }
            r.ropeSubdivision = std::clamp(c.integer("subdivision", c.name == "rope" ? 4 : 1), 1, 16);
            r.ropeUvScale = c.number("uvscale", 1.0f, &m_props) > 0.0f ? c.number("uvscale", 1.0f, &m_props) : 1.0f;
            r.ropeUvScrolling = c.number("uvscrolling", 0.0f, &m_props) != 0.0f;
            r.ropeUvSmoothing = c.number("uvsmoothing", 1.0f, &m_props) != 0.0f;
        }
    }
    for (const ParticleComponent &c : def.initializers) {
        if (c.name == "lifetimerandom") {
            r.uniformLifetimes = c.number("min", 0.0f, &m_props) == c.number("max", 1.0f, &m_props);
        }
    }
    r.pass = particleMaterialPass(def, sheet);
    r.orientation = particleOrientation(def);
    std::string error;
    r.shader = m_shaders.get(r.pass, boundSlotsOf(r.pass, particleSlots(r.pass)), &error);
    if (!r.shader) {
        throw std::runtime_error(error);
    }
    if (!follow) {
        r.instances.push_back(makeSystem(layer, r, def, particleSeed(salt)));
        r.instances.back()->warmUp();
    }
    layer.systems.push_back(std::move(r));
    const size_t self = layer.systems.size() - 1;

    if (depth < kMaxDepth) {
        for (const ParticleChild &child : def.children) {
            if (!child.definition) {
                continue;
            }
            const uint32_t childSalt = salt + 977u * static_cast<uint32_t>(layer.systems.size());
            if (child.type == "eventfollow" || child.type == "eventspawn" || child.type == "eventdeath") {
                // one copy of the child follows each particle of this system (eventfollow), or a copy is made where one is born
                // (eventspawn) or dies (eventdeath); it lives in this system's space
                build(layer, *child.definition, relative, depth + 1, childSalt, &child, self);
            } else {
                build(layer, *child.definition, relative * childMatrix(child), depth + 1, childSalt);
            }
        }
    }
}

bool ParticleRenderer::anyVisible() const
{
    for (const ParticleLayer &p : m_scene.particles) {
        if (nodeVisible(m_scene, p.node, m_props)) {
            return true;
        }
    }
    return false;
}

uint64_t ParticleRenderer::liveParticles() const
{
    uint64_t n = 0;
    for (const auto &layer : m_layers) {
        for (const Runtime &r : layer->systems) {
            for (const auto &s : r.instances) {
                n += s->count();
            }
        }
    }
    return n;
}

void ParticleRenderer::update(double dt, Vec2 pointer, bool pointerKnown, Vec2, const AudioSpectrum *audio)
{
    // a new frame is being recorded: the index buffers retired during the last one are no longer in use
    m_retiredIndices.clear();
    // overrides change with user properties and with scripts: what spawns from now on follows at once, only a new pool
    // size (`count`) makes a system start again
    m_clock += dt;
    for (size_t i = 0; i < m_layers.size(); ++i) {
        const ParticleOverrides now = overridesOf(m_scene.particles[i]);
        ParticleOverrides &was = m_layers[i]->overrides;
        if (now.alpha == was.alpha && now.size == was.size && now.lifetime == was.lifetime && now.rate == was.rate && now.speed == was.speed
            && now.count == was.count && now.color.x == was.color.x && now.color.y == was.color.y && now.color.z == was.color.z) {
            continue;
        }
        if (now.count != was.count) {
            // a slider moving `count` asks for a rebuild every frame: at most one every kRebuildInterval, the last value
            // wins. Nothing of the previous frame is in flight (the engine waits for each frame), so the old
            // buffers can go at once and no device wait is needed.
            if (m_clock - m_layers[i]->lastRebuild >= kRebuildInterval) {
                m_layers[i]->lastRebuild = m_clock;
                rebuild(*m_layers[i], i);
            }
            continue;
        }
        was = now;
        for (Runtime &r : m_layers[i]->systems) {
            for (auto &s : r.instances) {
                s->setOverrides(now);
            }
        }
    }
    const Vec2 pointerScene{pointer.x * m_scene.orthoSize.x, (1.0f - pointer.y) * m_scene.orthoSize.y};
    const AudioSpectrum silence;
    for (size_t i = 0; i < m_layers.size(); ++i) {
        const ParticleLayer &p = m_scene.particles[i];
        if (!nodeVisible(m_scene, p.node, m_props)) {
            continue;
        }
        if (m_usesAudio) {
            // (followers made during this frame take the spectrum from the next one)
            const AudioSpectrum &now = audio ? *audio : silence;
            for (Runtime &r : m_layers[i]->systems) {
                for (auto &s : r.instances) {
                    s->setAudio(now.left16, now.right16);
                }
            }
        }
        const Mat4 world = nodeWorldMatrix(m_scene, p.node);
        for (Runtime &r : m_layers[i]->systems) {
            r.toScene = world * r.relative;
            r.fromScene = inverse(r.toScene);
            r.sceneKnown = true;
            for (auto &s : r.instances) {
                s->setSceneSpace(m_scene.orthoSize, r.toScene, r.fromScene);
            }
            if (r.follower) {
                r.event ? syncEvents(*m_layers[i], r, dt) : syncFollowers(*m_layers[i], r, dt);
                continue;
            }
            r.instances[0]->updateControlPoints(pointerScene, pointerKnown, r.fromScene);
            // what scripts do to the system (IParticleSystem): stop ends the emission, pause freezes everything, emit asks for more
            r.instances[0]->setEmitting(!p.stopped);
            if (m_layers[i]->burstServed != p.burst) {
                const uint32_t wanted = p.burst - m_layers[i]->burstServed;
                if (!p.paused) {
                    r.instances[0]->burst(wanted);
                }
            }
            if (!p.paused) {
                r.instances[0]->advance(dt);
            }
        }
        m_layers[i]->burstServed = p.burst;
    }
}

// The follower copies of a child: one per live particle of the parent (the newest `maxFollowers`), each with its
// control point 0 on that particle. A copy whose particle died stops emitting and goes when its own particles do.
void ParticleRenderer::syncFollowers(Layer &layer, Runtime &r, double dt)
{
    if (layer.systems[r.parent].instances.empty()) {
        return; // a follower of a follower: not supported (its parent has no particles of its own to follow)
    }
    const ParticleSystem &parent = *layer.systems[r.parent].instances[0];
    const auto &serials = parent.serials();
    const size_t first = serials.size() > r.maxFollowers ? serials.size() - r.maxFollowers : 0;
    const auto &X = parent.column(Col::X);
    const auto &Y = parent.column(Col::Y);
    const auto &Z = parent.column(Col::Z);
    std::vector<bool> alive(r.owners.size(), false);
    std::unordered_map<uint32_t, size_t> slotOf; // serial of the parent particle -> index of its copy
    slotOf.reserve(r.owners.size() + (serials.size() - first));
    for (size_t k = 0; k < r.owners.size(); ++k) {
        slotOf.emplace(r.owners[k], k);
    }
    for (size_t i = first; i < serials.size(); ++i) {
        if (!particleEventChosen(serials[i], r.salt, r.probability)) {
            continue; // `probability`: this particle has no follower
        }
        size_t slot;
        if (const auto it = slotOf.find(serials[i]); it != slotOf.end()) {
            slot = it->second;
            alive[slot] = true;
        } else {
            slot = r.owners.size();
            r.instances.push_back(makeSystem(layer, r, *r.def, particleSeed(r.salt + serials[i] * 13u)));
            r.owners.push_back(serials[i]);
            alive.push_back(true);
            slotOf.emplace(serials[i], slot);
        }
        r.instances[slot]->setControlPoint(0, {X[i], Y[i], Z[i]});
        r.instances[slot]->setEventSource(parent.eventValues(i));
    }
    for (size_t k = 0; k < r.instances.size(); ++k) {
        if (!alive[k]) {
            r.instances[k]->setEmitting(false);
        }
        r.instances[k]->advance(dt);
    }
    // drop the copies that are done, keeping the order (one pass)
    size_t keep = 0;
    for (size_t k = 0; k < r.instances.size(); ++k) {
        if (!alive[k] && r.instances[k]->count() == 0) {
            continue;
        }
        if (keep != k) {
            r.instances[keep] = std::move(r.instances[k]);
            r.owners[keep] = r.owners[k];
        }
        ++keep;
    }
    r.instances.resize(keep);
    r.owners.resize(keep);
}

// Event children: a copy of the child is made at the place a particle of the parent is born (eventspawn) or dies (eventdeath),
// with its control point 0 there. The copy emits once (its emitters' instantaneous burst) and lives until its particles are gone.
// How many live at a time, and how many a frame may make, are capped.
void ParticleRenderer::syncEvents(Layer &layer, Runtime &r, double dt)
{
    if (layer.systems[r.parent].instances.empty()) {
        return;
    }
    const ParticleSystem &parent = *layer.systems[r.parent].instances[0];
    const auto &serials = parent.serials();
    // the serials grow with the births, so both lists are sorted
    // (a particle the probability leaves out never makes a copy: its values are not read)
    std::vector<std::pair<uint32_t, ParticleEventValues>> &now = r.current;
    now.clear();
    now.reserve(serials.size());
    for (size_t i = 0; i < serials.size(); ++i) {
        now.emplace_back(serials[i], particleEventChosen(serials[i], r.salt, r.probability) ? parent.eventValues(i) : ParticleEventValues{});
    }
    std::sort(now.begin(), now.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    constexpr size_t kMaxEventsPerFrame = 16;
    size_t made = 0;
    const auto spawnAt = [&](const ParticleEventValues &where, uint32_t serial) {
        if (r.instances.size() >= r.maxFollowers || made >= kMaxEventsPerFrame || !particleEventChosen(serial, r.salt, r.probability)) {
            return;
        }
        auto system = makeSystem(layer, r, *r.def, particleSeed(r.salt + serial * 13u));
        system->setControlPoint(0, where.position);
        system->setEventSource(where); // what inheritvaluefromevent and inheritinitialvaluefromevent read
        r.instances.push_back(std::move(system));
        ++made;
    };
    size_t a = 0, b = 0;
    while (a < r.known.size() || b < now.size()) {
        if (b >= now.size() || (a < r.known.size() && r.known[a].first < now[b].first)) {
            if (r.event == 2) {
                spawnAt(r.known[a].second, r.known[a].first); // gone since the last frame: it died where it was last seen
            }
            ++a;
        } else if (a >= r.known.size() || now[b].first < r.known[a].first) {
            if (r.event == 1) {
                spawnAt(now[b].second, now[b].first);
            }
            ++b;
        } else {
            ++a;
            ++b;
        }
    }
    std::swap(r.known, r.current);
    for (size_t k = 0; k < r.instances.size(); ++k) {
        r.instances[k]->advance(dt);
        r.instances[k]->setEmitting(false); // one burst per event, whatever else the emitters would go on to do
    }
    r.instances.erase(std::remove_if(r.instances.begin(), r.instances.end(), [](const std::unique_ptr<ParticleSystem> &s) { return s->count() == 0; }),
                      r.instances.end());
}

// Makes `r`'s vertex buffer hold `quads` quads within the memory budget and returns how many it can hold now (the request,
// clamped; 0 when the buffer cannot be made). A failure to allocate is logged once and the system is skipped.
size_t ParticleRenderer::ensureCapacity(Runtime &r, size_t quads)
{
    const size_t quadBytes = 4 * static_cast<size_t>(r.rope ? kRopeStride : kStride);
    const size_t others = m_vertexBytes - r.vertexBytes;
    const size_t room = kGlobalVertexBytes > others ? kGlobalVertexBytes - others : 0;
    const size_t limit = std::min({kMaxQuads, kSystemVertexBytes / quadBytes, room / quadBytes});
    if (quads > limit && !r.warned) {
        r.warned = true;
        logError("particle system of {} quads is clamped to {} by the vertex memory budget", quads, limit);
    }
    quads = std::min(quads, limit);
    if (quads <= r.vertexParticles) {
        return quads;
    }
    const size_t wanted = std::min(std::max<size_t>(r.vertexParticles ? r.vertexParticles * 2 : kInitialParticles, quads), limit);
    try {
        // the previous frame is complete (the engine waits for each one), so the old buffer can go at once
        r.vertices = m_ctx.createBuffer(wanted * quadBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
    } catch (const std::exception &e) {
        if (!r.warned) {
            r.warned = true;
            logError("particle vertex buffer of {} bytes: {}; the system is not drawn", wanted * quadBytes, e.what());
        }
        r.vertices = GpuBuffer();
        r.vertexParticles = 0;
        m_vertexBytes = others;
        r.vertexBytes = 0;
        return 0;
    }
    r.vertexParticles = wanted;
    r.vertexBytes = wanted * quadBytes;
    m_vertexBytes = others + r.vertexBytes;
    return quads;
}

// The index buffer is shared by every system, so a draw already recorded may use the one being replaced: the old buffer is
// kept until the next frame starts (update()). False when it cannot be made.
bool ParticleRenderer::ensureIndices(size_t particles)
{
    if (particles <= m_indexParticles) {
        return true;
    }
    particles = std::min(particles, kMaxQuads);
    const size_t wanted = std::min(std::max<size_t>(m_indexParticles ? m_indexParticles * 2 : kInitialParticles, particles), kMaxQuads);
    GpuBuffer fresh;
    try {
        fresh = m_ctx.createBuffer(wanted * 6 * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
    } catch (const std::exception &e) {
        if (!m_indexWarned) {
            m_indexWarned = true;
            logError("particle index buffer: {}; particles are not drawn", e.what());
        }
        return m_indexParticles >= particles;
    }
    auto *idx = static_cast<uint32_t *>(fresh.mapped());
    for (size_t i = 0; i < wanted; ++i) {
        const uint32_t b = static_cast<uint32_t>(i * 4);
        const uint32_t quad[6] = {b, b + 1, b + 2, b + 2, b + 3, b};
        std::memcpy(idx + i * 6, quad, sizeof(quad));
    }
    fresh.flush(0, wanted * 6 * sizeof(uint32_t));
    if (m_indices.handle() != VK_NULL_HANDLE) {
        m_retiredIndices.push_back(std::move(m_indices));
    }
    m_indices = std::move(fresh);
    m_indexParticles = wanted;
    return true;
}

size_t ParticleRenderer::draw(const Frame &frame, size_t layerIndex)
{
    size_t drawn = 0;
    const ParticleLayer &p = m_scene.particles[layerIndex];
    if (!nodeVisible(m_scene, p.node, m_props)) {
        return 0;
    }
    const Mat4 world = nodeWorldMatrix(m_scene, p.node);
    const Mat4 shift = translation({p.parallaxDepth.x * frame.parallax.x * frame.orthoSize.x, p.parallaxDepth.y * frame.parallax.y * frame.orthoSize.y, 0.0f});
        double coveredArea = 0.0; // sum of the squared sizes of the sprites written, in scene units
        const auto writeSprites = [&](Runtime &rt, ParticleSystem &sys, float *dst, size_t maxVertices) -> size_t {
            const size_t n = sys.count();
            size_t written = 0;
            const auto &X = sys.column(Col::X);
            const auto &Y = sys.column(Col::Y);
            const auto &Z = sys.column(Col::Z);
            const auto &VX = sys.column(Col::VX);
            const auto &VY = sys.column(Col::VY);
            const auto &VZ = sys.column(Col::VZ);
            const auto &RX = sys.column(Col::RotX);
            const auto &RY = sys.column(Col::RotY);
            const auto &RZ = sys.column(Col::RotZ);
            const auto &R = sys.column(Col::R);
            const auto &G = sys.column(Col::G);
            const auto &B = sys.column(Col::B);
            const auto &A = sys.column(Col::Alpha);
            const auto &Size = sys.column(Col::Size);
            const auto &Age = sys.column(Col::Age);
            const auto &Life = sys.column(Col::Lifetime);
            const auto &Frame_ = sys.column(Col::Frame);
            static const float corners[4][2] = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}};
            for (size_t i = 0; i < n; ++i) {
                if (written + 4 > maxVertices) {
                    break;
                }
                if (!std::isfinite(X[i]) || !std::isfinite(Y[i]) || !std::isfinite(Z[i]) || !(Size[i] > 0.0f) || Size[i] > 20000.0f
                    || !std::isfinite(R[i]) || !std::isfinite(G[i]) || !std::isfinite(B[i]) || !std::isfinite(A[i]) || !std::isfinite(RZ[i])) {
                    continue;
                }
                coveredArea += static_cast<double>(Size[i]) * Size[i];
                float lifeField = Life[i] > 0.0f ? Age[i] / Life[i] : 1.0f;
                if (rt.sheetFrames > 0 && Frame_[i] >= 0.0f) {
                    const float frames = static_cast<float>(rt.sheetFrames);
                    lifeField = rt.def->animationMode == "randomframe" ? (Frame_[i] + 0.5f) / frames : Frame_[i] / frames;
                }
                for (const auto &c : corners) {
                    float *v = dst + written * kFloatsPerVertex;
                    v[0] = X[i];
                    v[1] = Y[i];
                    v[2] = Z[i];
                    v[3] = c[0];
                    v[4] = c[1];
                    v[5] = RZ[i];
                    v[6] = Size[i];
                    v[7] = R[i];
                    v[8] = G[i];
                    v[9] = B[i];
                    v[10] = A[i];
                    v[11] = VX[i];
                    v[12] = VY[i];
                    v[13] = VZ[i];
                    v[14] = lifeField;
                    v[15] = RX[i];
                    v[16] = RY[i];
                    ++written;
                }
            }
            return written;
        };
    for (Runtime &r : m_layers[layerIndex]->systems) {
        size_t quads = 0;
        for (const auto &s : r.instances) {
            const size_t count = s->count();
            if (r.ropeTrail) {
                for (size_t i = 0; i < count; ++i) {
                    quads += static_cast<size_t>(s->trailCount(i));
                }
            } else {
                quads += r.rope ? (count > 1 ? (count - 1) * static_cast<size_t>(r.ropeSubdivision) : 0) : count;
            }
        }
        if (quads == 0) {
            continue;
        }
        const size_t allowed = ensureCapacity(r, quads);
        if (allowed == 0 || !ensureIndices(allowed)) {
            continue;
        }
        if (allowed < quads && (r.rope || r.ropeTrail)) {
            continue; // a ribbon cut short would be a different picture: over budget, it is not drawn
        }
        auto *out = static_cast<float *>(r.vertices.mapped());
        const uint32_t floats = r.rope ? kRopeFloats : kFloatsPerVertex;
        size_t written = 0;
        coveredArea = 0.0;
        for (const auto &s : r.instances) {
            float *dst = out + written * floats;
            written += r.ropeTrail ? buildRopeTrail(*s, dst)
                : r.rope       ? buildRope(*s, r.ropeSubdivision, r.ropeUvScale, r.ropeUvScrolling, r.ropeUvSmoothing && r.uniformLifetimes, frame.inputs.time, dst)
                               : writeSprites(r, *s, dst, allowed * 4 - written);
        }
        if (written == 0) {
            continue;
        }
        r.vertices.flush(0, written * (r.rope ? kRopeStride : kStride));

        Geometry geometry;
        geometry.vertexBuffer = r.vertices.handle();
        geometry.stride = r.rope ? kRopeStride : kStride;
        geometry.indexBuffer = m_indices.handle();
        geometry.indexCount = static_cast<uint32_t>(written / 4 * 6);
        geometry.attributes = r.rope ? kRopeAttributes : kAttributes;
        geometry.attributeCount = static_cast<uint32_t>(r.rope ? std::size(kRopeAttributes) : std::size(kAttributes));

        DrawRequest req;
        req.shader = r.shader;
        req.geometry = &geometry;
        req.uniforms.material = &r.pass;
        req.uniforms.properties = &m_props;
        req.uniforms.inputs = frame.inputs;
        if (!frame.perspectiveScene) {
            req.uniforms.inputs.eye = {0.0f, 0.0f, 1000.0f}; // the camera perspective particles are seen from (below)
        }
        const Mat4 model = world * r.relative;
        req.uniforms.model = model;
        if ((r.def->flags & 4) != 0 && !frame.perspectiveScene) {
            // perspective particles (snow, rain with depth): a camera 1000 units in front of the middle of the scene
            // looks at it; particles with z towards the camera come out larger
            const float aspect = frame.orthoSize.x / frame.orthoSize.y;
            const Mat4 projection = perspectiveGl(frame.fov * 3.14159265f / 180.0f, aspect, frame.nearZ, frame.farZ)
                * translation({0.0f, 0.0f, -1000.0f}) * translation({-frame.orthoSize.x / 2.0f, -frame.orthoSize.y / 2.0f, 0.0f});
            req.uniforms.viewProjection = projection;
            req.uniforms.mvp = projection * shift * model;
        } else {
            req.uniforms.viewProjection = frame.viewProjection;
            req.uniforms.mvp = frame.viewProjection * shift * model;
        }
        if (r.orientation.mode != 0) {
            // sprites face the renderer's axis instead of the screen (the engine's default is the identity basis)
            const auto axis = [](Vec3 v) { return std::array<float, 4>{v.x, v.y, v.z, 0.0f}; };
            req.uniforms.overrides["g_OrientationRight"] = axis(r.orientation.right);
            req.uniforms.overrides["g_OrientationUp"] = axis(r.orientation.up);
            req.uniforms.overrides["g_OrientationForward"] = axis(r.orientation.forward);
        }
        req.blending = r.pass.blending;
        req.blend = true;
        req.target = frame.target;
        req.targetFormat = frame.targetFormat;
        req.extent = frame.extent;
        req.clear = false;

        req.textures[0] = passTexture(*r.texture, m_runner);
        req.uniforms.textures[0] = req.textures[0].info;
        if (r.normal) {
            req.textures[1] = passTexture(*r.normal, m_runner);
            req.uniforms.textures[1] = req.textures[1].info;
        }
        if (r.refract && frame.snapshot) {
            RenderTarget &full = frame.snapshot();
            PassTexture t;
            t.view = full.view();
            t.sampler = m_runner.linearClamp();
            t.info = {full.extent.width, full.extent.height, full.extent.width, full.extent.height};
            req.textures[3] = t;
            req.uniforms.textures[3] = t.info;
        }
        // g_RenderVar0: trail length factor, longest and shortest trail; g_RenderVar1: sheet size, frames, aspect ratio
        if (r.ropeTrail) {
            // g_RenderVar0 of the rope shader's trail branch: x, w = most points of a ribbon, z = how far the head
            // piece is towards its next history sample
            const float most = static_cast<float>(r.trailSegments + 1);
            const float phase = r.instances.empty() ? 0.0f : r.instances[0]->trailPhase();
            req.uniforms.renderVar0 = {most, 0.0f, phase, most};
        } else {
            req.uniforms.renderVar0 = {r.trailLength, r.trailMax, r.trailMin, 0.0f};
        }
        const float w = static_cast<float>(r.texture->width());
        const float h = static_cast<float>(r.texture->height());
        if (r.sheetFrames > 0) {
            const float fw = 1.0f / static_cast<float>(r.sheetCols);
            const float fh = 1.0f / static_cast<float>(r.sheetRows);
            req.uniforms.renderVar1 = {fw, fh, static_cast<float>(r.sheetFrames), w > 0.0f ? (h * fh) / (w * fw) : 1.0f};
        } else {
            req.uniforms.renderVar1 = {0.0f, 0.0f, 0.0f, w > 0.0f ? h / w : 1.0f};
        }
        // Big soft systems (clouds, fog, light: dozens of sprites each covering much of the scene) are fill-rate bound, and
        // nothing in them is sharp. A translucent or additive system adds up the same in a transparent buffer (premultiplied
        // alpha for "over", plain sums for additive), so it is drawn into one at half size and the buffer is put on the scene
        // with the matching blend: the same picture for a quarter of the pixels.
        const double sceneArea = static_cast<double>(frame.orthoSize.x) * frame.orthoSize.y;
        const bool halfSize = frame.scratch && !r.rope && !r.ropeTrail && !r.refract && (r.def->flags & 4) == 0
            && (r.pass.blending == Blending::Translucent || r.pass.blending == Blending::Additive) && coveredArea >= kHalfSizeOverdraw * sceneArea;
        if (halfSize) {
            const VkExtent2D half{std::max(1u, frame.extent.width / 2), std::max(1u, frame.extent.height / 2)};
            RenderTarget &buffer = frame.scratch(half);
            transition(frame.cb, buffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
            req.target = buffer.view();
            req.targetFormat = buffer.format;
            req.extent = buffer.extent;
            req.clear = true;
            m_runner.draw(frame.cb, req);
            transition(frame.cb, buffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            std::string error;
            if (const TranslatedShader *composite = m_shaders.composite(&error)) {
                DrawRequest put;
                put.shader = composite;
                put.uniforms.inputs = frame.inputs;
                put.uniforms.mvp = Mat4::identity();
                put.uniforms.model = Mat4::identity();
                put.uniforms.viewProjection = Mat4::identity();
                put.uniforms.overrides["g_Tint"] = {1.0f, 1.0f, 1.0f, 1.0f};
                put.blending = r.pass.blending;
                put.blend = true;
                put.premultiplied = true;
                put.textures[0].view = buffer.view();
                put.textures[0].sampler = m_runner.linearClamp();
                put.textures[0].info = {buffer.extent.width, buffer.extent.height, buffer.extent.width, buffer.extent.height};
                put.uniforms.textures[0] = put.textures[0].info;
                put.quad = {-1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
                put.target = frame.target;
                put.targetFormat = frame.targetFormat;
                put.extent = frame.extent;
                put.clear = false;
                m_runner.draw(frame.cb, put);
            }
        } else {
            m_runner.draw(frame.cb, req);
        }
        drawn += written / 4;
    }
    return drawn;
}

} // namespace murasaki::scene
