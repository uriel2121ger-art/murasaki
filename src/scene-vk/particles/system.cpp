#include "particles/system.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace murasaki::scene {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

using Col = ParticleColumn;

// Linear value of a change over [t0, t1], held before and after (what alpha fades, size changes and colour changes use).
float fade(float t, float t0, float t1, float v0, float v1)
{
    if (t1 <= t0) {
        return t >= t1 ? v1 : v0;
    }
    const float x = std::clamp((t - t0) / (t1 - t0), 0.0f, 1.0f);
    return v0 + (v1 - v0) * x;
}

// Colours of particle files are written 0..255; a value that does not fit 0..1 is read that way.
Vec3 normalisedColor(Vec3 c)
{
    if (c.x > 1.0f || c.y > 1.0f || c.z > 1.0f) {
        return {c.x / 255.0f, c.y / 255.0f, c.z / 255.0f};
    }
    return c;
}

float hashNoise(int x, int y, int z)
{
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + static_cast<uint32_t>(z) * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / 8388607.5f - 1.0f; // -1..1
}

float smooth(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

// Value noise with smoothstep interpolation and its gradient, from the eight corners of the cell (the gradient is the
// derivative of the interpolation itself, so a curl needs no finite differences).
float valueNoiseGrad(Vec3 p, Vec3 *grad)
{
    const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy), iz = static_cast<int>(fz);
    const float rx = p.x - fx, ry = p.y - fy, rz = p.z - fz;
    const float tx = smooth(rx), ty = smooth(ry), tz = smooth(rz);
    const float dtx = 6.0f * rx * (1.0f - rx), dty = 6.0f * ry * (1.0f - ry), dtz = 6.0f * rz * (1.0f - rz);
    const float c000 = hashNoise(ix, iy, iz), c100 = hashNoise(ix + 1, iy, iz), c010 = hashNoise(ix, iy + 1, iz), c110 = hashNoise(ix + 1, iy + 1, iz);
    const float c001 = hashNoise(ix, iy, iz + 1), c101 = hashNoise(ix + 1, iy, iz + 1), c011 = hashNoise(ix, iy + 1, iz + 1),
                c111 = hashNoise(ix + 1, iy + 1, iz + 1);
    const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float x00 = lerp(c000, c100, tx), x10 = lerp(c010, c110, tx), x01 = lerp(c001, c101, tx), x11 = lerp(c011, c111, tx);
    const float y0 = lerp(x00, x10, ty), y1 = lerp(x01, x11, ty);
    if (grad) {
        const float dx = lerp(lerp(c100 - c000, c110 - c010, ty), lerp(c101 - c001, c111 - c011, ty), tz) * dtx;
        const float dy = lerp(x10 - x00, x11 - x01, tz) * dty;
        const float dz = (y1 - y0) * dtz;
        *grad = {dx, dy, dz};
    }
    return lerp(y0, y1, tz);
}

Vec3 normalised(Vec3 v, Vec3 fallback)
{
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return len > 1e-6f ? Vec3{v.x / len, v.y / len, v.z / len} : fallback;
}

float length(Vec3 v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

float dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Three octaves of value noise, about -1..1.
float fbmNoise(Vec3 p)
{
    return (valueNoiseGrad(p, nullptr) + 0.5f * valueNoiseGrad({p.x * 2.0f + 17.0f, p.y * 2.0f, p.z * 2.0f}, nullptr)
            + 0.25f * valueNoiseGrad({p.x * 4.0f, p.y * 4.0f + 31.0f, p.z * 4.0f}, nullptr)) / 1.75f;
}

// The lifetime fraction of a ramp from a to b: 0 before, 1 after (a step at a when b is not after it).
float ramp(float x, float a, float b)
{
    if (b <= a) {
        return x >= a ? 1.0f : 0.0f;
    }
    return std::clamp((x - a) / (b - a), 0.0f, 1.0f);
}

} // namespace

Vec3 hsvToRgb(float h, float s, float v)
{
    h = std::isfinite(h) ? h - std::floor(h) : 0.0f;
    const float k[3] = {5.0f, 3.0f, 1.0f};
    float out[3];
    for (int n = 0; n < 3; ++n) {
        const float x = std::fmod(k[n] + h * 6.0f, 6.0f);
        out[n] = v - v * s * std::clamp(std::min(x, 4.0f - x), 0.0f, 1.0f);
    }
    return {out[0], out[1], out[2]};
}

namespace {

} // namespace

Vec3 curlNoise(Vec3 p)
{
    // the potential is three independent noises (offset so they differ); the curl is built from their gradients
    Vec3 gx, gy, gz;
    valueNoiseGrad({p.x + 11.3f, p.y, p.z}, &gx);
    valueNoiseGrad({p.x, p.y + 47.9f, p.z}, &gy);
    valueNoiseGrad({p.x, p.y, p.z + 91.7f}, &gz);
    return {gz.y - gy.z, gx.z - gz.x, gy.x - gx.y};
}

bool particleEventChosen(uint32_t serial, uint32_t salt, float probability)
{
    if (!(probability < 1.0f)) {
        return true;
    }
    if (!(probability > 0.0f)) {
        return false;
    }
    uint32_t h = serial * 2654435761u ^ (salt + 0x9E3779B9u);
    h = (h ^ (h >> 15)) * 2246822519u;
    h = (h ^ (h >> 13)) * 3266489917u;
    h ^= h >> 16;
    return static_cast<float>(h >> 8) / 16777216.0f < probability;
}

uint32_t particleSeed(uint32_t salt)
{
    if (const char *env = std::getenv("MURASAKI_SCENE_VK_SEED"); env && *env) {
        return static_cast<uint32_t>(std::strtoul(env, nullptr, 10)) + salt * 7919u;
    }
    return std::random_device{}();
}

ParticleOverrides sanitizeOverrides(ParticleOverrides o)
{
    const auto fix = [](float v, float hi) { return std::isfinite(v) ? std::clamp(v, 0.0f, hi) : 1.0f; };
    o.alpha = fix(o.alpha, 1.0e4f);
    o.size = fix(o.size, 1.0e4f);
    o.lifetime = fix(o.lifetime, 1.0e4f);
    o.rate = fix(o.rate, 1.0e4f);
    o.speed = fix(o.speed, 1.0e4f);
    o.count = fix(o.count, 1.0e4f);
    o.color = {fix(o.color.x, 1.0e3f), fix(o.color.y, 1.0e3f), fix(o.color.z, 1.0e3f)};
    return o;
}

ParticleSystem::ParticleSystem(const ParticleDefinition &definition, const ParticleOverrides &overrides, uint32_t seed, const PropertyStore *props)
    : m_def(definition), m_overrides(sanitizeOverrides(overrides)), m_props(props), m_rng(seed)
{
    const double scale = std::isfinite(overrides.count) ? static_cast<double>(std::max(0.0f, overrides.count)) : 1.0;
    const double wanted = static_cast<double>(definition.maxCount) * scale;
    m_capacity = static_cast<uint32_t>(std::clamp(std::isfinite(wanted) ? wanted : 0.0, 0.0, static_cast<double>(kGlobalCap)));
    if (m_capacity == 0 && definition.maxCount == 0) {
        m_capacity = 100; // a file without maxcount: a small default pool
    }
    for (auto &c : m_columns) {
        c.reserve(std::min<uint32_t>(m_capacity, 4096));
    }
    for (const ParticleComponent &e : m_def.emitters) {
        EmitterState s;
        s.delayLeft = e.number("delay", 0.0f, m_props);
        m_emitterStates.push_back(s);
    }
    for (const ParticleComponent &c : m_def.initializers) {
        static const std::map<std::string, InitKind> kinds = {
            {"colorrandom", InitKind::Color},         {"hsvcolorrandom", InitKind::HsvColor},   {"sizerandom", InitKind::Size},
            {"alpharandom", InitKind::Alpha},         {"lifetimerandom", InitKind::Lifetime},
            {"velocityrandom", InitKind::Velocity},   {"rotationrandom", InitKind::Rotation},
            {"angularvelocityrandom", InitKind::AngularVelocity}, {"turbulentvelocityrandom", InitKind::TurbulentVelocity},
            {"mapsequencearoundcontrolpoint", InitKind::MapSequenceAround}, {"mapsequencebetweencontrolpoints", InitKind::MapSequenceBetween},
            {"colorlist", InitKind::ColorList}, {"inheritcontrolpointvelocity", InitKind::InheritControlPointVelocity},
            {"positionoffsetrandom", InitKind::PositionOffsetRandom}, {"remapinitialvalue", InitKind::RemapInitialValue},
            {"inheritinitialvaluefromevent", InitKind::InheritInitialValueFromEvent}};
        if (const auto it = kinds.find(c.name); it != kinds.end()) {
            int mode = 0;
            const std::string behaviour = c.text("limitbehavior");
            if (behaviour == "wrap" || behaviour == "loop") {
                mode = 1;
            } else if (behaviour == "mirror") {
                mode = 2;
            }
            Initializer init{it->second, &c, mode};
            if (init.kind == InitKind::RemapInitialValue) {
                init.remap = readRemap(c, m_props);
                m_usesTimeOfDay = m_usesTimeOfDay || init.remap.input == ParticleRemapValue::TimeOfDay;
            } else if (init.kind == InitKind::InheritInitialValueFromEvent) {
                init.event = particleEventValueOf(c);
            }
            m_initializers.push_back(init);
        }
    }
    // operators that set size, alpha or colour again every step from the particle's initial value: a multiply or an add after them
    // works on this step's value, before them (or without them) on the initial one
    bool sizeSet = false, alphaSet = false, colorSet = false;
    for (const ParticleComponent &c : m_def.operators) {
        static const std::map<std::string, OpKind> kinds = {
            {"movement", OpKind::Movement},
            {"angularmovement", OpKind::AngularMovement},
            {"alphafade", OpKind::AlphaFade},
            {"sizechange", OpKind::SizeChange},
            {"alphachange", OpKind::AlphaChange},
            {"colorchange", OpKind::ColorChange},
            {"turbulence", OpKind::Turbulence},
            {"controlpointattract", OpKind::ControlPointAttract},
            {"vortex", OpKind::Vortex},
            {"oscillatealpha", OpKind::OscillateAlpha},
            {"oscillatesize", OpKind::OscillateSize},
            {"oscillateposition", OpKind::OscillatePosition},
            {"remapvalue", OpKind::RemapValue},
            {"boids", OpKind::Boids},
            {"vortex_v2", OpKind::VortexV2},
            {"capvelocity", OpKind::CapVelocity},
            {"maintaindistancetocontrolpoint", OpKind::MaintainDistance},
            {"maintaindistancebetweencontrolpoints", OpKind::MaintainBetween},
            {"reducemovementnearcontrolpoint", OpKind::ReduceMovement},
            {"collisionplane", OpKind::CollisionPlane},
            {"collisionsphere", OpKind::CollisionSphere},
            {"collisionbox", OpKind::CollisionBox},
            {"collisionquad", OpKind::CollisionQuad},
            {"collisionbounds", OpKind::CollisionBounds},
            {"inheritvaluefromevent", OpKind::InheritValueFromEvent}};
        if (const auto it = kinds.find(c.name); it != kinds.end()) {
            Operator op{it->second, &c};
            if (op.kind == OpKind::Turbulence) {
                // the phase and the speed are drawn once per operator, not per particle
                op.phase = uniform(c.number("phasemin", 0.0f, m_props), c.number("phasemax", 0.0f, m_props));
                op.speed = uniform(c.number("speedmin", 500.0f, m_props), c.number("speedmax", 1000.0f, m_props));
            }
            op.blend.in = c.has("blendinstart") || c.has("blendinend");
            op.blend.out = c.has("blendoutstart") || c.has("blendoutend");
            op.blend.inStart = c.number("blendinstart", 0.0f, m_props);
            op.blend.inEnd = c.number("blendinend", op.blend.inStart, m_props);
            op.blend.outStart = c.number("blendoutstart", 1.0f, m_props);
            op.blend.outEnd = c.number("blendoutend", 1.0f, m_props);
            op.behavior = particleCollisionBehavior(c.text("collisionbehavior")).value_or(ParticleCollisionBehavior::Bounce);
            op.colorRebuilt = colorSet;
            op.alphaRebuilt = alphaSet;
            op.sizeRebuilt = sizeSet;
            if (op.kind == OpKind::RemapValue) {
                op.remap = readRemap(c, m_props);
                m_usesTimeOfDay = m_usesTimeOfDay || op.remap.input == ParticleRemapValue::TimeOfDay;
                const ParticleRemapValue out = op.remap.output;
                op.remap.rebuilt = out == ParticleRemapValue::Size ? sizeSet : out == ParticleRemapValue::Opacity ? alphaSet : out == ParticleRemapValue::Color && colorSet;
            } else if (op.kind == OpKind::InheritValueFromEvent) {
                op.event = particleEventValueOf(c);
            }
            sizeSet = sizeSet || op.kind == OpKind::SizeChange || op.kind == OpKind::OscillateSize;
            alphaSet = alphaSet || op.kind == OpKind::AlphaFade || op.kind == OpKind::AlphaChange || op.kind == OpKind::OscillateAlpha;
            colorSet = colorSet || op.kind == OpKind::ColorChange;
            m_operators.push_back(op);
        }
    }
    m_cpTrackedAt.fill(-1.0);
    for (const ParticleControlPoint &cp : m_def.controlPoints) {
        const size_t i = static_cast<size_t>(cp.id);
        m_cpOffset[i] = overrides.controlPoints[i].value_or(cp.offset);
        m_pointerLinked[i] = (cp.flags & 1) != 0;
        m_sceneSpace[i] = (cp.flags & 2) != 0;
        m_controlPoints[i] = cp.offset; // scene-space offsets are fixed up by updateControlPoints
    }
}

