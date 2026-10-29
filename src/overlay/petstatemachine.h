#pragma once

// The states of a pet as a pure machine (docs/pets/states.md; plan 4.2): idle, grabbed, moving, released, and the sequence of animations each state shows. No Qt
// GUI, no clock (every input carries `nowMs`, one monotonic clock, never going backwards: an earlier value counts as the latest one seen), no I/O. The window
// code feeds it what PetInteraction already decided (a press that starts a drag, the pointer moving, the release, a dodge step, a click) and applies what it
// answers; PetInteraction itself is not changed and does not know this machine exists.
//
//   - A state shows the animation its `states` entry names; an absent one falls back (moving to grabbed, grabbed to idle, released to idle, idle to the
//     placement's variant), so a pet without `states` shows its variant in every state, as today. A named variant the item lacks (the variantExists predicate)
//     is skipped like an absent one.
//   - Sequences: `linear` plays the entries in order and wraps, `random` picks a valid one that is not the one just played. `auto` moves on after one pass of the
//     animation when the host says it ends (setAnimation: a finite one) or after holdMs; `click` moves on at click(). `once` holds the last frame of a pass
//     (holding()); an automatic linear sequence that is `once` stops on its last entry instead of wrapping.
//   - Entering a state starts its sequence over (linear: the first valid entry, random: a random one), except that a click sequence keeps its place (the click
//     is what moves it) and that reset() keeps the place of idle's sequence. A state that falls back onto the animation already on screen changes nothing.
//   - released lasts one pass of its animation when that is finite, else 1.2 s, and is clamped to a sane range: the machine always comes back to idle.

#include "petgeometry.h"
#include "petstates.h" // src/core: the data

#include <QString>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <vector>

namespace murasaki {

struct PetStateTuning {
    double grabMovePx = 6;      // the pointer this far from where it rested, button down: moving
    int movingIdleMs = 300;     // no motion this long, button still down: back to grabbed
    int releasedFallbackMs = 1200;
    int releasedMinMs = 200;    // a finite pass outside this range is clamped
    int releasedMaxMs = 10000;
    int dodgeMaxMs = 5000;      // a dodge step shows moving for at most this long
};

class PetStateMachine
{
public:
    explicit PetStateMachine(PetStateTuning tuning = {}, std::uint32_t seed = 1);

    // ---- configuration ----
    // Replaces the states; the pet starts again in idle with every sequence at its start. Equal states change nothing.
    void setStates(const PetStates &states, std::int64_t nowMs);
    // The placement's variant: what idle shows when it has no entry of its own.
    void setBaseVariant(const QString &variant, std::int64_t nowMs);
    // Whether the item has a variant of that name. Without one every name counts as existing.
    void setVariantExists(std::function<bool(const QString &)> exists, std::int64_t nowMs);
    // What the host knows about the animation now on screen (variant()): the length of one pass in ms (0: unknown) and whether its file loops forever (the
    // overlay plays every file in a loop, so that is what it says). `released` lasts one pass when it is known (the 1.2 s are for a picture with no length), an
    // automatic sequence waits holdMs for an animation that loops, and `once` ends at the pass. Forgotten whenever the animation changes, so the host calls it
    // again after loading the new file.
    void setAnimation(int passMs, bool loopsForever, std::int64_t nowMs);

    // The first frame of the file that a change of state handed the window is out (it took the window some time to load it): `released`, a sequence's hold, a
    // `once` pass and a dodge step are counted from here and not from the moment the state was entered.
    void animationStarted(std::int64_t nowMs);
    // The pet is hidden, paused, the session is locked or it is being arranged: nothing is due (no deadline, no wake) until it is shown again, and what is waited
    // for then starts from that moment (a sequence does not catch up on the time nobody saw). reset() is the caller's to say too.
    void setSuspended(bool suspended, std::int64_t nowMs);

    // ---- inputs ----
    // `startsDrag`: the press began a drag (an opaque pixel of a pet that can be grabbed); any other press is not an event of this machine.
    void press(std::int64_t nowMs, PetPoint pointer, bool startsDrag);
    void motion(std::int64_t nowMs, PetPoint pointer); // button down; non-finite positions are ignored
    void release(std::int64_t nowMs);
    void click(std::int64_t nowMs);                         // a press and release that was a click (PetInteraction::takeClick), after release()
    void dodgeStep(std::int64_t nowMs, int durationMs);     // PetInteraction::takeMove() started a step that takes this long
    // Shows a state for a while (the "try" button of the GUI), then idle. A press that starts a drag, a dodge step, reset() or the state ending by itself (released)
    // end it. While it lasts moving does not fall back to grabbed (nobody is holding the pet). False, and nothing changes, while the pet is being dragged.
    bool preview(PetState state, std::int64_t nowMs, int durationMs);
    bool dragging() const { return m_dragging; }
    bool previewing() const { return m_previewing; }
    void tick(std::int64_t nowMs);
    // The pet is hidden, paused, the session locks, arrange mode starts, the placement changes: idle (idle's sequence keeps its place).
    void reset(std::int64_t nowMs);

