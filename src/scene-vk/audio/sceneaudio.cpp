#include "audio/sceneaudio.h"

#include "util/log.h"

#include <algorithm>

namespace murasaki::scene {

SceneAudio::SceneAudio(std::vector<SoundTrack> tracks, bool wantsSpectrum, std::unique_ptr<AudioBackend> backend)
    : m_backend(std::move(backend)), m_wantsSpectrum(wantsSpectrum)
{
    if (!m_backend) {
        return;
    }
    if (!tracks.empty()) {
        m_player = std::make_unique<SoundPlayer>(std::move(tracks), &m_errors);
        if (m_player->trackCount() > 0) {
            std::string error;
            m_playing = m_backend->startPlayback([this](float *out, size_t frames) { m_player->pull(out, frames); }, &error);
            if (!m_playing) {
                m_errors.push_back("audio playback: " + error);
            }
            updatePlayback();
        }
    }
    if (m_wantsSpectrum) {
        startCapture();
    }
    for (const std::string &e : m_errors) {
        logError("{}", e);
    }
}

SceneAudio::~SceneAudio()
{
    if (m_backend) {
        m_backend->stopPlayback();
        m_backend->stopCapture();
    }
    m_player.reset(); // after the streams: the callbacks pull from it
}

void SceneAudio::startCapture()
{
    std::string error;
    m_capturing = m_backend->startCapture([this](const float *frames, size_t n) { m_analyzer.push(frames, n); }, &error);
    if (!m_capturing) {
        m_errors.push_back("audio capture: " + error);
        logError("{}", m_errors.back());
    }
}

void SceneAudio::setVolume(unsigned bridgeVolume)
{
    m_volume = std::min(bridgeVolume, 128u);
    if (m_player) {
        m_player->setMasterVolume(static_cast<float>(m_volume) / 128.0f);
        m_player->wake();
    }
    updatePlayback();
}

// The playback stream runs only while someone can hear it.
void SceneAudio::updatePlayback()
{
    if (m_backend && m_playing) {
        m_backend->setPlaybackActive(m_volume > 0 && !m_paused);
    }
}

void SceneAudio::setPaused(bool paused)
{
    if (paused == m_paused) {
        return;
    }
    m_paused = paused;
    if (m_player) {
        m_player->setPaused(paused);
    }
    updatePlayback();
    if (m_wantsSpectrum && m_backend) {
        if (paused) {
            m_backend->stopCapture();
            m_capturing = false;
            m_analyzer.reset();
        } else {
            startCapture();
        }
    }
}

void SceneAudio::setTrackVolume(size_t track, float volume)
{
    if (m_player) {
        m_player->setTrackVolume(track, volume);
    }
}

const AudioSpectrum &SceneAudio::spectrum(double dt)
{
    return m_wantsSpectrum ? m_analyzer.update(dt) : m_analyzer.current();
}

} // namespace murasaki::scene
