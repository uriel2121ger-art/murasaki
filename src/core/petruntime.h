#pragma once

// The runtime layer of a pet (docs/pets/automation.md, "Runtime layer"): what a timeline or a trigger changes about a pet without touching its stored placement.
// It is never written to disk; the daemon keeps it, the overlay applies it on top of the placement and animates a motion with its own clock.

#include "petstates.h"

#include <QHash>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace murasaki {

inline constexpr double kMaxRuntimeCoordinate = 100000;

// A linear move of the pet from one position to another. startMs is CLOCK_MONOTONIC in milliseconds (the daemon and the overlay share the clock).
struct PetMotion {
    double fromX = 0;
    double fromY = 0;
    double toX = 0;
    double toY = 0;
    qint64 startMs = 0;
    int durationMs = 0;

    bool operator==(const PetMotion &other) const = default;
};

struct PetPosition {
    double x = 0;
    double y = 0;

    bool operator==(const PetPosition &other) const = default;
};

// Where the motion has the pet at nowMs: the start before it, the end after it, linear between (the easing is linear, docs/pets/automation.md).
PetPosition positionAt(const PetMotion &motion, qint64 nowMs);
bool motionDone(const PetMotion &motion, qint64 nowMs);

struct PetRuntime {
    bool hidden = false;
    std::optional<PetPosition> position; // replaces the stored x and y
    QString output;                      // with a position: the output it is relative to (empty: the stored one)
    std::optional<PetMotion> motion;     // while it runs, replaces the position
    std::optional<double> opacity;       // replaces the stored opacity
    QString variant;                     // replaces the stored variant (the animation the pet plays); empty: the stored one
    std::optional<PetTrackingMode> tracking; // replaces the stored tracking mode (distance, speed and return stay the pet's own)

    // Nothing is overridden: the layer holds no entry for such a pet.
    bool isDefault() const { return !hidden && !position && !motion && !opacity && variant.isEmpty() && !tracking; }
    QJsonObject toJson() const;
    bool operator==(const PetRuntime &other) const = default;
};

bool validatePetRuntime(const PetRuntime &runtime, QString *error = nullptr);
// Unknown keys are ignored; a known key of the wrong type or out of range is an error.
std::optional<PetRuntime> parsePetRuntime(const QJsonObject &json, QString *error = nullptr);

// Where the pet is drawn: the motion's position while it runs, else the override, else the stored one.
PetPosition effectivePosition(const PetRuntime &runtime, const PetPosition &stored, qint64 nowMs);

// The layer of every pet, as the daemon sends it: {petId: runtime}. Pets with a default runtime are left out.
QJsonObject runtimeLayerToJson(const QHash<QString, PetRuntime> &layer);
// The reverse, for the overlay. A pet whose entry is invalid is left out (the rest still applies); *skipped counts them.
QHash<QString, PetRuntime> runtimeLayerFromJson(const QJsonObject &json, int *skipped = nullptr);

} // namespace murasaki
