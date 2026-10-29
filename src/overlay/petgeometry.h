#pragma once

// The geometry of a pet window, pure and header only: the footprint of a rotated pet, the window rectangle around it,
// the clamp that keeps a pet reachable, and the hit test of a point against a rotated and flipped pet. No Qt types, so
// the state machine and the tests use it as it is.
//
// Conventions (the same as Qt Quick's, y grows downward):
//   - A pet is a rectangle (x, y, w, h) in output coordinates: its position and size before rotation. `rotation` is in
//     degrees, clockwise, about the centre of that rectangle.
//   - Flips mirror the picture in its own frame, then the rotation turns the mirrored picture. So the local point of a
//     hit test is the point of the unrotated, unflipped image that is drawn under the pointer.
//   - The window of a rotated pet is the bounding box of the turned rectangle (its footprint), centred on the pet's centre.
//   - Every function is total: a NaN or infinite input gives a defined answer (zero size, no hit, the origin), never undefined
//     behaviour. Positions and sizes are doubles; the window code rounds them (see petWindowRect).

#include <algorithm>
#include <cmath>
#include <optional>

namespace murasaki {

struct PetPoint {
    double x = 0;
    double y = 0;
};

struct PetSize {
    double w = 0;
    double h = 0;
};

struct PetRect {
    double x = 0;
    double y = 0;
    double w = 0;
    double h = 0;

