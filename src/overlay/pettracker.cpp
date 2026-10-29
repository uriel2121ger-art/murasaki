#include "pettracker.h"

#include <algorithm>
#include <cmath>

namespace murasaki {

namespace {
bool finite(PetPoint p)
{
    return std::isfinite(p.x) && std::isfinite(p.y);
}
} // namespace

PetTracker::PetTracker(PetTrackerTuning tuning)
    : m_tuning(tuning)
{
}

std::int64_t PetTracker::clock(std::int64_t nowMs)
{
    m_now = std::max(m_now, nowMs);
    return m_now;
}

void PetTracker::setConfig(const PetTrackerConfig &config, std::int64_t nowMs)
{
    const std::int64_t now = clock(nowMs);
    if (config == m_config)
        return;
    const bool looked = m_config.mode == PetTrackingMode::Look;
    const bool followed = m_config.mode == PetTrackingMode::Follow;
    m_config = config;
    if (m_config.mode == PetTrackingMode::Follow) {
        m_phase = Phase::Following;
        m_commit.reset();
    } else if (followed) {
        endFollow();
    }
    if (m_config.mode != PetTrackingMode::Look || !looked) {
        m_mirror.reset();
        m_wanted.reset();
    }
    evaluateLook(now);
}

void PetTracker::setGeometry(PetRect home, PetRect output)
{
    m_home = home;
    m_output = output;
}

void PetTracker::cursor(std::int64_t nowMs, PetPoint position)
{
    const std::int64_t now = clock(nowMs);
    if (!finite(position))
        return;
    m_cursor = position;
    evaluateLook(now);
    step(now);
}

void PetTracker::tick(std::int64_t nowMs)
{
    const std::int64_t now = clock(nowMs);
    evaluateLook(now);
    step(now);
}

void PetTracker::reset(std::int64_t nowMs)
{
    halt(nowMs);
    m_cursor.reset();
    m_mirror.reset();
    m_wanted.reset();
}

void PetTracker::endFollow()
{
    m_moving = false;
    if (m_offset.x == 0 && m_offset.y == 0) {
        m_phase = Phase::None;
    } else if (m_config.returnHome) {
        m_phase = Phase::Returning;
    } else {
        m_phase = Phase::None;
        m_commit = m_offset; // the owner stores it once; the offset stays until it has
    }
}

void PetTracker::halt(std::int64_t nowMs)
{
    clock(nowMs);
    m_moving = false;
    m_held = false; // (the window that held the pet is gone, or the pet is not shown: no Release will come to say so)
    m_phase = Phase::None;
    if (m_offset.x != 0 || m_offset.y != 0) {
        if (m_config.returnHome || m_config.mode != PetTrackingMode::Follow)
            m_offset = {};
        else if (!m_commit)
            m_commit = m_offset;
    }
    if (m_config.mode == PetTrackingMode::Follow)
        m_phase = Phase::Following;
}

void PetTracker::hold(bool on, std::int64_t nowMs)
{
    const std::int64_t now = clock(nowMs);
    if (on == m_held)
        return;
    m_held = on;
    m_moving = false;
    if (!on)
        m_lastStep = now;
}

void PetTracker::park(bool on, std::int64_t nowMs)
{
    const std::int64_t now = clock(nowMs);
    if (on == m_parked)
        return;
    m_parked = on;
    m_moving = false;
    if (!on)
        m_lastStep = now;
}

void PetTracker::adopt()
{
    m_offset = {};
    m_commit.reset();
    m_moving = false;
    if (m_phase == Phase::Returning)
        m_phase = Phase::None;
}

std::optional<PetPoint> PetTracker::takeCommit()
{
    const auto commit = m_commit;
    if (commit) {
        m_commit.reset();
        m_offset = {};
    }
    return commit;
}

void PetTracker::step(std::int64_t now)
{
    if (m_phase == Phase::None)
        return;
    if (m_held || m_parked) {
        m_moving = false;
        return;
    }
    if (now - m_lastStep < m_tuning.stepIntervalMs)
        return;
    const double dt = std::min<std::int64_t>(now - m_lastStep, 100) / 1000.0;
    m_lastStep = now;
    const PetPoint centre{m_home.center().x + m_offset.x, m_home.center().y + m_offset.y};
    double dx = 0, dy = 0, remaining = 0;
    if (m_phase == Phase::Following) {
        if (!m_cursor) {
            m_moving = false;
            return;
        }
        dx = m_cursor->x - centre.x;
        dy = m_cursor->y - centre.y;
        const double dist = std::hypot(dx, dy);
        if (dist <= m_config.distance + m_tuning.arriveEpsilon) {
            m_moving = false;
            return;
        }
        remaining = dist - m_config.distance;
        dx /= dist;
        dy /= dist;
    } else {
        dx = -m_offset.x;
        dy = -m_offset.y;
        const double dist = std::hypot(dx, dy);
        if (dist <= m_tuning.arriveEpsilon) {
            m_offset = {};
            m_phase = Phase::None;
            m_moving = false;
            return;
        }
        remaining = dist;
        dx /= dist;
        dy /= dist;
    }
    const double cap = m_tuning.baseSpeed * std::clamp(m_config.speed, kMinTrackingSpeed, kMaxTrackingSpeed);
    const double pace = std::min(cap, remaining * m_tuning.easeRate);
    const double length = std::min(remaining, pace * dt);
    PetRect rect = m_home.translated(m_offset.x + dx * length, m_offset.y + dy * length);
    if (m_phase == Phase::Following) {
        // inside the output; on an axis where the picture is as big as the output or bigger it has nowhere to go and stays (clamping it would throw it to one side)
        const PetRect inside = clampInsideOutput(rect, m_output);
        if (m_home.w < m_output.w)
            rect.x = inside.x;
        else
            rect.x = m_home.x;
        if (m_home.h < m_output.h)
            rect.y = inside.y;
        else
            rect.y = m_home.y;
    }
    const PetPoint next{rect.x - m_home.x, rect.y - m_home.y};
    const bool moved = std::hypot(next.x - m_offset.x, next.y - m_offset.y) >= 0.01;
    m_moving = moved;
    if (moved)
        m_offset = next;
}

void PetTracker::evaluateLook(std::int64_t now)
{
    if (m_config.mode != PetTrackingMode::Look || !m_cursor)
        return;
    const double dx = m_cursor->x - m_home.center().x;
    if (std::abs(dx) >= m_tuning.deadZonePx) {
        // The picture faces right: the cursor on its left asks for the mirror; an item that faces left is the other way round.
        const bool want = m_config.facesLeft ? dx > 0 : dx < 0;
        m_wanted = want;
    } else {
        m_wanted.reset(); // inside the dead zone nothing is asked: the picture stays as it is
    }
    if (!m_wanted || m_wanted == m_mirror)
        return;
    if (m_mirror && now - m_lastFlip < m_tuning.minFlipIntervalMs)
        return; // held back; tick() applies it when the interval is over
    m_mirror = *m_wanted;
    m_lastFlip = now;
    m_wanted.reset();
}

std::optional<std::int64_t> PetTracker::nextWakeMs() const
{
    if (!m_held && !m_parked && (m_phase == Phase::Returning || (m_phase == Phase::Following && m_moving)))
        return m_lastStep + m_tuning.stepIntervalMs;
    if (m_config.mode == PetTrackingMode::Look && m_wanted && m_wanted != m_mirror)
        return m_lastFlip + m_tuning.minFlipIntervalMs;
    return std::nullopt;
}

} // namespace murasaki