    // ---- outputs ----
    PetState state() const { return m_state; }
    // The variant to show: a name from the states, or the base variant.
    QString variant() const { return m_variant; }
    // The entry of the sequence being played, or -1 when a single variant is.
    int sequenceIndex() const { return m_index; }
    // The state whose entry is being shown (idle when moving falls back on it), or none when it is the base variant.
    std::optional<PetState> owner() const { return m_owner; }
    // `once` and the pass is over: the last frame stays.
    bool holding() const { return m_holding; }
    // The picture the pet would show if it entered `state` now, without entering it: a single variant, the place a click sequence keeps or the first entry of a
    // linear one. None for a random sequence (not known beforehand). For the controller to prepare the picture of the state likely to come next.
    std::optional<QString> peekVariant(PetState state) const;
    // The states a pet in the current one is likely to be in next, most likely first.
    std::vector<PetState> likelyNext() const;
    // "variant" or "sequence:<i>", as Windows() says it.
    QString source() const;
    // Some state names a variant the item lacks.
    bool missingVariant() const { return m_missing; }
    // Grows whenever state(), variant(), sequenceIndex() or holding() changes, or a click replays the entry on screen (the host starts its pass again).
    unsigned generation() const { return m_generation; }
    // How many times a click has made the entry on screen play again (the host starts its picture from the first frame each time).
    unsigned restarts() const { return m_restarts; }
    // When tick() next has something to do, or none.
    std::optional<std::int64_t> nextWakeMs() const;

private:
    enum class Kind { MovingIdle, DodgeEnd, ReleasedEnd, PreviewEnd, SequenceAdvance, PassEnd };
    struct Deadline {
        std::int64_t at;
        Kind kind;
    };

    std::int64_t clock(std::int64_t nowMs);
    void settle(std::int64_t nowMs); // runs every deadline up to nowMs
    std::optional<Deadline> nextDeadline() const;
    void enter(PetState state, std::int64_t at, bool keepIndex);
    std::optional<PetState> ownerOf(PetState state) const;
    bool usable(const PetStateAnimation &animation) const;
    bool valid(const QString &variant) const;
    std::vector<int> validEntries(const PetSequence &sequence) const;
    int pickInitial(const PetSequence &sequence);
    // The next entry after `from`, or -1 to hold (linear + once at the last entry).
    int pickNext(const PetSequence &sequence, int from, bool holdAtEnd);
    void restartAnimation(std::int64_t at);
    int effectivePassMs(const PetSequence *sequence) const;
    int hold(const PetSequence &sequence) const;
    void publish();
    void recompute(std::int64_t at, bool keepIndex);
    const PetStateAnimation *ownerAnimation() const;

    PetStateTuning m_tuning;
    std::mt19937 m_rng;
    PetStates m_states;
    QString m_base;
    std::function<bool(const QString &)> m_exists;
    std::int64_t m_now = 0;

    PetState m_state = PetState::Idle;
    std::optional<PetState> m_owner;
    int m_index = -1;
    std::array<int, kPetStateCount> m_kept = {-1, -1, -1, -1}; // a click sequence's place, per owner
    QString m_variant;
    bool m_holding = false;
    bool m_missing = false;
    unsigned m_generation = 0;
    unsigned m_restarts = 0; // a click replayed the same entry

    std::int64_t m_enteredAt = 0;
    std::int64_t m_seqStart = 0;
    int m_passMs = 0;
    bool m_loopsForever = true;

    bool m_dragging = false;
    bool m_dodging = false;
    bool m_previewing = false;
    PetState m_previewState = PetState::Idle;
    bool m_suspended = false;
    int m_dodgeLen = 0;
    std::int64_t m_previewUntil = 0;
    PetPoint m_ref;
    PetPoint m_last;
    std::int64_t m_lastMotion = 0;
    std::int64_t m_dodgeUntil = 0;

    // What publish() compared against last.
    PetState m_pubState = PetState::Idle;
    QString m_pubVariant;
    int m_pubIndex = -1;
    bool m_pubHolding = false;
    unsigned m_pubRestarts = 0;
};

} // namespace murasaki
