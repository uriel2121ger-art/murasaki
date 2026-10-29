#pragma once

#include "puppet/params.h"

namespace murasaki::puppet {

// Looking at the cursor (docs/pets/puppets.md, section 4): the cursor's place relative to the pet becomes a target in -1 .. 1 on each axis, the head, body and eyes follow it with a
// short smoothing, and the standard parameters (ParamAngleX/Y/Z, ParamBodyAngleX, ParamEyeBallX/Y) are driven by it. Positive x is the screen's right, positive y is up. It is the
// host's decision to run it at all (plan question 6: off until Mouse Tracking look is on).
struct LookAtConfig {
    double reach = 2.0;         // the cursor is "all the way" at this many pet long-sides from the pet's centre
    double deadZone = 0.02;     // a target this close to the centre counts as the centre
    double smoothing = 0.15;    // seconds for the pose to cover most of the way to a new target
};

struct Rect {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};

class LookAt
{
public:
    explicit LookAt(LookAtConfig config = {}) : m_config(config) {}
    // The target for a cursor at (cx, cy) (screen pixels, y down) and a pet on screen at `pet`. A degenerate pet gives the centre.
    Vec2 targetFor(double cx, double cy, const Rect &pet) const;
    void setTarget(Vec2 target);
    void update(double dt);
    Vec2 current() const { return m_current; }
    // Writes the parameters that exist; returns how many. A parameter's side of the range is used for each sign (x >= 0 reaches max, x < 0 reaches min).
    int apply(ParameterSet &parameters) const;

private:
    LookAtConfig m_config;
    Vec2 m_target;
    Vec2 m_current;
};

} // namespace murasaki::puppet
