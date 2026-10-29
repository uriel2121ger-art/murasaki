#pragma once

#include "pausepolicy.h"

namespace murasaki {

// The most frames per second the pets may change at (docs/pets/timing.md): the user's own limit, and a lower one
// while the machine is saving power. 0: no limit. Frames above it are skipped, never slowed.
inline constexpr int kPetEnergySaveFps = kBatteryFpsCap; // the cap the wallpapers get on battery

struct PetFpsInputs {
    int userCap = 0;           // the daemon's setting petMaxFps; 0: unlimited
    bool saveEnergy = true;    // the daemon's setting petSaveEnergy
    bool onBattery = false;    // UPower
    bool powerSaver = false;   // power-profiles-daemon
};

constexpr int petFpsCap(const PetFpsInputs &in)
{
    int cap = in.userCap > 0 ? in.userCap : 0;
    if (in.saveEnergy && (in.onBattery || in.powerSaver))
        cap = cap > 0 && cap < kPetEnergySaveFps ? cap : kPetEnergySaveFps;
    return cap;
}

} // namespace murasaki
