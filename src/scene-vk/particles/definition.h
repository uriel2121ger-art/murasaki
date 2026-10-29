#pragma once

// Particle definitions: `particles/*.json`, the recipe of one particle system. A scene object of type particle names
// one; its `children` name more. The file lists emitters, initializers, operators and renderers, each a name and a
// bag of values (a number, an "x y z" string, or `{"user": property, "value": default}` bound to a user property),
// plus a material, `maxcount`, `starttime`, `flags`, control points and children.
//
// Values are kept as they are written, keyed by lower-case member name, and read with defaults at the point of use
// (`ParticleComponent::number`): the survey of the library (docs/scene-vk/particles.md) shows each type uses few members.
// Positions, directions, velocities and gravity are in scene axes (x right, y up), no flip is needed.

#include "assets/vfs.h"
#include "scene/material.h"
#include "scene/properties.h"
#include "util/math.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace murasaki::scene {

struct ParticleComponent {
    std::string name; // "boxrandom", "lifetimerandom", "movement", "sprite", ...
    std::map<std::string, Constant> values;
    std::map<std::string, std::string> strings; // members written as text ("limitbehavior": "mirror")
    std::map<std::string, std::vector<Vec3>> lists; // members written as arrays (`colorlist` "colors": ["0 0.6 1", ...])
    std::vector<std::string> cutLists;               // array members that had more entries than are kept (256)

    bool has(const std::string &key) const { return values.count(key) != 0; }
    std::string text(const std::string &key, const std::string &fallback = {}) const
    {
        const auto it = strings.find(key);
        return it == strings.end() ? fallback : it->second;
    }
    // A number (the first component of a vector), the fallback when absent. Bound values read the user property.
    float number(const std::string &key, float fallback, const PropertyStore *props = nullptr) const;
    // Up to three components; a single number fills all three ("distancemax": 512).
    Vec3 vector(const std::string &key, Vec3 fallback, const PropertyStore *props = nullptr) const;
    // Numbers are finite and within +-1e9 (a user property may hold anything): what is not finite reads as the fallback.
    int integer(const std::string &key, int fallback) const;
};

struct ParticleControlPoint {
    int id = -1;
    uint32_t flags = 0; // bit 0: follows the pointer, bit 1: offset is in scene space (else relative to the system)
    Vec3 offset;
};

struct ParticleDefinition;

struct ParticleChild {
    std::string type = "static"; // "static", "eventfollow", "eventspawn", "eventdeath"
    std::string file;
    Vec3 origin;
    Vec3 scale{1, 1, 1};
    Vec3 angles;
    int maxCount = 0; // eventfollow: how many follow at most
    float probability = 1.0f; // event children: the chance a parent particle gets a copy (0..1)
    std::shared_ptr<const ParticleDefinition> definition;
};

struct ParticleDefinition {
    std::string file;
    std::string material;       // "materials/presets/ember.json"
    MaterialPass materialPass;  // its first pass (shader, blending, textures, combos, constants)
    uint32_t maxCount = 100;
    float startTime = 0.0f;     // seconds the system has been running when the wallpaper starts
    uint32_t flags = 0;         // bit 2: perspective particles
    std::string animationMode = "sequence"; // sprite sheets: "randomframe", anything else plays the sequence (as WE reads it)
    float sequenceMultiplier = 1.0f;
    std::vector<ParticleComponent> emitters;
    std::vector<ParticleComponent> initializers;
    std::vector<ParticleComponent> operators;
    std::vector<ParticleComponent> renderers; // never empty: a plain sprite renderer when the file names none
    std::vector<ParticleControlPoint> controlPoints;
    std::vector<ParticleChild> children;
    size_t systemCount = 1;     // this system plus every system its children expand to (a child listed twice counts twice)
    std::vector<std::string> unsupported; // reasons, e.g. "particles: operator vortex", no duplicates
    std::vector<std::string> degraded;    // members that are read but only partly honoured (the scene still draws), no duplicates
};

