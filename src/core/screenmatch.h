#pragma once

// Screen matching, the pure part: grey pictures, the average hash, the
// "structure" score, the aggregation over regions, and the state machine that turns a stream of per-tick scores into activation and deactivation. No threads, no timers, no I/O:
// the pixels come in as bytes and the clock is a parameter (milliseconds), so a script of ticks tests all of it.

#include <QByteArray>
#include <QList>
#include <QString>

#include <array>

namespace murasaki {

inline constexpr int kScreenMatchMaxSide = 360;   // the longer side of a picture before it is scored
inline constexpr int kScreenHashSide = 8;         // the hash is 8x8 cells, 64 bits
inline constexpr int kScreenStructureSide = 32;   // the structure score looks at 32x32 grey
inline constexpr double kScreenDefaultThreshold = 0.65;
inline constexpr qint64 kScreenDefaultCooldownMs = 800;
inline constexpr qint64 kScreenDefaultGraceMs = 250;
inline constexpr int kScreenDefaultStartFrames = 1;
inline constexpr int kScreenMatchMaxDimension = 16384;      // widest or tallest picture taken from pixels; larger is refused
inline constexpr qint64 kScreenMatchMaxPixels = qint64(1) << 27; // 134 million pixels in all
inline constexpr qint64 kScreenMaxDurationMs = 24 * 60 * 60 * 1000; // grace, cooldown and loop delay are held to a day

// ---- Pictures -------------------------------------------------------------------------------------------------------------------------------------------------------------

enum class ScreenPixelFormat { Grey8, Rgb24, Rgba32, Bgra32 }; // Rgba32 and Bgra32 ignore alpha (and the X of BGRx)

// One byte a pixel, rows packed (no padding).
struct GreyImage {
    int width = 0;
    int height = 0;
    QByteArray pixels;

