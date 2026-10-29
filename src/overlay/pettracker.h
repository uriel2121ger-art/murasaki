#pragma once

// Mouse Tracking of one pet as a pure machine (docs/pets/states.md; plan 4.5): `look` (the pet faces the cursor) and `follow` (the pet walks toward it and stops
// `distance` px away). No Qt GUI, no clock (every input carries `nowMs`, one monotonic clock, never going backwards: an earlier value counts as the latest seen),
// no I/O. The controller feeds it the global cursor and applies its answers; tst_pettracker drives it with scripted cursor paths.
//
//   - Coordinates: the cursor, `home` (the pet's stored rectangle) and `output` are in one space, the output's (its origin included).
//   - look: the picture is mirrored when the cursor is on the side the item does not face. A dead zone of 24 px around the pet's centre keeps it from flickering,
//     and it changes at most 4 times a second. mirror() is a runtime override: nothing is stored. The item faces right unless the config says it faces left.
//   - follow: the pet's window walks toward the cursor along the straight line and stops `distance` px from it (centre to cursor), with an ease-out and a cap of
//     200 px/s times `speed`, at most 30 steps a second, kept inside the output; it never goes away from the cursor when it is closer than `distance` (that is
//     what smart dodge is for). offset() is the displacement from the stored place: a runtime layer, never stored by the tracker. When following ends (the mode
//     changes) the pet glides home (`returnHome`) or its displacement is handed to the owner once (takeCommit) to be stored. hold() stops it for the length of a
//     drag; park() stops it over a fullscreen window; halt() ends it at once (hidden, paused, arranged).
//   - Non-finite positions are ignored.

#include "petgeometry.h"
#include "petstates.h" // src/core: PetTrackingMode

#include <cstdint>
#include <optional>

namespace murasaki {

struct PetTrackerTuning {
    double deadZonePx = 24;
    int minFlipIntervalMs = 250; // 4 changes a second at most
    int stepIntervalMs = 33;     // 30 moves a second at most (a layer-shell margin change makes the compositor reconfigure the surface)
    double baseSpeed = 200;      // px per second at speed 1
    double easeRate = 3;         // the pace is the remaining distance times this, per second, up to the cap
    double arriveEpsilon = 1;    // px: closer than this to where it is going is there
};

struct PetTrackerConfig {
    PetTrackingMode mode = PetTrackingMode::Off;
    double distance = kDefaultTrackingDistance;
    double speed = 1;
    bool returnHome = true;
    bool facesLeft = false; // the item's own picture looks to the left
    bool operator==(const PetTrackerConfig &) const = default;
};

class PetTracker
{
public:
    explicit PetTracker(PetTrackerTuning tuning = {});

    void setConfig(const PetTrackerConfig &config, std::int64_t nowMs);
    // The pet's stored rectangle (where it lives when nothing tracks) and its output, in the output's space.
    void setGeometry(PetRect home, PetRect output);
    // The global cursor in the same space.
    void cursor(std::int64_t nowMs, PetPoint position);
    void tick(std::int64_t nowMs);
    // Forget the cursor and every override (the pet is hidden, paused, arranged, or tracking is off).
    void reset(std::int64_t nowMs);
    // Ends following at once: the displacement goes (returnHome) or waits to be taken (takeCommit). The cursor is kept.
    void halt(std::int64_t nowMs);
    // The pet is held (dragged): it does not move meanwhile. Letting go resumes.
    void hold(bool on, std::int64_t nowMs);
    // The pet is parked (a fullscreen window on its output): it does not move meanwhile.
    void park(bool on, std::int64_t nowMs);
    // The owner stored the pet where it is (a drop): the displacement is part of the stored place now.
    void adopt();

    // look: whether the picture is to be mirrored (on top of the placement's own flip); none until a cursor was seen.
    std::optional<bool> mirror() const { return m_mirror; }
    // follow: the displacement from the stored place, whether the last tick moved the pet, whether the pet is away from home or on its way (nothing may drop the
    // tracker meanwhile) and the displacement to store once when following ended without returnHome (reading it clears the tracker's own copy).
    PetPoint offset() const { return m_offset; }
    bool moving() const { return m_moving; }
    bool busy() const { return m_phase != Phase::None || m_offset.x != 0 || m_offset.y != 0 || m_commit; }
    bool parked() const { return m_parked; }
    std::optional<PetPoint> takeCommit();
    // When tick() next has something to do (a flip held back by the rate limit), or none.
    std::optional<std::int64_t> nextWakeMs() const;

private:
    std::int64_t clock(std::int64_t nowMs);
    void evaluateLook(std::int64_t nowMs);
    void step(std::int64_t nowMs);
    void endFollow();
    enum class Phase { None, Following, Returning };

    PetTrackerTuning m_tuning;
    PetTrackerConfig m_config;
    PetRect m_home;
    PetRect m_output;
    std::int64_t m_now = 0;
    std::optional<PetPoint> m_cursor;
    std::optional<bool> m_mirror;
    std::optional<bool> m_wanted; // what the cursor asks for, when the rate limit holds it back
    std::int64_t m_lastFlip = -1'000'000;
    Phase m_phase = Phase::None;
    PetPoint m_offset;
    std::optional<PetPoint> m_commit;
    bool m_moving = false;
    bool m_held = false;
    bool m_parked = false;
    std::int64_t m_lastStep = -1'000'000;
};

} // namespace murasaki
