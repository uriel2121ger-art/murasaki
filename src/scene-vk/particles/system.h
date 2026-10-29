#pragma once

// The simulation of one particle system, on the CPU, in a structure-of-arrays layout (one float column per
// attribute, so an operator walks one or two contiguous arrays). No GPU and no scene knowledge: the renderer reads
// the columns and uploads them.
//
// Time runs in fixed steps (kStep); advance() takes any elapsed time and runs the whole steps in it, never more than
// kMaxCatchUp seconds at once (a stalled process must not simulate minutes on resume). With the same seed and the
// same calls the result is identical, which the tests rely on. MURASAKI_SCENE_VK_SEED overrides the seed.
//
// Behaviour follows what the library's systems need (docs/scene-vk/particles.md); each emitter, initializer and
// operator below says what it does where it is implemented (system.cpp).

#include "particles/definition.h"
#include "scene/properties.h"
#include "util/math.h"

#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

namespace murasaki::scene {

// What an object in scene.json overrides for its system (`instanceoverride`): multipliers, all 1 by default.
struct ParticleOverrides {
    float alpha = 1.0f;
    float size = 1.0f;
    float lifetime = 1.0f;
    float rate = 1.0f;
    float speed = 1.0f;
    float count = 1.0f;
    Vec3 color{1.0f, 1.0f, 1.0f}; // multiplies every particle's colour (0..1)
    // `controlpointN` of the object: replaces the offset of control point N (in scene space for a point with flag 2)
    std::array<std::optional<Vec3>, 8> controlPoints{};
};

// The multipliers made safe to compute with: a value that is not finite becomes 1, the rest is kept in 0..10000
// (colour 0..1000), which is what user properties and scripts may otherwise push into the simulation.
ParticleOverrides sanitizeOverrides(ParticleOverrides overrides);

enum class ParticleColumn : size_t {
    X, Y, Z,                 // position, in the system's space
    VX, VY, VZ,              // velocity
    RotX, RotY, RotZ,        // rotation (radians)
    AngX, AngY, AngZ,        // angular velocity
    R, G, B, Alpha, Size,
    Age, Lifetime,
    Frame,                   // sprite sheet frame, -1 until chosen
    InitialR, InitialG, InitialB, InitialAlpha, InitialSize,
    AlphaOscFrequency, AlphaOscScale, AlphaOscPhase, AlphaOscBase,
    SizeOscFrequency, SizeOscScale, SizeOscPhase, SizeOscBase,
    PosOscFrequencyX, PosOscFrequencyY, PosOscFrequencyZ,
    PosOscScaleX, PosOscScaleY, PosOscScaleZ,
    PosOscPhaseX, PosOscPhaseY, PosOscPhaseZ,
    OscInitialised,          // bit 0: alpha oscillator, bit 1: size oscillator, bit 2: position oscillator, bit 3: kept distance, bit 4: kept place
    KeptDistance,            // maintaindistancetocontrolpoint: the distance the particle keeps
    KeptPlace,               // maintaindistancebetweencontrolpoints: where along the line it stays (0 at the start, 1 at the end)
    Count
};

// What a particle of a parent system hands to the copies of an event child (`inheritvaluefromevent`,
// `inheritinitialvaluefromevent`), in the parent's space (which is the child's).
struct ParticleEventValues {
    Vec3 position, velocity;
    Vec3 color{1.0f, 1.0f, 1.0f};
    float alpha = 1.0f;
    float size = 20.0f;
    Vec3 rotation, angularVelocity;
};

class ParticleSystem
{
public:
    static constexpr float kStep = 1.0f / 60.0f;
    static constexpr float kMaxCatchUp = 0.1f;
    static constexpr uint32_t kGlobalCap = 150000;
    static constexpr int kControlPoints = 8;
    static constexpr double kWarmUpBudget = 5.0e6;     // particle-steps simulated by warmUp() at most
    static constexpr size_t kMaxTrailStates = 2000000; // history states over all particles of a system (64 MB)

    // `props` (may be null) resolves values bound to user properties; it must outlive the system.
    ParticleSystem(const ParticleDefinition &definition, const ParticleOverrides &overrides, uint32_t seed, const PropertyStore *props = nullptr);

