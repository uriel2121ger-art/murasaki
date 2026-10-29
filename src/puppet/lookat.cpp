#include "puppet/lookat.h"

#include <algorithm>
#include <cmath>

namespace murasaki::puppet {

namespace {

double clamp1(double v)
{
    return std::clamp(v, -1.0, 1.0);
}

// 0 toward the side of the range the sign asks for.
double reach(const Parameter &p, double n)
{
    return n >= 0 ? n * p.max : -n * p.min;
}

} // namespace

Vec2 LookAt::targetFor(double cx, double cy, const Rect &pet) const
{
    if (!std::isfinite(cx) || !std::isfinite(cy) || !(pet.width > 0) || !(pet.height > 0) || !std::isfinite(pet.x) || !std::isfinite(pet.y))
        return {};
    const double longSide = std::max(pet.width, pet.height);
    const double distance = m_config.reach * longSide;
    Vec2 t{clamp1((cx - (pet.x + pet.width / 2)) / distance), clamp1(-(cy - (pet.y + pet.height / 2)) / distance)};
    if (std::hypot(t.x, t.y) < m_config.deadZone)
        return {};
    return t;
}

void LookAt::setTarget(Vec2 target)
{
    if (std::isfinite(target.x) && std::isfinite(target.y))
        m_target = {clamp1(target.x), clamp1(target.y)};
}

void LookAt::update(double dt)
{
    if (!std::isfinite(dt) || dt <= 0)
        return;
    const double alpha = m_config.smoothing > 0 ? 1.0 - std::exp(-dt / m_config.smoothing) : 1.0;
    m_current.x += (m_target.x - m_current.x) * alpha;
    m_current.y += (m_target.y - m_current.y) * alpha;
}

int LookAt::apply(ParameterSet &parameters) const
{
    int touched = 0;
    const auto write = [&](const char *id, double n) {
        const int index = parameters.find(id);
        if (index < 0)
            return;
        parameters.set(index, reach(parameters.at(static_cast<size_t>(index)), n));
        ++touched;
    };
    write("ParamAngleX", m_current.x);
    write("ParamAngleY", m_current.y);
    write("ParamAngleZ", -m_current.x * m_current.y); // a head turned right and up leans back to the left
    write("ParamBodyAngleX", m_current.x);
    write("ParamEyeBallX", m_current.x);
    write("ParamEyeBallY", m_current.y);
    return touched;
}

} // namespace murasaki::puppet
