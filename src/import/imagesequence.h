#pragma once

#include "importspec.h"

#include <QList>
#include <QString>

namespace murasaki::import {

struct ImageDim {
    int width = 0;
    int height = 0;
    bool operator==(const ImageDim &) const = default;
};

// Where one image's pixels go on the canvas: scaled to width x height (equal to the image's own size unless the policy scales), at (x, y).
struct Placement {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool operator==(const Placement &) const = default;
};

struct SequencePlan {
    int canvasWidth = 0;
    int canvasHeight = 0;
    QList<Placement> placements; // one per image, in the order given
    QString error;               // a name of petimport.h (+ ": detail"); empty when the plan is valid
    bool ok() const { return error.isEmpty(); }
};

// The canvas of a set of images and where each goes (docs/pets/import.md, "Images"). Pure arithmetic on the headers' sizes, so a hostile set is
// refused before any pixel is decoded: fewer than two images, more than kMaxFrames, a side that is not 1..kMaxCanvasSide, a canvas over the limit.
//   first: the canvas is the first image's size and every image must have it (else images: sizes differ);
//   fit:   the canvas is the largest width by the largest height, each image centred and unscaled;
//   scale: the canvas is the first image's size and every image is scaled to it.
SequencePlan planSequence(const QList<ImageDim> &sizes, SizePolicy policy);

} // namespace murasaki::import