    // Simulates the time the system was running before the wallpaper started (`starttime` of the definition).
    void warmUp();
    // Runs the whole steps in `dt` seconds (plus what is left from the last call).
    void advance(double dt);
    // Control points: those that follow the pointer take `pointerScene` (scene units, y up) through `sceneToSystem`;
    // the others are placed by their offset (scene space ones through the same matrix).
    void updateControlPoints(Vec2 pointerScene, bool pointerKnown, const Mat4 &sceneToSystem);

    // Where the scene is, for `collisionbounds`: the scene's rectangle (0..size, y up, scene units) and the matrices between the
    // system's space and the scene's. Until called the system has no bounds.
    void setSceneSpace(Vec2 size, const Mat4 &systemToScene, const Mat4 &sceneToSystem)
    {
        m_sceneSize = size;
        m_toScene = systemToScene;
        m_fromScene = sceneToSystem;
        m_haveScene = size.x > 0.0f && size.y > 0.0f;
    }
    // The values of the parent particle of an event copy (followers: every frame; spawn and death copies: once).
    void setEventSource(const ParticleEventValues &values)
    {
        m_event = values;
        m_haveEvent = true;
    }
    // The values of live particle `i`, for the event children of this system.
    ParticleEventValues eventValues(size_t i) const;
    // How fast each control point moves (units/s in the system's space), from where it was at the last update.
    const std::array<Vec3, kControlPoints> &controlPointVelocities() const { return m_cpVelocity; }

    // Sprite sheet particles pick a frame each step: `frames` of the texture and the sheet's `duration` in seconds
    // (0: spread the frames over the particle's lifetime).
    void setSpriteSheet(int frames, float duration)
    {
        m_sheetFrames = frames;
        m_sheetDuration = duration;
    }

    // New multipliers for what spawns from now on (rate, speed, alpha, size, lifetime, colour); the pool's size (`count`)
    // and the control point offsets are fixed when the system is made.
    void setOverrides(const ParticleOverrides &overrides)
    {
        const auto controlPoints = m_overrides.controlPoints;
        m_overrides = sanitizeOverrides(overrides);
        m_overrides.controlPoints = controlPoints;
    }
    // The 16-band spectrum (0..1 per band, low to high pitch) emitters with `audioprocessingmode` follow: the emission rate is
    // multiplied by boundsMin + (boundsMax - boundsMin) * pow(level, exponent), the level being the mean of the bands
    // `audioprocessingfrequencystart`..`end` (clamped to 0..15, swapped when reversed) of the channel the mode names (1 left,
    // 2 right, 3 their mean). Until called the spectrum is silence. See docs/scene-vk/particles.md.
    void setAudio(const std::array<float, 16> &left, const std::array<float, 16> &right)
    {
        m_audioLeft = left;
        m_audioRight = right;
    }
    // Stops (or resumes) the emitters; the particles alive carry on. A follower system whose parent particle died stops.
    void setEmitting(bool emitting) { m_emitting = emitting; }
    // Spawns `count` particles at once from the first emitter (IParticleSystem.emitParticles); nothing when it has none.
    void burst(size_t count);
    bool emitting() const { return m_emitting; }
    // Places a control point by hand (a follower system's control point 0 is its parent particle).
    void setControlPoint(int index, Vec3 position)
    {
        if (index >= 0 && index < kControlPoints) {
            m_controlPoints[static_cast<size_t>(index)] = position;
            trackControlPoint(static_cast<size_t>(index));
        }
    }
    // Keeps a history of every particle (rope trails): `points` states, one every `interval` seconds, newest first.
    // trailPoint() is {x, y, z, size, r, g, b, a}.
    void enableTrails(int points, float interval)
    {
        // the history is capacity x points states: a big pool gets a shorter trail rather than hundreds of megabytes
        const size_t room = kMaxTrailStates / std::max<size_t>(1, m_capacity);
        m_trailPoints = static_cast<int>(std::min<size_t>(static_cast<size_t>(std::max(0, points)), room));
        m_trailInterval = std::max(1e-3f, interval);
    }
    int trailCount(size_t i) const { return i < m_trailCounts.size() ? m_trailCounts[i] : 0; }
    const float *trailPoint(size_t i, int k) const { return &m_trailData[(i * static_cast<size_t>(m_trailPoints) + static_cast<size_t>(k)) * 8]; }
    // How far the next history sample is: 0 right after one, towards 1 just before the next.
    float trailPhase() const { return m_trailInterval > 0.0f ? std::min(1.0f, m_trailClock / m_trailInterval) : 0.0f; }
    int trailPoints() const { return m_trailPoints; }