// How the renderer turns sprites: `orientation` "upright" is mode 1, "fixed" 2, anything else (including "screen" and no
// member) 0, screen facing as always. `axis` is the direction the sprite faces (A; all zero or absent: WE's default, 0 1 0);
// right and up complete it to a right-handed basis (B = normalize(cross(y, A)), x when A runs along y; C = cross(A, B)). The
// shaders' g_OrientationRight / Up / Forward are B, C, A, so axis 0 0 1 is the identity and the default lies in the xz plane;
// screen mode keeps the identity whatever the axis.
struct ParticleOrientation {
    int mode = 0;
    Vec3 right{1.0f, 0.0f, 0.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    Vec3 forward{0.0f, 0.0f, 1.0f};
};
ParticleOrientation particleOrientation(const ParticleDefinition &definition);

// Whether an emitter of this system (or of a child) reacts to the audio spectrum (`audioprocessingmode` above 0).
bool particleUsesAudio(const ParticleDefinition &definition);

// The words of `remapvalue` / `remapinitialvalue` (`input`, `output`, `operation`, `transformfunction`), of the collision operators
// (`collisionbehavior`) and of the event inheritance (`inheritvaluefromevent` / `inheritinitialvaluefromevent`), as the editor
// offers them. An absent or unknown word reads as nullopt.
enum class ParticleRemapValue {
    LifetimeFraction, MaxLifetime, Size, Opacity, Speed, Rotation, AngularSpeed, DistanceToControlPoint, PositionBetweenControlPoints,
    Runtime, TimeOfDay, SystemTime, LayerTime,                                                   // scalars
    Color, Position, Velocity, ControlPoint, DeltaToControlPoint, DirectionToControlPoint, LayerOrigin, // vectors
};
std::optional<ParticleRemapValue> particleRemapValue(const std::string &word);
bool particleRemapIsVector(ParticleRemapValue v);
bool particleRemapWritable(ParticleRemapValue v); // whether the simulation can write it as an output
enum class ParticleRemapOperation { Assign, Multiply, Add, Subtract };
std::optional<ParticleRemapOperation> particleRemapOperation(const std::string &word); // "remap" is Assign
enum class ParticleTransformFunction { None, Sine, Square, Saw, Triangle, SimplexNoise, FbmNoise };
std::optional<ParticleTransformFunction> particleTransformFunction(const std::string &word);
enum class ParticleCollisionBehavior { Bounce, Stop, Slide, Delete };
std::optional<ParticleCollisionBehavior> particleCollisionBehavior(const std::string &word);
enum class ParticleEventValue {
    SetColor, MultiplyColor, SetOpacity, MultiplyOpacity, SetColorOpacity, MultiplyColorOpacity, SetVelocity, AddVelocity,
    SetSize, MultiplySize, SetRotation, AddRotation, SetAngularVelocity, AddAngularVelocity,
};
std::optional<ParticleEventValue> particleEventValue(const std::string &word);
// The value an inherit-from-event element copies: the first text member that names one (the member's own name is not known), else
// "setcolor".
ParticleEventValue particleEventValueOf(const ParticleComponent &c);

// The material pass particles are drawn with: the definition's own plus the combos the renderer forces (THICKFORMAT
// for the full vertex layout, SPRITESHEET for a texture with frames, TRAILRENDERER for the trail renderer).
MaterialPass particleMaterialPass(const ParticleDefinition &definition, bool spriteSheet);
// Texture slots a particle pass binds: the sprite, and slot 1 (normal map) when the material names one.
std::vector<int> particleSlots(const MaterialPass &pass);

// The most systems (a file plus its expanded children) one particle object may have: more is refused by --check.
constexpr size_t kMaxParticleSystems = 256;

// The names the simulation implements, for --check and the tests.
bool particleEmitterSupported(const std::string &name);
bool particleInitializerSupported(const std::string &name);
bool particleOperatorSupported(const std::string &name);
bool particleRendererSupported(const std::string &name);

// Loads `file` from the Vfs (and its children, to a depth of four). A file that cannot be read or parsed is an
// error; features the engine lacks are reasons in `unsupported` (children's reasons included).
std::optional<ParticleDefinition> loadParticleDefinition(const Vfs &vfs, const std::string &file, std::string *error = nullptr);

} // namespace murasaki::scene
