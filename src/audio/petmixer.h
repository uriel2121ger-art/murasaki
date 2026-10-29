#pragma once

// The mixer of the pets' sounds (docs/pets/audio.md): up to kMaxPetAudioVoices voices, one per pet, over clips of normalised audio (PCM 16-bit,
// stereo, 48 kHz). Pure and Qt-free. Two sides: the control side (any thread; commands are queued) and render() (the audio thread: no allocation,
// no blocking lock, no I/O). A start can be scheduled for a target time on a monotonic clock; render() is told when its buffer begins on that clock,
// so a start lands on the sample, whatever the latency of the command's path.

#include "petaudiolimits.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace murasaki::audio {

inline constexpr int kRate = kPetAudioRate;
inline constexpr int kMaxPets = 256;              // pet ids are small integers the controller assigns
inline constexpr int kMaxPendingStartsPerPet = 2;  // scheduled starts that may wait for their time, per pet
inline constexpr int kRampFrames = kRate * 5 / 1000;   // 5 ms: starts, stops, pauses and replacements never click
inline constexpr std::int64_t kMaxLateNs = 100'000'000; // a scheduled start later than this is dropped, not played out of time

// Interleaved stereo 16-bit frames. The helper backs it with a mapped file, the tests with a vector.
class Clip
{
public:
    virtual ~Clip() = default;
    virtual const std::int16_t *data() const = 0;
    virtual std::size_t frames() const = 0;
};

class VectorClip final : public Clip
{
public:
    explicit VectorClip(std::vector<std::int16_t> samples) : m_samples(std::move(samples)) {}
    const std::int16_t *data() const override { return m_samples.data(); }
    std::size_t frames() const override { return m_samples.size() / 2; }

private:
    std::vector<std::int16_t> m_samples;
};

struct PlayCommand {
    int pet = 0;
    unsigned generation = 0;       // older than the pet's last one: dropped
    std::shared_ptr<const Clip> clip;
    std::size_t startFrame = 0;    // where in the clip it begins (a negative offset)
    float gain = 1;                // 0..1
    bool loop = false;
    std::int64_t atNs = 0;         // monotonic ns to start at; 0 or in the past: now
};

struct Event {
    enum class Kind { Started, Ended, Dropped } kind = Kind::Ended;
    int pet = 0;
    unsigned generation = 0;
    std::int64_t timeNs = 0; // Started: when its first sample is heard, on the clock of atNs
};

class PetMixer
{
public:
    PetMixer();

    // ---- control side
    void play(PlayCommand command);
    void stop(int pet);
    // Drops the pet's scheduled start that has not begun (a sound already playing goes on).
    void cancelPending(int pet);
    void pause(int pet);
    void resume(int pet);
    void setGain(int pet, float gain);   // a live volume change: ramped, the voice goes on
    void setMaster(float gain);          // 0..1, ramped
    // Events since the last call (voices that started, ended or were dropped for being late), oldest first.
    std::vector<Event> takeEvents();

    // ---- audio side
    // Writes `frames` interleaved stereo floats; `bufferStartNs` is when the first one is heard, on the clock `atNs` uses. Returns whether anything
    // was audible (false: the buffer is exact silence and the caller may cork the stream).
    bool render(float *out, std::size_t frames, std::int64_t bufferStartNs);

    // Voices playing, paused or pending: 0 means the stream can be corked. Safe from any thread.
    int voices() const { return m_voices.load(std::memory_order_relaxed) + m_queued.load(std::memory_order_relaxed); }
    // Voices that make the stream worth keeping open: playing, scheduled or about to be started, not those that are only paused. 0: cork it.
    int active() const { return m_active.load(std::memory_order_relaxed) + m_queued.load(std::memory_order_relaxed); }

private:
    struct Command {
        enum class Kind { Play, Stop, Cancel, Pause, Resume, Gain, Master } kind = Kind::Stop;
        PlayCommand play;
        int pet = 0;
        float value = 0;
    };
    struct Voice {
        std::shared_ptr<const Clip> clip;
        int pet = -1;
        unsigned generation = 0;
        std::size_t position = 0;
        float gain = 1;       // the target
        float current = 0;    // what is applied now, ramped
        float fade = 0;       // 0..1 envelope for starts, stops and pauses
        float fadeTarget = 1;
        bool loop = false;
        bool pending = false; // waiting for atNs
        bool paused = false;
        bool stopping = false;
        bool announced = false;
        std::int64_t atNs = 0;
    };
    static constexpr int kSlots = 2 * kMaxPetAudioVoices; // the active ones and those fading out after a replacement

    void post(Command command);
    void apply(const Command &command, std::int64_t bufferStartNs);
    void applyPlay(const PlayCommand &command, std::int64_t bufferStartNs);
    template <typename F> void eachVoice(int pet, F &&f)
    {
        for (Voice &v : m_slots)
            if (v.pet == pet && v.clip && !v.stopping)
                f(v);
    }
    bool hasVoice(int pet) const
    {
        for (const Voice &v : m_slots)
            if (v.pet == pet && v.clip && !v.stopping)
                return true;
        return false;
    }
    void finish(Voice &voice, bool ended);
    void push(Event::Kind kind, int pet, unsigned generation, std::int64_t timeNs = 0);

    std::mutex m_commandLock;
    std::vector<Command> m_commands;           // reserved up front
    std::array<Voice, kSlots> m_slots{};
    std::array<unsigned, kMaxPets> m_generation{};
    float m_master = 1;
    float m_masterCurrent = 1;
    std::atomic<int> m_voices{0};
    std::atomic<int> m_active{0};
    std::atomic<int> m_queued{0}; // commands waiting for the next render(): a corked stream must be woken for any of them (a resume, a stop, a cancel as much as a play)
    // Events travel from the audio thread to the control side through a ring that needs no lock (one producer, one consumer); when it is full the
    // newest are lost, which only a controller that never reads can cause.
    static constexpr std::size_t kEventRing = 1024;
    std::array<Event, kEventRing> m_ring{};
    std::atomic<std::size_t> m_ringHead{0}; // written by the producer
    std::atomic<std::size_t> m_ringTail{0}; // written by the consumer
    std::vector<Command> m_scratch;            // the commands being applied, reserved up front
};

} // namespace murasaki::audio
