#pragma once

#include <QtGlobal>

namespace murasaki::import {

// The colour key (docs/pets/import.md, "Colour key"): an explicit tool, not a model. Alpha is set from how far a pixel's colour is from the key colour:
//   d = Euclidean distance in linear RGB, divided by the largest possible distance (so 0..1);
//   d <= tolerance                      -> fully transparent;
//   tolerance < d < tolerance + softness -> alpha scaled by (d - tolerance) / softness, and the key colour's spill removed from the pixel;
//   beyond                               -> unchanged.
// Existing alpha is multiplied, never raised. tolerance and softness are 0..1. A pure function of the pixel and the key.
struct ColorKey {
    bool enabled = false;
    quint8 red = 0;
    quint8 green = 255;
    quint8 blue = 0;
    double tolerance = 0.5;
    double softness = 0.1;
};

// RGBA8888, straight alpha, `stride` bytes between rows. Does nothing when the key is not enabled.
void applyColorKey(uchar *rgba, int width, int height, qsizetype stride, const ColorKey &key);

} // namespace murasaki::import