float ParticleSystem::uniform(float lo, float hi)
{
    // ranges come from files and user properties: a non-finite end reads as 0 and the span is kept finite, which
    // std::uniform_real_distribution requires
    constexpr float kLimit = 1.0e18f;
    lo = std::isfinite(lo) ? std::clamp(lo, -kLimit, kLimit) : 0.0f;
    hi = std::isfinite(hi) ? std::clamp(hi, -kLimit, kLimit) : 0.0f;
    if (hi <= lo) {
        return lo;
    }
    return std::uniform_real_distribution<float>(lo, hi)(m_rng);
}

Vec3 ParticleSystem::uniform3(Vec3 lo, Vec3 hi)
{
    return {uniform(lo.x, hi.x), uniform(lo.y, hi.y), uniform(lo.z, hi.z)};
}

bool ParticleSystem::active() const
{
    if (m_count > 0) {
        return true;
    }
    for (size_t i = 0; i < m_def.emitters.size(); ++i) {
        const ParticleComponent &e = m_def.emitters[i];
        const double duration = e.number("duration", 0.0f, m_props);
        if (duration <= 0.0 || m_emitterStates[i].elapsed < duration) {
            return true;
        }
    }
    return false;
}

void ParticleSystem::updateControlPoints(Vec2 pointerScene, bool pointerKnown, const Mat4 &sceneToSystem)
{
    const auto transform = [&](Vec3 p) {
        return Vec3{sceneToSystem.at(0, 0) * p.x + sceneToSystem.at(0, 1) * p.y + sceneToSystem.at(0, 2) * p.z + sceneToSystem.at(0, 3),
                    sceneToSystem.at(1, 0) * p.x + sceneToSystem.at(1, 1) * p.y + sceneToSystem.at(1, 2) * p.z + sceneToSystem.at(1, 3),
                    sceneToSystem.at(2, 0) * p.x + sceneToSystem.at(2, 1) * p.y + sceneToSystem.at(2, 2) * p.z + sceneToSystem.at(2, 3)};
    };
    for (size_t i = 0; i < kControlPoints; ++i) {
        if (m_pointerLinked[i]) {
            if (!pointerKnown) {
                m_cpTrackedAt[i] = -1.0; // where the pointer is is not known: its first known place gives no velocity
                m_cpVelocity[i] = {};
                continue;
            }
            const Vec3 p = transform({pointerScene.x + m_cpOffset[i].x, pointerScene.y + m_cpOffset[i].y, 0.0f});
            m_controlPoints[i] = p;
        } else if (m_sceneSpace[i]) {
            m_controlPoints[i] = transform(m_cpOffset[i]);
        } else {
            m_controlPoints[i] = m_cpOffset[i];
        }
        trackControlPoint(i);
    }
}

// A control point's velocity: how far it moved since the last update, over the simulated time in between (two updates in the same
// instant keep the velocity and take the newer place).
void ParticleSystem::trackControlPoint(size_t index)
{
    const double now = m_time + m_pending;
    const Vec3 &p = m_controlPoints[index];
    if (m_cpTrackedAt[index] >= 0.0 && now > m_cpTrackedAt[index] + 1e-9) {
        const float span = static_cast<float>(now - m_cpTrackedAt[index]);
        const Vec3 &was = m_cpTracked[index];
        Vec3 v{(p.x - was.x) / span, (p.y - was.y) / span, (p.z - was.z) / span};
        const auto fix = [](float f) { return std::isfinite(f) ? std::clamp(f, -1.0e6f, 1.0e6f) : 0.0f; };
        m_cpVelocity[index] = {fix(v.x), fix(v.y), fix(v.z)};
    }
    m_cpTracked[index] = p;
    m_cpTrackedAt[index] = now;
}

ParticleEventValues ParticleSystem::eventValues(size_t i) const
{
    const auto col = [&](ParticleColumn c) { return m_columns[static_cast<size_t>(c)][i]; };
    ParticleEventValues e;
    e.position = {col(Col::X), col(Col::Y), col(Col::Z)};
    e.velocity = {col(Col::VX), col(Col::VY), col(Col::VZ)};
    e.color = {col(Col::R), col(Col::G), col(Col::B)};
    e.alpha = col(Col::Alpha);
    e.size = col(Col::Size);
    e.rotation = {col(Col::RotX), col(Col::RotY), col(Col::RotZ)};
    e.angularVelocity = {col(Col::AngX), col(Col::AngY), col(Col::AngZ)};
    return e;
}

void ParticleSystem::warmUp()
{
    // At most two minutes, in coarser steps than the live ones: the result only has to look like a running system.
    // The work is bounded as well: a dense system stops warming up once it has simulated kWarmUpBudget particle-steps.
    const double total = std::min<double>(m_def.startTime, 120.0);
    const float coarse = 1.0f / 30.0f;
    double work = 0.0;
    for (double t = 0.0; t < total && work < kWarmUpBudget; t += coarse) {
        step(coarse);
        work += static_cast<double>(m_count) + 1.0;
    }
}

void ParticleSystem::advance(double dt)
{
    m_pending += std::clamp(dt, 0.0, static_cast<double>(kMaxCatchUp));
    // (a tiny tolerance: 60 calls of 1/60 s are 60 steps, not 59 because of rounding)
    while (m_pending + 1e-7 >= kStep) {
        step(kStep);
        m_pending -= kStep;
    }
    m_pending = std::max(m_pending, 0.0);
}

uint64_t g_particleSteps = 0;
void ParticleSystem::step(float dt)
{
    g_particleSteps += m_count;
    m_time += dt;
    if (m_usesTimeOfDay) {
        // the local day, read once a step (remap inputs `timeofday`)
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        m_dayFraction = static_cast<float>(local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec) / 86400.0f;
    }
    for (size_t i = 0; i < m_emitterStates.size(); ++i) {
        emit(i, dt);
    }
    for (size_t i = 0; i < m_count; ++i) {
        at(Col::Age, i) += dt;
    }
    runOperators(dt);
    if (m_trailPoints > 0) {
        m_trailClock += dt;
        if (m_trailClock >= m_trailInterval) {
            m_trailClock -= m_trailInterval;
            const size_t stride = static_cast<size_t>(m_trailPoints) * 8;
            for (size_t i = 0; i < m_count; ++i) {
                float *h = &m_trailData[i * stride];
                std::move_backward(h, h + stride - 8, h + stride); // older states move one place back
                h[0] = at(Col::X, i);
                h[1] = at(Col::Y, i);
                h[2] = at(Col::Z, i);
                h[3] = at(Col::Size, i);
                h[4] = at(Col::R, i);
                h[5] = at(Col::G, i);
                h[6] = at(Col::B, i);
                h[7] = at(Col::Alpha, i);
                m_trailCounts[i] = static_cast<uint8_t>(std::min<int>(m_trailCounts[i] + 1, m_trailPoints));
            }
        }
    }
    if (m_sheetFrames > 0) {
        const float anim = m_def.sequenceMultiplier > 0.0f ? m_def.sequenceMultiplier : 1.0f;
        for (size_t i = 0; i < m_count; ++i) {
            const float life = at(Col::Lifetime, i) > 0.0f ? at(Col::Age, i) / at(Col::Lifetime, i) : 1.0f;
            const float frames = static_cast<float>(m_sheetFrames);
            if (m_def.animationMode == "randomframe") {
                if (at(Col::Frame, i) < 0.0f) {
                    at(Col::Frame, i) = std::floor(uniform(0.0f, frames - 0.001f));
                }
            } else if (m_sheetDuration > 0.0f) {
                const float cycle = std::fmod(at(Col::Age, i) * anim, m_sheetDuration) / m_sheetDuration;
                at(Col::Frame, i) = std::fmod(cycle * frames, frames);
            } else {
                at(Col::Frame, i) = std::fmod(life * frames * anim, frames);
            }
        }
    }
    compact();
}

void ParticleSystem::compact()
{
    size_t write = 0;
    for (size_t read = 0; read < m_count; ++read) {
        if (at(Col::Age, read) < at(Col::Lifetime, read)) {
            if (write != read) {
                for (auto &c : m_columns) {
                    c[write] = c[read];
                }
                m_serials[write] = m_serials[read];
                if (m_trailPoints > 0) {
                    std::copy_n(m_trailData.begin() + static_cast<std::ptrdiff_t>(read * static_cast<size_t>(m_trailPoints) * 8), static_cast<size_t>(m_trailPoints) * 8,
                                m_trailData.begin() + static_cast<std::ptrdiff_t>(write * static_cast<size_t>(m_trailPoints) * 8));
                    m_trailCounts[write] = m_trailCounts[read];
                }
            }
            ++write;
        }
    }
    for (auto &c : m_columns) {
        c.resize(write);
    }
    m_serials.resize(write);
    if (m_trailPoints > 0) {
        m_trailData.resize(write * static_cast<size_t>(m_trailPoints) * 8);
        m_trailCounts.resize(write);
    }
    m_count = write;
}

// Emission. Rate: `rate` particles a second times the instance's rate multiplier, kept fractional between steps.
// `instantaneous`: that many at once, the first time. `delay`: seconds before the emitter starts; `duration`: seconds
// it runs for (0: forever). Flag 2 caps a step at one particle; flag 4 emits in random bursts (`minperiodic*`).
// The factor an audio-reactive emitter's rate (or a vortex's or turbulence's strength) is multiplied by (1 for mode 0): the spectrum level between the bounds.
float ParticleSystem::audioResponse(const ParticleComponent &e) const
{
    const int mode = e.integer("audioprocessingmode", 0);
    if (mode <= 0) {
        return 1.0f;
    }
    int first = std::clamp(e.integer("audioprocessingfrequencystart", 0), 0, 15);
    int last = std::clamp(e.integer("audioprocessingfrequencyend", 1), 0, 15);
    if (last < first) {
        std::swap(first, last);
    }
    float left = 0.0f, right = 0.0f;
    for (int band = first; band <= last; ++band) {
        left += m_audioLeft[static_cast<size_t>(band)];
        right += m_audioRight[static_cast<size_t>(band)];
    }
    const float bands = static_cast<float>(last - first + 1);
    left /= bands;
    right /= bands;
    float level = mode == 1 ? left : mode == 2 ? right : 0.5f * (left + right);
    level = std::isfinite(level) ? std::clamp(level, 0.0f, 1.0f) : 0.0f;
    const Vec3 bounds = e.vector("audioprocessingbounds", {0.8f, 1.0f, 1.0f}, m_props);
    const float exponent = std::clamp(e.number("audioprocessingexponent", 2.0f, m_props), 0.0f, 100.0f);
    const float response = bounds.x + (bounds.y - bounds.x) * std::pow(level, exponent);
    return std::isfinite(response) ? std::max(0.0f, response) : 1.0f;
}

void ParticleSystem::emit(size_t index, float dt)
{
    const ParticleComponent &e = m_def.emitters[index];
    EmitterState &s = m_emitterStates[index];
    if (!m_emitting || !particleEmitterSupported(e.name)) {
        return;
    }
    if (s.delayLeft > 0.0) {
        s.delayLeft -= dt;
        return;
    }
    const double duration = e.number("duration", 0.0f, m_props);
    s.elapsed += dt;
    if (duration > 0.0 && s.elapsed >= duration) {
        return;
    }
    const int flags = e.integer("flags", 0);
    if ((flags & 4) != 0) {
        s.periodTimer += dt;
        if (!s.emitting) {
            if (s.periodTimer < s.periodLength) {
                return;
            }
            s.emitting = true;
            s.periodTimer = 0.0;
            s.periodLength = uniform(e.number("minperiodicduration", 2.0f, m_props), e.number("maxperiodicduration", 3.0f, m_props));
        } else if (s.periodTimer >= s.periodLength) {
            s.emitting = false;
            s.periodTimer = 0.0;
            s.periodLength = uniform(e.number("minperiodicdelay", 1.0f, m_props), e.number("maxperiodicdelay", 2.0f, m_props));
            return;
        }
    }
    uint32_t toEmit = 0;
    const int instant = e.integer("instantaneous", 0);
    if (instant > 0 && !s.instantDone) {
        toEmit = static_cast<uint32_t>(std::min<int>(instant, static_cast<int>(kGlobalCap)));
        s.instantDone = true;
    }
    // (at capacity the clocks above still run; the emitter only spawns nothing, and owes nothing afterwards)
    const float rateMultiplier = std::isfinite(m_overrides.rate) ? std::max(0.0f, m_overrides.rate) : 1.0f;
    const float rate = std::min(e.number("rate", 10.0f, m_props) * rateMultiplier * audioResponse(e), 1.0e7f);
    if (e.number("rate", 10.0f, m_props) > 0.0f && !(instant > 0 && e.values.count("rate") == 0)) {
        s.timer += static_cast<double>(dt) * rate;
        s.timer = std::min(s.timer, static_cast<double>(kGlobalCap) + 1.0);
        uint32_t byRate = static_cast<uint32_t>(s.timer);
        s.timer -= byRate;
        if ((flags & 2) != 0 && byRate > 1) {
            byRate = 1;
        }
        toEmit += byRate;
    }
    for (uint32_t i = 0; i < toEmit && m_count < m_capacity; ++i) {
        spawn(e);
    }
}

