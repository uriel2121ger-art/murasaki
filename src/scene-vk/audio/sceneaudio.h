#pragma once

// All the audio of one scene: the sound objects through a SoundPlayer into a playback stream, and, when something reads
// the spectrum, a capture stream of the default output into a SpectrumAnalyzer. Capture exists only while a shader or a
// script asks for it, and is closed while the scene is paused. The bridge's volume (0..128) scales every sound.

#include "audio/backend.h"
#include "audio/soundplayer.h"
#include "audio/spectrum.h"

#include <memory>
#include <string>
#include <vector>

namespace murasaki::scene {

class SceneAudio
{
public:
    // `backend` may be null (no audio system): nothing plays and the spectrum stays at zero.
    SceneAudio(std::vector<SoundTrack> tracks, bool wantsSpectrum, std::unique_ptr<AudioBackend> backend);
    ~SceneAudio();

    void setVolume(unsigned bridgeVolume); // 0..128, as the bridge sends it
    void setPaused(bool paused);
    void setTrackVolume(size_t track, float volume);
    // The spectrum smoothed over `dt` seconds since the last call.
    const AudioSpectrum &spectrum(double dt);

    bool wantsSpectrum() const { return m_wantsSpectrum; }
    bool playing() const { return m_playing; }
    const std::vector<std::string> &errors() const { return m_errors; }

private:
    void startCapture();

    std::unique_ptr<AudioBackend> m_backend;
    std::unique_ptr<SoundPlayer> m_player;
    SpectrumAnalyzer m_analyzer;
    bool m_wantsSpectrum = false;
    bool m_playing = false;
    bool m_paused = false;
    unsigned m_volume = 0;       // the bridge's volume; the stream stays corked until it is above 0
    void updatePlayback();
    bool m_capturing = false;
    std::vector<std::string> m_errors;
};

} // namespace murasaki::scene
