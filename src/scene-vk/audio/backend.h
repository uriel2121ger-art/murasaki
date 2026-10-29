#pragma once

// Where audio meets the system: a playback stream for the scene's sounds and a capture stream of what the default
// output is playing (for the spectrum). The callbacks run on the backend's own real-time thread. The PipeWire backend is
// the real one; tests plug in a fake that calls the callbacks by hand.

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace murasaki::scene {

class AudioBackend
{
public:
    // Both formats are interleaved stereo float at 48 kHz.
    using Pull = std::function<void(float *stereo, size_t frames)>;        // fill the output
    using Push = std::function<void(const float *stereo, size_t frames)>; // receive captured audio

    virtual ~AudioBackend() = default;
    virtual bool startPlayback(Pull pull, std::string *error) = 0;
    virtual bool startCapture(Push push, std::string *error) = 0;
    virtual void stopPlayback() = 0;
    virtual void stopCapture() = 0;
    // Corks (false) or uncorks the playback stream: a stream that has nothing to say (volume 0, paused) costs the system no wakeups.
    virtual void setPlaybackActive(bool) {}
};

// The PipeWire backend; null when the engine was built without PipeWire.
std::unique_ptr<AudioBackend> makePipeWireBackend();

} // namespace murasaki::scene