void ParticleSystem::burst(size_t count)
{
    if (m_def.emitters.empty() || !particleEmitterSupported(m_def.emitters[0].name)) {
        return;
    }
    for (size_t i = 0; i < count && m_count < kGlobalCap; ++i) {
        spawn(m_def.emitters[0]);
    }
}

void ParticleSystem::spawn(const ParticleComponent &e)
{
    const Vec3 origin = e.vector("origin", {0, 0, 0}, m_props);
    const int cpIndex = e.integer("controlpoint", 0);
    Vec3 base = origin;
    if (cpIndex >= 0 && cpIndex < kControlPoints) {
        const Vec3 &cp = m_controlPoints[static_cast<size_t>(cpIndex)];
        base = {base.x + cp.x, base.y + cp.y, base.z + cp.z};
    }
    const Vec3 directions = e.vector("directions", {1, 1, 0}, m_props);
    const Vec3 dmin = e.vector("distancemin", {0, 0, 0}, m_props);
    const Vec3 dmax = e.vector("distancemax", {256, 256, 0}, m_props);

    Vec3 offset{};
    if (e.name == "boxrandom") {
        // A box centred on the origin: each axis a distance in [min, max] with a random sign, scaled by `directions`.
        const float lo[3] = {dmin.x, dmin.y, dmin.z};
        const float hi[3] = {dmax.x, dmax.y, dmax.z};
        float out[3];
        for (int a = 0; a < 3; ++a) {
            float d = uniform(lo[a], hi[a]);
            if (uniform(0.0f, 1.0f) < 0.5f) {
                d = -d;
            }
            out[a] = d;
        }
        offset = {out[0] * directions.x, out[1] * directions.y, out[2] * directions.z};
    } else {
        // sphererandom: flat systems (flag 4 off) fill a ring in x/y, perspective ones a shell, then `sign` forces the
        // side of each axis (1 positive, -1 negative, 0 both).
        const float rmin = dmin.x;
        const float rmax = dmax.x;
        if ((m_def.flags & 4) == 0) {
            const float angle = uniform(0.0f, kTwoPi);
            const float r = std::sqrt(uniform(rmin * rmin, rmax * rmax));
            offset = {r * std::cos(angle) * directions.x, r * std::sin(angle) * directions.y, uniform(-rmax, rmax) * directions.z};
        } else {
            const float theta = uniform(0.0f, kTwoPi);
            const float cosPhi = uniform(-1.0f, 1.0f);
            const float sinPhi = std::sqrt(std::max(0.0f, 1.0f - cosPhi * cosPhi));
            const float r = std::cbrt(uniform(rmin * rmin * rmin, rmax * rmax * rmax));
            offset = {sinPhi * std::cos(theta) * r * directions.x, sinPhi * std::sin(theta) * r * directions.y, cosPhi * r * directions.z};
        }
        const Vec3 sign = e.vector("sign", {0, 0, 0}, m_props);
        const float sg[3] = {sign.x, sign.y, sign.z};
        float o[3] = {offset.x, offset.y, offset.z};
        for (int a = 0; a < 3; ++a) {
            if (sg[a] > 0.5f) {
                o[a] = std::fabs(o[a]);
            } else if (sg[a] < -0.5f) {
                o[a] = -std::fabs(o[a]);
            }
        }
        offset = {o[0], o[1], o[2]};
    }

    for (auto &c : m_columns) {
        c.push_back(0.0f);
    }
    const size_t i = m_count++;
    m_serials.push_back(m_nextSerial++);
    if (m_trailPoints > 0) {
        m_trailData.resize(m_count * static_cast<size_t>(m_trailPoints) * 8, 0.0f);
        m_trailCounts.push_back(0);
    }
    at(Col::X, i) = base.x + offset.x;
    at(Col::Y, i) = base.y + offset.y;
    at(Col::Z, i) = base.z + offset.z;

    // the emitter's own speed: outward from the origin
    const float speedMin = e.number("speedmin", 0.0f, m_props);
    const float speedMax = e.number("speedmax", 0.0f, m_props);
    if (e.name == "sphererandom" && (speedMax > 0.0f || speedMin != 0.0f)) {
        const Vec3 dir = normalised(offset, {0, 1, 0});
        const float speed = uniform(speedMin, speedMax);
        at(Col::VX, i) = dir.x * speed;
        at(Col::VY, i) = dir.y * speed;
        at(Col::VZ, i) = dir.z * speed;
    }

    // the defaults an initializer may replace
    at(Col::R, i) = m_overrides.color.x;
    at(Col::G, i) = m_overrides.color.y;
    at(Col::B, i) = m_overrides.color.z;
    at(Col::Alpha, i) = m_overrides.alpha;
    at(Col::Size, i) = 20.0f * m_overrides.size;
    at(Col::Lifetime, i) = 1.0f * m_overrides.lifetime;
    at(Col::Frame, i) = -1.0f;
    at(Col::InitialR, i) = at(Col::R, i);
    at(Col::InitialG, i) = at(Col::G, i);
    at(Col::InitialB, i) = at(Col::B, i);
    at(Col::InitialAlpha, i) = at(Col::Alpha, i);
    at(Col::InitialSize, i) = at(Col::Size, i);

    for (const Initializer &init : m_initializers) {
        const ParticleComponent &c = *init.c;
        const auto exponent = [&] { return std::max(0.0f, c.number("exponent", 1.0f, m_props)); };
        switch (init.kind) {
        case InitKind::Color: {
            const Vec3 lo = normalisedColor(c.vector("min", {0, 0, 0}, m_props));
            const Vec3 hi = normalisedColor(c.vector("max", {1, 1, 1}, m_props));
            const Vec3 v = uniform3(lo, hi);
            at(Col::R, i) = v.x * m_overrides.color.x;
            at(Col::G, i) = v.y * m_overrides.color.y;
            at(Col::B, i) = v.z * m_overrides.color.z;
            at(Col::InitialR, i) = at(Col::R, i);
            at(Col::InitialG, i) = at(Col::G, i);
            at(Col::InitialB, i) = at(Col::B, i);
            break;
        }
        case InitKind::HsvColor: {
            // a colour drawn in HSV: hue, saturation and value each between their min and max (0..1; a hue range may run past 1
            // and wraps), so a range of hues keeps the colours as saturated and bright as asked
            const float hueMin = c.number("huemin", 0.0f, m_props), hueMax = c.number("huemax", 1.0f, m_props);
            // `huesteps`: only that many hues, evenly spread over the range
            const int steps = std::clamp(c.integer("huesteps", 0), 0, 360);
            const float h = steps > 0 ? hueMin + (hueMax - hueMin) * std::floor(uniform(0.0f, static_cast<float>(steps) - 1e-3f)) / static_cast<float>(steps)
                                      : uniform(hueMin, hueMax);
            const float sat = std::clamp(uniform(c.number("saturationmin", 1.0f, m_props), c.number("saturationmax", 1.0f, m_props)), 0.0f, 1.0f);
            const float val = std::clamp(uniform(c.number("valuemin", 1.0f, m_props), c.number("valuemax", 1.0f, m_props)), 0.0f, 1.0f);
            const Vec3 v = hsvToRgb(h, sat, val);
            at(Col::R, i) = v.x * m_overrides.color.x;
            at(Col::G, i) = v.y * m_overrides.color.y;
            at(Col::B, i) = v.z * m_overrides.color.z;
            at(Col::InitialR, i) = at(Col::R, i);
            at(Col::InitialG, i) = at(Col::G, i);
            at(Col::InitialB, i) = at(Col::B, i);
            break;
        }
        case InitKind::Size: {
            // `size` is the particle's full width; the files' numbers read as twice that (checked against Wallpaper Engine)
            const float lo = c.number("min", 0.0f, m_props);
            const float hi = c.number("max", 20.0f, m_props);
            at(Col::Size, i) = (lo + std::pow(uniform(0.0f, 1.0f), exponent()) * (hi - lo)) * m_overrides.size / 2.0f;
            at(Col::InitialSize, i) = at(Col::Size, i);
            break;
        }
        case InitKind::Alpha: {
            const float lo = c.number("min", 0.05f, m_props);
            const float hi = c.number("max", 1.0f, m_props);
            at(Col::Alpha, i) = (lo + std::pow(uniform(0.0f, 1.0f), exponent()) * (hi - lo)) * m_overrides.alpha;
            at(Col::InitialAlpha, i) = at(Col::Alpha, i);
            break;
        }
        case InitKind::Lifetime: {
            const float lo = c.number("min", 0.0f, m_props);
            const float hi = c.number("max", 1.0f, m_props);
            at(Col::Lifetime, i) = (lo + std::pow(uniform(0.0f, 1.0f), exponent()) * (hi - lo)) * m_overrides.lifetime;
            break;
        }
        case InitKind::Velocity: {
            const Vec3 v = uniform3(c.vector("min", {-32, -32, -32}, m_props), c.vector("max", {32, 32, 32}, m_props));
            at(Col::VX, i) += v.x * m_overrides.speed;
            at(Col::VY, i) += v.y * m_overrides.speed;
            at(Col::VZ, i) += v.z * m_overrides.speed;
            break;
        }
        case InitKind::Rotation: {
            const Vec3 v = uniform3(c.vector("min", {0, 0, 0}, m_props), c.vector("max", {0, 0, kTwoPi}, m_props));
            at(Col::RotX, i) = v.x;
            at(Col::RotY, i) = v.y;
            at(Col::RotZ, i) = v.z;
            break;
        }
        case InitKind::AngularVelocity: {
            const Vec3 lo = c.vector("min", {0, 0, -5}, m_props);
            const Vec3 hi = c.vector("max", {0, 0, 5}, m_props);
            const float ex = exponent();
            const float lov[3] = {lo.x, lo.y, lo.z};
            const float hiv[3] = {hi.x, hi.y, hi.z};
            float out[3];
            for (int a = 0; a < 3; ++a) {
                out[a] = lov[a] + std::pow(uniform(0.0f, 1.0f), ex) * (hiv[a] - lov[a]);
            }
            at(Col::AngX, i) = out[0] * m_overrides.speed;
            at(Col::AngY, i) = out[1] * m_overrides.speed;
            at(Col::AngZ, i) = out[2] * m_overrides.speed;
            break;
        }
        case InitKind::MapSequenceBetween: {
            // Particles take turns along the line between two control points (0 and 1 unless named), `count` places
            // apart; `limitbehavior` says what follows the last place: "mirror" goes back, "wrap" starts over, else
            // the last place is kept. The emitter's own offset stays, as the jitter of the line.
            const int count = std::max(1, c.integer("count", 1));
            const int a = std::clamp(c.integer("controlpointstart", 0), 0, kControlPoints - 1);
            const int b = std::clamp(c.integer("controlpointend", 1), 0, kControlPoints - 1);
            const uint32_t serial = m_serials.back();
            const int span = std::max(1, count - 1);
            int place = 0;
            if (init.mode == 2) {
                const int period = 2 * span;
                const int k = static_cast<int>(serial % static_cast<uint32_t>(period));
                place = k <= span ? k : period - k;
            } else if (init.mode == 1) {
                place = static_cast<int>(serial % static_cast<uint32_t>(count));
            } else {
                place = static_cast<int>(std::min<uint32_t>(serial, static_cast<uint32_t>(count - 1)));
            }
            const float t = count > 1 ? static_cast<float>(place) / static_cast<float>(span) : 0.0f;
            const Vec3 &pa = m_controlPoints[static_cast<size_t>(a)];
            const Vec3 &pb = m_controlPoints[static_cast<size_t>(b)];
            const Vec3 &p0 = m_controlPoints[static_cast<size_t>(std::clamp(e.integer("controlpoint", 0), 0, kControlPoints - 1))];
            at(Col::X, i) += pa.x + (pb.x - pa.x) * t - p0.x;
            at(Col::Y, i) += pa.y + (pb.y - pa.y) * t - p0.y;
            at(Col::Z, i) += pa.z + (pb.z - pa.z) * t - p0.z;
            break;
        }
        case InitKind::MapSequenceAround: {
            // Particles leave a control point in `count` evenly spread directions (the velocity range turned by the
            // particle's place around the circle).
            const int count = std::max(1, c.integer("count", 1));
            const int cp = std::clamp(c.integer("controlpoint", 0), 0, kControlPoints - 1);
            const float angle = static_cast<float>(m_serials.back() % static_cast<uint32_t>(count)) / static_cast<float>(count) * kTwoPi;
            const Vec3 center = m_controlPoints[static_cast<size_t>(cp)];
            at(Col::X, i) = center.x;
            at(Col::Y, i) = center.y;
            at(Col::Z, i) = center.z;
            const Vec3 speed = uniform3(c.vector("speedmin", {0, 0, 0}, m_props), c.vector("speedmax", {100, 100, 100}, m_props));
            const float cs = std::cos(angle), sn = std::sin(angle);
            at(Col::VX, i) = (speed.x * cs - speed.y * sn) * m_overrides.speed;
            at(Col::VY, i) = (speed.x * sn + speed.y * cs) * m_overrides.speed;
            at(Col::VZ, i) = speed.z * m_overrides.speed;
            break;
        }
        case InitKind::TurbulentVelocity: {
            // A direction from curl noise sampled where the particle is and when it was born, kept within
            // `scale` (0..2, fraction of a half turn) of `forward` and tilted by `offset` around `right`.
            const Vec3 forward = normalised(c.vector("forward", {0, 1, 0}, m_props), {0, 1, 0});
            const float speed = uniform(c.number("speedmin", 100.0f, m_props), c.number("speedmax", 250.0f, m_props));
            const float scale = c.number("scale", 1.0f, m_props);
            const float tilt = c.number("offset", 0.0f, m_props);
            const float timeScale = c.number("timescale", 1.0f, m_props);
            const float phase = uniform(c.number("phasemin", 0.0f, m_props), c.number("phasemax", 0.1f, m_props));
            const Vec3 p{at(Col::X, i) * 0.1f + static_cast<float>(m_time) * timeScale + phase,
                         at(Col::Y, i) * 0.1f + static_cast<float>(m_time) * timeScale + phase * 0.7f,
                         at(Col::Z, i) * 0.1f + static_cast<float>(m_time) * timeScale + phase * 1.3f};
            Vec3 dir = normalised(curlNoise(p), forward);
            if (scale < 2.0f) {
                // clamp the angle to forward: blend towards forward when the angle exceeds the allowed fraction
                const float cosAngle = std::clamp(dir.x * forward.x + dir.y * forward.y + dir.z * forward.z, -1.0f, 1.0f);
                const float angle = std::acos(cosAngle) / kPi;
                const float maxAngle = scale / 2.0f;
                if (angle > maxAngle && angle > 1e-5f) {
                    const float keep = maxAngle / angle;
                    dir = normalised({forward.x + (dir.x - forward.x) * keep, forward.y + (dir.y - forward.y) * keep,
                                      forward.z + (dir.z - forward.z) * keep},
                                     forward);
                }
            }
            if (std::fabs(tilt) > 1e-4f) {
                // rotate around `right` (x by default... the z axis in the files): in flat systems that is a turn in x/y
                const float cs = std::cos(tilt), sn = std::sin(tilt);
                dir = {dir.x * cs - dir.y * sn, dir.x * sn + dir.y * cs, dir.z};
            }
            if ((m_def.flags & 4) == 0) {
                dir.z = 0.0f;
                dir = normalised(dir, forward);
            }
            at(Col::VX, i) += dir.x * speed * m_overrides.speed;
            at(Col::VY, i) += dir.y * speed * m_overrides.speed;
            at(Col::VZ, i) += dir.z * speed * m_overrides.speed;
            break;
        }
        case InitKind::ColorList: {
            // one of `colors` (0..1, or 0..255 when a value is above 1), each as likely, times the instance colour
            const auto it = c.lists.find("colors");
            if (it == c.lists.end() || it->second.empty()) {
                break;
            }
            const auto &colors = it->second;
            const size_t k = std::min(colors.size() - 1, static_cast<size_t>(uniform(0.0f, static_cast<float>(colors.size()))));
            const Vec3 v = normalisedColor(colors[k]);
            at(Col::R, i) = v.x * m_overrides.color.x;
            at(Col::G, i) = v.y * m_overrides.color.y;
            at(Col::B, i) = v.z * m_overrides.color.z;
            at(Col::InitialR, i) = at(Col::R, i);
            at(Col::InitialG, i) = at(Col::G, i);
            at(Col::InitialB, i) = at(Col::B, i);
            break;
        }
        case InitKind::InheritControlPointVelocity: {
            // the control point's velocity (system space, from its last two updates) times a factor between `min` and `max`
            const int cp = std::clamp(c.integer("controlpoint", 0), 0, kControlPoints - 1);
            const float k = uniform(c.number("min", 1.0f, m_props), c.number("max", 1.0f, m_props));
            const Vec3 &v = m_cpVelocity[static_cast<size_t>(cp)];
            at(Col::VX, i) += v.x * k;
            at(Col::VY, i) += v.y * k;
            at(Col::VZ, i) += v.z * k;
            break;
        }
        case InitKind::PositionOffsetRandom: {
            // An offset of up to about `distance` units from fractal noise of the place (`scale` per 100 units), the clock (`timescale`)
            // and the particle's birth number, so the particles of one burst lie on a smooth random line (lightning) and the line
            // changes with time. Flat systems stay flat.
            const float distance = c.number("distance", 100.0f, m_props);
            const float scale = c.number("scale", 1.0f, m_props) * 0.01f;
            const float shift = static_cast<float>(m_time) * c.number("timescale", 1.0f, m_props) + static_cast<float>(m_serials.back() % 100000u) * 0.1f;
            const Vec3 q{at(Col::X, i) * scale + shift, at(Col::Y, i) * scale + shift, at(Col::Z, i) * scale + shift};
            const Vec3 o{fbmNoise({q.x + 3.1f, q.y, q.z}), fbmNoise({q.x, q.y + 17.3f, q.z}), (m_def.flags & 4) != 0 ? fbmNoise({q.x, q.y, q.z + 41.9f}) : 0.0f};
            at(Col::X, i) += o.x * distance;
            at(Col::Y, i) += o.y * distance;
            at(Col::Z, i) += o.z * distance;
            break;
        }
        case InitKind::RemapInitialValue:
            remap(init.remap, i, 1.0f, 0.0f);
            break;
        case InitKind::InheritInitialValueFromEvent:
            if (m_haveEvent) {
                applyEvent(init.event, i, true, false, false, false, 0.0f);
            }
            break;
        }
    }
}