    double right() const { return x + w; }
    double bottom() const { return y + h; }
    PetPoint center() const { return {x + w / 2, y + h / 2}; }
    PetPoint topLeft() const { return {x, y}; }
    // Half open, like QRectF::contains for the purposes of a pointer: the right and bottom edges are outside.
    bool contains(PetPoint p) const { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; }
    PetRect translated(double dx, double dy) const { return {x + dx, y + dy, w, h}; }
    PetRect inflated(double margin) const { return {x - margin, y - margin, w + 2 * margin, h + 2 * margin}; }
    bool operator==(const PetRect &) const = default;
};

inline constexpr double kPetPi = 3.14159265358979323846;

// The rotation wrapped to [0, 360).
inline double normalizeRotation(double degrees)
{
    if (!std::isfinite(degrees))
        return 0.0;
    double r = std::fmod(degrees, 360.0);
    if (r < 0)
        r += 360.0;
    return r >= 360.0 ? 0.0 : r;
}

// The bounding box of a w x h rectangle turned by `degrees`: w|cos a| + h|sin a| by w|sin a| + h|cos a|. Exact at the
// multiples of 90 degrees (a window size is a pixel count: no 100.00000000000001).
inline PetSize petFootprint(PetSize size, double degrees)
{
    size.w = std::isfinite(size.w) ? std::max(0.0, size.w) : 0.0;
    size.h = std::isfinite(size.h) ? std::max(0.0, size.h) : 0.0;
    const double r = normalizeRotation(degrees);
    if (r == 0.0 || r == 180.0)
        return size;
    if (r == 90.0 || r == 270.0)
        return {size.h, size.w};
    const double a = r * kPetPi / 180.0;
    const double c = std::fabs(std::cos(a));
    const double s = std::fabs(std::sin(a));
    return {size.w * c + size.h * s, size.w * s + size.h * c};
}

// Rounding: the window is whole pixels (`petCeilPx`, `lround` of the position) while the scene graph centres the pet in it. With an odd difference between the
// footprint and the pet at an angle that is not a multiple of 90 degrees, the pet's centre can be up to a pixel off the true one and the picture sits at a half
// pixel; the linear sampler blurs it a little, which a turned picture does anyway. At the multiples of 90 everything is pixel aligned.
// The window rectangle of a pet: the footprint centred on the pet's own centre. The margins of a top-left anchored layer
// surface are its x and y. `x`, `y` and `w` keep meaning "the unrotated pet's top-left and width". The result is unrounded:
// the window is `petCeilPx` of its size, so a caller rounds the position consistently with it (for example
// `floor(x)` and `ceil` of the size) or accepts up to half a pixel of offset from the centre.
inline PetRect petWindowRect(PetRect pet, double degrees)
{
    const PetSize f = petFootprint({pet.w, pet.h}, degrees);
    return {pet.x - (f.w - pet.w) / 2, pet.y - (f.h - pet.h) / 2, f.w, f.h};
}

// Whole pixels for a window: the smallest integer size that holds `value` (a tiny epsilon keeps 120.0000000001 at 120).
inline int petCeilPx(double value)
{
    if (!std::isfinite(value))
        return 1;
    return int(std::clamp(std::ceil(value - 1e-6), 1.0, 1e9));
}

// std::clamp without the precondition lo <= hi: a degenerate range gives lo.
inline double clampSafe(double v, double lo, double hi)
{
    if (!std::isfinite(v))
        return lo;
    return hi < lo ? lo : std::clamp(v, lo, hi);
}

// A window dragged off the output would be lost: keep at least `keep` px of it (or all of it when smaller) inside. The same
// rule as arrange mode's clampToOutput, for a window of any size. `output` is a rectangle in the same coordinates as `window`.
inline PetRect clampToOutput(PetRect window, PetRect output, double keep = 48)
{
    const double keepX = std::min(keep, window.w);
    const double keepY = std::min(keep, window.h);
    window.x = clampSafe(window.x, output.x + keepX - window.w, output.right() - keepX);
    window.y = clampSafe(window.y, output.y + keepY - window.h, output.bottom() - keepY);
    return window;
}

// A window kept entirely inside the output (smart dodge): a window larger than the output sticks to the top-left.
inline PetRect clampInsideOutput(PetRect window, PetRect output)
{
    window.x = clampSafe(window.x, output.x, output.right() - window.w);
    window.y = clampSafe(window.y, output.y, output.bottom() - window.h);
    return window;
}

inline PetPoint rotatePoint(PetPoint p, PetPoint about, double degrees)
{
    const double a = degrees * kPetPi / 180.0;
    const double c = std::cos(a);
    const double s = std::sin(a);
    const double dx = p.x - about.x;
    const double dy = p.y - about.y;
    return {about.x + dx * c - dy * s, about.y + dx * s + dy * c};
}

// The top-left of a pet whose size changes from `before` to newW x newH while the corner OPPOSITE to the dragged one (its top-left corner in its own frame, the
// drag handle being the bottom-right) stays where it is on the screen. The pet turns about its own centre, so growing it with (x, y) fixed would move that
// corner; solving for the centre keeps it. Flips do not matter: the controls are not mirrored with the picture. At rotation 0 the result is (before.x, before.y).
inline PetPoint petTopLeftKeepingTopLeftCorner(PetRect before, PetSize newSize, double degrees)
{
    const double r = normalizeRotation(degrees);
    const PetPoint anchor = rotatePoint(before.topLeft(), before.center(), r); // where the corner is now, on the screen
    // the corner is at (-w/2, -h/2) from the centre in the pet's frame; turned, it is offset from the centre by rotatePoint of that vector
    const PetPoint offset = rotatePoint({-newSize.w / 2, -newSize.h / 2}, {0, 0}, r);
    const PetPoint centre{anchor.x - offset.x, anchor.y - offset.y};
    return {centre.x - newSize.w / 2, centre.y - newSize.h / 2};
}

// Where a pointer at `p` (output coordinates) falls on the picture of a pet at `pet`, rotated and flipped: the point of the
// unrotated, unflipped image, in [0, w) x [0, h), or nothing when the pointer is outside the turned rectangle.
inline std::optional<PetPoint> pointInPet(PetPoint p, PetRect pet, double degrees, bool flipH, bool flipV)
{
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(pet.x) || !std::isfinite(pet.y))
        return std::nullopt;
    const PetPoint c = pet.center();
    PetPoint q = rotatePoint(p, c, -normalizeRotation(degrees));
    if (flipH)
        q.x = 2 * c.x - q.x;
    if (flipV)
        q.y = 2 * c.y - q.y;
    const PetPoint local{q.x - pet.x, q.y - pet.y};
    if (local.x < 0 || local.y < 0 || local.x >= pet.w || local.y >= pet.h)
        return std::nullopt;
    return local;
}

} // namespace murasaki
