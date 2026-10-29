#pragma once

// The sound of a pet (docs/pets/audio.md): which stored audio asset it plays, when, how loud and with what offset. A value of the placement's
// `audio` key. Pure: no I/O. The limits and names are the plan's, in one
// table with the document that lists them (a test keeps both equal), as petcheck.h and petbehaviours.h do.

#include "petaudiolimits.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <array>
#include <optional>
#include <string_view>

namespace murasaki {

// When a pet's sound starts (Anima Engine's five modes).
enum class PetAudioMode { None, Once, AnimationLoop, Continuous, Click };

const char *petAudioModeName(PetAudioMode mode);
std::optional<PetAudioMode> petAudioModeFromName(const QString &name);

struct PetAudio {
    QString asset;                              // 64 lowercase hex characters, or empty: no sound
    PetAudioMode mode = PetAudioMode::None;
    int volume = kDefaultPetAudioVolume;        // 0..100, linear amplitude
    int offsetMs = 0;                           // -5000..5000: positive delays the start, negative starts that far into the file

    // True when this pet can make a sound at all (a mode and an asset).
    bool active() const { return mode != PetAudioMode::None && !asset.isEmpty(); }
    bool isDefault() const { return *this == PetAudio{}; }
    bool operator==(const PetAudio &other) const = default;

    QJsonObject toJson() const;
};

// Whether `id` is an asset id: exactly 64 lowercase hex characters.
bool isPetAudioAssetId(const QString &id);

// Empty when valid, else the reason.
QString validatePetAudio(const PetAudio &audio);

// Applies the keys of `change` (asset, mode, volume, offsetMs) onto `base`; `null` gives the default (no sound). Unknown keys are ignored; a known
// key of the wrong type or a value out of range is an error (nullopt, *error set).
std::optional<PetAudio> applyPetAudioChanges(const PetAudio &base, const QJsonValue &change, QString *error = nullptr);

} // namespace murasaki