void ParticleSystem::runOperators(float dt)
{
    const float speed = m_overrides.speed;
    const auto weightOf = [this](const Operator &op, size_t i) {
        if (!op.blend.in && !op.blend.out) {
            return 1.0f;
        }
        return op.blend.weight(at(Col::Lifetime, i) > 0.0f ? at(Col::Age, i) / at(Col::Lifetime, i) : 1.0f);
    };
    for (Operator &op : m_operators) {
        const ParticleComponent &c = *op.c;
        switch (op.kind) {
        case OpKind::Movement: {
            // position first with the velocity of the step, then gravity and drag for the next one
            const float drag = c.number("drag", 0.0f, m_props);
            const Vec3 g = c.vector("gravity", {0, 0, 0}, m_props);
            const float keep = std::max(0.0f, 1.0f - drag * dt);
            for (size_t i = 0; i < m_count; ++i) {
                at(Col::X, i) += at(Col::VX, i) * dt;
                at(Col::Y, i) += at(Col::VY, i) * dt;
                at(Col::Z, i) += at(Col::VZ, i) * dt;
                at(Col::VX, i) = (at(Col::VX, i) + g.x * dt * speed) * keep;
                at(Col::VY, i) = (at(Col::VY, i) + g.y * dt * speed) * keep;
                at(Col::VZ, i) = (at(Col::VZ, i) + g.z * dt * speed) * keep;
            }
            break;
        }
        case OpKind::AngularMovement: {
            const float drag = c.number("drag", 0.0f, m_props);
            const Vec3 force = c.vector("force", {0, 0, 0}, m_props);
            const float keep = std::max(0.0f, 1.0f - drag * dt);
            for (size_t i = 0; i < m_count; ++i) {
                at(Col::RotX, i) += at(Col::AngX, i) * dt * speed;
                at(Col::RotY, i) += at(Col::AngY, i) * dt * speed;
                at(Col::RotZ, i) += at(Col::AngZ, i) * dt * speed;
                at(Col::AngX, i) = (at(Col::AngX, i) + force.x * dt * speed) * keep;
                at(Col::AngY, i) = (at(Col::AngY, i) + force.y * dt * speed) * keep;
                at(Col::AngZ, i) = (at(Col::AngZ, i) + force.z * dt * speed) * keep;
                for (ParticleColumn r : {Col::RotX, Col::RotY, Col::RotZ}) {
                    float &v = at(r, i);
                    if (v > kPi || v < -kPi) {
                        v = std::remainder(v, kTwoPi);
                    }
                }
            }
            break;
        }
        case OpKind::AlphaFade: {
            const float in = c.number("fadeintime", 0.5f, m_props);
            const float out = c.number("fadeouttime", 0.5f, m_props);
            for (size_t i = 0; i < m_count; ++i) {
                const float life = at(Col::Lifetime, i) > 0.0f ? at(Col::Age, i) / at(Col::Lifetime, i) : 1.0f;
                float f = 1.0f;
                if (life <= in) {
                    f = fade(life, 0.0f, in, 0.0f, 1.0f);
                } else if (life > out) {
                    f = 1.0f - fade(life, out, 1.0f, 0.0f, 1.0f);
                }
                at(Col::Alpha, i) = at(Col::InitialAlpha, i) * f;
                at(Col::AlphaOscBase, i) = at(Col::Alpha, i);
            }
            break;
        }
        case OpKind::AlphaChange:
        case OpKind::SizeChange: {
            const float t0 = c.number("starttime", 0.0f, m_props);
            const float t1 = c.number("endtime", 1.0f, m_props);
            const float v0 = c.number("startvalue", 1.0f, m_props);
            const float v1 = c.number("endvalue", 0.0f, m_props);
            const bool size = op.kind == OpKind::SizeChange;
            for (size_t i = 0; i < m_count; ++i) {
                const float life = at(Col::Lifetime, i) > 0.0f ? at(Col::Age, i) / at(Col::Lifetime, i) : 1.0f;
                const float m = fade(life, t0, t1, v0, v1);
                if (size) {
                    at(Col::Size, i) = at(Col::InitialSize, i) * m;
                    at(Col::SizeOscBase, i) = at(Col::Size, i);
                } else {
                    at(Col::Alpha, i) = at(Col::InitialAlpha, i) * m;
                    at(Col::AlphaOscBase, i) = at(Col::Alpha, i);
                }
            }
            break;
        }
        case OpKind::ColorChange: {
            const float t0 = c.number("starttime", 0.0f, m_props);
            const float t1 = c.number("endtime", 1.0f, m_props);
            const Vec3 v0 = c.vector("startvalue", {1, 1, 1}, m_props);
            const Vec3 v1 = c.vector("endvalue", {1, 1, 1}, m_props);
            for (size_t i = 0; i < m_count; ++i) {
                const float life = at(Col::Lifetime, i) > 0.0f ? at(Col::Age, i) / at(Col::Lifetime, i) : 1.0f;
                at(Col::R, i) = at(Col::InitialR, i) * fade(life, t0, t1, v0.x, v1.x);
                at(Col::G, i) = at(Col::InitialG, i) * fade(life, t0, t1, v0.y, v1.y);
                at(Col::B, i) = at(Col::InitialB, i) * fade(life, t0, t1, v0.z, v1.z);
            }
            break;
        }
        case OpKind::Turbulence: {
            // Accelerates along curl noise sampled at the particle (shifted by the operator's phase and the clock),
            // at the speed drawn for the operator, along the axes of `mask`.
            const float scale = c.number("scale", 0.005f, m_props) * 2.0f;
            const float timeScale = c.number("timescale", 0.01f, m_props);
            const Vec3 mask = c.vector("mask", {1, 1, 0}, m_props);
            if (op.speed <= 1e-4) {
                break;
            }
            const float shift = static_cast<float>(op.phase + timeScale * m_time);
            const float audio = audioResponse(c); // 1 without `audioprocessingmode`
            for (size_t i = 0; i < m_count; ++i) {
                const Vec3 p{(at(Col::X, i) + shift) * scale, at(Col::Y, i) * scale, at(Col::Z, i) * scale};
                const Vec3 curl = curlNoise(p);
                const float len = length(curl);
                if (len <= 1e-4f) {
                    continue;
                }
                const float k = static_cast<float>(op.speed) / len * dt * speed * weightOf(op, i) * audio;
                at(Col::VX, i) += curl.x * k * mask.x;
                at(Col::VY, i) += curl.y * k * mask.y;
                at(Col::VZ, i) += curl.z * k * mask.z;
            }
            break;
        }
        case OpKind::ControlPointAttract: {
            // within `threshold` of the control point (plus `origin`) a constant pull of `scale` units/s towards it
            const int cp = c.integer("controlpoint", 0);
            if (cp < 0 || cp >= kControlPoints) {
                break;
            }
            const Vec3 o = c.vector("origin", {0, 0, 0}, m_props);
            const Vec3 center{m_controlPoints[static_cast<size_t>(cp)].x + o.x, m_controlPoints[static_cast<size_t>(cp)].y + o.y,
                              m_controlPoints[static_cast<size_t>(cp)].z + o.z};
            const float scale = c.number("scale", 100.0f, m_props);
            const float threshold = c.number("threshold", 1000.0f, m_props);
            for (size_t i = 0; i < m_count; ++i) {
                const Vec3 d{center.x - at(Col::X, i), center.y - at(Col::Y, i), center.z - at(Col::Z, i)};
                const float dist = length(d);
                if (dist > 0.001f && dist < threshold) {
                    const float k = scale * dt * speed / dist * weightOf(op, i);
                    at(Col::VX, i) += d.x * k;
                    at(Col::VY, i) += d.y * k;
                    at(Col::VZ, i) += d.z * k;
                }
            }
            break;
        }
        case OpKind::Vortex: {
            // around the axis through the control point (plus `offset`): inside `distanceouter` a particle is pushed along the circle
            // at `speedinner` close to the axis, `speedouter` at the outer distance and in between by distance, units/s added to
            // its velocity each second. With `audioprocessingmode` the push is scaled by the spectrum like an emitter's rate (audioResponse).
            const int cp = c.integer("controlpoint", 0);
            if (cp < 0 || cp >= kControlPoints) {
                break;
            }
            const Vec3 o = c.vector("offset", {0, 0, 0}, m_props);
            const Vec3 &p = m_controlPoints[static_cast<size_t>(cp)];
            const Vec3 center{p.x + o.x, p.y + o.y, p.z + o.z};
            Vec3 axis = c.vector("axis", {0, 0, 1}, m_props);
            const float axisLength = length(axis);
            if (axisLength <= 1e-6f) {
                break;
            }
            axis = {axis.x / axisLength, axis.y / axisLength, axis.z / axisLength};
            const float inner = c.number("distanceinner", 0.0f, m_props);
            const float outer = c.number("distanceouter", 0.0f, m_props);
            const float speedInner = c.number("speedinner", 0.0f, m_props);
            const float speedOuter = c.number("speedouter", 0.0f, m_props);
            const float audio = audioResponse(c);
            for (size_t i = 0; i < m_count; ++i) {
                const Vec3 d{at(Col::X, i) - center.x, at(Col::Y, i) - center.y, at(Col::Z, i) - center.z};
                const float along = d.x * axis.x + d.y * axis.y + d.z * axis.z;
                const Vec3 radial{d.x - axis.x * along, d.y - axis.y * along, d.z - axis.z * along};
                const float dist = length(radial);
                if (dist < 1e-3f || dist >= outer) {
                    continue;
                }
                const float t = outer > inner ? std::clamp((dist - inner) / (outer - inner), 0.0f, 1.0f) : 1.0f;
                const float push = (speedInner + (speedOuter - speedInner) * t) * dt * speed / dist * weightOf(op, i) * audio;
                // axis x radial: the circle's direction (counter-clockwise seen from the axis' tip)
                at(Col::VX, i) += (axis.y * radial.z - axis.z * radial.y) * push;
                at(Col::VY, i) += (axis.z * radial.x - axis.x * radial.z) * push;
                at(Col::VZ, i) += (axis.x * radial.y - axis.y * radial.x) * push;
            }
            break;
        }
        case OpKind::Boids:
            runBoids(c, dt * speed);
            break;
        case OpKind::RemapValue:
            // see remap(); outputs the simulation cannot write are left alone (and reported by the definition)
            for (size_t i = 0; i < m_count; ++i) {
                remap(op.remap, i, weightOf(op, i), dt);
            }
            break;
        case OpKind::VortexV2:
            runVortexV2(op, dt * speed);
            break;
        case OpKind::CapVelocity: {
            // the speed is cut to `maxspeed`
            const float maxSpeed = std::max(0.0f, c.number("maxspeed", 100.0f, m_props));
            for (size_t i = 0; i < m_count; ++i) {
                const float sp = length({at(Col::VX, i), at(Col::VY, i), at(Col::VZ, i)});
                if (sp > maxSpeed) {
                    const float k = 1.0f + (maxSpeed / sp - 1.0f) * weightOf(op, i);
                    at(Col::VX, i) *= k;
                    at(Col::VY, i) *= k;
                    at(Col::VZ, i) *= k;
                }
            }
            break;
        }
        case OpKind::MaintainDistance: {
            // Each particle keeps its distance to the control point: `distance` when given, else the one it had when the operator first
            // saw it. The gap closes and the velocity along it fades at `variablestrength` per second.
            const Vec3 centre = m_controlPoints[static_cast<size_t>(std::clamp(c.integer("controlpoint", 0), 0, kControlPoints - 1))];
            const float strength = std::max(0.0f, c.number("variablestrength", 5.0f, m_props));
            const bool fixed = c.has("distance");
            const float wanted = c.number("distance", 0.0f, m_props);
            for (size_t i = 0; i < m_count; ++i) {
                const Vec3 d{at(Col::X, i) - centre.x, at(Col::Y, i) - centre.y, at(Col::Z, i) - centre.z};
                const float dist = length(d);
                uint32_t flags = static_cast<uint32_t>(at(Col::OscInitialised, i));
                if ((flags & 8u) == 0) {
                    at(Col::KeptDistance, i) = fixed ? wanted : dist;
                    at(Col::OscInitialised, i) = static_cast<float>(flags | 8u);
                }
                if (dist < 1e-4f) {
                    continue;
                }
                const Vec3 dir{d.x / dist, d.y / dist, d.z / dist};
                const float k = (1.0f - std::exp(-strength * dt)) * weightOf(op, i);
                const float gap = at(Col::KeptDistance, i) - dist;
                const float along = at(Col::VX, i) * dir.x + at(Col::VY, i) * dir.y + at(Col::VZ, i) * dir.z;
                at(Col::X, i) += dir.x * gap * k;
                at(Col::Y, i) += dir.y * gap * k;
                at(Col::Z, i) += dir.z * gap * k;
                at(Col::VX, i) -= dir.x * along * k;
                at(Col::VY, i) -= dir.y * along * k;
                at(Col::VZ, i) -= dir.z * along * k;
            }
            break;
        }
        case OpKind::MaintainBetween: {
            // Each particle keeps its place along the line from control point `controlpointstart` (0) to `controlpointend` (1), the
            // place it had when the operator first saw it: it moves with the line when the points move, and what pushes it along the
            // line is undone (across the line it moves freely, so turbulence still bends a lightning bolt).
            const size_t ia = static_cast<size_t>(std::clamp(c.integer("controlpointstart", 0), 0, kControlPoints - 1));
            const size_t ib = static_cast<size_t>(std::clamp(c.integer("controlpointend", 1), 0, kControlPoints - 1));
            const Vec3 a = m_controlPoints[ia], b = m_controlPoints[ib];
            const Vec3 pa = m_cpLastStepKnown ? m_cpLastStep[ia] : a, pb = m_cpLastStepKnown ? m_cpLastStep[ib] : b;
            const Vec3 line{b.x - a.x, b.y - a.y, b.z - a.z};
            const float len = length(line);
            for (size_t i = 0; i < m_count; ++i) {
                const float w = weightOf(op, i);
                uint32_t flags = static_cast<uint32_t>(at(Col::OscInitialised, i));
                if ((flags & 16u) == 0) {
                    const float t = len > 1e-4f ? dot({at(Col::X, i) - a.x, at(Col::Y, i) - a.y, at(Col::Z, i) - a.z}, line) / (len * len) : 0.0f;
                    at(Col::KeptPlace, i) = std::clamp(t, 0.0f, 1.0f);
                    at(Col::OscInitialised, i) = static_cast<float>(flags | 16u);
                }
                const float t = at(Col::KeptPlace, i);
                // the line moved since the last step: so does the particle
                at(Col::X, i) += ((a.x - pa.x) + ((b.x - a.x) - (pb.x - pa.x)) * t) * w;
                at(Col::Y, i) += ((a.y - pa.y) + ((b.y - a.y) - (pb.y - pa.y)) * t) * w;
                at(Col::Z, i) += ((a.z - pa.z) + ((b.z - a.z) - (pb.z - pa.z)) * t) * w;
                if (len <= 1e-4f) {
                    continue;
                }
                const Vec3 dir{line.x / len, line.y / len, line.z / len};
                const float off = (t * len - dot({at(Col::X, i) - a.x, at(Col::Y, i) - a.y, at(Col::Z, i) - a.z}, dir)) * w;
                const float vAlong = dot({at(Col::VX, i), at(Col::VY, i), at(Col::VZ, i)}, dir) * w;
                at(Col::X, i) += dir.x * off;
                at(Col::Y, i) += dir.y * off;
                at(Col::Z, i) += dir.z * off;
                at(Col::VX, i) -= dir.x * vAlong;
                at(Col::VY, i) -= dir.y * vAlong;
                at(Col::VZ, i) -= dir.z * vAlong;
            }
            break;
        }
        case OpKind::ReduceMovement: {
            // Near the control point the velocity decays at `reductioninner` per second (within `distanceinner`), `reductionouter` at
            // `distanceouter` and beyond, and by distance in between: v *= exp(-reduction * dt).
            const Vec3 centre = m_controlPoints[static_cast<size_t>(std::clamp(c.integer("controlpoint", 0), 0, kControlPoints - 1))];
            const float inner = c.number("distanceinner", 0.0f, m_props);
            const float outer = c.number("distanceouter", 100.0f, m_props);
            const float rIn = c.number("reductioninner", 1.0f, m_props);
            const float rOut = c.number("reductionouter", 0.0f, m_props);
            if (rIn == 0.0f && rOut == 0.0f) {
                break;
            }
            for (size_t i = 0; i < m_count; ++i) {
                const float dist = length({at(Col::X, i) - centre.x, at(Col::Y, i) - centre.y, at(Col::Z, i) - centre.z});
                const float reduction = rIn + (rOut - rIn) * ramp(dist, inner, outer);
                const float k = std::exp(-std::max(0.0f, reduction) * dt * weightOf(op, i));
                at(Col::VX, i) *= k;
                at(Col::VY, i) *= k;
                at(Col::VZ, i) *= k;
            }
            break;
        }
        case OpKind::CollisionPlane:
        case OpKind::CollisionSphere:
        case OpKind::CollisionBox:
        case OpKind::CollisionQuad:
        case OpKind::CollisionBounds:
            runCollision(op, dt);
            break;
        case OpKind::InheritValueFromEvent:
            if (m_haveEvent) {
                for (size_t i = 0; i < m_count; ++i) {
                    applyEvent(op.event, i, false, op.colorRebuilt, op.alphaRebuilt, op.sizeRebuilt, dt);
                }
            }
            break;
        case OpKind::OscillateAlpha:
        case OpKind::OscillateSize: {
            // value = base * mix(scalemin, scalemax, (cos(f * age + phase) + 1) / 2); the base follows fades and size changes
            const bool alpha = op.kind == OpKind::OscillateAlpha;
            const float fmin = c.number("frequencymin", 0.0f, m_props);
            const float fmax = c.number("frequencymax", 10.0f, m_props);
            const float smin = c.number("scalemin", alpha ? 0.0f : 0.8f, m_props);
            const float smax = c.number("scalemax", alpha ? 1.0f : 1.2f, m_props);
            const float pmin = c.number("phasemin", 0.0f, m_props);
            const float pmax = c.number("phasemax", kTwoPi, m_props);
            const ParticleColumn freq = alpha ? Col::AlphaOscFrequency : Col::SizeOscFrequency;
            const ParticleColumn phase = alpha ? Col::AlphaOscPhase : Col::SizeOscPhase;
            const ParticleColumn base = alpha ? Col::AlphaOscBase : Col::SizeOscBase;
            const ParticleColumn target = alpha ? Col::Alpha : Col::Size;
            const uint32_t bit = alpha ? 1u : 2u;
            for (size_t i = 0; i < m_count; ++i) {
                uint32_t flags = static_cast<uint32_t>(at(Col::OscInitialised, i));
                if ((flags & bit) == 0) {
                    at(freq, i) = uniform(fmin, fmax);
                    at(phase, i) = uniform(pmin, pmax + kTwoPi);
                    at(base, i) = at(target, i);
                    flags |= bit;
                    at(Col::OscInitialised, i) = static_cast<float>(flags);
                }
                const float wave = (std::cos(at(freq, i) * at(Col::Age, i) + at(phase, i)) + 1.0f) * 0.5f;
                // (outside its blend window the value is the base one: the oscillation fades in and out)
                at(target, i) = at(base, i) * (1.0f + (smin + (smax - smin) * wave - 1.0f) * weightOf(op, i));
            }
            break;
        }
        case OpKind::OscillatePosition: {
            // each axis moves by the derivative of its own oscillation, so the particle sways around where it is
            const float fmin = c.number("frequencymin", 0.0f, m_props);
            const float fmax = c.number("frequencymax", 5.0f, m_props);
            const float smin = c.number("scalemin", 0.0f, m_props);
            const float smax = c.number("scalemax", 10.0f, m_props);
            const float pmin = c.number("phasemin", 0.0f, m_props);
            const float pmax = c.number("phasemax", kTwoPi, m_props);
            const Vec3 mask = c.vector("mask", {1, 1, 0}, m_props);
            const float m[3] = {mask.x, mask.y, mask.z};
            const ParticleColumn freqs[3] = {Col::PosOscFrequencyX, Col::PosOscFrequencyY, Col::PosOscFrequencyZ};
            const ParticleColumn scales[3] = {Col::PosOscScaleX, Col::PosOscScaleY, Col::PosOscScaleZ};
            const ParticleColumn phases[3] = {Col::PosOscPhaseX, Col::PosOscPhaseY, Col::PosOscPhaseZ};
            const ParticleColumn axes[3] = {Col::X, Col::Y, Col::Z};
            for (size_t i = 0; i < m_count; ++i) {
                uint32_t flags = static_cast<uint32_t>(at(Col::OscInitialised, i));
                if ((flags & 4u) == 0) {
                    for (int a = 0; a < 3; ++a) {
                        at(freqs[a], i) = uniform(fmin, fmax);
                        at(scales[a], i) = uniform(smin, smax);
                        at(phases[a], i) = uniform(pmin, pmax + kTwoPi);
                    }
                    flags |= 4u;
                    at(Col::OscInitialised, i) = static_cast<float>(flags);
                }
                const float blend = weightOf(op, i);
                for (int a = 0; a < 3; ++a) {
                    const float w = at(freqs[a], i);
                    at(axes[a], i) += -at(scales[a], i) * w * std::sin(w * at(Col::Age, i) + at(phases[a], i)) * dt * m[a] * speed * blend;
                }
            }
            break;
        }
        }
    }
    m_cpLastStep = m_controlPoints;
    m_cpLastStepKnown = true;
}

