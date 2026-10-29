#pragma once

// Profiles, triggers and timelines (docs/pets/automation.md): the limits and the names that the daemon, the CLI, the GUI and the documentation share.
// docs/pets/automation.md lists the same names; tst_automationnames keeps the two equal.

#include "petstore.h"

#include <QLatin1String>
#include <QtGlobal>
#include <QStringList>

namespace murasaki {

inline constexpr int kMaxProfiles = 50;
inline constexpr int kMaxProfilePets = kMaxPets; // the store's own limit
inline constexpr int kMaxTriggers = 100;
inline constexpr int kMaxTimelines = 100;
inline constexpr int kMaxTimelineTracks = 16;
inline constexpr int kMaxTrackActions = 256;
inline constexpr int kMaxRunningTimelines = 8;
inline constexpr int kMaxAutomationNameLength = 100;
inline constexpr int kMaxShortcutTriggers = 8;
inline constexpr int kMaxDBusTriggers = 8;
inline constexpr int kMaxTriggerFields = 128; // the length of an application class, a signal name, an output name

inline constexpr double kMinTimelineSpeed = 0.25;
inline constexpr double kMaxTimelineSpeed = 4;
inline constexpr int kMaxMoveDurationMs = 60'000;
inline constexpr int kMaxWaitMs = 3'600'000;
inline constexpr qint64 kMaxTimelineLengthMs = 24LL * 3'600'000; // the waits and smooth moves of one track at speed 1

inline constexpr int kDefaultTriggerCooldownMs = 500; // Anima's own
inline constexpr int kMinTriggerCooldownMs = 100;
inline constexpr int kMaxTriggerCooldownMs = 86'400'000;
inline constexpr int kMaxFiresPerSecond = 20;
inline constexpr int kMaxFiresPerSecondPerTrigger = 8;
inline constexpr int kMaxTriggerChain = 4;      // trigger -> action -> trigger hops
inline constexpr int kTriggerChainWindowMs = 2000;
inline constexpr int kMaxTriggerLog = 50;
inline constexpr int kMissedTimeToleranceMs = 2 * 60'000;

// The names the states and degrades go by (docs/pets/automation.md). A name is a state or a refusal, never a crash.
inline constexpr QLatin1String kProfileItemMissing("profile: item missing");
inline constexpr QLatin1String kProfileVariantMissing("profile: variant missing");
inline constexpr QLatin1String kProfileTooManyPets("profile: too many pets");
inline constexpr QLatin1String kTriggerRateLimited("trigger: rate limited");
inline constexpr QLatin1String kTriggerCooldown("trigger: cooldown");
inline constexpr QLatin1String kTriggerChainTooDeep("trigger: chain too deep");
inline constexpr QLatin1String kTriggerDisabled("trigger: disabled");
inline constexpr QLatin1String kTriggerSourceUnavailable("trigger: source unavailable");
inline constexpr QLatin1String kTriggerShortcutUnbound("trigger: shortcut unbound");
inline constexpr QLatin1String kTimelineTooManyRunning("timeline: too many running");
inline constexpr QLatin1String kTimelinePetNotFound("timeline: pet not found");
inline constexpr QLatin1String kTimelineTooLong("timeline: too long");
inline constexpr QLatin1String kAutomationFileRefused("automation: file refused");
inline constexpr QLatin1String kAutomationScreenInactive("automation: screen triggers are not active");

QStringList automationNames();

} // namespace murasaki
