#pragma once

// The states of a pet and its Mouse Tracking (docs/pets/states.md). Two values of a placement, `states` and `tracking`: both off by default, which is today's pet. Pure: no I/O. The limits and names are in one
// table with the document that lists them (a test keeps both equal), as petstage.h and petbehaviours.h do.

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <array>
#include <optional>
#include <string_view>

namespace murasaki {

// What a pet can look like depending on what happens to it (Anima's four lists; its "static" is `idle` here).
enum class PetState { Idle, Grabbed, Moving, Released };
inline constexpr int kPetStateCount = 4;

const char *petStateName(PetState state);
std::optional<PetState> petStateFromName(const QString &name);

// A sequence is 1 to this many variants of the item, and the states of a pet name at most this many different ones in all (each is converted).
inline constexpr int kMaxStateSequenceItems = 16;
inline constexpr int kMaxStateVariants = 16;
// How long an animation that loops forever shows in an automatic sequence before the next one.
inline constexpr int kMinStateHoldMs = 500;
inline constexpr int kMaxStateHoldMs = 60000;
inline constexpr int kDefaultStateHoldMs = 4000;

enum class PetSequenceOrder { Linear, Random };
enum class PetSequenceAdvance { Auto, Click };

const char *petSequenceOrderName(PetSequenceOrder order);
std::optional<PetSequenceOrder> petSequenceOrderFromName(const QString &name);
const char *petSequenceAdvanceName(PetSequenceAdvance advance);
std::optional<PetSequenceAdvance> petSequenceAdvanceFromName(const QString &name);

// An ordered or random list of variants. `once`: each entry plays one pass and its last frame stays until the next event ("play once on touch").
struct PetSequence {
    QStringList items;
    PetSequenceOrder order = PetSequenceOrder::Linear;
    PetSequenceAdvance advance = PetSequenceAdvance::Auto;
    bool once = false;
    int holdMs = kDefaultStateHoldMs;

    bool operator==(const PetSequence &other) const = default;
};

// What one state shows: one variant (looping, as a pet does today), or a sequence. Exactly one of the two: a sequence has items.
struct PetStateAnimation {
    QString variant;
    PetSequence sequence;

    bool isSequence() const { return !sequence.items.isEmpty(); }
    bool operator==(const PetStateAnimation &other) const = default;
    QJsonObject toJson() const;
};

struct PetStates {
    std::array<std::optional<PetStateAnimation>, kPetStateCount> byState; // absent: the state falls back (idle -> the placement's variant)

    const std::optional<PetStateAnimation> &of(PetState state) const { return byState[size_t(state)]; }
    std::optional<PetStateAnimation> &of(PetState state) { return byState[size_t(state)]; }
    // The default is the key being absent from the file: a placement without states writes nothing.
    bool isDefault() const { return *this == PetStates{}; }
    // Every variant any state names, sequences in order, without repeats.
    QStringList variants() const;
    bool operator==(const PetStates &other) const = default;

    QJsonObject toJson() const;
};

// Applies `change` onto `base`: an object whose keys are state names (idle, grabbed, moving, released) with `{variant}` or `{sequence: {items, order, advance,
// once, holdMs}}` replacing that state, or `null` clearing it; `null` as a whole gives the default (no states). Unknown keys are ignored; a known key of the
// wrong type or a value out of range is an error (nullopt, *error set), so that a typo never silently changes what a pet shows. A variant is not checked
// against the item here (the pets check names it later).
std::optional<PetStates> applyPetStatesChanges(const PetStates &base, const QJsonValue &change, QString *error = nullptr);

enum class PetTrackingMode { Off, Look, Follow };
const char *petTrackingModeName(PetTrackingMode mode);
std::optional<PetTrackingMode> petTrackingModeFromName(const QString &name);

inline constexpr double kMaxTrackingDistance = 4000;
inline constexpr double kDefaultTrackingDistance = 160;
inline constexpr double kMinTrackingSpeed = 0.1;
inline constexpr double kMaxTrackingSpeed = 4;

struct PetTracking {
    PetTrackingMode mode = PetTrackingMode::Off;
    double distance = kDefaultTrackingDistance; // follow: the gap it keeps from the cursor, px
    double speed = 1;                           // follow: a multiplier on the base speed
    bool returnHome = true;                     // follow: when tracking ends the pet goes back to its stored place

    bool active() const { return mode != PetTrackingMode::Off; }
    bool isDefault() const { return *this == PetTracking{}; }
    bool operator==(const PetTracking &other) const = default;
    QJsonObject toJson() const;
};

// Applies the keys of `change` (mode, distance, speed, returnHome) onto `base`; `null` gives the default (off). Unknown keys are ignored; a known key of the
// wrong type or a value out of range is an error.
std::optional<PetTracking> applyPetTrackingChanges(const PetTracking &base, const QJsonValue &change, QString *error = nullptr);

// The names a pet's `degraded` list can carry for states and tracking (docs/pets/states.md); none is an error.
namespace statesdegrade {
inline constexpr std::string_view kMissingVariant = "states: missing variant";
inline constexpr std::string_view kTranscodePending = "states: transcode pending";
inline constexpr std::string_view kSwitchingSlow = "states: switching slow";
inline constexpr std::string_view kOverBudget = "states: over budget, playing from video";
inline constexpr std::string_view kNeedsGlobalPointer = "tracking: needs the global pointer";
inline constexpr std::string_view kParkedOverFullscreen = "tracking: parked over a fullscreen window";
inline constexpr std::string_view kMovesLimited = "tracking: moves limited to 30 a second";
inline constexpr std::string_view kStageFollowOneScreen = "tracking: follow needs a stage on one screen";

inline constexpr std::array<std::string_view, 8> kAll = {kMissingVariant, kTranscodePending, kSwitchingSlow, kOverBudget,
                                                         kNeedsGlobalPointer, kParkedOverFullscreen, kMovesLimited, kStageFollowOneScreen};
} // namespace statesdegrade

} // namespace murasaki