// Flocking (`boids`): each particle steers by the others within `neighborthreshold` units, towards their centre (`cohesionfactor`,
// per second, of the distance to it), towards their mean velocity (`alignmentfactor`, per second, of the difference) and away from
// those closer than `separationthreshold` (half the neighbour distance unless given; `separationfactor` times the neighbour
// distance per second, stronger the closer they are). `maxspeed`, when given, caps the speed. A flat system flocks in x and y.
// The neighbours come from a grid of cells the size of the neighbour distance, and a particle looks at kMaxBoidNeighbours of them
// at most, so a dense system costs linear time.
void ParticleSystem::runBoids(const ParticleComponent &c, float dt)
{
    constexpr size_t kMaxBoidNeighbours = 48;
    const float radius = c.number("neighborthreshold", 100.0f, m_props);
    if (!(radius > 1e-3f) || m_count < 2) {
        return;
    }
    const float separationRadius = std::max(1e-3f, c.number("separationthreshold", radius * 0.5f, m_props));
    const float cohesion = c.number("cohesionfactor", 0.0f, m_props);
    const float alignment = c.number("alignmentfactor", 0.0f, m_props);
    const float separation = c.number("separationfactor", 0.0f, m_props);
    const float maxSpeed = c.number("maxspeed", 0.0f, m_props);
    const bool flat = (m_def.flags & 4) == 0;
    const auto cellOf = [&](float v) { return static_cast<int64_t>(std::floor(std::clamp(v / radius, -1.0e6f, 1.0e6f))); };
    const auto key = [](int64_t x, int64_t y, int64_t z) {
        return (static_cast<uint64_t>(x + (1 << 20)) << 42) | (static_cast<uint64_t>(y + (1 << 20)) << 21) | static_cast<uint64_t>(z + (1 << 20));
    };
    m_boidCells.resize(m_count);
    for (size_t i = 0; i < m_count; ++i) {
        m_boidCells[i] = {key(cellOf(at(Col::X, i)), cellOf(at(Col::Y, i)), flat ? 0 : cellOf(at(Col::Z, i))), static_cast<uint32_t>(i)};
    }
    std::sort(m_boidCells.begin(), m_boidCells.end());
    m_boidSteer.assign(m_count, Vec3{});
    for (size_t i = 0; i < m_count; ++i) {
        const Vec3 p{at(Col::X, i), at(Col::Y, i), flat ? 0.0f : at(Col::Z, i)};
        const int64_t cx = cellOf(p.x), cy = cellOf(p.y), cz = flat ? 0 : cellOf(p.z);
        Vec3 centre{}, velocity{}, apart{};
        size_t neighbours = 0;
        for (int64_t dz = flat ? 0 : -1; dz <= (flat ? 0 : 1) && neighbours < kMaxBoidNeighbours; ++dz) {
            for (int64_t dy = -1; dy <= 1 && neighbours < kMaxBoidNeighbours; ++dy) {
                for (int64_t dx = -1; dx <= 1 && neighbours < kMaxBoidNeighbours; ++dx) {
                    const uint64_t k = key(cx + dx, cy + dy, cz + dz);
                    auto it = std::lower_bound(m_boidCells.begin(), m_boidCells.end(), std::make_pair(k, uint32_t{0}));
                    for (; it != m_boidCells.end() && it->first == k && neighbours < kMaxBoidNeighbours; ++it) {
                        const size_t j = it->second;
                        if (j == i) {
                            continue;
                        }
                        const Vec3 d{at(Col::X, j) - p.x, at(Col::Y, j) - p.y, flat ? 0.0f : at(Col::Z, j) - p.z};
                        const float dist = length(d);
                        if (dist >= radius) {
                            continue;
                        }
                        ++neighbours;
                        centre = {centre.x + d.x, centre.y + d.y, centre.z + d.z};
                        velocity = {velocity.x + at(Col::VX, j), velocity.y + at(Col::VY, j), velocity.z + (flat ? 0.0f : at(Col::VZ, j))};
                        if (dist < separationRadius && dist > 1e-4f) {
                            const float push = (1.0f - dist / separationRadius) / dist;
                            apart = {apart.x - d.x * push, apart.y - d.y * push, apart.z - d.z * push};
                        }
                    }
                }
            }
        }
        if (neighbours == 0) {
            continue;
        }
        const float n = static_cast<float>(neighbours);
        const Vec3 v{at(Col::VX, i), at(Col::VY, i), flat ? 0.0f : at(Col::VZ, i)};
        m_boidSteer[i] = {cohesion * centre.x / n + alignment * (velocity.x / n - v.x) + separation * radius * apart.x,
                          cohesion * centre.y / n + alignment * (velocity.y / n - v.y) + separation * radius * apart.y,
                          cohesion * centre.z / n + alignment * (velocity.z / n - v.z) + separation * radius * apart.z};
    }
    for (size_t i = 0; i < m_count; ++i) {
        const Vec3 &s = m_boidSteer[i];
        // (the alignment term is a rate: over a long step it may not overshoot the mean it steers towards)
        const float step = std::min(dt, alignment > 0.0f ? 1.0f / alignment : dt);
        at(Col::VX, i) += s.x * step;
        at(Col::VY, i) += s.y * step;
        at(Col::VZ, i) += s.z * step;
        if (maxSpeed > 0.0f) {
            const float sp = length({at(Col::VX, i), at(Col::VY, i), at(Col::VZ, i)});
            if (sp > maxSpeed) {
                const float k = maxSpeed / sp;
                at(Col::VX, i) *= k;
                at(Col::VY, i) *= k;
                at(Col::VZ, i) *= k;
            }
        }
    }
}

