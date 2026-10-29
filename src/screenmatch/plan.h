#pragma once

// What the daemon tells the matching helper, decided in pure functions: the settings of a screen trigger as the
// store keeps them, the regions of the desktop mapped to the pixels of the monitor streams the portal gave, the pets' rectangles, and the list of protocol commands that sets a helper up. No I/O: the
// references come in through a function, the geometry as plain numbers, so every rule is tested.
//
// A screen trigger's `source.screen` (kept by the store, written by a pack import or by the editor) is
//   { regions: [{kind, x, y, w, h, id?}], match: {mode, agg, threshold, cooldownMs, cooldownFrom, startFramesToCheck, stopGraceMs, stopOnMismatch, refMode, seqLen, seqStep, loopDelayMs},
//     event, pack }
// the bounded form of src/import/triggerspack.cpp. Regions are rectangles in the desktop's logical coordinates; a circle, a polygon or a path is matched as its bounding rectangle.

#include "protocol.h"
#include "screenmatch.h"
#include "trigger.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace murasaki::screenmatch {

inline constexpr int kDefaultStartFrames = 3;           // Anima's pack defaults (plan 4.3)
inline constexpr qint64 kDefaultGraceMs = 250;
inline constexpr qint64 kDefaultCooldownMs = 500;
inline constexpr int kMinRegionSide = 2;                // a region needs at least 2 pixels each way to be scored
inline constexpr int kMaxEnabledScreenTriggers = 4;

struct RegionSpec {
    int index = 0;
    double x = 0, y = 0, w = 0, h = 0;   // logical desktop coordinates
};

// One screen trigger as the matcher needs it.
struct TriggerSpec {
    QString id;
    ScreenMatchMode mode = ScreenMatchMode::Hash;
    ScreenMatchConfig match;
    QList<RegionSpec> regions;           // at most kMaxRegionsPerTrigger; a region that is not a usable rectangle is left out of the list but keeps its index
    int regionCount = 0;                 // the regions in the record (the helper's region numbers are 0 to this minus 1)
    QStringList notes;                   // "screen: matched by structure, not SIFT; check the threshold", ...
};

// The spec of a trigger of kind Screen. Values out of range are clamped; nothing makes it fail (a trigger with no usable region simply never passes).
TriggerSpec specOf(const Trigger &trigger);

// One monitor of the cast and how its pixels relate to the desktop's logical coordinates (the portal's position and size; `scale` is the device pixel ratio of the output, 1 when unknown).
struct StreamGeometry {
    quint32 node = 0;
    int x = 0, y = 0, width = 0, height = 0; // logical
    double scale = 1.0;
    bool scaleKnown = true; // false: the output of this monitor is not known, so its pixels can not be told from its logical coordinates
};

struct Mapped {
    bool onScreen = false;
    bool scaleKnown = true;                  // the stream under the centre has a known scale (false: not on screen for the pixels' sake)
    int stream = 0;                          // the index in the list given
    int x = 0, y = 0, w = 0, h = 0;          // pixels of that stream
};

// A logical rectangle in the stream under its centre, in that stream's pixels and held inside it. Not on screen when no stream holds the centre or less than kMinRegionSide pixels are left.
Mapped mapRectangle(double x, double y, double w, double h, const QList<StreamGeometry> &streams);

struct LogicalRect {
    double x = 0, y = 0, w = 0, h = 0;
};
// The rectangles of our own pets, mapped into streams (those off every stream are dropped), at most kMaxPets.
QList<PetRect> mapPets(const QList<LogicalRect> &pets, const QList<StreamGeometry> &streams);

// The sentences of `screen: ...` that name why a region does not score.
inline constexpr QLatin1String kRegionOutside("screen: region outside the shared screens");
inline constexpr QLatin1String kRegionNoReference("screen: region has no reference picture");
inline constexpr QLatin1String kScaleUnknown("screen: the scale of that screen is not known yet; its regions are not watched");
inline constexpr QLatin1String kTooManyTriggers("screen: more than 4 screen triggers are enabled; the rest are not watched");
inline constexpr QLatin1String kTooManyRegions("screen: more than 16 regions in all; the rest are not watched");

struct PlannedRegion {
    int index = 0;
    bool declared = false;                   // sent to the helper (on screen and with a reference)
    QString problem;                         // kRegionOutside, kRegionNoReference or empty
};

struct PlannedTrigger {
    TriggerSpec spec;
    QList<PlannedRegion> regions;            // one per region of the record, in order
    int steps = 1;                           // how many reference steps the sequencer walks (1 in anchor mode)
};

struct Plan {
    QList<PlannedTrigger> triggers;
    QList<Command> commands;                 // clear, trigger, region, ref, rate: everything but start
    QStringList notes;                       // one per problem, for the state the GUI shows
    int declaredRegions = 0;
};

struct PlanInput {
    QList<Trigger> triggers;                 // the enabled triggers of kind Screen
    QList<StreamGeometry> streams;
    // The 32x32 grey grid (1024 bytes) of a reference, or empty when there is none: (trigger id, region, step).
    std::function<QByteArray(const QString &, int, int)> grid;
    // The steps a region has, ascending.
    std::function<QList<int>(const QString &, int)> steps;
    int ticksPerSecond = 5;
};

Plan makePlan(const PlanInput &input);

} // namespace murasaki::screenmatch
