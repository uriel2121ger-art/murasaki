#include "pettiming.h"

#include <algorithm>
#include <cmath>

namespace murasaki {

int fixedFrameMs(int fps)
{
    return fps > 0 ? std::max(1, int(std::lround(1000.0 / fps))) : 0;
}

void FrameClock::setFrames(const QList<int> &fileDelaysMs, double now)
{
    m_file.assign(fileDelaysMs.begin(), fileDelaysMs.end());
    for (double &delay : m_file)
        delay = std::max(1.0, delay);
    m_shown = 0;
    m_origin = now;
    m_lastChange = now;
    m_pausedAt = now;
    m_loopsBefore = 0;
    rebuild();
}

void FrameClock::setTiming(PetTiming timing, double now)
{
    if (timing.fps < 0 || timing.fps > kMaxPetFps)
        timing.fps = 0;
    if (!std::isfinite(timing.speed))
        timing.speed = 1;
    timing.speed = std::clamp(timing.speed, kMinPetSpeed, kMaxPetSpeed);
    if (timing == m_timing)
        return;
    const double reference = m_paused ? m_pausedAt : now;
    const long long before = loopsAt(reference);
    m_timing = timing;
    rebuild();
    // The frame on screen stays, and the clock goes on from its start; the wraps counted so far stay counted.
    m_origin = reference - startOfShown();
    m_loopsBefore = before;
}

void FrameClock::pause(double now)
{
    if (m_paused)
        return;
    m_paused = true;
    m_pausedAt = now;
}

void FrameClock::resume(double now)
{
    if (!m_paused)
        return;
    m_paused = false;
    m_origin += now - m_pausedAt;
    m_lastChange += now - m_pausedAt;
}

long long FrameClock::loopsAt(double time) const
{
    if (m_ends.size() < 2 || !(m_total > 0))
        return m_loopsBefore;
    const double whole = std::floor((time - m_origin) / m_total);
    return m_loopsBefore + std::max<long long>(0, static_cast<long long>(std::min(whole, 9e15)));
}

long long FrameClock::loops(double now) const
{
    return loopsAt(m_paused ? m_pausedAt : now);
}

double FrameClock::wrapTimeMs(long long loop) const
{
    return m_origin + double(loop - m_loopsBefore) * m_total;
}

double FrameClock::nextWrapMs(double now) const
{
    if (m_ends.size() < 2 || m_paused || !(m_total > 0))
        return -1;
    return wrapTimeMs(loops(now) + 1);
}

double FrameClock::frameMs(int index) const
{
    if (index < 0 || index >= int(m_ends.size()))
        return 0;
    return m_ends[size_t(index)] - (index == 0 ? 0.0 : m_ends[size_t(index) - 1]);
}

double FrameClock::startOfShown() const
{
    if (m_shown <= 0 || m_shown >= int(m_ends.size()))
        return 0;
    return m_ends[size_t(m_shown) - 1];
}

void FrameClock::rebuild()
{
    m_ends.clear();
    m_ends.reserve(m_file.size());
    double end = 0;
    for (const double delay : m_file) {
        double length = delay;
        if (m_timing.fps > 0)
            length = fixedFrameMs(m_timing.fps);
        else
            length = delay / m_timing.speed;
        end += length;
        m_ends.push_back(end);
    }
    m_total = end;
    if (m_shown >= int(m_ends.size()))
        m_shown = 0;
}

FrameClock::Step FrameClock::update(double now)
{
    if (m_ends.size() < 2 || m_paused || !(m_total > 0))
        return {m_shown, -1, loops(now)};
    double position = std::fmod(now - m_origin, m_total);
    if (position < 0)
        position += m_total;
    const auto it = std::upper_bound(m_ends.begin(), m_ends.end(), position);
    const int target = std::min(int(it - m_ends.begin()), int(m_ends.size()) - 1);
    const double interval = m_capFps > 0 ? 1000.0 / m_capFps : 0.0;
    if (target != m_shown) {
        const double ready = m_lastChange + interval;
        if (now + 1e-6 < ready)
            return {m_shown, ready, loops(now)}; // due, but the cap says wait; the frames passed meanwhile are skipped
        m_shown = target;
        m_lastChange = now;
    }
    // The shown frame is the one at `now`, or the cap held it: ask again when the next one starts, not before the cap allows.
    const double boundary = now - position + m_ends[size_t(m_shown)];
    return {m_shown, std::max(boundary, m_lastChange + interval), loops(now)};
}

} // namespace murasaki
