#pragma once

// The fullscreen animation mode of a pet, "the stage" (docs/pets/stage.md). A mode of a placement, not a new object: off by default, which is today's pet. Pure: no I/O. The limits and names are in one table
// with the document that lists them (a test keeps both equal), as petcheck.h and petbehaviours.h do.

#include <QJsonObject>
#include <QJsonValue>
#include <QSizeF>
#include <QString>

#include <array>
#include <optional>
#include <string_view>

namespace murasaki {

// A stage is at most this many logical pixels a side and covers at most this many outputs; more outputs are refused with kStageTooManyOutputs
// and the pet is shown on the first ones.
inline constexpr int kMaxStageSide = 16384;
inline constexpr int kMaxStageOutputs = 8;

// The pixels a stage may draw in total (device pixels, summed over its windows): the setting stageMaxPixels. 3840 x 2160 by default.
inline constexpr long long kMinStageMaxPixels = 1000000;
inline constexpr long long kMaxStageMaxPixels = 33177600; // 7680 x 4320
inline constexpr long long kDefaultStageMaxPixels = 8294400;
inline constexpr bool kDefaultStageHideOverFullscreen = true;

enum class StageMode { Off, Output, Desktop };
// How an animation of another shape than its target is fitted (plan U1): inside with transparent bands, over it with the excess cropped, or distorted.
enum class StageFit { Contain, Cover, Stretch };
// Whether the stage hides while a fullscreen window is on its output: follow the setting stageHideOverFullscreen, or always show, or always hide.
enum class StageOverFullscreen { Inherit, Show, Hide };

const char *stageModeName(StageMode mode);
std::optional<StageMode> stageModeFromName(const QString &name);
const char *stageFitName(StageFit fit);
std::optional<StageFit> stageFitFromName(const QString &name);
const char *stageOverFullscreenName(StageOverFullscreen value);
std::optional<StageOverFullscreen> stageOverFullscreenFromName(const QString &name);

struct PetStage {
    StageMode mode = StageMode::Off;
    StageFit fit = StageFit::Contain;
    StageOverFullscreen overFullscreen = StageOverFullscreen::Inherit;

    bool active() const { return mode != StageMode::Off; }
    // The default is the key being absent from the file: a placement that is not a stage writes nothing.
    bool isDefault() const { return *this == PetStage{}; }
    bool operator==(const PetStage &other) const = default;

    QJsonObject toJson() const;
};

// Applies the keys of `change` (mode, fit, overFullscreen) onto `base`; `null` gives the default (off). Unknown keys are ignored; a known key of the
// wrong type or an unknown value is an error (nullopt, *error set), so that a typo never silently turns a stage on.
std::optional<PetStage> applyPetStageChanges(const PetStage &base, const QJsonValue &change, QString *error = nullptr);

// The size a picture of `content` takes when fitted to `target` (contain: the largest that fits inside, cover: the smallest that covers it, stretch: the target
// itself); empty when either is zero, negative or not finite. The same arithmetic places the picture (src/overlay/stagegeometry.h) and sizes its transcode.
QSizeF stageFittedSize(QSizeF target, QSizeF content, StageFit fit);
// The bounding box of an animation of size `native` turned by `rotation` degrees: what a stage fits to the screen.
QSizeF stageRotatedFootprint(QSizeF native, double rotation);

// Two stages that can not be shown together: `desktop` covers every output, `output` one, and an output holds one stage (question 8 of the plan). The
// outputs are compared as the placements name them; a "*" (the primary) and a connector that is the primary are told apart by the overlay at run time.
bool stagesConflict(StageMode modeA, const QString &outputA, StageMode modeB, const QString &outputB);

// The names a stage's `degraded` list can carry (docs/pets/stage.md); none is an error.
namespace stagedegrade {
inline constexpr std::string_view kTooManyOutputs = "stage: too many outputs";
inline constexpr std::string_view kNotFullscreen = "stage: not fullscreen on this session";
inline constexpr std::string_view kFullscreenUnknown = "stage: fullscreen apps unknown";
inline constexpr std::string_view kResolutionReduced = "stage: resolution reduced";
inline constexpr std::string_view kBelowUnsupported = "stage: below windows unsupported";
inline constexpr std::string_view kTranscodePending = "stage: transcode pending";
inline constexpr std::string_view kHiddenByFullscreen = "stage: hidden by a fullscreen window"; // informational, with the app
inline constexpr std::string_view kLayerShellMissing = "stage: layer-shell missing";
inline constexpr std::string_view kOutputTaken = "stage: output already has a stage";
inline constexpr std::string_view kOutputMissing = "stage: output missing";
inline constexpr std::string_view kOutputTooLarge = "stage: output too large";
inline constexpr std::string_view kCopyTooSmall = "stage: no copy big enough";
inline constexpr std::string_view kNeedsCompositor = "stage: needs a compositor";

inline constexpr std::array<std::string_view, 13> kAll = {kTooManyOutputs, kNotFullscreen, kFullscreenUnknown, kResolutionReduced, kBelowUnsupported,
                                                         kTranscodePending, kHiddenByFullscreen, kLayerShellMissing, kOutputTaken, kOutputMissing, kOutputTooLarge, kCopyTooSmall, kNeedsCompositor};
} // namespace stagedegrade

} // namespace murasaki