namespace {

Vec3 transformPoint(const Mat4 &m, Vec3 p)
{
    return {m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3), m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3),
            m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3)};
}

Vec3 transformVector(const Mat4 &m, Vec3 v)
{
    return {m.at(0, 0) * v.x + m.at(0, 1) * v.y + m.at(0, 2) * v.z, m.at(1, 0) * v.x + m.at(1, 1) * v.y + m.at(1, 2) * v.z,
            m.at(2, 0) * v.x + m.at(2, 1) * v.y + m.at(2, 2) * v.z};
}

float fraction(float x)
{
    return x - std::floor(x);
}

// A transform function of the remap elements, of an input already brought to 0..1 by the input range: the result is 0..1.
float transformed(ParticleTransformFunction f, float x, float scale)
{
    using F = ParticleTransformFunction;
    if (!std::isfinite(x)) {
        return 0.0f;
    }
    const float u = std::clamp(x * scale, -1.0e6f, 1.0e6f);
    switch (f) {
    case F::None:
        return std::clamp(x, 0.0f, 1.0f);
    case F::Sine:
        return 0.5f + 0.5f * std::sin(kTwoPi * u);
    case F::Square:
        return fraction(u) < 0.5f ? 1.0f : 0.0f;
    case F::Saw:
        return fraction(u);
    case F::Triangle:
        return 1.0f - std::fabs(2.0f * fraction(u) - 1.0f);
    case F::SimplexNoise:
        return std::clamp(valueNoiseGrad({u, 0.37f, 0.71f}, nullptr) * 0.5f + 0.5f, 0.0f, 1.0f);
    case F::FbmNoise:
        return std::clamp(fbmNoise({u, 0.37f, 0.71f}) * 0.5f + 0.5f, 0.0f, 1.0f);
    }
    return 0.0f;
}

} // namespace

float ParticleSystem::Blend::weight(float life) const
{
    float w = 1.0f;
    if (in) {
        w *= ramp(life, inStart, inEnd);
    }
    if (out) {
        w *= 1.0f - ramp(life, outStart, outEnd);
    }
    return w;
}

ParticleSystem::Remap ParticleSystem::readRemap(const ParticleComponent &c, const PropertyStore *props)
{
    Remap r;
    r.function = particleTransformFunction(c.text("transformfunction")).value_or(ParticleTransformFunction::None);
    const std::string input = c.text("input");
    r.input = particleRemapValue(input).value_or(ParticleRemapValue::Size);
    r.positionNoise = input.empty() && (r.function == ParticleTransformFunction::SimplexNoise || r.function == ParticleTransformFunction::FbmNoise);
    const std::string output = c.text("output");
    const auto out = output.empty() ? std::optional(ParticleRemapValue::Size) : particleRemapValue(output);
    r.writable = out && particleRemapWritable(*out);
    r.output = out.value_or(ParticleRemapValue::Size);
    r.operation = particleRemapOperation(c.text("operation")).value_or(ParticleRemapOperation::Assign);
    r.inputScale = c.number("transforminputscale", 1.0f, props);
    r.inputMin = c.number("inputrangemin", 0.0f, props);
    r.inputMax = c.number("inputrangemax", 1.0f, props);
    r.outputMin = c.vector("outputrangemin", {0.0f, 0.0f, 0.0f}, props);
    r.outputMax = c.vector("outputrangemax", {1.0f, 1.0f, 1.0f}, props);
    if (r.output == ParticleRemapValue::Color) {
        r.outputMin = normalisedColor(r.outputMin);
        r.outputMax = normalisedColor(r.outputMax);
    }
    r.cp0 = std::clamp(c.integer("inputcontrolpoint0", 0), 0, kControlPoints - 1);
    r.cp1 = std::clamp(c.integer("inputcontrolpoint1", 1), 0, kControlPoints - 1);
    return r;
}

float ParticleSystem::remapScalarInput(const Remap &r, size_t i) const
{
    using V = ParticleRemapValue;
    const auto col = [&](ParticleColumn c) { return m_columns[static_cast<size_t>(c)][i]; };
    const Vec3 p{col(Col::X), col(Col::Y), col(Col::Z)};
    const Vec3 &a = m_controlPoints[static_cast<size_t>(r.cp0)];
    switch (r.input) {
    case V::LifetimeFraction:
        return col(Col::Lifetime) > 0.0f ? col(Col::Age) / col(Col::Lifetime) : 1.0f;
    case V::MaxLifetime:
        return col(Col::Lifetime);
    case V::Size:
        return col(Col::Size) * 2.0f; // in the files' units, as sizerandom writes them
    case V::Opacity:
        return col(Col::Alpha);
    case V::Speed:
        return length({col(Col::VX), col(Col::VY), col(Col::VZ)});
    case V::Rotation:
        return col(Col::RotZ);
    case V::AngularSpeed:
        return col(Col::AngZ);
    case V::DistanceToControlPoint:
        return length({p.x - a.x, p.y - a.y, p.z - a.z});
    case V::PositionBetweenControlPoints: {
        const Vec3 &b = m_controlPoints[static_cast<size_t>(r.cp1)];
        const Vec3 line{b.x - a.x, b.y - a.y, b.z - a.z};
        const float len2 = dot(line, line);
        return len2 > 1e-8f ? dot({p.x - a.x, p.y - a.y, p.z - a.z}, line) / len2 : 0.0f;
    }
    case V::TimeOfDay:
        return m_dayFraction;
    case V::Runtime:
    case V::SystemTime:
    case V::LayerTime:
        return static_cast<float>(m_time);
    default: { // a vector read as a scalar: its length
        const Vec3 v = remapVectorInput(r, i);
        return length(v);
    }
    }
}

