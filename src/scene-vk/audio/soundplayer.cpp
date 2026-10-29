#include "audio/soundplayer.h"

#include "util/log.h"

#include <algorithm>
#include <chrono>

namespace murasaki::scene {

SoundPlayer::SoundPlayer(std::vector<SoundTrack> tracks, std::vector<std::string> *errors)
{
    for (SoundTrack &t : tracks) {
        auto track = std::make_unique<Track>(); // kept even when it cannot play, so indexes match the scene's sound objects
        if (!t.sources.empty()) {
            track->sources = std::move(t.sources);
            track->streams.resize(track->sources.size());
            track->bad.assign(track->sources.size(), false);
        } else {
            std::vector<std::vector<uint8_t>> files;
            files.push_back(std::move(t.mp3));
            for (auto &more : t.more) {
                files.push_back(std::move(more));
            }
            for (auto &bytes : files) {
                std::string error;
                if (auto stream = SoundStream::open(std::move(bytes), kRate, &error)) {
                    track->streams.push_back(std::move(stream));
                } else if (errors) {
                    errors->push_back("sound '" + t.name + "': " + error);
                }
            }
        }
        bool playable = track->streams.size() > 0 && track->sources.empty();
        if (!track->sources.empty()) {
            // a lazy playlist plays when some file opens: the first that does is where it starts
            for (size_t i = 0; i < track->sources.size() && !playable; ++i) {
                if (track->stream(i)) {
                    playable = true;
                    track->current = i;
                } else if (errors) {
                    errors->push_back("sound '" + t.name + "': file " + std::to_string(i + 1) + " does not open");
                }
            }
        }
        if (!playable) {
            track->finished = true;
        } else {
            ++m_usable;
        }
        track->volume = t.volume;
        track->loop = t.loop;
        track->random = t.random;
        track->minDelay = std::max(0.0f, t.minDelay);
        track->maxDelay = std::max(track->minDelay, t.maxDelay);
        track->rng.seed(t.seed ? t.seed : std::random_device{}());
        if (track->random && track->count() > 1) {
            track->current = track->rng() % track->count(); // the first file is a random one too
        }
        m_tracks.push_back(std::move(track));
    }
    m_ring.assign(kRingFrames * 2, 0.0f);
    if (m_usable > 0) {
        m_thread = std::thread([this] { run(); });
    }
}

SoundPlayer::~SoundPlayer()
{
    m_quit = true;
    m_wake.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

// The end of the current file: the next one of the playlist (loop), or a random one after a random rest (random).
SoundStream *SoundPlayer::Track::stream(size_t i)
{
    if (sources.empty()) {
        return i < streams.size() ? streams[i].get() : nullptr;
    }
    if (i >= streams.size() || bad[i]) {
        return nullptr;
    }
    if (!streams[i]) {
        // the previous file goes first: only one is ever in memory
        for (auto &other : streams) {
            other.reset();
        }
        std::unique_ptr<SoundStream> opened;
        if (auto bytes = sources[i]()) {
            opened = SoundStream::open(std::move(*bytes), kRate);
        }
        if (!opened) {
            bad[i] = true;
            return nullptr;
        }
        streams[i] = std::move(opened);
    }
    return streams[i].get();
}

void SoundPlayer::Track::next()
{
    if (random) {
        if (count() > 1) {
            const size_t other = rng() % (count() - 1); // never the same file twice in a row
            current = other >= current ? other + 1 : other;
        }
        const double rest = minDelay + (maxDelay - minDelay) * (static_cast<double>(rng() - std::mt19937::min()) / (static_cast<double>(std::mt19937::max() - std::mt19937::min()) + 1.0));
        silence = static_cast<size_t>(rest * kRate);
    } else {
        current = (current + 1) % count();
    }
    if (SoundStream *s = stream(current)) {
        s->rewind();
    }
}

size_t SoundPlayer::Track::fill(float *stereo, size_t frames)
{
    size_t got = 0;
    size_t emptyReads = 0; // consecutive files that gave nothing: a playlist of empty files must not spin
    while (got < frames && !finished) {
        if (silence > 0) {
            const size_t n = std::min(silence, frames - got);
            std::fill(stereo + got * 2, stereo + (got + n) * 2, 0.0f);
            got += n;
            silence -= n;
            continue;
        }
        SoundStream *playing = stream(current);
        const size_t n = playing ? playing->read(stereo + got * 2, frames - got) : 0;
        got += n;
        if (n > 0) {
            emptyReads = 0;
            continue;
        }
        // the file is over
        if (!loop && !random) {
            finished = true;
        } else if (++emptyReads > count() + 1) {
            finished = true;
        } else {
            next();
        }
    }
    return got;
}

void SoundPlayer::setTrackVolume(size_t track, float volume)
{
    if (track < m_tracks.size()) {
        m_tracks[track]->volume = std::clamp(volume, 0.0f, 4.0f);
    }
}

void SoundPlayer::setMasterVolume(float volume)
{
    m_master = std::clamp(volume, 0.0f, 1.0f);
}

void SoundPlayer::setPaused(bool paused)
{
    m_paused = paused;
    m_wake.notify_all();
}

size_t SoundPlayer::freeFrames() const
{
    return kRingFrames - (m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire));
}

void SoundPlayer::run()
{
    constexpr size_t kChunk = 1024;
    std::vector<float> mix(kChunk * 2), part(kChunk * 2);
    while (!m_quit) {
        const bool idle = m_paused || m_master.load() <= 0.0f; // nobody hears it: no decoding either
        if (idle || freeFrames() < kChunk) {
            std::unique_lock lock(m_mutex);
            m_wake.wait_for(lock, std::chrono::milliseconds(idle ? 200 : 10));
            continue;
        }
        std::fill(mix.begin(), mix.end(), 0.0f);
        for (auto &track : m_tracks) {
            const size_t got = track->finished ? 0 : track->fill(part.data(), kChunk);
            const float v = track->volume.load();
            for (size_t i = 0; i < got * 2; ++i) {
                mix[i] += part[i] * v;
            }
        }
        const size_t w = m_write.load(std::memory_order_relaxed);
        for (size_t i = 0; i < kChunk; ++i) {
            const size_t slot = (w + i) % kRingFrames;
            m_ring[slot * 2] = mix[i * 2];
            m_ring[slot * 2 + 1] = mix[i * 2 + 1];
        }
        m_write.store(w + kChunk, std::memory_order_release);
    }
}

void SoundPlayer::pull(float *out, size_t frames)
{
    if (m_paused) {
        std::fill(out, out + frames * 2, 0.0f);
        return;
    }
    const size_t r = m_read.load(std::memory_order_relaxed);
    const size_t available = m_write.load(std::memory_order_acquire) - r;
    const size_t n = std::min(frames, available);
    const float target = m_master.load();
    for (size_t i = 0; i < frames; ++i) {
        // ramp the gain over a few milliseconds so a volume change does not click
        m_appliedMaster += (target - m_appliedMaster) * 0.002f;
        if (i < n) {
            const size_t slot = (r + i) % kRingFrames;
            out[i * 2] = m_ring[slot * 2] * m_appliedMaster;
            out[i * 2 + 1] = m_ring[slot * 2 + 1] * m_appliedMaster;
        } else {
            out[i * 2] = out[i * 2 + 1] = 0.0f;
        }
    }
    m_read.store(r + n, std::memory_order_release); // the decoder polls every 10 ms: nothing to signal on this thread
}

} // namespace murasaki::scene
