#pragma once

// Bone physics of puppets: the editor's constraint JSON of a bone, read, and a spring simulation that moves the flagged bones
// about the animated pose. docs/scene-vk/puppets.md ("Bone physics") lists the keys the library's models carry, which are read and
// what is assumed: the keys are measured, their meaning is guessed from their names and from how the models use them.
//
// A simulated bone keeps its own position and angles (in its parent's space) and a velocity for each. Every fixed step of 1/60 s
// they are pulled towards the animated pose by a spring and slowed by damping; gravity (the bone's `ge` flag, the scene's
// `gravitydirection` and `gravitystrength`), the scene's wind and script impulses push them. The bone is drawn at the simulated
// position and angles. The cost is linear in the bones (a few multiplications per bone and step), at most 128 bones.

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "puppet/mdl.h"
#include "util/math.h"

namespace murasaki::scene {

// Hostile input limits and simulation constants.
inline constexpr float kBonePhysicsStep = 1.0f / 60.0f; // the fixed step, as the particles'
inline constexpr double kBonePhysicsCatchUp = 0.1;      // at most this much time is simulated in one frame (6 steps)
inline constexpr int kBonePhysicsMaxSteps = 6;
inline constexpr float kMaxBoneStiffness = 12000.0f;    // clamped so the explicit spring step stays stable (k * step^2 < 4, k < 14400)
inline constexpr float kMaxBoneDamping = 10000.0f;      // the damping is implicit (unconditionally stable), any value is safe
inline constexpr float kGravityUnits = 980.0f; // units/s^2 of a gravity strength of 1 (assumed: 9.8 m/s^2 at 100 units a metre)
inline constexpr float kWindUnits = 1.0f;      // units/s^2 of a wind strength of 1 (assumed; the library's strong winds are 100 to 1000)

// The scene's forces (`general.gravitydirection`, `gravitystrength`, `windenabled`, `windstrength`, `winddirection`).
struct BoneWorld {
    Vec3 gravityDirection{0, -1, 0};
    float gravityStrength = 1.0f;
    bool windEnabled = false;
    Vec3 windDirection{0.707f, 0.707f, 0.0f};
    float windStrength = 1.0f;
};

// `world` with its wind direction taken from scene space into the space of a layer's mesh (`layerWorld` is the layer's node matrix:
// rotation, flips and scale). The bones' own gravity direction (`gd`) is already a mesh-space vector.
BoneWorld boneWorldInLayer(const BoneWorld &world, const Mat4 &layerWorld);

// The constraint JSON of a bone; nullopt for a string that is empty, is not a JSON object or has nothing a simulation reads.
std::optional<BoneConstraint> parseBoneConstraint(std::string_view json);

struct BoneSim {
    bool valid = false; // false: the next step starts from the animated pose, at rest
    Vec3 position, angles;
    Vec3 velocity, angularVelocity;
    Vec3 impulseLinear, impulseAngular; // a script's impulses, taken at the next step
};

// The simulation of one puppet instance (its image layer): per bone state and the fixed step clock.
class BonePhysicsState
{
public:
    BonePhysicsState(size_t bones, uint32_t seed);

    // A script's `applyBonePhysicsImpulse`: `linear` changes the velocity of a translating bone (a rotating one turns about its
    // lever), `angular` the angular velocity. Non-finite values, or a bone out of range, change nothing; the values are clamped.
    void applyImpulse(size_t bone, Vec3 linear, Vec3 angular);
    // `resetBonePhysicsSimulation`: bone `bone`, or every bone with a negative index, goes back to the animated pose at rest.
    void reset(int bone = -1);

    // Starts a frame at `time` seconds: how many fixed steps it advances (0 to kBonePhysicsMaxSteps; a clock that goes back or a
    // stall longer than kBonePhysicsCatchUp does not simulate the gap).
    void beginFrame(double time);
    int steps() const { return m_steps; }

    // Runs this frame's steps for one bone and returns the frame to draw: `target` is the animated pose (local to the parent),
    // `parentWorld` the parent's matrix as drawn, `length` the bone's lever (the distance to its child).
    PuppetFrame step(size_t bone, const BoneConstraint &constraint, const PuppetFrame &target, const Mat4 &parentWorld, const BoneWorld &world);

    size_t size() const { return m_bones.size(); }
    const BoneSim &bone(size_t i) const { return m_bones[i]; }

private:
    std::vector<BoneSim> m_bones;
    uint32_t m_seed;
    bool m_started = false;
    double m_lastTime = 0.0;
    double m_accumulator = 0.0;
    double m_simTime = 0.0;
    int m_steps = 0;
    float m_gust[kBonePhysicsMaxSteps] = {1, 1, 1, 1, 1, 1}; // the wind's strength factor at each step of this frame
};

// The seed of the wind's gusts: MURASAKI_SCENE_VK_SEED when set (a run is then reproducible), else random.
uint32_t bonePhysicsSeed();

} // namespace murasaki::scene
