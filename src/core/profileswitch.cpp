#include "profileswitch.h"

#include <QSet>

using namespace Qt::StringLiterals;

namespace murasaki {

SwitchPlan planProfileSwitch(const QList<PetPlacement> &current, const Profile &profile, const ProfileItemCheck &check)
{
    SwitchPlan plan;
    QList<bool> taken(current.size(), false);
    // The entries that can be shown, in profile order, each with the pet it matches (or -1).
    struct Match {
        const ProfileEntry *entry;
        int pet = -1;
    };
    QList<Match> matches;
    for (const ProfileEntry &entry : profile.pets) {
        const QString reason = check ? check(entry.pet.itemId, entry.pet.variant) : QString();
        if (!reason.isEmpty()) {
            plan.skipped.append({entry.slot, entry.pet.itemId, reason});
            continue;
        }
        matches.append({&entry, -1});
    }
    // First pass: the same item and variant; second: the same item.
    for (const bool sameVariant : {true, false}) {
        for (Match &m : matches) {
            if (m.pet >= 0)
                continue;
            for (int i = 0; i < current.size(); ++i) {
                if (taken[i] || current[i].itemId != m.entry->pet.itemId || (sameVariant && current[i].variant != m.entry->pet.variant))
                    continue;
                m.pet = i;
                taken[i] = true;
                break;
            }
        }
    }
    for (const Match &m : matches) {
        PetPlacement wanted = m.entry->pet;
        if (m.pet >= 0) {
            wanted.petId = current[m.pet].petId;
            if (wanted == current[m.pet])
                ++plan.kept;
            else
                ++plan.updated;
        } else {
            wanted.petId.clear();
            ++plan.added;
        }
        plan.result.append(wanted);
        plan.entrySlots.append(m.entry->slot);
    }
    for (int i = 0; i < current.size(); ++i)
        plan.removed += taken[i] ? 0 : 1;
    return plan;
}

bool profileDiffers(const QList<PetPlacement> &current, const Profile &profile, const ProfileItemCheck &check)
{
    return !planProfileSwitch(current, profile, check).isNoop();
}

} // namespace murasaki