Vec3 ParticleSystem::remapVectorInput(const Remap &r, size_t i) const
{
    using V = ParticleRemapValue;
    const auto col = [&](ParticleColumn c) { return m_columns[static_cast<size_t>(c)][i]; };
    const Vec3 p{col(Col::X), col(Col::Y), col(Col::Z)};
    const Vec3 &a = m_controlPoints[static_cast<size_t>(r.cp0)];
    switch (r.input) {
    case V::Color:
        return {col(Col::R), col(Col::G), col(Col::B)};
    case V::Position:
        return p;
    case V::Velocity:
        return {col(Col::VX), col(Col::VY), col(Col::VZ)};
    case V::ControlPoint:
        return a;
    case V::DeltaToControlPoint:
        return {a.x - p.x, a.y - p.y, a.z - p.z};
    case V::DirectionToControlPoint:
        return normalised({a.x - p.x, a.y - p.y, a.z - p.z}, {0.0f, 0.0f, 0.0f});
    case V::LayerOrigin:
        return {0.0f, 0.0f, 0.0f}; // the system's own origin, in its own space
    default: {
        const float v = remapScalarInput(r, i);
        return {v, v, v};
    }
    }
}

// Remap: an input (a value of the particle, a control point or a clock), brought to 0..1 by `inputrangemin`..`inputrangemax`, through the
// transform function (`transforminputscale` periods or noise cells per unit of that; "none" clamps), then from 0..1 onto
// `outputrangemin`..`outputrangemax` and assigned to the output, or multiplying, added to or subtracted from it. A vector input gives each
// component its own value. Without an `input` a noise function reads the particle's position instead (`transforminputscale` cells
// per 1000 units, the rain presets). Sizes are in the files' units (twice the drawn width).
void ParticleSystem::remap(const Remap &r, size_t i, float weight, float dt)
{
    using V = ParticleRemapValue;
    using O = ParticleRemapOperation;
    if (!r.writable || !(weight > 0.0f)) {
        return;
    }
    Vec3 t;
    if (r.positionNoise) {
        const float scale = r.inputScale * 0.001f;
        const Vec3 p{at(Col::X, i) * scale, at(Col::Y, i) * scale, at(Col::Z, i) * scale};
        const float n = r.function == ParticleTransformFunction::FbmNoise ? fbmNoise(p) : valueNoiseGrad(p, nullptr);
        t.x = std::clamp(n * 0.5f + 0.5f, 0.0f, 1.0f);
        // each component has a noise of its own, or the velocities would all lie on one line
        t.y = std::clamp(valueNoiseGrad({p.x + 5.3f, p.y + 9.1f, p.z}, nullptr) * 0.5f + 0.5f, 0.0f, 1.0f);
        t.z = std::clamp(valueNoiseGrad({p.x + 23.7f, p.y, p.z + 3.9f}, nullptr) * 0.5f + 0.5f, 0.0f, 1.0f);
    } else {
        const float span = r.inputMax - r.inputMin;
        const auto normal = [&](float v) { return std::fabs(span) > 1e-12f ? (v - r.inputMin) / span : (v >= r.inputMin ? 1.0f : 0.0f); };
        if (particleRemapIsVector(r.input) && particleRemapIsVector(r.output)) {
            const Vec3 v = remapVectorInput(r, i);
            t = {transformed(r.function, normal(v.x), r.inputScale), transformed(r.function, normal(v.y), r.inputScale),
                 transformed(r.function, normal(v.z), r.inputScale)};
        } else {
            const float x = transformed(r.function, normal(remapScalarInput(r, i)), r.inputScale);
            t = {x, x, x};
        }
    }
    const Vec3 target{r.outputMin.x + (r.outputMax.x - r.outputMin.x) * t.x, r.outputMin.y + (r.outputMax.y - r.outputMin.y) * t.y,
                      r.outputMin.z + (r.outputMax.z - r.outputMin.z) * t.z};
    const bool rate = dt > 0.0f; // an operator: changes of the motion are per second
    // a value of the particle combined with the target: `base` is what multiply / add work on
    const auto combine = [&](float base, float value, bool motion) {
        switch (r.operation) {
        case O::Assign:
            return value;
        case O::Multiply:
            return motion && rate ? base * std::pow(std::max(0.0f, value), dt) : base * value;
        case O::Add:
            return base + (motion && rate ? value * dt : value);
        case O::Subtract:
            return base - (motion && rate ? value * dt : value);
        }
        return base;
    };
    const auto blendInto = [&](ParticleColumn column, float base, float value, bool motion) {
        float &cur = at(column, i);
        const float next = combine(base, value, motion);
        if (std::isfinite(next)) {
            cur += (next - cur) * weight;
        }
    };
    switch (r.output) {
    case V::Size: {
        const float base = rate && !r.rebuilt ? at(Col::InitialSize, i) : at(Col::Size, i);
        const float value = r.operation == O::Assign || r.operation == O::Add || r.operation == O::Subtract ? target.x * 0.5f * m_overrides.size : target.x;
        blendInto(Col::Size, base, value, false);
        at(Col::SizeOscBase, i) = at(Col::Size, i);
        if (!rate) {
            at(Col::InitialSize, i) = at(Col::Size, i);
        }
        break;
    }
    case V::Opacity: {
        const float base = rate && !r.rebuilt ? at(Col::InitialAlpha, i) : at(Col::Alpha, i);
        blendInto(Col::Alpha, base, target.x, false);
        at(Col::AlphaOscBase, i) = at(Col::Alpha, i);
        if (!rate) {
            at(Col::InitialAlpha, i) = at(Col::Alpha, i);
        }
        break;
    }
    case V::Color: {
        const ParticleColumn cols[3] = {Col::R, Col::G, Col::B};
        const ParticleColumn initial[3] = {Col::InitialR, Col::InitialG, Col::InitialB};
        const float value[3] = {target.x, target.y, target.z};
        for (int k = 0; k < 3; ++k) {
            blendInto(cols[k], rate && !r.rebuilt ? at(initial[k], i) : at(cols[k], i), value[k], false);
            if (!rate) {
                at(initial[k], i) = at(cols[k], i);
            }
        }
        break;
    }
    case V::MaxLifetime:
        blendInto(Col::Lifetime, at(Col::Lifetime, i), target.x, false);
        at(Col::Lifetime, i) = std::max(0.0f, at(Col::Lifetime, i));
        break;
    case V::Rotation:
        blendInto(Col::RotZ, at(Col::RotZ, i), target.x, true);
        break;
    case V::AngularSpeed:
        blendInto(Col::AngZ, at(Col::AngZ, i), target.x, true);
        break;
    case V::Speed: {
        const Vec3 v{at(Col::VX, i), at(Col::VY, i), at(Col::VZ, i)};
        const float now = length(v);
        const Vec3 dir = normalised(v, {0, 1, 0});
        float next = combine(now, target.x, true);
        next = std::isfinite(next) ? now + (next - now) * weight : now;
        at(Col::VX, i) = dir.x * next;
        at(Col::VY, i) = dir.y * next;
        at(Col::VZ, i) = dir.z * next;
        break;
    }
    case V::Velocity:
    case V::Position: {
        const bool velocity = r.output == V::Velocity;
        const ParticleColumn cols[3] = {velocity ? Col::VX : Col::X, velocity ? Col::VY : Col::Y, velocity ? Col::VZ : Col::Z};
        const float value[3] = {target.x, target.y, target.z};
        for (int k = 0; k < 3; ++k) {
            blendInto(cols[k], at(cols[k], i), value[k], true);
        }
        break;
    }
    default:
        break;
    }
}

// What an event copy takes from its parent particle: colour, opacity, both, velocity, size, rotation or angular velocity, set or
// combined. At birth (`initial`) multiply and add work on the particle's own initial value. As an operator they work on this step's
// value when an earlier operator rebuilt it, else on the initial value (so nothing piles up from step to step), and the "add" of a
// motion carries the particle along: the parent's velocity moves it, the parent's angular velocity turns it.
void ParticleSystem::applyEvent(ParticleEventValue v, size_t i, bool initial, bool colorRebuilt, bool alphaRebuilt, bool sizeRebuilt, float dt)
{
    using E = ParticleEventValue;
    const ParticleEventValues &e = m_event;
    const bool color = v == E::SetColor || v == E::MultiplyColor || v == E::SetColorOpacity || v == E::MultiplyColorOpacity;
    const bool alpha = v == E::SetOpacity || v == E::MultiplyOpacity || v == E::SetColorOpacity || v == E::MultiplyColorOpacity;
    const bool multiply = v == E::MultiplyColor || v == E::MultiplyOpacity || v == E::MultiplyColorOpacity;
    if (color) {
        const ParticleColumn cols[3] = {Col::R, Col::G, Col::B};
        const ParticleColumn init[3] = {Col::InitialR, Col::InitialG, Col::InitialB};
        const float parent[3] = {e.color.x, e.color.y, e.color.z};
        for (int k = 0; k < 3; ++k) {
            const float base = initial || colorRebuilt ? at(cols[k], i) : at(init[k], i);
            at(cols[k], i) = multiply ? base * parent[k] : parent[k];
            if (initial) {
                at(init[k], i) = at(cols[k], i);
            }
        }
    }
    if (alpha) {
        const float base = initial || alphaRebuilt ? at(Col::Alpha, i) : at(Col::InitialAlpha, i);
        at(Col::Alpha, i) = multiply ? base * e.alpha : e.alpha;
        at(Col::AlphaOscBase, i) = at(Col::Alpha, i);
        if (initial) {
            at(Col::InitialAlpha, i) = at(Col::Alpha, i);
        }
    }
    if (v == E::SetSize || v == E::MultiplySize) {
        const float base = initial || sizeRebuilt ? at(Col::Size, i) : at(Col::InitialSize, i);
        at(Col::Size, i) = v == E::MultiplySize ? base * e.size : e.size;
        at(Col::SizeOscBase, i) = at(Col::Size, i);
        if (initial) {
            at(Col::InitialSize, i) = at(Col::Size, i);
        }
    }
    const auto motion = [&](ParticleColumn x, Vec3 value, bool set, ParticleColumn carried, Vec3 rate) {
        const ParticleColumn target = set || initial ? x : carried;
        const Vec3 add = set || initial ? value : Vec3{rate.x * dt, rate.y * dt, rate.z * dt};
        const size_t c = static_cast<size_t>(target);
        float *p[3] = {&m_columns[c][i], &m_columns[c + 1][i], &m_columns[c + 2][i]};
        if (set) {
            *p[0] = add.x;
            *p[1] = add.y;
            *p[2] = add.z;
        } else {
            *p[0] += add.x;
            *p[1] += add.y;
            *p[2] += add.z;
        }
    };
    switch (v) {
    case E::SetVelocity:
        motion(Col::VX, e.velocity, true, Col::VX, {});
        break;
    case E::AddVelocity:
        motion(Col::VX, e.velocity, false, Col::X, e.velocity);
        break;
    case E::SetRotation:
        motion(Col::RotX, e.rotation, true, Col::RotX, {});
        break;
    case E::AddRotation:
        motion(Col::RotX, e.rotation, false, Col::RotX, e.angularVelocity);
        break;
    case E::SetAngularVelocity:
        motion(Col::AngX, e.angularVelocity, true, Col::AngX, {});
        break;
    case E::AddAngularVelocity:
        motion(Col::AngX, e.angularVelocity, false, Col::RotX, e.angularVelocity);
        break;
    default:
        break;
    }
}

namespace {

constexpr float kSkin = 1e-3f;     // how far a collided particle is put in front of the surface
constexpr float kTolerance = 0.01f; // how far behind the surface a particle may have been the step before and still collide

// The answer to a collision with a surface of normal `n` (towards the free side), the particle `depth` units behind it: it is put
// back on the surface and bounces (the velocity across the surface reversed and scaled by `bounce`), slides (the velocity across it
// removed), stops (no velocity and no spin) or is deleted (returns true).
bool respond(ParticleCollisionBehavior behavior, float bounce, Vec3 &p, Vec3 &v, Vec3 n, float depth)
{
    using B = ParticleCollisionBehavior;
    if (behavior == B::Delete) {
        return true;
    }
    const float push = std::max(0.0f, depth) + kSkin;
    p = {p.x + n.x * push, p.y + n.y * push, p.z + n.z * push};
    const float across = dot(v, n);
    if (behavior == B::Stop) {
        v = {};
    } else if (across < 0.0f) {
        const float k = behavior == B::Bounce ? 1.0f + bounce : 1.0f;
        v = {v.x - n.x * across * k, v.y - n.y * across * k, v.z - n.z * across * k};
    }
    return false;
}

} // namespace