    bool isEmpty() const { return width < 1 || height < 1 || pixels.size() != qsizetype(width) * height; }
};

// Rec. 601 weights in integers: (77 R + 150 G + 29 B + 128) >> 8. `dataSize` is the length of the buffer in bytes. Returns an empty image when the arguments make no sense: null data,
// an unknown format, a size under 1 or over kScreenMatchMaxDimension (or kScreenMatchMaxPixels in all), a stride shorter than a row, or a buffer shorter than
// stride * (height - 1) + width * bytesPerPixel. Every function below that reads pixels refuses the same way and never allocates for a refused picture.
GreyImage greyFromPixels(const uchar *data, qsizetype dataSize, int width, int height, qsizetype stride, ScreenPixelFormat format);

// The same, fused with the scaling to a longer side of `maxSide`: the crop of a large buffer is turned grey and shrunk in one go, without a full-size grey copy. The result is the
// same as greyFromPixels followed by fitLongerSide. `rowStep` above 1 reads only every rowStep-th source row (the cost falls in proportion, the scores move a little): for the
// big crops of a 4K screen, where the area filter would read millions of pixels a tick.
GreyImage greyScaledFromPixels(const uchar *data, qsizetype dataSize, int width, int height, qsizetype stride, ScreenPixelFormat format, int maxSide = kScreenMatchMaxSide, int rowStep = 1);

// Area-average scaling (every source pixel counts by the share of it that falls in the destination pixel), rounded to the nearest byte. Works up as well as down.
GreyImage resizeArea(const GreyImage &image, int width, int height);

// The picture scaled down, keeping its shape, so that its longer side is at most `maxSide`; a picture that already fits is returned as it is.
GreyImage fitLongerSide(const GreyImage &image, int maxSide = kScreenMatchMaxSide);

// What a score needs from one picture, computed once (a reference once, a live crop once a tick).
struct ScreenPicture {
    bool valid = false;
    quint64 hash = 0;                 // bit y*8+x is set when cell (x, y) of the 8x8 is brighter than the mean of the 64 cells
    bool hasStructure = false;
    std::array<float, kScreenStructureSide * kScreenStructureSide> centred{}; // the 32x32 grey minus its mean
    float flatMean = 0.0f;            // the mean of that 32x32, 0..255
    float norm = 0.0f;                // sqrt of the sum of squares of `centred`; 0 for a flat picture
};

// Fits the picture to a longer side of 360, then takes the hash and, when asked, the 32x32 data. An empty image gives an invalid picture.
ScreenPicture prepareScreenPicture(const GreyImage &image, bool withStructure = true);

// The same picture straight from the pixels of a crop, in one pass and without the 360 px intermediate: the integer grey of each pixel goes into 32x32 area bins (exact fractional
// weights), the 8x8 hash cells are the means of 4x4 bins each. The reference path (greyFromPixels -> prepareScreenPicture) stays as the definition; this one agrees with it to
// within rounding (tests/tst_screenmatch.cpp measures how much). `rowStep` above 1 reads only every rowStep-th source row (held to half the rows of a bin; screenRowStepFor is the intended source). An invalid argument (see greyFromPixels)
// gives an invalid picture, which scores 0 against anything.
ScreenPicture prepareScreenPictureFromPixels(const uchar *data, qsizetype dataSize, int width, int height, qsizetype stride, ScreenPixelFormat format, bool withStructure = true, int rowStep = 1);

// The row step the tick should use for a crop of this height: every row for crops up to 511 rows, then enough skipping to keep about 256 rows read (720 rows -> 2, 1080 -> 4,
// 2160 -> 8). Eight or more rows still fall in each of the 32 bins, so the picture changes by rounding only (measured in tests/tst_screenmatch.cpp).
int screenRowStepFor(int height);

// A reference picture from its 32x32 grey grid (1024 bytes, row by row), as a ScreenPicture: the hash is taken from the 8x8 means of 4x4 blocks and the structure data from the grid, the same
// way prepareScreenPictureFromPixels and prepareScreenPicture derive them. This is how a reference travels to the matching helper (a small fixed-size record, no picture decoder in the helper).
// `grid` must point to kScreenStructureSide * kScreenStructureSide bytes.
ScreenPicture screenPictureFromGrid(const uchar *grid);

// The 32x32 grey grid of a picture prepared from pixels or a grey image (what screenPictureFromGrid takes back), rounded to bytes. Empty when the picture is invalid or has no structure data.
QByteArray screenGridOf(const GreyImage &image);

// ---- Scores ---------------------------------------------------------------------------------------------------------------------------------------------------------------

enum class ScreenMatchMode { Hash, Structure };

// "hash" or "structure".
QString screenMatchModeName(ScreenMatchMode mode);

// Anima's rule, by substring on the lower-cased name: "sift" gives Structure, else "hash" gives Hash, else "orb" gives Structure, anything else gives Structure. An empty or blank
// name is a pack that left the mode out, so its default (ORB_HASH) applies: Hash. `fromFeatureMode` is set whenever Structure comes from a name other than our own "structure"
// (SIFT, ORB or anything else), so the caller can say that the pack was matched by structure.
ScreenMatchMode screenMatchModeFromPack(const QString &name, bool *fromFeatureMode = nullptr);

// 1 - hamming/64, in steps of 1/64. 0 if either picture is invalid.
double hashScore(const ScreenPicture &reference, const ScreenPicture &live);

// 0.8 x ncc01 + 0.2 x hashScore. ncc01 is the zero-mean normalised cross-correlation of the two 32x32 pictures mapped from [-1, 1] to [0, 1] as (ncc + 1) / 2, so two
// unrelated pictures sit near 0.5 and a brightness or contrast change leaves it at 1. A flat picture has no correlation: against a non-flat one ncc is 0 (ncc01 0.5); two flat
// ones are compared by brightness alone, ncc01 = 1 - |mean difference| / 255. 0 if either picture is invalid or lacks the structure data.
double structureScore(const ScreenPicture &reference, const ScreenPicture &live);

double screenScore(ScreenMatchMode mode, const ScreenPicture &reference, const ScreenPicture &live);

// ---- Regions --------------------------------------------------------------------------------------------------------------------------------------------------------------

// Any: one region passing is enough (the runner's rule, and the default). All: every region must pass (Anima's "min"). Mean: the average score must pass.
enum class ScreenAgg { Any, All, Mean };

QString screenAggName(ScreenAgg agg);                 // "any", "all", "mean"
ScreenAgg screenAggFromName(const QString &name);     // "all" and "min" give All, "mean" gives Mean, anything else Any

// The number compared with the threshold: the highest score (Any), the lowest (All) or the mean (Mean). A region that cannot be scored (outside the shared screens, under 2 pixels)
// is given as 0, so it never passes. No regions gives 0.
double aggregateScores(const QList<double> &regionScores, ScreenAgg agg);

// ---- The state machine ----------------------------------------------------------------------------------------------------------------------------------------------------

enum class ScreenCooldownFrom { Activate, Deactivate };

struct ScreenMatchConfig {
    double threshold = kScreenDefaultThreshold;
    ScreenAgg agg = ScreenAgg::Any;
    int startFrames = kScreenDefaultStartFrames;       // detected ticks in a row to activate; in sequencer mode, passing ticks in a row for each step
    qint64 graceMs = kScreenDefaultGraceMs;            // with stopOnMismatch: no detection for more than this ends the trigger
    qint64 cooldownMs = kScreenDefaultCooldownMs;
    ScreenCooldownFrom cooldownFrom = ScreenCooldownFrom::Activate;
    bool stopOnMismatch = true;                        // false: the trigger ends only when end() is called (its timeline ran out)
    bool sequencer = false;                            // walk seqLen references one after another instead of one anchor
    int seqLen = 1;
    qint64 loopDelayMs = 0;                            // sequencer: after a run completes, no new walk starts for this long
};

enum class ScreenEvent { None, Activated, Deactivated };

struct ScreenTickResult {
    ScreenEvent event = ScreenEvent::None;
    bool detected = false;   // the aggregate reached the threshold this tick (also while cooling down or active)
    double score = 0.0;      // the aggregate
    bool active = false;     // after this tick
    bool cooling = false;    // the cooldown was running at this tick
    int step = 0;            // the reference step to compare at the next tick (sequencer), 0 in anchor mode
};

// Feeds on one aggregate decision a tick. The clock must not go back: a `nowMs` earlier than the previous tick's is taken as that previous time (the machine never sees time
// run backwards, so a stepped wall clock cannot stretch a hold or a cooldown). Durations in the config are held to a day. No regions (an empty list) is never a detection. The caller compares each region with the reference of `wantedStep()` and passes the scores in; the machine does not know pictures.
//  - Activation: `startFrames` detected ticks in a row outside the cooldown; any undetected tick, or a tick inside the cooldown, starts the count again.
//  - Sequencer: a step passes after `startFrames` passing ticks in a row; a failing tick goes back to step 0; passing the last step activates. While active the machine
//    stays on the last step: the picture that holds the trigger is the last reference.
//  - Active: each detected tick refreshes the hold. With stopOnMismatch, more than graceMs since the last detection ends it. Ticks are not counted while active.
//  - Cooldown: from activation, or from the end, by cooldownFrom.
class ScreenMatcher
{
public:
    explicit ScreenMatcher(const ScreenMatchConfig &config = {});

    const ScreenMatchConfig &config() const { return m_config; }
    bool active() const { return m_active; }
    int wantedStep() const { return m_step; }
    bool cooling(qint64 nowMs) const { return nowMs < m_cooldownUntilMs; }

    ScreenTickResult tick(qint64 nowMs, const QList<double> &regionScores);

    // The action ended by itself (stopOnMismatch off, or the rescue): ends the activation now. Returns whether there was one.
    bool end(qint64 nowMs);

    // Back to the start: not active, no streak, no cooldown (watching stopped and started again).
    void reset();

private:
    void deactivate(qint64 nowMs);

    ScreenMatchConfig m_config;
    bool m_active = false;
    int m_streak = 0;
    int m_step = 0;
    qint64 m_lastDetectedMs = 0;
    qint64 m_lastTickMs = 0;
    qint64 m_cooldownUntilMs = 0;
    qint64 m_blockedUntilMs = 0;
};

} // namespace murasaki
