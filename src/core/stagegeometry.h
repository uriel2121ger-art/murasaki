#pragma once

// The geometry of a stage (docs/pets/stage.md, plan 4.2): where the picture of a pet in the fullscreen mode goes on the desktop and which part of it
// each output's window shows. Pure: Qt value types only, no windows. Logical pixels throughout (the compositor's layout, which has negative
// coordinates, gaps and L shapes); each window samples its part at its own device pixels (stageDevicePixels).
//
// One rounding decides every seam: the fitted picture's four edges are rounded to whole logical pixels once, and every output takes the part of that
// rectangle that lies on it. Two outputs that touch in the layout therefore share an edge of the picture to the pixel (and the same number, for the
// normalised source rectangles), with no gap and no overlap.

#include "petstage.h"

#include <QList>
#include <QRect>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>

namespace murasaki {

struct StageOutput {
    QString name;
    QRect rect;         // logical, in the global layout
    double scale = 1;   // device pixels per logical pixel
    bool primary = false; // the compositor's primary output (what "*" means); the daemon is told which
};

// What one output's window shows.
struct StageSlice {
    QString output;
    QRect global;       // the part of the output the picture covers, in global logical coordinates
    QRect window;       // the same, in the output's own coordinates (the window's)
    QRectF source;      // the part of the picture it shows, normalised: (0, 0) is the picture's top-left, (1, 1) its bottom-right
    double scale = 1;   // of the output
};

struct StageLayout {
    QRectF picture;                 // the fitted picture in global logical coordinates, whole pixels; may be larger than the target (cover)
    QList<StageSlice> slices;       // in the order of the outputs; an output the picture does not touch has none
    QRect target;                   // the rectangle the picture was fitted to: the output, or the bounding box of the outputs
    QStringList degraded;           // names from stagedegrade
    bool isEmpty() const { return slices.isEmpty(); }
};

// The bounding box of a list of rectangles (empty for none or only empty ones).
QRect boundingBox(const QList<QRect> &rects);

// The rectangle `content` takes when fitted to `target`: contain scales it uniformly to the largest size that fits inside and centres it, cover to the
// smallest size that covers `target` (centred, the excess is outside), stretch fills `target`. Empty (a null rectangle) when either is zero, negative
// or not finite.
QRectF fitRect(const QRectF &target, QSizeF content, StageFit fit);

// The stage of a pet. `mode` is Output (the output named `output`; empty or unknown gives no stage) or Desktop (every output, at most
// kMaxStageOutputs; the bounding box of those is the target). `footprint` is the size of the animation after its rotation (petFootprint). Outputs that
// are empty, larger than kMaxStageSide or that would make the target larger than that are left out, and kTooManyOutputs says so.
StageLayout stageLayout(const QList<StageOutput> &outputs, StageMode mode, const QString &output, QSizeF footprint, StageFit fit);

// The footprint of an animation of size `native` turned by `rotation` degrees (what a stage fits): petFootprint, as a QSizeF.
QSizeF stageFootprint(QSizeF native, double rotation);

// The rectangle of the (unrotated) animation inside the window of `slice`, in the window's coordinates, for the pet item to be drawn in: its centre is the
// centre of the fitted picture, and turned by `rotation` it covers the picture. At 0, 90, 180 and 270 degrees each side is scaled by its own factor, so
// `stretch` fills the picture exactly; at other angles one factor is used (the smaller of the two), so a stretched picture there is contained.
QRectF stagePetRect(const StageLayout &layout, const StageSlice &slice, QSizeF native, double rotation, const QRect &outputRect);

// What a pet in the fullscreen mode asks for: the overlay and the daemon build the same request from a placement and the same list of outputs, and so size the same
// copy (docs/pets/stage.md, "Playback").
struct StageRequest {
    StageMode mode = StageMode::Off;
    StageFit fit = StageFit::Contain;
    QString output;     // a connector, or "*" (the primary; the largest output in device pixels when none is told)
    double rotation = 0;
    QSize native;
};

// The outputs sorted by name (the order does not depend on who lists them), which is also how a `desktop` stage picks the first kMaxStageOutputs.
QList<StageOutput> sortedStageOutputs(QList<StageOutput> outputs);
// The name "*" stands for: the primary, else the output with the most device pixels; empty when there are no outputs.
QString resolveStageOutput(const QList<StageOutput> &outputs, const QString &name);
// The layout of a request (empty when the stage can not be drawn). `outputs` may be in any order.
StageLayout stageLayoutOf(const QList<StageOutput> &outputs, const StageRequest &request);
// The device pixels all the requests draw together, and the fraction of the full size they are drawn at to stay within maxPixels.
long long stageRequestPixels(const QList<StageOutput> &outputs, const QList<StageRequest> &requests);
// The longest side the copy of a stage's animation must have: stageNeed() of its layout at the largest scale of the outputs it covers, times `factor`. A request for "*"
// with no primary told is sized for the output that needs the most. 0 when it can not be told (no layout).
int stageRequestNeed(const QList<StageOutput> &outputs, const StageRequest &request, double factor, int sourceLongest);

// Device pixels the slices of a layout draw in total (each window at its own output's scale).
long long stageDevicePixels(const StageLayout &layout);

// The fraction of the full resolution (1 down to a small floor) a stage is drawn at so that it costs at most `maxPixels` device pixels: the square
// root of the budget over the pixels. 1 when it fits or when a number is not usable.
double stageResolutionFactor(long long pixels, long long maxPixels);

// The largest output scale among the slices (1 when none): the picture is wanted at that many device pixels per logical pixel.
double stageMaxScale(const StageLayout &layout);

} // namespace murasaki