void ParticleSystem::collide(const Operator &op, size_t i, Vec3 normal, float depth)
{
    Vec3 p{at(Col::X, i), at(Col::Y, i), at(Col::Z, i)};
    Vec3 v{at(Col::VX, i), at(Col::VY, i), at(Col::VZ, i)};
    if (respond(op.behavior, std::max(0.0f, op.c->number("bouncefactor", 1.0f, m_props)), p, v, normal, depth)) {
        at(Col::Age, i) = std::max(at(Col::Age, i), at(Col::Lifetime, i)); // gone at the end of the step
        return;
    }
    at(Col::X, i) = p.x;
    at(Col::Y, i) = p.y;
    at(Col::Z, i) = p.z;
    at(Col::VX, i) = v.x;
    at(Col::VY, i) = v.y;
    at(Col::VZ, i) = v.z;
    if (op.behavior == ParticleCollisionBehavior::Stop) {
        at(Col::AngX, i) = at(Col::AngY, i) = at(Col::AngZ, i) = 0.0f;
    }
}

// Collisions. A particle collides when it went through the surface during the step (it was in front of it, by its velocity, a step ago
// and is behind it now): one that starts behind a surface is left alone. Every shape is placed relative to control point
// `controlpoint` (0). Plane: normal `plane` (0 1 0), `distance` along it. Sphere: `origin`, `radius` (50), hit from outside.
// Box: `origin`, `size` (full extents, 100 100 100), hit from outside. Quad: `origin`, `size` (width along its x, depth along its z,
// 100 100), turned by `angles` (degrees), normal its y, hit only from the front. Bounds: the scene's rectangle, hit from inside, in
// the scene's x and y.
void ParticleSystem::runCollision(const Operator &op, float dt)
{
    const ParticleComponent &c = *op.c;
    const Vec3 cp = m_controlPoints[static_cast<size_t>(std::clamp(c.integer("controlpoint", 0), 0, kControlPoints - 1))];
    const Vec3 o = c.vector("origin", {0.0f, 0.0f, 0.0f}, m_props);
    const Vec3 centre{cp.x + o.x, cp.y + o.y, cp.z + o.z};
    const auto position = [&](size_t i) { return Vec3{at(Col::X, i), at(Col::Y, i), at(Col::Z, i)}; };
    const auto velocity = [&](size_t i) { return Vec3{at(Col::VX, i), at(Col::VY, i), at(Col::VZ, i)}; };
    switch (op.kind) {
    case OpKind::CollisionPlane: {
        const Vec3 n = normalised(c.vector("plane", {0.0f, 1.0f, 0.0f}, m_props), {0.0f, 1.0f, 0.0f});
        const float distance = c.number("distance", 0.0f, m_props);
        for (size_t i = 0; i < m_count; ++i) {
            const Vec3 p = position(i);
            const float d = dot(n, {p.x - cp.x, p.y - cp.y, p.z - cp.z}) - distance;
            if (d < 0.0f && d - dot(n, velocity(i)) * dt >= -kTolerance) {
                collide(op, i, n, -d);
            }
        }
        break;
    }
    case OpKind::CollisionSphere: {
        const float radius = std::max(0.0f, c.number("radius", 50.0f, m_props));
        for (size_t i = 0; i < m_count; ++i) {
            const Vec3 p = position(i), v = velocity(i);
            const Vec3 d{p.x - centre.x, p.y - centre.y, p.z - centre.z};
            const float dist = length(d);
            if (dist >= radius) {
                continue;
            }
            const float before = length({d.x - v.x * dt, d.y - v.y * dt, d.z - v.z * dt});
            if (before >= radius - kTolerance) {
                collide(op, i, normalised(d, normalised({-v.x, -v.y, -v.z}, {0.0f, 1.0f, 0.0f})), radius - dist);
            }
        }
        break;
    }
    case OpKind::CollisionBox: {
        const Vec3 size = c.vector("size", {100.0f, 100.0f, 100.0f}, m_props);
        const float half[3] = {std::fabs(size.x) * 0.5f, std::fabs(size.y) * 0.5f, std::fabs(size.z) * 0.5f};
        for (size_t i = 0; i < m_count; ++i) {
            const Vec3 p = position(i), v = velocity(i);
            const float d[3] = {p.x - centre.x, p.y - centre.y, p.z - centre.z};
            const float vel[3] = {v.x, v.y, v.z};
            if (std::fabs(d[0]) >= half[0] || std::fabs(d[1]) >= half[1] || std::fabs(d[2]) >= half[2]) {
                continue;
            }
            // the face it came through: of the axes it was outside of a step ago, the one it is least deep behind
            int axis = -1;
            float depth = 0.0f;
            for (int a = 0; a < 3; ++a) {
                if (std::fabs(d[a] - vel[a] * dt) >= half[a] - kTolerance) {
                    const float deep = half[a] - std::fabs(d[a]);
                    if (axis < 0 || deep < depth) {
                        axis = a;
                        depth = deep;
                    }
                }
            }
            if (axis >= 0) {
                const float side = d[axis] - vel[axis] * dt >= 0.0f ? 1.0f : -1.0f;
                collide(op, i, {axis == 0 ? side : 0.0f, axis == 1 ? side : 0.0f, axis == 2 ? side : 0.0f}, depth);
            }
        }
        break;
    }
    case OpKind::CollisionQuad: {
        const Vec3 size = c.vector("size", {100.0f, 100.0f, 0.0f}, m_props);
        const Vec3 angles = c.vector("angles", {0.0f, 0.0f, 0.0f}, m_props);
        const Mat4 turn = rotationEuler({angles.x * kPi / 180.0f, angles.y * kPi / 180.0f, angles.z * kPi / 180.0f});
        const Vec3 ax = transformVector(turn, {1.0f, 0.0f, 0.0f}), n = transformVector(turn, {0.0f, 1.0f, 0.0f}), az = transformVector(turn, {0.0f, 0.0f, 1.0f});
        const float halfW = std::fabs(size.x) * 0.5f, halfD = std::fabs(size.y) * 0.5f;
        for (size_t i = 0; i < m_count; ++i) {
            const Vec3 p = position(i), v = velocity(i);
            const Vec3 rel{p.x - centre.x, p.y - centre.y, p.z - centre.z};
            const float d = dot(n, rel);
            const float before = d - dot(n, v) * dt;
            if (!(d < 0.0f && before >= -kTolerance)) {
                continue;
            }
            // where it went through the plane of the quad
            const float s = before > d ? std::clamp(before / (before - d), 0.0f, 1.0f) : 1.0f;
            const Vec3 hit{rel.x - v.x * dt * (1.0f - s), rel.y - v.y * dt * (1.0f - s), rel.z - v.z * dt * (1.0f - s)};
            if (std::fabs(dot(hit, ax)) <= halfW && std::fabs(dot(hit, az)) <= halfD) {
                collide(op, i, n, -d);
            }
        }
        break;
    }
    case OpKind::CollisionBounds: {
        if (!m_haveScene) {
            break;
        }
        const float bounce = std::max(0.0f, c.number("bouncefactor", 1.0f, m_props));
        const float limit[2] = {m_sceneSize.x, m_sceneSize.y};
        for (size_t i = 0; i < m_count; ++i) {
            Vec3 p = transformPoint(m_toScene, position(i));
            Vec3 v = transformVector(m_toScene, velocity(i));
            bool hit = false, gone = false;
            for (int a = 0; a < 2 && !gone; ++a) {
                const float x = a == 0 ? p.x : p.y, vx = a == 0 ? v.x : v.y;
                const Vec3 axis{a == 0 ? 1.0f : 0.0f, a == 1 ? 1.0f : 0.0f, 0.0f};
                if (x < 0.0f && x - vx * dt >= -kTolerance) {
                    gone = respond(op.behavior, bounce, p, v, axis, -x);
                    hit = true;
                } else if (x > limit[a] && x - vx * dt <= limit[a] + kTolerance) {
                    gone = respond(op.behavior, bounce, p, v, {-axis.x, -axis.y, 0.0f}, x - limit[a]);
                    hit = true;
                }
            }
            if (gone) {
                at(Col::Age, i) = std::max(at(Col::Age, i), at(Col::Lifetime, i));
            } else if (hit) {
                const Vec3 back = transformPoint(m_fromScene, p), vel = transformVector(m_fromScene, v);
                at(Col::X, i) = back.x;
                at(Col::Y, i) = back.y;
                at(Col::Z, i) = back.z;
                at(Col::VX, i) = vel.x;
                at(Col::VY, i) = vel.y;
                at(Col::VZ, i) = vel.z;
                if (op.behavior == ParticleCollisionBehavior::Stop) {
                    at(Col::AngX, i) = at(Col::AngY, i) = at(Col::AngZ, i) = 0.0f;
                }
            }
        }
        break;
    }
    default:
        break;
    }
}

// vortex_v2: the vortex's push along the circle around the axis (`axis`, z) through control point `controlpoint` (plus `offset`), at
// every distance: `speedinner` up to `distanceinner`, `speedouter` from `distanceouter` on, by distance in between (units/s added each
// second). `centerforce` pulls towards the axis (units/s^2). Flags: 1 measures the distance from the axis line (an infinite axis), else
// from the centre point; 2 keeps the distance (the velocity across the circle is removed and the turn is kept on the circle); 4 is the
// ring shape: particles within `ringpulldistance` of the ring (`ringradius` out, `ringwidth` wide) are steered onto it, their speed
// across it moving towards closing the gap at `ringpullforce` (5) per second of the gap, and fading inside the ring's width.
void ParticleSystem::runVortexV2(const Operator &op, float dt)
{
    const ParticleComponent &c = *op.c;
    const Vec3 &cpPos = m_controlPoints[static_cast<size_t>(std::clamp(c.integer("controlpoint", 0), 0, kControlPoints - 1))];
    const Vec3 o = c.vector("offset", {0.0f, 0.0f, 0.0f}, m_props);
    const Vec3 centre{cpPos.x + o.x, cpPos.y + o.y, cpPos.z + o.z};
    const Vec3 axis = normalised(c.vector("axis", {0.0f, 0.0f, 1.0f}, m_props), {0.0f, 0.0f, 0.0f});
    if (length(axis) < 0.5f) {
        return;
    }
    const float inner = c.number("distanceinner", 0.0f, m_props);
    const float outer = c.number("distanceouter", 0.0f, m_props);
    const float speedInner = c.number("speedinner", 0.0f, m_props);
    const float speedOuter = c.number("speedouter", 0.0f, m_props);
    const float centerForce = c.number("centerforce", 0.0f, m_props);
    const int flags = c.integer("flags", 0);
    const bool infinite = (flags & 1) != 0, keep = (flags & 2) != 0, ring = (flags & 4) != 0;
    const float ringRadius = c.number("ringradius", 0.0f, m_props);
    const float ringHalf = std::max(0.0f, c.number("ringwidth", 0.0f, m_props)) * 0.5f;
    const float pullDistance = c.number("ringpulldistance", 1.0e9f, m_props);
    const float pullForce = std::max(0.0f, c.number("ringpullforce", 5.0f, m_props));
    for (size_t i = 0; i < m_count; ++i) {
        const float w = op.blend.in || op.blend.out ? op.blend.weight(at(Col::Lifetime, i) > 0.0f ? at(Col::Age, i) / at(Col::Lifetime, i) : 1.0f) : 1.0f;
        const Vec3 d{at(Col::X, i) - centre.x, at(Col::Y, i) - centre.y, at(Col::Z, i) - centre.z};
        const float along = dot(d, axis);
        const Vec3 radial{d.x - axis.x * along, d.y - axis.y * along, d.z - axis.z * along};
        const float dist = length(radial);
        if (dist < 1e-3f) {
            continue;
        }
        const Vec3 out{radial.x / dist, radial.y / dist, radial.z / dist};
        const Vec3 around{axis.y * out.z - axis.z * out.y, axis.z * out.x - axis.x * out.z, axis.x * out.y - axis.y * out.x};
        const float push = (speedInner + (speedOuter - speedInner) * ramp(infinite ? dist : length(d), inner, outer)) * dt * w;
        Vec3 v{at(Col::VX, i) + around.x * push - out.x * centerForce * dt * w, at(Col::VY, i) + around.y * push - out.y * centerForce * dt * w,
               at(Col::VZ, i) + around.z * push - out.z * centerForce * dt * w};
        if (keep) {
            // no speed across the circle, but the inward speed that brings the next step's straight move back onto it
            const float across = dot(v, out);
            const float turning = dot(v, around) * dt;
            const float inward = dt > 0.0f ? (std::sqrt(dist * dist + turning * turning) - dist) / dt : 0.0f;
            const float k = (across + inward) * w;
            v = {v.x - out.x * k, v.y - out.y * k, v.z - out.z * k};
        }
        if (ring) {
            const float gap = dist - ringRadius;
            if (std::fabs(gap) < pullDistance) {
                const float excess = std::fabs(gap) > ringHalf ? gap - std::copysign(ringHalf, gap) : 0.0f;
                const float across = dot(v, out);
                const float wanted = -excess * pullForce;
                const float k = std::min(1.0f, 10.0f * dt) * w;
                const float change = (wanted - across) * k;
                v = {v.x + out.x * change, v.y + out.y * change, v.z + out.z * change};
            }
        }
        at(Col::VX, i) = v.x;
        at(Col::VY, i) = v.y;
        at(Col::VZ, i) = v.z;
    }
}

} // namespace murasaki::scene
