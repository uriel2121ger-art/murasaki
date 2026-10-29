#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace murasaki::puppet {

// The values a puppet is driven by (docs/pets/puppets.md, sections 3 and 4): the parameters of a model, the opacity of its parts, and the small amounts of "life" the engine adds on
// its own (breathing, blinking). Pure: no Qt, no clock, no randomness except a seeded generator, so a run is reproducible.
struct Vec2 {
    double x = 0;
    double y = 0;
};

// xorshift64*: small, seedable, the same on every machine.
class Rng
{
public:
    explicit Rng(std::uint64_t seed) : m_state(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    std::uint64_t next()
    {
        m_state ^= m_state >> 12;
        m_state ^= m_state << 25;
        m_state ^= m_state >> 27;
        return m_state * 0x2545F4914F6CDD1Dull;
    }
    double uniform() { return static_cast<double>(next() >> 11) / 9007199254740992.0; } // [0, 1)

private:
    std::uint64_t m_state;
};

struct Parameter {
    std::string id;
    double min = 0;
    double def = 0;
    double max = 1;
    double value = 0;
};

class ParameterSet
{
public:
    // False (nothing added) for a repeated id, a range that is not finite or has min > max or a default outside it, an empty or over-long id, or more than kMaxParameters.
    bool add(std::string id, double min, double def, double max);
    int find(std::string_view id) const; // -1 when absent
    size_t size() const { return m_items.size(); }
    // Bounds-checked: an index past the end gives a parameter that is not in the set (empty id, range 0 to 0), never memory that is not ours.
    const Parameter &at(size_t index) const;
    double value(int index) const { return index >= 0 && static_cast<size_t>(index) < m_items.size() ? m_items[static_cast<size_t>(index)].value : 0.0; }
    // Sets a value clamped to the parameter's range; a value that is not finite is ignored. False for an unknown index.
    bool set(int index, double value);
    bool setById(std::string_view id, double value);
    void resetToDefaults();
    std::vector<double> snapshot() const;
    bool restore(const std::vector<double> &values); // false (nothing changed) if the size differs

private:
    std::vector<Parameter> m_items;
};

// The opacity of the parts of a model, 0 to 1.
class PartSet
{
public:
    bool add(std::string id); // false for a repeated or empty id or more than kMaxParts
    int find(std::string_view id) const;
    size_t size() const { return m_ids.size(); }
    const std::string &id(size_t index) const;
    double opacity(int index) const { return index >= 0 && static_cast<size_t>(index) < m_opacity.size() ? m_opacity[static_cast<size_t>(index)] : 0.0; }
    bool set(int index, double opacity); // clamped to 0..1, non-finite ignored
    void resetToOpaque();

private:
    std::vector<std::string> m_ids;
    std::vector<double> m_opacity;
};

// Breathing: a slow sine on the standard parameter ParamBreath (docs section 4): it moves between the parameter's minimum and maximum, centred, once every `period` seconds.
struct BreathConfig {
    double period = 3.2;
};

class Breath
{
public:
    explicit Breath(BreathConfig config = {}) : m_config(config) {}
    void update(double dt); // negative or non-finite dt is ignored
    double phase() const { return m_time; }
    // Writes ParamBreath when the model has it; true if it did.
    bool apply(ParameterSet &parameters) const;

private:
    BreathConfig m_config;
    double m_time = 0;
};

// Automatic blinking: after a random wait between minInterval and maxInterval seconds the eyes close, stay closed, and open again. The waits come from a seeded generator, so the same
// seed and the same elapsed time give the same blinks, whatever the size of the steps.
struct BlinkConfig {
    double minInterval = 2.0;
    double maxInterval = 6.0;
    double closeTime = 0.10;
    double holdTime = 0.05;
    double openTime = 0.15;
};

class AutoBlink
{
public:
    AutoBlink(std::uint64_t seed, BlinkConfig config = {});
    void update(double dt);
    // 1 eyes fully open .. 0 closed.
    double openness() const;
    // Multiplies each named eye parameter (that exists) by the openness; returns how many it touched.
    int apply(ParameterSet &parameters, const std::vector<std::string> &eyeIds) const;

private:
    enum class Phase { Waiting, Closing, Closed, Opening };
    void startWait();

    BlinkConfig m_config;
    Rng m_rng;
    Phase m_phase = Phase::Waiting;
    double m_left = 0; // seconds left in the phase
};

} // namespace murasaki::puppet
