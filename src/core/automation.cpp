#include "automation.h"

namespace murasaki {

QStringList automationNames()
{
    return {kProfileItemMissing,        kProfileVariantMissing,    kProfileTooManyPets,         kTriggerRateLimited,   kTriggerCooldown,
            kTriggerChainTooDeep,       kTriggerDisabled,          kTriggerSourceUnavailable,   kTriggerShortcutUnbound, kTimelineTooManyRunning,
            kTimelinePetNotFound,       kTimelineTooLong,          kAutomationFileRefused,      kAutomationScreenInactive};
}

} // namespace murasaki
