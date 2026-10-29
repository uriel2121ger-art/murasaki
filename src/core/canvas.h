#pragma once

#include <QHash>
#include <QList>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include <optional>

namespace murasaki {

// How content fits a rectangle: the assignment's `fill` option.
enum class FillMode { Fill, Fit, Stretch };

std::optional<FillMode> parseFillMode(const QString &text); // "fill", "fit" or "stretch"

// An output as the compositor lays it out: logical is its rectangle in the logical coordinate space (device pixels
// divided by the output's scale, with the position the user arranged it at).
struct SpanOutput {
    QString name;
    QRectF logical;
};

// One wallpaper drawn across several outputs (the assignment option `span`). The canvas is the bounding box of the
// outputs' logical rectangles, with its corner at (0, 0); each output shows the viewport of the canvas that it
// covers on the desktop. Everything is in logical pixels: an output renders its viewport at its own scale, so a
// mixed-scale layout stays one picture.
struct SpanLayout {
    QSizeF canvas;
    QHash<QString, QRectF> viewports; // output name -> its rectangle of the canvas

    bool isValid() const { return !canvas.isEmpty(); }
};

// Outputs with an empty logical rectangle are left out, and so is a repeated name (the first stays). The result is
// not valid when no output is left.
SpanLayout spanLayout(const QList<SpanOutput> &outputs);

// Where content of the given size lies on a canvas under the fill mode: Fill covers it and is cropped at the
// edges, Fit is contained and centered (the rest is empty), Stretch ignores the aspect ratio. The rectangle may
// reach beyond the canvas (Fill) or leave bands of it (Fit).
QRectF placeContent(const QSizeF &canvas, const QSizeF &content, FillMode fill);

// What one output of a spanned canvas draws.
struct ViewportMapping {
    // False when the content does not reach the output at all (a Fit letterbox band wider than the output).
    bool visible = false;
    // The part of the content shown, in content pixels.
    QRectF source;
    // Where it lies on the output, in the output's logical pixels with the origin at its corner. For Fill and
    // Stretch that is the whole output; for Fit it may be a part of it.
    QRectF dest;
};

// The fill mode applies to the whole canvas, so the viewport is a window onto the placed content.
ViewportMapping mapViewport(const QSizeF &canvas, const QRectF &viewport, const QSizeF &content, FillMode fill);

// The same mapping as the properties of an mpv player that fills a window of the viewport's size: keep the
// content's own aspect (or the canvas's, for Stretch) with `video-aspect-override`, zoom (`video-zoom`, a power of
// two) and pan (`video-pan-x/y`, fractions of the zoomed video's size, positive to the right and down) so that the
// whole picture lies where placeContent() puts it. A player that has to show nothing is not shown at all.
struct MpvViewport {
    bool visible = false;
    double aspect = 1;
    double zoom = 0;
    double panX = 0;
    double panY = 0;
};

MpvViewport mpvViewport(const QSizeF &canvas, const QRectF &viewport, const QSizeF &content, FillMode fill);

} // namespace murasaki
