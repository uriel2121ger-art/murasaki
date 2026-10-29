#pragma once

// Where the pets' mixer meets the system: one playback stream of interleaved stereo float at 48 kHz, filled by a real-time callback. Three
// implementations: PipeWire (the real one), and two for tests and for sessions without sound: a paced one that renders and throws the samples away
// (so the mixer's clock, events and idle exit are exercised exactly as with a real device), and none.

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace murasaki::audio {

class Player
{
public:
    // Fills `stereo` with `frames` frames; runs on the player's own real-time thread. Returns whether anything was audible.
    using Render = std::function<bool(float *stereo, std::size_t frames)>;

    virtual ~Player() = default;
    virtual bool start(Render render, std::string *error) = 0;
    // Corks (false) or uncorks the stream: a stream with nothing to say costs the system no wakeups.
    virtual void setActive(bool active) = 0;
    virtual void stop() = 0;
    // The device's latency in nanoseconds as far as it is known (0: unknown); the mixer places a scheduled start that much earlier.
    virtual long long latencyNs() const { return 0; }
    // The stream broke after it started (PipeWire restarted, the device went): nothing more will play from this player. The owner ends the process, so
    // that whoever started it starts a new one.
    virtual bool dead() const { return false; }
};

// Null when this build has no PipeWire.
std::unique_ptr<Player> makePipeWirePlayer();
// Renders on a timer in steps of `stepFrames` and discards the output; always available.
std::unique_ptr<Player> makePacedPlayer(std::size_t stepFrames = 480);

} // namespace murasaki::audio
