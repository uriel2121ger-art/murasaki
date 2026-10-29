#pragma once

// Switching to a profile: the pets on the desktop become the profile's, keeping the window of every pet that exists in both (docs/pets/automation.md, "Switching").
// Pure: the daemon applies the result in ONE write of the store.

#include "profile.h"

#include <QList>
#include <QString>

#include <functional>

namespace murasaki {

struct SkippedEntry {
    int slot = 0;
    QString itemId;
    QString reason; // kProfileItemMissing or kProfileVariantMissing

    bool operator==(const SkippedEntry &other) const = default;
};

struct SwitchPlan {
    // The pets as they are to be after the switch, in the order of the profile. A pet that exists keeps its petId (and so its window); a new one has an empty petId
    // (the store gives it one).
    QList<PetPlacement> result;
    int kept = 0;    // existed and was already as the profile says
    int updated = 0; // existed and some field changed
    int added = 0;
    int removed = 0;
    QList<SkippedEntry> skipped;
    // The entry (by slot) each resulting pet came from, same order as `result`.
    QList<int> entrySlots;

    // Nothing changes: the switch writes nothing.
    bool isNoop() const { return updated == 0 && added == 0 && removed == 0; }
};

// Says why an item or variant can not be shown now: an empty string when it can, else kProfileItemMissing or kProfileVariantMissing.
using ProfileItemCheck = std::function<QString(const QString &itemId, const QString &variant)>;

// Matches the pets that exist with the entries by identity: the same item and variant first (in order), then the same item (its variant changes). The rest of the entries
// are added, the rest of the pets removed. Entries the check refuses are skipped with the reason and are not matched.
SwitchPlan planProfileSwitch(const QList<PetPlacement> &current, const Profile &profile, const ProfileItemCheck &check);

// Whether the pets on the desktop are not what the profile says (the identity matching and a field comparison): the "modified" mark. Skipped entries do not count.
bool profileDiffers(const QList<PetPlacement> &current, const Profile &profile, const ProfileItemCheck &check);

} // namespace murasaki
