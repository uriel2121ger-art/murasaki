#pragma once

// Plays a scene's sound objects: every track is decoded on a thread of its own pace into one ring of stereo float frames
// at 48 kHz (summed, each at its own volume, looping), and the backend's real-time callback only copies out of the ring
// (pull) under the master volume, which starts at 0. Paused, or at volume 0, nothing is decoded and the callback gets silence.

#include "audio/soundstream.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace murasaki::scene {

// Where the bytes of one playlist file come from, asked for when the file's turn comes (and dropped when it is over), so a playlist
// of a hundred files holds one in memory. Returns nothing when the file cannot be read.
using SoundSource = std::function<std::optional<std::vector<uint8_t>>()>;

struct SoundTrack {
    std::string name;
    std::vector<uint8_t> mp3; // the file, in any format SoundStream knows (the name is from the first)
    float volume = 1.0f;
    bool loop = true;
    // The rest of the playlist after `mp3` (a sound object may name several files).
    std::vector<std::vector<uint8_t>> more;
    // The playlist as sources instead: when this is not empty it is the playlist and `mp3` and `more` are not used.
    std::vector<SoundSource> sources;
    // Random mode: a random file of the playlist, then silence of minDelay..maxDelay seconds, and again. (`loop` is
    // ignored: a random track never ends.) The seed makes a run repeatable (tests); 0 picks one.
    bool random = false;
    float minDelay = 0.0f;
    float maxDelay = 0.0f;
    uint32_t seed = 0;
};

class SoundPlayer
{
public:
    static constexpr uint32_t kRate = 48000;

    // Tracks that cannot be decoded are skipped (the reason goes to `errors`).
    explicit SoundPlayer(std::vector<SoundTrack> tracks, std::vector<std::string> *errors = nullptr);
    ~SoundPlayer();

    void setTrackVolume(size_t track, float volume);
    void setMasterVolume(float volume);
    void setPaused(bool paused);
    void wake() { m_wake.notify_all(); }
    // Tracks that decode (a track that does not keeps its place but stays silent).
    size_t trackCount() const { return m_usable; }
    // Frames decoded so far (for tests: decoding stops while paused or at volume 0).
    size_t decodedFrames() const { return m_write.load(std::memory_order_acquire); }

    // Real-time: fills `frames` stereo frames, with silence where the decoder has not caught up or while paused.
    void pull(float *stereo, size_t frames);

private:
    struct Track {
        std::vector<std::unique_ptr<SoundStream>> streams; // the playlist: every file (in memory), or only the current one (sources)
        std::vector<SoundSource> sources;                  // lazy playlist: where each file comes from (empty when the files are in memory)
        std::vector<bool> bad;                             // lazy playlist: files that did not open
        size_t count() const { return sources.empty() ? streams.size() : sources.size(); }
        SoundStream *stream(size_t i); // the file's stream, opened now when it is lazy (null when it cannot be)
        size_t current = 0;
        std::atomic<float> volume{1.0f};
        bool loop = true;
        bool random = false;
        float minDelay = 0.0f, maxDelay = 0.0f;
        size_t silence = 0; // frames of rest left before the next file (random mode)
        std::mt19937 rng;
        bool finished = false;
        // Fills up to `frames` stereo frames from the playlist; fewer only when the track has ended. Silence is zeros.
        size_t fill(float *stereo, size_t frames);
        void next();
    };

    void run();
    size_t freeFrames() const;

    std::vector<std::unique_ptr<Track>> m_tracks;
    size_t m_usable = 0;
    std::vector<float> m_ring; // interleaved stereo, capacity kRingFrames
    static constexpr size_t kRingFrames = 24576;
    std::atomic<size_t> m_write{0}; // frames written, ever
    std::atomic<size_t> m_read{0};  // frames read, ever
    std::atomic<float> m_master{0.0f}; // silent until the first volume arrives (no blast at launch)
    float m_appliedMaster = 0.0f;      // the pull thread's smoothed gain
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_quit{false};
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::thread m_thread;
};

} // namespace murasaki::scene
