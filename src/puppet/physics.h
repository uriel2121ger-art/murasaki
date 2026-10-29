#pragma once

#include "puppet/params.h"

#include <string>
#include <string_view>
#include <vector>

namespace murasaki::puppet {

// Cubism physics (docs/pets/puppets.md, sections 3.3 and 15): rigs of pendulums (hair, clothes, ribbons) that read some parameters, swing, and write some others. The format
// lists the pieces and their numbers but gives no equation (Q-F5): the integrator here is ours, tuned to the properties the format does state (ranges, weights as percentages,
// a chain that hangs along gravity at rest), and written down in section 15.
enum class IoType { X, Angle };

struct PhysicsInput {
    std::string id;
    double weight = 100; // percent
    IoType type = IoType::X;
    bool reflect = false;
};

struct PhysicsOutput {
    std::string id;
    int vertex = 1; // the particle whose segment to its parent is read: 1 .. vertices - 1
    double scale = 1;
    double weight = 100; // percent
    IoType type = IoType::Angle;
    bool reflect = false;
};

struct PhysicsVertex {
    Vec2 position;
    double mobility = 1;
    double delay = 1;
    double acceleration = 1;
    double radius = 0; // the length of the segment to the parent; when 0 the distance between the listed positions is used
};

struct Normalization {
    double min = -1;
    double def = 0;
    double max = 1;
};

struct PhysicsRig {
    std::string id;
    std::vector<PhysicsInput> inputs;
    std::vector<PhysicsOutput> outputs;
    std::vector<PhysicsVertex> vertices;
    Normalization position;
    Normalization angle; // degrees
};

struct PhysicsSettings {
    double fps = 60;
    Vec2 gravity{0, -1};
    Vec2 wind{0, 0};
    std::vector<PhysicsRig> rigs;
};

struct PhysicsResult {
    bool ok = false;
    PhysicsSettings settings;
    std::string error;
};

// Reads a `*.physics3.json`: Version 3; bounded counts (puppetlimits.h); numbers finite; weights from 0 to 100; Fps from 1 to 240 (default 60); mobility 0 to 1; delay and
// acceleration from 0 to 100; radius from 0 to 1000; ranges with min <= default <= max; types X or Angle; targets "Parameter". The counts in Meta are not trusted.
PhysicsResult parsePhysics(std::string_view text);

class Physics
{
public:
    Physics(const Physics &) = delete; // the rigs point into m_settings
    Physics &operator=(const Physics &) = delete;
    Physics(Physics &&) = default;
    Physics &operator=(Physics &&) = default;

    // Resolves every rig's parameters against `parameters`. A rig that names a parameter the model does not have, or an output vertex it does not own, is skipped and named in skipped().
    Physics(PhysicsSettings settings, const ParameterSet &parameters);

    // Reads the inputs from `parameters`, advances the rigs by dt seconds in fixed steps of 1/fps (at most kMaxPhysicsStepsPerUpdate, the rest of a long step is dropped) and adds each
    // output's swing to its parameter. A negative or non-finite dt does nothing, and so does a parameter set of another size than the one the rigs were bound to.
    void update(ParameterSet &parameters, double dt);
    // Back to rest (the rigs start again from the next inputs without a jolt).
    void reset();

    // "puppet: physics rig skipped: <rig id> (<why>)" for each rig that is not simulated.
    const std::vector<std::string> &skipped() const { return m_skipped; }
    size_t activeRigs() const { return m_rigs.size(); }
    // Total kinetic-ish measure for tests and diagnostics: the sum of squared particle speeds.
    double energy() const;

private:
    struct Particle {
        Vec2 position;
        Vec2 velocity;
    };
    struct BoundInput {
        int index;
        const PhysicsInput *input;
    };
    struct BoundOutput {
        int index;
        const PhysicsOutput *output;
    };
    struct Rig {
        const PhysicsRig *source;
        std::vector<BoundInput> inputs;
        std::vector<BoundOutput> outputs;
        std::vector<Particle> particles;
        std::vector<double> lengths; // segment lengths, [0] unused
        double baseX = 0;
        double baseAngle = 0;
        bool started = false;
    };

    void step(Rig &rig, double baseX, double baseAngle, double h) const;
    void readInputs(const Rig &rig, const ParameterSet &parameters, double &baseX, double &baseAngle) const;
    void writeOutputs(const Rig &rig, ParameterSet &parameters) const;
    void initialise(Rig &rig, double baseX, double baseAngle) const;

    PhysicsSettings m_settings;
    std::vector<Rig> m_rigs;
    std::vector<std::string> m_skipped;
    double m_accumulator = 0;
    size_t m_parameterCount = 0;
};

} // namespace murasaki::puppet
