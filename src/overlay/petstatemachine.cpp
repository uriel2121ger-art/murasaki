#include "petstatemachine.h"

#include <algorithm>
#include <cmath>

namespace murasaki {

namespace {

// The states whose entries a state may show, in order.
std::vector<PetState> chain(PetState state)
{
    switch (state) {
    case PetState::Idle:
        return {PetState::Idle};
    case PetState::Grabbed:
        return {PetState::Grabbed, PetState::Idle};
    case PetState::Moving:
        return {PetState::Moving, PetState::Grabbed, PetState::Idle};
    case PetState::Released:
        return {PetState::Released, PetState::Idle};
    }
    return {PetState::Idle};
}

bool finite(PetPoint p)
{
    return std::isfinite(p.x) && std::isfinite(p.y);
}

} // namespace

PetStateMachine::PetStateMachine(PetStateTuning tuning, std::uint32_t seed)
    : m_tuning(tuning)
    , m_rng(seed)
{
    recompute(0, false);
    publish();
}

std::int64_t PetStateMachine::clock(std::int64_t nowMs)
{
    m_now = std::max(m_now, nowMs);
    return m_now;
}

bool PetStateMachine::valid(const QString &variant) const
{
    return !variant.isEmpty() && (!m_exists || m_exists(variant));
}

bool PetStateMachine::usable(const PetStateAnimation &animation) const
{
    if (!animation.isSequence())
        return valid(animation.variant);
    return !validEntries(animation.sequence).empty();
}

std::vector<int> PetStateMachine::validEntries(const PetSequence &sequence) const
{
    std::vector<int> entries;
    for (int i = 0; i < sequence.items.size(); ++i) {
        if (valid(sequence.items.at(i)))
            entries.push_back(i);
    }
    return entries;
}

std::optional<PetState> PetStateMachine::ownerOf(PetState state) const
{
    for (const PetState candidate : chain(state)) {
        const auto &animation = m_states.of(candidate);
        if (animation && usable(*animation))
            return candidate;
    }
    return std::nullopt;
}

const PetStateAnimation *PetStateMachine::ownerAnimation() const
{
    if (!m_owner)
        return nullptr;
    const auto &animation = m_states.of(*m_owner);
    return animation ? &*animation : nullptr;
}

int PetStateMachine::pickInitial(const PetSequence &sequence)
{
    const std::vector<int> entries = validEntries(sequence);
    if (entries.empty())
        return -1;
    if (sequence.order == PetSequenceOrder::Linear)
        return entries.front();
    return entries[m_rng() % entries.size()];
}

int PetStateMachine::pickNext(const PetSequence &sequence, int from, bool holdAtEnd)
{
    const std::vector<int> entries = validEntries(sequence);
    if (entries.empty())
        return -1;
    if (sequence.order == PetSequenceOrder::Random) {
        if (entries.size() == 1)
            return entries.front();
        std::vector<int> others;
        for (const int i : entries) {
            if (i != from)
                others.push_back(i);
        }
        return others[m_rng() % others.size()];
    }
    for (const int i : entries) {
        if (i > from)
            return i;
    }
    return holdAtEnd ? -1 : entries.front();
}

int PetStateMachine::effectivePassMs(const PetSequence *sequence) const
{
    // The pass is what ends an animation when the file ends it, or when `once` stops it after one pass.
    const bool once = sequence && sequence->once;
    return m_passMs > 0 && (once || !m_loopsForever) ? m_passMs : 0;
}

int PetStateMachine::hold(const PetSequence &sequence) const
{
    const int pass = effectivePassMs(&sequence);
    return pass > 0 ? pass : sequence.holdMs;
}

void PetStateMachine::restartAnimation(std::int64_t at)
{
    m_seqStart = at;
    m_passMs = 0;
    m_loopsForever = true;
    m_holding = false;
}

// Works out what is shown from the state, the owner and the index. `keepIndex`: a sequence that is already at a valid place stays there.
void PetStateMachine::recompute(std::int64_t at, bool keepIndex)
{
    const std::optional<PetState> owner = ownerOf(m_state);
    const bool sameOwner = owner == m_owner;
    const int previousIndex = m_index;
    m_owner = owner;
    m_missing = false;
    for (const QString &name : m_states.variants())
        m_missing = m_missing || !valid(name);
    if (!owner) {
        m_index = -1;
        m_variant = m_base;
    } else {
        const PetStateAnimation &animation = *ownerAnimation();
        if (!animation.isSequence()) {
            m_index = -1;
            m_variant = animation.variant;
        } else {
            const PetSequence &sequence = animation.sequence;
            int index = -1;
            const int kept = m_kept[size_t(*owner)];
            const bool keptValid = kept >= 0 && kept < sequence.items.size() && valid(sequence.items.at(kept));
            if (sequence.advance == PetSequenceAdvance::Click && keptValid)
                index = kept;
            else if (sameOwner && previousIndex >= 0 && previousIndex < sequence.items.size() && valid(sequence.items.at(previousIndex)))
                index = previousIndex;
            else if (keepIndex && keptValid)
                index = kept;
            else
                index = pickInitial(sequence);
            m_index = index;
            m_variant = index >= 0 ? sequence.items.at(index) : m_base;
            m_kept[size_t(*owner)] = index;
        }
    }
    if (!sameOwner || m_index != previousIndex)
        restartAnimation(at);
}

void PetStateMachine::enter(PetState state, std::int64_t at, bool keepIndex)
{
    m_state = state;
    m_enteredAt = at;
    recompute(at, keepIndex);
}

std::optional<QString> PetStateMachine::peekVariant(PetState state) const
{
    const auto owner = ownerOf(state);
    if (!owner)
        return m_base;
    const PetStateAnimation &animation = *m_states.of(*owner);
    if (!animation.isSequence())
        return animation.variant;
    const PetSequence &sequence = animation.sequence;
    if (sequence.advance == PetSequenceAdvance::Click) {
        const int kept = m_kept[size_t(*owner)];
        if (kept >= 0 && kept < sequence.items.size() && valid(sequence.items.at(kept)))
            return sequence.items.at(kept);
    }
    if (sequence.order == PetSequenceOrder::Random)
        return std::nullopt;
    const std::vector<int> entries = validEntries(sequence);
    if (entries.empty())
        return m_base;
    return sequence.items.at(entries.front());
}

std::vector<PetState> PetStateMachine::likelyNext() const
{
    switch (m_state) {
    case PetState::Idle:
        return {PetState::Grabbed, PetState::Moving}; // (moving when nothing is grabbed: a follower or a dodge)
    case PetState::Grabbed:
        return {PetState::Moving, PetState::Released, PetState::Idle};
    case PetState::Moving:
        return {PetState::Released, PetState::Grabbed, PetState::Idle};
    case PetState::Released:
        return {PetState::Idle};
    }
    return {};
}

QString PetStateMachine::source() const
{
    return m_index >= 0 ? QStringLiteral("sequence:") + QString::number(m_index) : QStringLiteral("variant");
}

void PetStateMachine::publish()
{
    if (m_state != m_pubState || m_variant != m_pubVariant || m_index != m_pubIndex || m_holding != m_pubHolding || m_restarts != m_pubRestarts) {
        m_pubState = m_state;
        m_pubVariant = m_variant;
        m_pubIndex = m_index;
        m_pubHolding = m_holding;
        m_pubRestarts = m_restarts;
        ++m_generation;
    }
}

std::optional<PetStateMachine::Deadline> PetStateMachine::nextDeadline() const
{
    std::optional<Deadline> best;
    if (m_suspended)
        return best;
    const auto offer = [&best](std::int64_t at, Kind kind) {
        if (!best || at < best->at)
            best = Deadline{at, kind};
    };
    if (m_state == PetState::Moving) {
        if (m_dodging)
            offer(m_dodgeUntil, Kind::DodgeEnd);
        else if (m_dragging)
            offer(m_lastMotion + m_tuning.movingIdleMs, Kind::MovingIdle);
    }
    if (m_previewing)
        offer(m_previewUntil, Kind::PreviewEnd);
    if (m_state == PetState::Released) {
        // one pass of the picture that shows (every file loops here, so a pass is what a "poof" is), 1.2 s for one with no length
        const int length = m_passMs > 0 ? std::clamp(m_passMs, m_tuning.releasedMinMs, m_tuning.releasedMaxMs) : m_tuning.releasedFallbackMs;
        offer(m_enteredAt + length, Kind::ReleasedEnd);
    }
    if (const PetStateAnimation *animation = ownerAnimation(); animation && animation->isSequence() && !m_holding) {
        const PetSequence &sequence = animation->sequence;
        if (sequence.advance == PetSequenceAdvance::Auto)
            offer(m_seqStart + hold(sequence), Kind::SequenceAdvance);
        else if (sequence.once && effectivePassMs(&sequence) > 0)
            offer(m_seqStart + effectivePassMs(&sequence), Kind::PassEnd);
    }
    return best;
}

std::optional<std::int64_t> PetStateMachine::nextWakeMs() const
{
    if (const auto deadline = nextDeadline())
        return deadline->at;
    return std::nullopt;
}

void PetStateMachine::settle(std::int64_t nowMs)
{
    if (m_suspended)
        return;
    for (int guard = 0; guard < 16; ++guard) {
        const auto deadline = nextDeadline();
        if (!deadline || deadline->at > nowMs)
            break;
        const std::int64_t at = deadline->at;
        switch (deadline->kind) {
        case Kind::MovingIdle:
            m_ref = m_last;
            enter(PetState::Grabbed, at, false);
            break;
        case Kind::DodgeEnd:
            m_dodging = false;
            enter(PetState::Idle, at, false);
            break;
        case Kind::ReleasedEnd:
            enter(PetState::Idle, at, false);
            break;
        case Kind::PreviewEnd:
            m_previewing = false;
            enter(PetState::Idle, at, false);
            break;
        case Kind::PassEnd:
            m_holding = true;
            break;
        case Kind::SequenceAdvance: {
            const PetSequence &sequence = ownerAnimation()->sequence;
            const bool holdAtEnd = sequence.once && sequence.order == PetSequenceOrder::Linear;
            const int next = pickNext(sequence, m_index, holdAtEnd);
            if (next < 0) {
                m_holding = true; // an automatic linear sequence that plays once stops on its last entry
                break;
            }
            m_index = next;
            m_kept[size_t(*m_owner)] = next;
            m_variant = sequence.items.at(next);
            // A long jump of the clock does not make the sequence run through every entry it missed: one step, then from now.
            const std::int64_t late = nowMs - at;
            restartAnimation(late < hold(sequence) ? at : nowMs);
            break;
        }
        }
    }
    if (m_previewing && m_state != m_previewState)
        m_previewing = false; // the state ended by itself (released): the preview is over with it
}

void PetStateMachine::setStates(const PetStates &states, std::int64_t nowMs)
{
    const std::int64_t now = clock(nowMs);
    if (states == m_states)
        return;
    m_states = states;
    m_kept = {-1, -1, -1, -1};
    m_owner.reset();
    m_index = -1;
    m_dragging = false;
    m_dodging = false;
    m_state = PetState::Idle;
    m_enteredAt = now;
    recompute(now, false);
    restartAnimation(now);
    publish();
}

void PetStateMachine::setBaseVariant(const QString &variant, std::int64_t nowMs)
{
    const std::int64_t now = clock(nowMs);
    if (variant == m_base)
        return;
    m_base = variant;
    const QString before = m_variant;
    recompute(now, true);
    if (m_variant != before)
        restartAnimation(now);
    publish();
}

void PetStateMachine::setVariantExists(std::function<bool(const QString &)> exists, std::int64_t nowMs)
{
    const std::int64_t now = clock(nowMs);
    m_exists = std::move(exists);
    const QString before = m_variant;
    recompute(now, true);
    if (m_variant != before)
        restartAnimation(now);
    publish();
}

void PetStateMachine::setAnimation(int passMs, bool loopsForever, std::int64_t nowMs)
{
    settle(clock(nowMs));
    m_passMs = std::max(0, passMs);
    m_loopsForever = loopsForever;
    settle(m_now);
    publish();
}

void PetStateMachine::press(std::int64_t nowMs, PetPoint pointer, bool startsDrag)
{
    const std::int64_t now = clock(nowMs);
    settle(now);
    if (startsDrag && finite(pointer)) {
        // (a press while the machine still believes in a drag is a new one: the old drag lost its Release)
        m_dragging = true;
        m_dodging = false;
        m_previewing = false;
        m_ref = pointer;
        m_last = pointer;
        m_lastMotion = now;
        enter(PetState::Grabbed, now, false);
    }
    publish();
}

void PetStateMachine::motion(std::int64_t nowMs, PetPoint pointer)
{
    const std::int64_t now = clock(nowMs);
    settle(now);
    if (m_dragging && finite(pointer)) {
        const bool moved = pointer.x != m_last.x || pointer.y != m_last.y;
        m_last = pointer;
        if (m_state == PetState::Grabbed) {
            if (std::hypot(pointer.x - m_ref.x, pointer.y - m_ref.y) >= m_tuning.grabMovePx) {
                m_lastMotion = now;
                enter(PetState::Moving, now, false);
            }
        } else if (m_state == PetState::Moving && moved) {
            m_lastMotion = now;
        }
    }
    publish();
}

void PetStateMachine::release(std::int64_t nowMs)
{
    const std::int64_t now = clock(nowMs);
    settle(now);
    if (m_dragging) {
        m_dragging = false;
        enter(PetState::Released, now, false);
    }
    publish();
}

void PetStateMachine::click(std::int64_t nowMs)
{
    const std::int64_t now = clock(nowMs);
    settle(now);
    // The click moves on the click sequences of idle (what the pet goes back to) and of the state it is in.
    std::vector<PetState> targets = {PetState::Idle, m_state};
    if (m_state == PetState::Idle)
        targets.pop_back();
    bool changed = false;
    bool replay = false;
    for (const PetState target : targets) {
        const auto &animation = m_states.of(target);
        if (!animation || !animation->isSequence() || animation->sequence.advance != PetSequenceAdvance::Click)
            continue;
        const PetSequence &sequence = animation->sequence;
        const int current = m_kept[size_t(target)];
        const int next = pickNext(sequence, current, false);
        if (next < 0)
            continue;
        if (next == current && !sequence.once)
            continue; // one entry that loops: nothing to move on to
        m_kept[size_t(target)] = next;
        changed = true;
        // "Play once on touch": the click plays the pass again, even of the same entry.
        replay = replay || (sequence.once && (target == m_state || m_owner == target));
    }
    if (changed) {
        const QString before = m_variant;
        const int beforeIndex = m_index;
        recompute(now, true);
        if (m_variant != before || m_index != beforeIndex || replay) {
            restartAnimation(now);
            if (replay)
                ++m_restarts;
        }
    }
    publish();
}

void PetStateMachine::dodgeStep(std::int64_t nowMs, int durationMs)
{
    const std::int64_t now = clock(nowMs);
    settle(now);
    const int length = std::clamp(durationMs, 1, m_tuning.dodgeMaxMs);
    if (m_dragging) {
        publish(); // a pet being carried is already grabbed or moving
        return;
    }
    const bool endsPreview = m_previewing; // a real step ends what the GUI was showing, and is shown itself
    m_previewing = false;
    if (endsPreview || m_state == PetState::Idle || (m_state == PetState::Moving && m_dodging)) {
        const bool already = m_state == PetState::Moving && m_dodging;
        m_dodging = true;
        m_dodgeLen = length;
        m_dodgeUntil = std::max(m_dodgeUntil, now + length);
        if (!already)
            enter(PetState::Moving, now, false);
    }
    publish();
}

bool PetStateMachine::preview(PetState state, std::int64_t nowMs, int durationMs)
{
    const std::int64_t now = clock(nowMs);
    settle(now);
    if (m_dragging)
        return false; // the user holds the pet: what they do is not interrupted
    m_dodging = false;
    m_previewing = true;
    m_previewState = state;
    m_previewUntil = now + std::clamp(durationMs, 1, 60000);
    enter(state, now, false);
    publish();
    return true;
}

void PetStateMachine::animationStarted(std::int64_t nowMs)
{
    const std::int64_t now = clock(nowMs);
    if (m_suspended)
        return;
    if (m_state == PetState::Released)
        m_enteredAt = std::max(m_enteredAt, now);
    m_seqStart = std::max(m_seqStart, now);
    if (m_dodging)
        m_dodgeUntil = std::max(m_dodgeUntil, now + m_dodgeLen);
    publish();
}

void PetStateMachine::setSuspended(bool suspended, std::int64_t nowMs)
{
    const std::int64_t now = clock(nowMs);
    if (suspended == m_suspended)
        return;
    m_suspended = suspended;
    if (!suspended) {
        // what was waited for starts again from now: nothing is caught up
        m_enteredAt = now;
        m_lastMotion = now;
        restartAnimation(now);
    }
}

void PetStateMachine::tick(std::int64_t nowMs)
{
    settle(clock(nowMs));
    publish();
}

void PetStateMachine::reset(std::int64_t nowMs)
{
    const std::int64_t now = clock(nowMs);
    m_dragging = false;
    m_dodging = false;
    m_previewing = false;
    if (m_state != PetState::Idle)
        enter(PetState::Idle, now, true);
    publish();
}

} // namespace murasaki
