#pragma once

#include <QHash>
#include <QList>
#include <QRect>
#include <QSize>
#include <QString>

#include <QSizeF>

#include <array>
#include <optional>

namespace murasaki {

// Longest sides, in pixels, a pet is transcoded to. The biggest one is also the cap: a pet is never converted larger.
inline constexpr std::array<int, 4> kTranscodeBuckets = {256, 384, 512, 650};
inline constexpr int kTranscodeMaxSide = kTranscodeBuckets.back();
// The buckets a stage (docs/pets/stage.md) adds: a stage is as big as the screen. Never above the source's own size, at most kStageMaxSide on the long side.
inline constexpr std::array<int, 5> kStageBuckets = {960, 1280, 1920, 2560, 3840};
inline constexpr int kStageMaxSide = kStageBuckets.back();
// A variant keeps at most this many converted copies.
inline constexpr int kMaxBucketsPerVariant = 2;

inline constexpr double kMinOutputScale = 0.5;
inline constexpr double kMaxOutputScale = 4;
inline constexpr int kMaxScaledOutputs = 32;

// A bucket of a pet or of a stage.
bool isTranscodeBucket(int bucket);

// The longest side of a pet shown `width` wide (output-logical pixels), following the animation's aspect ratio. An
// animation of unknown size is taken as square.
double displayedLongestSide(double width, QSize native);

// Pixels the longest side needs on the screen: ceil(displayed x scale), at most the source's longest side
// (sourceLongest <= 0 means unknown) and at most kTranscodeMaxSide, at least 1.
int transcodeNeed(double displayedLongest, double outputScale, int sourceLongest);

// The smallest bucket, of a pet or a stage, that is at least `need`, or the biggest one (kStageMaxSide) when the need is larger.
int bucketForNeed(int need);

// Of the finished buckets: the smallest one that is at least `need`, else the biggest. Nothing when none is done.
std::optional<int> pickDoneBucket(const QList<int> &done, int need);

// Which finished buckets of one variant stay in the cache. The biggest one always stays (it serves every smaller size), and so does
// every bucket that is the best for some need (the placements' needs); up to `limit` buckets are kept in total, the others being the
// newest of `doneNewestFirst`.
QList<int> bucketsToKeep(const QList<int> &doneNewestFirst, const QList<int> &needs, int limit = kMaxBucketsPerVariant);

// Pixels the longest side of a stage's animation needs: the animation (unrotated, `native`) is drawn so that, turned by `rotation`, it covers `picture` (the fitted
// picture, logical pixels) on an output of scale `maxScale`; at most the source's longest side (sourceLongest <= 0 means unknown) and kStageMaxSide, at least 1.
int stageNeed(QSizeF picture, QSize native, double rotation, double maxScale, int sourceLongest);

// What an output is: its scale and, from revision 17, where it is in the layout (the overlay says it so the daemon can size a stage's copy).
struct OutputInfo {
    double scale = 1;
    QRect rect; // logical, in the compositor's layout; null when not told
    bool primary = false; // the compositor's primary output (an object value only)
    bool operator==(const OutputInfo &) const = default;
};
// Parses the argument of SetOutputScales with the geometry: the value of an output is a number (its scale) or {scale, x, y, width, height[, primary]}.
std::optional<QHash<QString, OutputInfo>> parseOutputInfo(const QString &json, QString *problem = nullptr);

// Parses the argument of SetOutputScales: {"DP-1": 1.0, ...}. Up to kMaxScaledOutputs outputs, names accepted by
// isValidKeyPart, finite scales within kMinOutputScale..kMaxOutputScale. Nothing (and *problem set) otherwise.
std::optional<QHash<QString, double>> parseOutputScales(const QString &json, QString *problem = nullptr);

} // namespace murasaki
