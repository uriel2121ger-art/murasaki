#include "petmixer.h"

#include <algorithm>
#include <cmath>

namespace murasaki::audio {

namespace {

constexpr std::size_t kReserve = 512;
constexpr float kRampStep = 1.0f / float(kRampFrames);

// A smooth ceiling: identity up to 0.9, then a curve that reaches 1.0 only asymptotically.
inline float softLimit(float x)
{
    const float a = std::fabs(x);
    if (a <= 0.9f)
        return x;
    const float limited = 0.9f + 0.1f * std::tanh((a - 0.9f) * 10.0f);
    return x < 0 ? -limited : limited;
}

} // namespace

PetMixer::PetMixer()
{
    m_commands.reserve(kReserve);
    m_scratch.reserve(kReserve);
}

void PetMixer::post(Command command)
{
    std::lock_guard lock(m_commandLock);
    if (m_commands.size() >= kReserve) // a flood is dropped, never grown on the audio thread's account
        return;
    m_commands.push_back(std::move(command));
    m_queued.fetch_add(1, std::memory_order_relaxed);
}

void PetMixer::play(PlayCommand command)
{
    if (command.pet < 0 || command.pet >= kMaxPets || !command.clip)
        return;
    command.gain = std::clamp(command.gain, 0.0f, 1.0f);
    post(Command{Command::Kind::Play, std::move(command), 0, 0});
}

void PetMixer::stop(int pet) { post(Command{Command::Kind::Stop, {}, pet, 0}); }
void PetMixer::cancelPending(int pet) { post(Command{Command::Kind::Cancel, {}, pet, 0}); }
void PetMixer::pause(int pet) { post(Command{Command::Kind::Pause, {}, pet, 0}); }
void PetMixer::resume(int pet) { post(Command{Command::Kind::Resume, {}, pet, 0}); }
void PetMixer::setGain(int pet, float gain) { post(Command{Command::Kind::Gain, {}, pet, std::clamp(gain, 0.0f, 1.0f)}); }
void PetMixer::setMaster(float gain) { post(Command{Command::Kind::Master, {}, 0, std::clamp(gain, 0.0f, 1.0f)}); }

std::vector<Event> PetMixer::takeEvents()
{
    std::vector<Event> out;
    std::size_t tail = m_ringTail.load(std::memory_order_relaxed);
    const std::size_t head = m_ringHead.load(std::memory_order_acquire);
    while (tail != head) {
        out.push_back(m_ring[tail % kEventRing]);
        ++tail;
    }
    m_ringTail.store(tail, std::memory_order_release);
    return out;
}

void PetMixer::push(Event::Kind kind, int pet, unsigned generation, std::int64_t timeNs)
{
    const std::size_t head = m_ringHead.load(std::memory_order_relaxed);
    if (head - m_ringTail.load(std::memory_order_acquire) >= kEventRing)
        return;
    m_ring[head % kEventRing] = Event{kind, pet, generation, timeNs};
    m_ringHead.store(head + 1, std::memory_order_release);
}

void PetMixer::finish(Voice &voice, bool ended)
{
    if (ended && voice.announced)
        push(Event::Kind::Ended, voice.pet, voice.generation);
    voice.clip.reset();
    voice.pet = -1;
    voice.stopping = false;
    voice.pending = false;
}

void PetMixer::applyPlay(const PlayCommand &c, std::int64_t bufferStartNs)
{
    if (c.generation < m_generation[std::size_t(c.pet)])
        return; // a newer edit already replaced this one
    m_generation[std::size_t(c.pet)] = c.generation;
    if (c.atNs > 0 && bufferStartNs - c.atNs > kMaxLateNs) {
        push(Event::Kind::Dropped, c.pet, c.generation);
        return;
    }
    if (c.startFrame >= c.clip->frames()) {
        push(Event::Kind::Dropped, c.pet, c.generation); // an offset past the end of the file plays nothing
        return;
    }
    const bool future = c.atNs > bufferStartNs;
    if (future) {
        // A scheduled start waits for its time beside any other scheduled start of the pet (the next wrap is announced before the last one has quite begun, and
        // a withdrawn one is cancelled by `cancel`, not by being overtaken); the sound that is playing goes on until this one begins. Two stand at most (the
        // wrap that is about to come and the one after it): a source that announces more is wrong, and the extra start is refused.
        int standing = 0;
        for (const Voice &v : m_slots)
            standing += v.pet == c.pet && v.clip && v.pending;
        if (standing >= kMaxPendingStartsPerPet) {
            push(Event::Kind::Dropped, c.pet, c.generation);
            return;
        }
    } else {
        for (Voice &v : m_slots) // a start scheduled earlier would stop this sound when it came: it is replaced
            if (v.pet == c.pet && v.clip && v.pending) {
                v.clip.reset();
                v.pet = -1;
                v.pending = false;
            }
        eachVoice(c.pet, [](Voice &old) { // replaced now: the old one fades out in its own slot
            old.stopping = true;
            old.fadeTarget = 0;
        });
    }
    if (!hasVoice(c.pet)) {
        std::array<bool, kMaxPets> seen{};
        int pets = 0;
        for (const Voice &v : m_slots)
            if (v.clip && !v.stopping && v.pet >= 0 && !seen[std::size_t(v.pet)]) {
                seen[std::size_t(v.pet)] = true;
                ++pets;
            }
        if (pets >= kMaxPetAudioVoices) {
            push(Event::Kind::Dropped, c.pet, c.generation);
            return;
        }
    }
    for (Voice &v : m_slots) {
        if (v.clip)
            continue;
        v = Voice{};
        v.clip = c.clip;
        v.pet = c.pet;
        v.generation = c.generation;
        v.position = c.startFrame;
        v.gain = c.gain;
        v.current = c.gain;
        v.fade = 0;
        v.fadeTarget = 1;
        v.loop = c.loop;
        v.pending = future;
        v.atNs = c.atNs;
        return;
    }
    push(Event::Kind::Dropped, c.pet, c.generation); // every slot is busy fading out
}

void PetMixer::apply(const Command &c, std::int64_t bufferStartNs)
{
    switch (c.kind) {
    case Command::Kind::Play:
        applyPlay(c.play, bufferStartNs);
        break;
    case Command::Kind::Stop:
        for (Voice &v : m_slots) {
            if (v.pet != c.pet || !v.clip || v.stopping)
                continue;
            if (v.pending) { // never began: no fade, no Started
                v.clip.reset();
                v.pet = -1;
                v.pending = false;
                continue;
            }
            v.stopping = true;
            v.fadeTarget = 0;
        }
        break;
    case Command::Kind::Cancel:
        for (Voice &v : m_slots)
            if (v.pet == c.pet && v.clip && v.pending) {
                v.clip.reset();
                v.pet = -1;
                v.pending = false;
            }
        break;
    case Command::Kind::Pause:
        eachVoice(c.pet, [](Voice &v) {
            v.paused = true;
            v.fadeTarget = 0;
        });
        break;
    case Command::Kind::Resume:
        eachVoice(c.pet, [](Voice &v) {
            v.paused = false;
            v.fadeTarget = 1;
        });
        break;
    case Command::Kind::Gain:
        eachVoice(c.pet, [&c](Voice &v) { v.gain = c.value; });
        break;
    case Command::Kind::Master:
        m_master = c.value;
        break;
    }
}

bool PetMixer::render(float *out, std::size_t frames, std::int64_t bufferStartNs)
{
    {
        std::unique_lock lock(m_commandLock, std::try_to_lock);
        if (lock.owns_lock() && !m_commands.empty()) {
            m_scratch.swap(m_commands);
            m_commands.clear();
        }
    }
    const int applied = int(m_scratch.size());
    for (const Command &c : m_scratch)
        apply(c, bufferStartNs);
    m_scratch.clear();

    std::fill(out, out + frames * 2, 0.0f);
    bool audible = false;
    for (Voice &v : m_slots) {
        if (!v.clip)
            continue;
        std::size_t first = 0;
        if (v.pending) {
            const std::int64_t wait = v.atNs - bufferStartNs;
            if (wait >= std::int64_t(frames) * 1'000'000'000 / kRate) {
                audible = true; // not yet: the stream must stay open
                continue;
            }
            first = wait <= 0 ? 0 : std::size_t(wait * kRate / 1'000'000'000);
            v.pending = false;
            // This is the moment the sound it replaces ends: fade out the pet's other voices.
            for (Voice &other : m_slots)
                if (&other != &v && other.pet == v.pet && other.clip && !other.stopping && !other.pending) {
                    other.stopping = true;
                    other.fadeTarget = 0;
                }
        }
        if (!v.announced) {
            v.announced = true;
            push(Event::Kind::Started, v.pet, v.generation, bufferStartNs + std::int64_t(first) * 1'000'000'000 / kRate);
        }
        const std::int16_t *samples = v.clip->data();
        const std::size_t length = v.clip->frames();
        bool ended = false;
        for (std::size_t i = first; i < frames; ++i) {
            if (v.fade < v.fadeTarget)
                v.fade = std::min(v.fadeTarget, v.fade + kRampStep);
            else if (v.fade > v.fadeTarget)
                v.fade = std::max(v.fadeTarget, v.fade - kRampStep);
            if (v.fade <= 0.0f && (v.stopping || v.paused)) {
                if (v.stopping) {
                    ended = true;
                    break;
                }
                continue; // paused: silent and still
            }
            audible = true;
            if (v.current != v.gain) {
                const float d = v.gain - v.current;
                v.current += std::clamp(d, -kRampStep, kRampStep);
            }
            const float g = v.current * v.fade * (1.0f / 32768.0f);
            out[2 * i] += float(samples[2 * v.position]) * g;
            out[2 * i + 1] += float(samples[2 * v.position + 1]) * g;
            if (++v.position >= length) {
                if (!v.loop) {
                    ended = true;
                    break;
                }
                v.position = 0;
            }
        }
        if (ended)
            finish(v, !v.stopping); // a stop the controller asked for is not announced as an end
    }
    int count = 0;
    for (const Voice &v : m_slots)
        count += v.clip ? 1 : 0;
    // master: ramped over the buffer, then the limiter
    const float from = m_masterCurrent;
    const float to = m_master;
    for (std::size_t i = 0; i < frames; ++i) {
        const float m = from + (to - from) * (float(i + 1) / float(frames));
        out[2 * i] = softLimit(out[2 * i] * m);
        out[2 * i + 1] = softLimit(out[2 * i + 1] * m);
    }
    m_masterCurrent = to;
    int active = 0;
    for (const Voice &v : m_slots)
        active += v.clip && !v.stopping && (v.pending || !v.paused);
    m_voices.store(count, std::memory_order_relaxed);
    m_active.store(active, std::memory_order_relaxed);
    m_queued.fetch_sub(applied, std::memory_order_relaxed); // after the stores: a reader never sees the plays counted nowhere
    return audible;
}

} // namespace murasaki::audio