    // Birth number of each live particle (never reused), to tell particles apart as the arrays compact.
    const std::vector<uint32_t> &serials() const { return m_serials; }

    size_t count() const { return m_count; }
    uint32_t capacity() const { return m_capacity; }
    double time() const { return m_time; }
    const std::vector<float> &column(ParticleColumn c) const { return m_columns[static_cast<size_t>(c)]; }
    const ParticleDefinition &definition() const { return m_def; }
    const std::array<Vec3, kControlPoints> &controlPoints() const { return m_controlPoints; }
    // Whether anything can still happen: false once every emitter is done and no particle is alive.
    bool active() const;

private:
    struct EmitterState {
        double timer = 0.0;         // fractional particles owed by the rate
        double delayLeft = 0.0;
        double elapsed = 0.0;       // since the delay ended, for `duration`
        bool instantDone = false;
        bool emitting = true;       // random periodic emission: inside a burst
        double periodTimer = 0.0;
        double periodLength = 0.0;
    };

    float audioResponse(const ParticleComponent &emitter) const;
    void trackControlPoint(size_t index);
    void step(float dt);
    void emit(size_t emitter, float dt);
    void spawn(const ParticleComponent &emitter);
    void runOperators(float dt);
    void compact();
    float uniform(float lo, float hi);
    Vec3 uniform3(Vec3 lo, Vec3 hi);
    float& at(ParticleColumn c, size_t i) { return m_columns[static_cast<size_t>(c)][i]; }

    const ParticleDefinition &m_def;
    ParticleOverrides m_overrides;
    const PropertyStore *m_props;
    std::mt19937 m_rng;
    uint32_t m_capacity = 0;
    size_t m_count = 0;
    double m_time = 0.0;
    double m_pending = 0.0; // time not yet simulated (less than a step)
    std::array<std::vector<float>, static_cast<size_t>(ParticleColumn::Count)> m_columns;
    std::vector<EmitterState> m_emitterStates;
    std::array<Vec3, kControlPoints> m_controlPoints{};
    std::array<bool, kControlPoints> m_pointerLinked{};
    std::array<bool, kControlPoints> m_sceneSpace{};
    std::array<Vec3, kControlPoints> m_cpOffset{};
    bool m_emitting = true;
    std::array<float, 16> m_audioLeft{}, m_audioRight{};
    int m_trailPoints = 0;
    float m_trailInterval = 0.25f;
    float m_trailClock = 0.0f;
    std::vector<float> m_trailData;      // per particle: m_trailPoints states of 8 floats
    std::vector<uint8_t> m_trailCounts;  // states filled in
    std::vector<uint32_t> m_serials;
    uint32_t m_nextSerial = 0;
    int m_sheetFrames = 0;
    float m_sheetDuration = 0.0f;
    enum class InitKind { Color, HsvColor, ColorList, Size, Alpha, Lifetime, Velocity, Rotation, AngularVelocity, TurbulentVelocity, MapSequenceAround,
                          MapSequenceBetween, InheritControlPointVelocity, PositionOffsetRandom, RemapInitialValue, InheritInitialValueFromEvent };
    enum class OpKind { Movement, AngularMovement, AlphaFade, SizeChange, AlphaChange, ColorChange, Turbulence, ControlPointAttract, Vortex, VortexV2,
                        OscillateAlpha, OscillateSize, OscillatePosition, RemapValue, Boids, CapVelocity, MaintainDistance, MaintainBetween,
                        ReduceMovement, CollisionPlane, CollisionSphere, CollisionBox, CollisionQuad, CollisionBounds, InheritValueFromEvent };
    // A remap element, read once: what it reads, what it writes and how (see remap() in system.cpp).
    struct Remap {
        ParticleRemapValue input = ParticleRemapValue::Size;
        ParticleRemapValue output = ParticleRemapValue::Size;
        bool writable = true;
        bool positionNoise = false; // no `input` and a noise function: noise of the position (the rain presets)
        ParticleRemapOperation operation = ParticleRemapOperation::Assign;
        ParticleTransformFunction function = ParticleTransformFunction::None;
        float inputScale = 1.0f, inputMin = 0.0f, inputMax = 1.0f;
        Vec3 outputMin, outputMax{1.0f, 1.0f, 1.0f};
        int cp0 = 0, cp1 = 1;
        bool rebuilt = false; // operators: an earlier operator sets the output again every step (so multiply / add do not pile up)
    };
    // The lifetime window an operator works in (`blendinstart`..`blendinend` it fades in, `blendoutstart`..`blendoutend` out).
    struct Blend {
        bool in = false, out = false;
        float inStart = 0.0f, inEnd = 0.0f, outStart = 1.0f, outEnd = 1.0f;
        float weight(float life) const;
    };
    struct Initializer { InitKind kind; const ParticleComponent *c; int mode = 0; Remap remap{}; ParticleEventValue event = ParticleEventValue::SetColor; }; // mode: map sequences' limit behaviour (0 keep the last, 1 wrap, 2 mirror)
    struct Operator {
        OpKind kind;
        const ParticleComponent *c;
        double phase = 0.0, speed = 0.0; // turbulence: per system, not per particle
        Blend blend{};
        Remap remap{};
        ParticleCollisionBehavior behavior = ParticleCollisionBehavior::Bounce;
        ParticleEventValue event = ParticleEventValue::SetColor;
        bool colorRebuilt = false, alphaRebuilt = false, sizeRebuilt = false; // inheritvaluefromevent: as Remap::rebuilt
    };
    std::vector<Initializer> m_initializers;
    std::vector<Operator> m_operators;
    static Remap readRemap(const ParticleComponent &c, const PropertyStore *props);
    // Applies a remap to particle `i`: `weight` blends the result in, `dt` > 0 for operators (add and subtract of velocities, positions
    // and rotations are then rates per second; multiply of them a factor per second).
    void remap(const Remap &r, size_t i, float weight, float dt);
    float remapScalarInput(const Remap &r, size_t i) const;
    Vec3 remapVectorInput(const Remap &r, size_t i) const;
    void applyEvent(ParticleEventValue v, size_t i, bool initial, bool colorRebuilt, bool alphaRebuilt, bool sizeRebuilt, float dt);
    void collide(const Operator &op, size_t i, Vec3 normal, float depth);
    void runCollision(const Operator &op, float dt);
    void runVortexV2(const Operator &op, float dt);
    Vec2 m_sceneSize{};
    Mat4 m_toScene = Mat4::identity();
    Mat4 m_fromScene = Mat4::identity();
    bool m_haveScene = false;
    ParticleEventValues m_event{};
    bool m_haveEvent = false;
    std::array<Vec3, kControlPoints> m_cpVelocity{};
    std::array<Vec3, kControlPoints> m_cpTracked{};       // where each control point was at the last update
    std::array<double, kControlPoints> m_cpTrackedAt{};   // the simulated time of that update (-1: never)
    std::array<Vec3, kControlPoints> m_cpLastStep{};      // the control points at the end of the last step (maintaindistancebetween)
    bool m_cpLastStepKnown = false;
    bool m_usesTimeOfDay = false; // a remap reads `timeofday`: the local day is read once a step
    float m_dayFraction = 0.0f;
    void runBoids(const ParticleComponent &c, float dt);
    std::vector<std::pair<uint64_t, uint32_t>> m_boidCells; // boids: (grid cell, particle) sorted by cell, kept between steps
    std::vector<Vec3> m_boidSteer;
};

// HSV (all 0..1, the hue wrapping) to RGB 0..1. Exposed for the tests.
Vec3 hsvToRgb(float h, float s, float v);

// 3D curl noise (divergence free, smooth): used by the turbulence operator and initializer. Exposed for the tests.
Vec3 curlNoise(Vec3 p);

// Whether the parent particle born as `serial` gets a copy of an event child with this `probability` (0..1): a fixed draw per particle
// and child (`salt`), so a follower does not come and go from frame to frame.
bool particleEventChosen(uint32_t serial, uint32_t salt, float probability);

// The seed a system gets: MURASAKI_SCENE_VK_SEED (a number) plus `salt` when set, else a random one.
uint32_t particleSeed(uint32_t salt);

} // namespace murasaki::scene
